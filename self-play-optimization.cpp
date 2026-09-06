// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -Wl,-lchess,-rpath,/Users/ap/libchess -L /Users/ap/libchess -o self-play-optimization self-play-optimization.cpp

#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <chrono>
#include <format>
#include <math.h>
#include <cmath>
#include <vector>
#include <string>
#include <iostream>
#include <unordered_map>
#include "nnue/bitboard.h"

#include "libchess.h" // Ensure this path is correct

// --- Configuration ---
//creatica is the engine to tune: it is the one that exposes the policy settings as UCI
//options. The older creatica-kan this pointed at advertises none of them.
const char * CREATICA_PATH = "/Users/ap/libchess/creatica";
const char * SYZYGY_PATH = "/Users/ap/syzygy";
#define PGN_FILE "tuning_match.pgn"
//500 ms rather than 2000. A comparison is only as good as its sample size, and at 2000 ms a
//40-game match takes over three hours. Shorter games shift the optimum of a parameter slightly,
//but the ORDERING of candidate values almost always survives, and ordering is all a hill-climb
//consumes. Four times as many games per hour buys far more than the time control costs.
#define MOVETIME 500
#define DEPTH 0
#define HASH 1024
//4, not 8. Measured on this machine: 4 threads searched about 36% more nodes per second than 8
//on four performance cores, and won a 24-game match 14-10.
#define THREADS 4

// Tuning Settings
//40, not 10. With draw rates between 55% and 75% a 10-game match cannot resolve anything
//smaller than roughly 150 Elo -- a single game swing moves the score by 10 percentage points,
//which is more than any of these parameters is worth. 40 games at 500 ms is about 50 minutes.
#define GAMES_PER_MATCH 60  // one comparison, so spend the games here: ~60 resolves ~80 Elo
#define WIN_THRESHOLD 0.55  // Challenger must score > 55% to replace Champion (prevents drift from noise)
//A score above the threshold is still not enough on its own: with a greedy climb over several
//parameters and two directions each, a run makes enough comparisons that one of them clears
//55% by luck. A candidate must also beat 50% by this many standard errors of its own result.
#define MIN_SIGMA 1.5
//Per-ply output. A full tuning pass is several hundred thousand plies, and printing a board
//and a line for each is roughly 80 MB of terminal output per pass -- useless for a run nobody
//is watching, and slow if it is being redirected to a file. 0 prints one line per game and one
//per comparison, which is enough to see where a long run has got to.
#define SHOW_MOVES 0

// Global Engine Objects
struct Engine champion;
struct Engine challenger;
struct Board board = {};
struct Zobrist z = {};
struct ZobristHash zh = {};
struct Evaluation evaluation = {};
struct Evaluation * evaluations[1] = { nullptr };
char sanMoves[4096] = "";

// Tuning Parameter Structure
//
//Keyed by the option's NAME, not by a position in optionSpin[]. It used to carry an
//`optionEnum` indexing that array with EngineSpinOptions, but getOptions() fills the array in
//whatever order the engine advertises its options, so the enum was right only by coincidence.
//Against creatica the enum's ProbabilityMass (index 9) is PolicyMode, EvalScale (10) is
//PolicyBlend, and on the check side FinalInfoLines (0) is PerformanceCores -- so the tuner
//would have turned off performance-core pinning and tuned the wrong knobs while reporting the
//right names. Names also mean this file can tune options the library header knows nothing
//about, which is the whole point now that the policy settings are UCI options.
struct TunableParam {
    std::string name;
    int value;      // Current best value, in the UCI units the engine advertises
    int min;
    int max;
    int step;
    bool active = true;   // cleared at startup if the engine does not advertise this option
};

// Define your parameters here.
//
//Real-valued options are carried over UCI as scaled integers, because UCI has no float type:
//PolicyBlend 45 means 0.45, BlendScale 115 means 1.15. Tune them in the units below, which are
//the ones the engine advertises.
//
//What is here and why:
//  ExplorationMax/Min/DepthDecay - the PUCT constants. They were tuned against the OLD
//      eval-derived prior; the prior is now a blend with a learned policy head and has a
//      different shape (top-1 36% against 27%), so their optimum has most likely moved. Highest
//      chance of a real effect, so they go first.
//  PolicyBlend  - never tuned by play at all. The 0.45 in use came from offline top-k accuracy;
//      a 15-game match between 0.30 and 0.60 ended level, which resolves nothing.
//  BlendScale   - sets how concentrated the blended prior is. Set analytically to match the old
//      prior's concentration, never tested.
//  ProbabilityMass - reopened deliberately, and reimplemented. It measured worse below 100%
//      when the gate ran AFTER the child evaluations, where it could only discard moves and
//      never save the work. It now runs before them, on the policy score. Held-out positions
//      say a 990 gate keeps Stockfish's best move 98.5% of the time for about 35% fewer child
//      evaluations -- which measured as only about 3% more nodes per second, but a distinctly
//      deeper tree (seldepth 12-13 against 14-16). Whether that trade is worth anything is
//      exactly what a match has to say.
//
//Deliberately NOT here:
//  Temperature - what it does is now largely done by BlendScale, which is calibrated against
//      it. Tuning both means tuning two knobs against each other.
//  VirtualLoss - a threading device; it moves throughput far more than strength, and the thread
//      count it was tuned against has since changed from 8 to 4.
//  EvalScale   - creatica does not advertise it.
//Both are one uncommented line away if a pass over the list above comes back empty.
std::vector<TunableParam> params = {
    //ProbabilityMass ONLY for this run. The exploration constants came back unmoved, and
    //PolicyBlend was already checked at 30 vs 60 with no gain, so leaving them in would spend
    //hours re-confirming nulls. Uncomment to resume the full sweep.
    //
    //Step 10 rather than 5 deliberately. The climb tries value+step first, which is above the
    //maximum here, then value-step -- so with step 5 the only comparison it would ever run is
    //995 vs 1000, and 995 gates almost nothing. Step 10 makes the first comparison 990 vs 1000,
    //which is the setting the offline recall measurement actually flagged: 98.5% of Stockfish's
    //best moves retained for about 35% fewer child evaluations.
    //{"ExplorationMax",        160, 130, 190, 15},
    //{"ExplorationMin",        65,  45,  85,  10},
    //{"ExplorationDepthDecay", 5,   3,   8,   1},
    //{"PolicyBlend",           45,  20,  70,  10},   // 0.45, tuned in hundredths
    //{"BlendScale",            115, 85,  145, 15},   // 1.15, tuned in hundredths
    //Per-mille, not percent: 1000 keeps every move. 990 and 999 behave very differently and a
    //percent scale cannot tell them apart.
    {"ProbabilityMass",       1000, 980, 1000, 10},
    //{"Temperature",           58,  50,  66,  4},
    //{"VirtualLoss",           36,  28,  44,  4},
};

// A small suite of balanced opening positions (FENs) to ensure game variety.
// This prevents the "identical game" loop.
const std::vector<const char*> opening_fens = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",             // Start
    "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",          // 1. e4
    "rnbqkbnr/pppppppp/8/8/3P4/8/PPP1PPPP/RNBQKBNR b KQkq - 0 1",          // 1. d4
    "rnbqkbnr/pppppppp/8/8/2P5/8/PP1PPPPP/RNBQKBNR b KQkq - 0 1",          // 1. c4
    "rnbqkbnr/pppppppp/8/8/8/5N2/PPPPPPPP/RNBQKB1R b KQkq - 1 1",          // 1. Nf3
    "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",        // 1. e4 e5
    "rnbqkbnr/pppp1ppp/8/4p3/3P4/8/PPP1PPPP/RNBQKBNR w KQkq - 0 2",        // 1. d4 e5 (Englund/Borg) - aggressive
    "rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",        // Sicilian
    "rnbqkbnr/pp1ppppp/2p5/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",        // Caro-Kann
    "rnbqkbnr/pp1ppppp/8/2p5/3P4/8/PPP1PPPP/RNBQKBNR w KQkq - 0 2"         // Old Benoni
};

// --- Helper Functions ---

void apply_params(Engine& engine, const std::vector<TunableParam>& p_list) {
    for (const auto& p : p_list) {
        if (!p.active) continue;
        setEngineSpin(engine, p.name.c_str(), p.value);
    }
    //By name: the old code wrote optionCheck[FinalInfoLines], which is index 0, and creatica
    //advertises PerformanceCores at index 0 -- so this silently disabled performance-core
    //pinning and left the info lines on.
    setEngineCheck(engine, "FinalInfoLines", false);
    setEngineCheck(engine, "IntermittentInfoLines", false);
    setOptions(engine);
}

//Drop any parameter the engine does not advertise, and pull each one's range back inside the
//bounds the engine reports. Without this the tuner spends hours moving a value that the engine
//never receives, and reports the resulting noise as a tuning result.
void validate_params(const Engine& engine, std::vector<TunableParam>& p_list) {
    printf("\n=== PARAMETERS ===\n");
    for (auto& p : p_list) {
        int64_t lo = 0, hi = 0, def = 0;
        if (!engineSpinRange(engine, p.name.c_str(), lo, hi, def)) {
            p.active = false;
            printf("  %-22s SKIPPED - %s does not advertise it\n", p.name.c_str(), CREATICA_PATH);
            continue;
        }
        if (p.min < (int)lo) { printf("  %-22s min %d raised to the advertised %lld\n",
                                      p.name.c_str(), p.min, (long long)lo); p.min = (int)lo; }
        if (p.max > (int)hi) { printf("  %-22s max %d lowered to the advertised %lld\n",
                                      p.name.c_str(), p.max, (long long)hi); p.max = (int)hi; }
        if (p.value < p.min) p.value = p.min;
        if (p.value > p.max) p.value = p.max;
        printf("  %-22s start %-5d range %d..%d step %d   (engine default %lld)\n",
               p.name.c_str(), p.value, p.min, p.max, p.step, (long long)def);
    }
    printf("\n");
}

// Returns 1.0 for White win, 0.0 for Black win, 0.5 for Draw
double play_one_game(Engine& white, Engine& black, int game_id, const char* start_fen, double score, FILE * pgnFile) {
    newGame(white);
    newGame(black);
    fen2board(board, start_fen);
    bool whiteMove = board.sideToMove == ColorWhite ? true : false;
    int moveNumber = board.moveNumber;
    sanMoves[0] = 0;
    
    std::unordered_map<unsigned long long, int> position_history;
    bool repetition = false;
    char fen[MAX_FEN_STRING_LEN] = "";

    // Game Loop
    //generateMoves(board, movesFromSquares);
    isCheckMateStaleMate(board);
    while (!board.isMate && !board.isStaleMate && board.halfmoveClock < 100 && !repetition) {
        if (board.sideToMove == ColorWhite) {
          strncpy(white.position, board2fen(board, fen), MAX_FEN_STRING_LEN);
          if (!position(white) || go(white, evaluations)) {
            if (game_id % 2) //odd game
              fprintf(stderr, "Error in engine communication (White - Challenger)\n");
             else 
              fprintf(stderr, "Error in engine communication (White - Champion)\n");
             exit(1);
          }    
        } else { //sideToMove == ColorBlack
          strncpy(black.position, board2fen(board, fen), MAX_FEN_STRING_LEN);
          if (!position(black) || go(black, evaluations)) {
            if (game_id % 2)
              fprintf(stderr, "Error in engine communication (Black - Champion)\n");
            else 
              fprintf(stderr, "Error in engine communication (Black - Challenger)\n");
            exit(1);
          }    
        }
        Move move;
        uci2move_idx(evaluations[0]->bestmove, move);        
        char sanMove[12] = "";
        strcat(sanMoves, move2san(board, move, sanMove));
        strcat(sanMoves, " ");        
        int captured = ff_move(board, move);
        double percentage = score / GAMES_PER_MATCH;
        if (SHOW_MOVES) {
            fprintf(stdout, "Game %d Move %d: %s, score: %.1f/%d (%.1f%%)\n", game_id, board.moveNumber, evaluations[0]->bestmove, score, GAMES_PER_MATCH, percentage * 100);
            writeDebug(board);
        }
        (void)percentage;
        updateHash(zh, board, move, captured, z);
        if (++position_history[zh.hash] >= 3) { repetition = true; break; }
        isCheckMateStaleMate(board);
        if (board.isMate || board.isStaleMate || board.halfmoveClock >= 100) break;
    }

    // Result
    double result = 0.5;
    if (board.isMate) {
        result = (board.sideToMove == ColorWhite) ? 0.0 : 1.0; // White lost (0.0) or Black lost (1.0 -> White won)
    } else if (board.isStaleMate || repetition || board.halfmoveClock >= 100) {
        result = 0.5;
    } else if (__builtin_popcountl(board.side[ColorWhite] | board.side[ColorBlack]) <= 5 && evaluations[0]->scorecp == 0) {
        result = 0.5; //TB known draw
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

// Plays a match and returns the score for the Challenger (Engine 2)
// Challenger plays White in even games, Black in odd games
//Also fills wins/draws/losses, so the caller can say how large the error bar on the result is
//rather than treating a 55% score from 40 games and from 10 games as the same evidence.
double play_match(int n_games, FILE* pgnFile, int& wins, int& draws, int& losses) {
    double challenger_score = 0;
    wins = draws = losses = 0;
    
    for (int i = 1; i <= n_games; ++i) {
        double result; // Score from White's perspective
        
        // Pick an opening based on pair index. 
        // Games 1 & 2 use opening 0. Games 3 & 4 use opening 1.
        int fen_idx = ((i - 1) / 2) % opening_fens.size();
        const char* current_fen = opening_fens[fen_idx];

        if (i % 2 != 0) {
            // Odd Game: Challenger is White
            result = play_one_game(challenger, champion, i, current_fen, challenger_score, pgnFile);
            challenger_score += result;
            if (result > 0.75) ++wins; else if (result < 0.25) ++losses; else ++draws;
        } else {
            // Even Game: Challenger is Black
            result = play_one_game(champion, challenger, i, current_fen, challenger_score, pgnFile);
            // Result is from White's perspective. If White (Champion) won (1.0), Challenger gets 0.
            challenger_score += (1.0 - result);
            if (result < 0.25) ++wins; else if (result > 0.75) ++losses; else ++draws;
        }
        //One line per game, so an unattended run can be checked on with tail -f.
        printf("    game %2d/%d  %s  running %.1f  (+%d =%d -%d)\n", i, n_games,
               (i % 2 != 0) ? "challenger as White" : "challenger as Black",
               challenger_score, wins, draws, losses);
        fflush(stdout);
        
    }
    return challenger_score;
}

//Run one candidate value against the current champion and say whether it should be adopted.
//This was two near-identical copies, one for the increasing direction and one for decreasing.
//
//The acceptance test is the part that matters. A score rate is a sample mean, and its standard
//error at n games is what decides whether a result means anything; the previous test compared
//the rate to a fixed 55% with no reference to n, so at 10 games a single extra win was enough
//to "find an improvement" and the climb wandered.
bool try_value(std::vector<TunableParam>& p_list, const std::string& name, int test_val,
               int baseline, FILE* logFile) {
    std::vector<TunableParam> test_params = p_list;
    for (auto& p : test_params) if (p.name == name) p.value = test_val;
    apply_params(challenger, test_params);

    printf("Testing %s = %d vs baseline %d ... ", name.c_str(), test_val, baseline);
    fflush(stdout);
    fprintf(logFile, "Testing %s = %d vs baseline %d ... ", name.c_str(), test_val, baseline);

    int w = 0, d = 0, l = 0;
    const double score = play_match(GAMES_PER_MATCH, logFile, w, d, l);
    const int    n     = w + d + l;
    const double rate  = n ? score / n : 0.0;

    //Standard error of the mean of per-game scores in {0, 0.5, 1}.
    const double sumsq = (double)w * 1.0 + (double)d * 0.25;
    const double var   = n > 1 ? (sumsq - n * rate * rate) / (n - 1) : 0.0;
    const double se    = n > 1 && var > 0 ? std::sqrt(var / n) : 0.0;
    const double sigma = se > 0 ? (rate - 0.5) / se : 0.0;
    //Elo of the observed rate, for reading only -- the decision is made on sigma.
    const double elo   = (rate > 0.001 && rate < 0.999)
                       ? -400.0 * std::log10(1.0 / rate - 1.0) : 0.0;

    const bool accept = rate >= WIN_THRESHOLD && sigma >= MIN_SIGMA;

    //A losing result has a NEGATIVE sigma, so it is the lower bound that identifies it. Testing
    //sigma >= MIN_SIGMA here labelled a clear loss "not resolved".
    const char * verdict = accept ? "ADOPT"
                         : (sigma <= -MIN_SIGMA ? "worse" : "not resolved");
    printf("%.1f/%d  (+%d =%d -%d, %.1f%%, %+.0f Elo, %.1f sigma)  %s\n",
           score, n, w, d, l, rate * 100, elo, sigma, verdict);
    fprintf(logFile, "%.1f/%d  (+%d =%d -%d, %.1f%%, %+.0f Elo, %.1f sigma)  %s\n",
            score, n, w, d, l, rate * 100, elo, sigma, verdict);
    fflush(logFile);
    return accept;
}

int main(int argc, char ** argv) {
    Stockfish::Bitboards::init();
    //init_magic_bitboards();
    zobristHash(z);
    evaluations[0] = &evaluation;
    evaluations[0]->maxPlies = 1;

    // Init Engines
    initChessEngine(champion, CREATICA_PATH, MOVETIME, DEPTH, HASH, THREADS, SYZYGY_PATH, 1, false, false, 2300);
    initChessEngine(challenger, CREATICA_PATH, MOVETIME, DEPTH, HASH, THREADS, SYZYGY_PATH, 1, false, false, 2300);

    //Both engines are the same binary, so validating against one is enough.
    validate_params(champion, params);

    FILE* logFile = fopen(PGN_FILE, "a");

    bool improvement_found = true;
    int pass_num = 0;

    // Outer Loop: Keep iterating over all parameters until no improvements are found in a full pass
    while (improvement_found) {
        improvement_found = false;
        pass_num++;
        printf("\n=== STARTING TUNING PASS %d ===\n", pass_num);
        fprintf(logFile, "\n=== STARTING TUNING PASS %d ===\n", pass_num);

        for (auto& param : params) {
            if (!param.active) continue;

            // Apply current best params to Champion
            apply_params(champion, params);

            printf("\n--- Tuning %s (current %d) ---\n", param.name.c_str(), param.value);
            fprintf(logFile, "\n--- Tuning %s (current %d) ---\n", param.name.c_str(), param.value);

            bool local_improvement = false;

            if (param.value + param.step <= param.max) {
                const int test_val = param.value + param.step;
                if (try_value(params, param.name, test_val, param.value, logFile)) {
                    printf(">>> ADOPTED %s = %d\n", param.name.c_str(), test_val);
                    fprintf(logFile, ">>> ADOPTED %s = %d\n", param.name.c_str(), test_val);
                    param.value = test_val;
                    improvement_found = local_improvement = true;
                }
            }

            // Try decreasing only if increasing did not win.
            if (!local_improvement && param.value - param.step >= param.min) {
                const int test_val = param.value - param.step;
                if (try_value(params, param.name, test_val, param.value, logFile)) {
                    printf(">>> ADOPTED %s = %d\n", param.name.c_str(), test_val);
                    fprintf(logFile, ">>> ADOPTED %s = %d\n", param.name.c_str(), test_val);
                    param.value = test_val;
                    improvement_found = true;
                }
            }
        }

        // End of Pass Report
        printf("\n=== PASS %d COMPLETE ===\nCurrent Best Parameters:\n", pass_num);
        fprintf(logFile, "\nPASS %d Best Params:\n", pass_num);
        for (const auto& p : params) {
            printf("  %s: %d\n", p.name.c_str(), p.value);
            fprintf(logFile, "  %s: %d\n", p.name.c_str(), p.value);
        }
        fflush(logFile);
    }

    printf("\nOptimization Complete. No further improvements found.\n");
    fprintf(logFile, "\nOptimization Complete. No further improvements found.\n");
    fclose(logFile);
    quit(champion);
    quit(challenger);
    releaseChessEngine(champion);
    releaseChessEngine(challenger);
    //cleanup_magic_bitboards();
    return 0;
}