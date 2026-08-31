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
const char * ENGINE_1 = "/Users/ap/libchess/creatica-shared-root";
const char * ENGINE_2 = "/Users/ap/stockfish-macos-m1-apple-silicon";
const char * SYZYGY_PATH = "/Users/ap/syzygy";
#define PGN_FILE "match3.pgn"
#define MOVETIME 2000
#define DEPTH 0
#define HASH 2024
#define THREADS 8

// Tuning Settings
#define GAMES_PER_MATCH 20  // 10 is very noisy; 20-40 is better for statistical significance

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
    Move move;
    uci2move_idx(evaluations[0]->bestmove, move);        
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
    
  char res[8];
  if (board.isMate) {
    if (board.sideToMove == ColorWhite) strcpy(res, "0-1");
    else strcpy(res, "1-0"); 
  } 
  else if (board.isStaleMate || board.halfmoveClock == 100 || (__builtin_popcountl(board.side[ColorWhite] | board.side[ColorBlack]) <= 5 && evaluations[0]->scorecp == 0) || repetition) strcpy(res, "1/2-1/2");
  auto now = std::chrono::system_clock::now();
  std::string date = std::format("{:%Y.%m.%d}", now);
  fprintf(pgnFile, "[Event \"Match of Champions\"]\n[Site \"sv Beruta\"]\n[Date \"%s\"]\n[Round \"%d\"]\n[White \"%s\"]\n[Black \"%s\"]\n[FEN \"%s\"]\n[Result \"%s\"]\n\n", date.c_str(), game_id, white.id, black.id, start_fen, res);

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
    initChessEngine(engine_1, ENGINE_1, MOVETIME, DEPTH, HASH, THREADS, SYZYGY_PATH, 1, false, true, 2600); //MultiPV, logging, limitStrength, Elo
    initChessEngine(engine_2, ENGINE_2, MOVETIME, DEPTH, HASH, THREADS, SYZYGY_PATH, 1, false, false, 2300);

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