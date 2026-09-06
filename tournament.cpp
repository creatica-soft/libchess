// compile with c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -Wl,-lchess,-rpath,/Users/ap/libchess -L /Users/ap/libchess -o tournament tournament.cpp

#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <chrono>
#include <format>
#include <math.h>
#include <vector>
#include <string>
#include <iostream>
#include <unordered_map>
#include "nnue/bitboard.h"
#include "libchess.h" // Ensure this path is correct

// --- Configuration ---
//A/B test of ONE option. Both sides are the same binary, so any difference in result is
//attributable to ReuseTree and nothing else -- no difference in net, search, or build.
const char * ENGINE_1 = "/Users/ap/libchess/creatica";   // ReuseTree ON
const char * ENGINE_2 = "/Users/ap/libchess/creatica";   // ReuseTree OFF (the incumbent)
const char * SYZYGY_PATH = "/Users/ap/syzygy";
#define PGN_FILE "reuse_match.pgn"
//1000 ms rather than 2000: a comparison is only as good as its sample size, and the ordering of
//two configurations survives a shorter time control even where the absolute strength does not.
#define MOVETIME 1000
#define DEPTH 0
//Identical resources on both sides -- this is an A/B test, so anything asymmetric would
//confound it. 4 threads each (measured best on four performance cores) and 1 GB of tree each;
//only one side searches at a time, so the machine is not oversubscribed.
//
//Hash matters more than usual here: with reuse the tree is not discarded every move, so it
//grows until GcThreshold. Both sides get the same allocation so neither is advantaged.
#define HASH_1 1024
#define HASH_2 1024
#define THREADS_1 4
#define THREADS_2 4
//Unused: creatica advertises neither UCI_LimitStrength nor UCI_Elo. Kept so the
//initChessEngine() call below stays readable.
#define OPPONENT_ELO 0

//Per-engine settings, BY NAME.
//
//initChessEngine() only knows the handful of options every engine has -- Hash, Threads,
//MultiPV, SyzygyPath, UCI_Elo -- so a match could previously only ever compare two engines at
//their default settings. Anything engine-specific had no way in. Listing options by name here
//means either side can be configured, including options that exist in one engine and not the
//other; a name the engine does not advertise is reported and skipped rather than written into
//whatever option happens to sit at that position in its list.
//
//This is also how a match answers "does this setting help": put the same binary on both sides
//and change one line.
struct NamedSpin  { const char * name; int64_t value; };
struct NamedCheck { const char * name; bool    value; };

//Leave a list empty to run that engine at its own defaults.
//THE ONE VARIABLE. Everything else is identical between the sides.
static const std::vector<NamedSpin>  SPINS_1  = { };
static const std::vector<NamedCheck> CHECKS_1 = { {"ReuseTree", true},
                                                  {"FinalInfoLines", false},
                                                  {"IntermittentInfoLines", false} };

static const std::vector<NamedSpin>  SPINS_2  = { };
static const std::vector<NamedCheck> CHECKS_2 = { {"FinalInfoLines", false},
                                                  {"IntermittentInfoLines", false} };

//Which side is which, for the PGN. With the same binary on both sides both [White] and [Black]
//read "creatica", so without this the games are unattributable after the fact -- and a match
//whose result cannot be traced back to a configuration is not a measurement.
static std::string optionsLabel(const std::vector<NamedSpin>& spins,
                                const std::vector<NamedCheck>& checks) {
    std::string out;
    for (const NamedSpin& o : spins) {
        if (!out.empty()) out += ", ";
        out += std::string(o.name) + "=" + std::to_string((long long)o.value);
    }
    for (const NamedCheck& o : checks) {
        if (!out.empty()) out += ", ";
        out += std::string(o.name) + "=" + (o.value ? "true" : "false");
    }
    return out.empty() ? "defaults" : out;
}

static void applyNamedOptions(Engine& e, const std::vector<NamedSpin>& spins,
                              const std::vector<NamedCheck>& checks) {
    for (const NamedSpin& o : spins)   setEngineSpin(e, o.name, o.value);
    for (const NamedCheck& o : checks) setEngineCheck(e, o.name, o.value);
    setOptions(e);
}

// Tuning Settings
#define GAMES_PER_MATCH 40  // 40 resolves ~100 Elo at the observed draw rates; 10-20 resolves nothing

// Global Engine Objects
struct Engine engine_1;
struct Engine engine_2;
struct Board board = {};
struct Zobrist z = {};
struct ZobristHash zh = {};
struct Evaluation evaluation = {};
struct Evaluation * evaluations[1] = { nullptr };
char sanMoves[4096] = "";

// A small suite of balanced opening positions (FENs) to ensure game variety.
// This prevents the "identical game" loop.
const std::vector<const char*> opening_fens = {
    "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",        // 1. e4 e5
    "rnbqkbnr/pppp1ppp/8/4p3/3P4/8/PPP1PPPP/RNBQKBNR w KQkq - 0 2",        // 1. d4 e5 (Englund/Borg) - aggressive
    "rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",        // Sicilian
    "rnbqkbnr/pp1ppppp/2p5/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",        // Caro-Kann
    "rnbqkbnr/pp1ppppp/8/2p5/3P4/8/PPP1PPPP/RNBQKBNR w KQkq - 0 2",         // Old Benoni
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",             // Start
    "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",          // 1. e4
    "rnbqkbnr/pppppppp/8/8/3P4/8/PPP1PPPP/RNBQKBNR b KQkq - 0 1",          // 1. d4
    "rnbqkbnr/pppppppp/8/8/2P5/8/PP1PPPPP/RNBQKBNR b KQkq - 0 1",          // 1. c4
    "rnbqkbnr/pppppppp/8/8/8/5N2/PPPPPPPP/RNBQKB1R b KQkq - 1 1"          // 1. Nf3
};


// Returns 1.0 for White win, 0.0 for Black win, 0.5 for Draw
double play_one_game(Engine& white, Engine& black, int game_id, const char* start_fen, double& score_engine_1, double& score_engine_2, FILE* pgnFile) {
  newGame(white);
  newGame(black);
  fen2board(board, start_fen);
  bool whiteMove = board.sideToMove == ColorWhite ? true : false;
  int moveNumber = board.moveNumber;
  sanMoves[0] = 0;
  
  std::unordered_map<unsigned long long, int> position_history;
  bool repetition = false;
  char fen[MAX_FEN_STRING_LEN] = "";

  isCheckMateStaleMate(board);
  while (!board.isMate && !board.isStaleMate && board.halfmoveClock < 100 && !repetition) {
    if (board.sideToMove == ColorWhite) {
      strncpy(white.position, board2fen(board, fen), MAX_FEN_STRING_LEN);
      if (!position(white)) {
        if (game_id % 2) //engine_1 is white
          fprintf(stderr, "Error in engine communication (White - engine 1)\n");
        else //engine_2 is white
          fprintf(stderr, "Error in engine communication (White - engine 2)\n");
        exit(1);        
      }
      if (go(white, evaluations)) {
        if (game_id % 2) //engine_1 is white
          fprintf(stderr, "Error in engine communication (White - engine 1)\n");
        else //engine_2 is white
          fprintf(stderr, "Error in engine communication (White - engine 2)\n");
        exit(1);
      }    
    } else { //sideToMove == ColorBlack
      strncpy(black.position, board2fen(board, fen), MAX_FEN_STRING_LEN);
      if (!position(black)) {
        if (game_id % 2) //engine_2 is black
          fprintf(stderr, "Error in engine communication (Black - engine 2)\n");
        else //engine_1 is black
          fprintf(stderr, "Error in engine communication (Black - engine 1)\n");
        exit(1);        
      }
      if (go(black, evaluations)) {
        if (game_id % 2) //engine_2 is black
          fprintf(stderr, "Error in engine communication (Black - engine 2)\n");
        else //engine_1 is black
          fprintf(stderr, "Error in engine communication (Black - engine 1)\n");
        exit(1);
      }    
    }
    Move move = {}; //was uninitialised, so move.type below was stack garbage
    uci2move_idx(evaluations[0]->bestmove, move);
    //move2san() reads move.type to decide whether to emit 'x', but ff_move() further
    //down is what actually SETS that type - so every capture in every match PGN was
    //written without its capture marker ("Nf7" for Nxf7, "d4" for exd4), which is not
    //legal SAN and cannot be re-imported. move2san needs the PRE-move board for
    //disambiguation, so it cannot simply be moved after ff_move; classify here instead.
    if (board.piecesOnSquares[move.dst] != PieceNone) move.type = MoveTypeCapture;
    //Castling likewise, or it is written as a plain king move ("Kc1" instead of O-O-O).
    //Standard chess only: Chess960 encodes castling as king-takes-own-rook.
    if (!board.isChess960 && PC_TYPE(board.piecesOnSquares[move.src]) == King) {
      const int d = (int)move.dst - (int)move.src;
      if (d == 2) move.type = MoveTypeCastlingKingside;
      else if (d == -2) move.type = MoveTypeCastlingQueenside;
    }
    char sanMove[12] = "";
    strcat(sanMoves, move2san(board, move, sanMove));
    strcat(sanMoves, " ");        
    fprintf(stdout, "Game %d of %d. Move %d: %s, cp %d, score: %.1f-%.1f\n", game_id, GAMES_PER_MATCH, board.moveNumber, evaluations[0]->bestmove, evaluations[0]->scorecp, score_engine_1, score_engine_2);
    int captured = ff_move(board, move);
    writeDebug(board);
    updateHash(zh, board, move, captured, z);
    if (++position_history[zh.hash] >= 3) { repetition = true; break; }
    isCheckMateStaleMate(board);
  }

  // Result
  double result = 0.5;
  if (board.isMate) {
    if (board.sideToMove == ColorWhite) {
      result = 0.0; //white lost, black win
      if (game_id % 2) //engine_1 is white
        score_engine_2 += 1;
      else score_engine_1 += 1; //engine_1 is black
    } else {
      result = 1.0; //white win, black lost
      if (game_id % 2) //engine_1 is white
        score_engine_1 += 1;
      else score_engine_2 += 1; //engine_1 is black
    }
  } else if (board.isStaleMate || repetition || board.halfmoveClock >= 100) {
      result = 0.5;
      score_engine_1 += 0.5;
      score_engine_2 += 0.5;
  } else if (__builtin_popcountl(board.side[ColorWhite] | board.side[ColorBlack]) <= 5 && evaluations[0]->scorecp == 0) {
      result = 0.5; // Insufficient material heuristic
      score_engine_1 += 0.5;
      score_engine_2 += 0.5;
  }
    
  char res[8] = "*"; //"*" is the PGN tag for an unfinished/unknown result - this was
                     //uninitialised, so any game ending outside the branches below wrote
                     //stack garbage into the [Result] tag.
  if (board.isMate) {
    if (board.sideToMove == ColorWhite) strcpy(res, "0-1");
    else strcpy(res, "1-0"); 
  } 
  else if (board.isStaleMate || board.halfmoveClock == 100 || (__builtin_popcountl(board.side[ColorWhite] | board.side[ColorBlack]) <= 5 && evaluations[0]->scorecp == 0) || repetition) strcpy(res, "1/2-1/2");
  auto now = std::chrono::system_clock::now();
  std::string date = std::format("{:%Y.%m.%d}", now);
  //Which configuration played which colour, recorded per game.
  const bool white_is_1 = (&white == &engine_1);
  const std::string wopts = white_is_1 ? optionsLabel(SPINS_1, CHECKS_1) : optionsLabel(SPINS_2, CHECKS_2);
  const std::string bopts = white_is_1 ? optionsLabel(SPINS_2, CHECKS_2) : optionsLabel(SPINS_1, CHECKS_1);
  fprintf(pgnFile, "[Event \"Match of Champions\"]\n[Site \"sv Beruta\"]\n[Date \"%s\"]\n[Round \"%d\"]\n[White \"%s\"]\n[Black \"%s\"]\n[FEN \"%s\"]\n[Result \"%s\"]\n[TimeControl \"%d+0\"]\n[WhiteOptions \"%s\"]\n[BlackOptions \"%s\"]\n\n", date.c_str(), game_id, white.id, black.id, start_fen, res, (int)(MOVETIME / 1000), wopts.c_str(), bopts.c_str());

	char * token = strtok(sanMoves, " ");
	bool first_move = true;
	while (token) {
	  if (whiteMove) {
      fprintf(pgnFile, "%d.%s ", moveNumber, token);
      whiteMove = false;
      first_move = false;
    } else {
      if (first_move) {
        fprintf(pgnFile, "%d...%s ", moveNumber, token);
        first_move = false;
      }
      else fprintf(pgnFile, "%s ", token);
      whiteMove = true; 
      moveNumber++;       
    }
		token = strtok(NULL, " ");	  
  }
  fprintf(pgnFile, "%s\n\n", res);
  fflush(pgnFile);
    
  return result;
}

void play_match(int n_games, FILE* pgnFile) {
    double score_engine_1 = 0;
    double score_engine_2 = 0;
    
    for (int i = 1; i <= n_games; ++i) {
        double result; // Score from White's perspective
        
        // Pick an opening based on pair index. 
        // Games 1 & 2 use opening 0. Games 3 & 4 use opening 1, and so on.
        int fen_idx = ((i - 1) / 2) % opening_fens.size();
        const char * current_fen = opening_fens[fen_idx];

        if (i % 2) {
            // Odd Game: engine_1 is White, engine_2 is Black
            play_one_game(engine_1, engine_2, i, current_fen, score_engine_1, score_engine_2, pgnFile);
        } else {
            // Even Game: engine_1 is Black, engine_2 is White
            play_one_game(engine_2, engine_1, i, current_fen, score_engine_1, score_engine_2, pgnFile);
        }        
    }
    printf("Final Score. Engine 1: %.1f, Engine 2: %.1f\n", score_engine_1, score_engine_2);
    fprintf(pgnFile, "Final Score. Engine 1: %.1f, Engine 2: %.1f\n", score_engine_1, score_engine_2);
}

int main(int argc, char ** argv) {
    Stockfish::Bitboards::init();
    zobristHash(z);
    evaluations[0] = &evaluation;
    evaluations[0]->maxPlies = 1;

    // Init Engines
    //limitStrength/Elo were on the WRONG engine: Creatica got them (a silent no-op,
    //it advertises neither option) while Stockfish got false and therefore played at
    //full strength. Every match run this way measured Creatica against an unlimited
    //Stockfish.
    initChessEngine(engine_1, ENGINE_1, MOVETIME, DEPTH, HASH_1, THREADS_1, SYZYGY_PATH, 1, false, false, 0);          //same binary, ReuseTree on
    initChessEngine(engine_2, ENGINE_2, MOVETIME, DEPTH, HASH_2, THREADS_2, SYZYGY_PATH, 1, false, false, 0);          //same binary, ReuseTree off

    //Engine-specific settings, applied by name. Both lists are declared at the top of the file.
    applyNamedOptions(engine_1, SPINS_1, CHECKS_1);
    applyNamedOptions(engine_2, SPINS_2, CHECKS_2);
    printf("engine_1: %s\nengine_2: %s\n", engine_1.id, engine_2.id);

    FILE* logFile = fopen(PGN_FILE, "a");

    int pass_num = 0;

    play_match(GAMES_PER_MATCH, logFile);
    fflush(logFile);

    fclose(logFile);
    quit(engine_1);
    quit(engine_2);
    releaseChessEngine(engine_1);
    releaseChessEngine(engine_2);
    return 0;
}