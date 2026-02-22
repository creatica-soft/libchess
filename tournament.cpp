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

#include "libchess.h" // Ensure this path is correct

// --- Configuration ---
const char * CREATICA_PATH = "/Users/ap/libchess/creatica";
const char * SYZYGY_PATH = "/Users/ap/syzygy";
#define PGN_FILE "tuning_match.pgn"
#define MOVETIME 2000
#define DEPTH 0
#define HASH 1024
#define THREADS 8

// Tuning Settings
#define GAMES_PER_MATCH 20  // 10 is very noisy; 20-40 is better for statistical significance
#define WIN_THRESHOLD 0.55  // Challenger must score > 55% to replace Champion (prevents drift from noise)

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
struct TunableParam {
    std::string name;
    int optionEnum; // The index in optionSpin array
    int value;      // Current best value
    int min;
    int max;
    int step;
};

// Define your parameters here
std::vector<TunableParam> params = {
    {"ExplorationDepthDecay", ExplorationDepthDecay, 6,   5,   6,  1}, //9 is too fast, 6 is better than 7
    {"ExplorationMin",        ExplorationMin,        60,  60,  65,  5}, //60 became better than 50 but 70 is worse than 60
    {"ExplorationMax",        ExplorationMax,        150, 150, 200, 10}, //150 is better than 140
    {"VirtualLoss",           VirtualLoss,           40,   36,   41,   1}, //42 was too much and 30 was too low, 38 - no diff to 40
    //{"ProbabilityMass",       ProbabilityMass,       100, 100,  100, 0}, //reducing to 95% makes it worse
    {"EvalScale",             EvalScale,             62,   62,   64,   1}, //62 seems to be optimal
    {"Temperature",           Temperature,           58,  56,  59,  1} //62 is same as 60 but 58 is better
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
        engine.optionSpin[p.optionEnum].value = p.value;
    }
    //engine.optionCheck[ProbabilityMass].value = 100; //it is fixed to 100 and removed from the engine
    // Disable info lines for speed/clean logs
    engine.optionCheck[FinalInfoLines].value = false;
    engine.optionCheck[IntermittentInfoLines].value = false;
    setOptions(engine);
}

// Returns 1.0 for White win, 0.0 for Black win, 0.5 for Draw
double play_one_game(Engine& white, Engine& black, int game_id, const char* start_fen, double score) {
    newGame(white);
    newGame(black);
    fen2board(board, start_fen);
    //sanMoves[0] = 0;
    
    std::unordered_map<unsigned long long, int> position_history;
    bool repetition = false;
    char fen[MAX_FEN_STRING_LEN] = "";
    //MovesContext movesContext;
    //unsigned long long movesFromSquares[64] = {0};

    // Game Loop
    //generateMoves(board, movesFromSquares);
    isCheckMateStaleMate(board);
    while (!board.isMate && !board.isStaleMate && board.halfmoveClock < 100 && !repetition) {
        strncpy(white.position, board2fen(board, fen), MAX_FEN_STRING_LEN);
        if (!position(white) || go(white, evaluations)) {
            fprintf(stderr, "Error in engine communication (White)\n"); exit(1);
        }
        Move move;
        uci2move_idx(evaluations[0]->bestmove, move);        
        //char sanMove[12] = "";
        //strcat(sanMoves, move2san(board, move, sanMove));
        //strcat(sanMoves, " ");        
        int captured = ff_move(board, move);
        double percentage = score / GAMES_PER_MATCH;
        fprintf(stdout, "Game %d Move %d: %s, score: %.1f/%d (%.1f%%)\n", game_id, board.moveNumber, evaluations[0]->bestmove, score, GAMES_PER_MATCH, percentage * 100);
        writeDebug(board);
        updateHash(zh, board, move, captured, z);
        if (++position_history[zh.hash] >= 3) { repetition = true; break; }
        // Clear movesFromSquares for next SAN generation
        //memset(movesFromSquares, 0, sizeof(movesFromSquares));
        //generateMoves(board,  movesFromSquares);
        isCheckMateStaleMate(board);
        if (board.isMate || board.isStaleMate || board.halfmoveClock >= 100) break;

        // --- Black to Move ---
        strncpy(black.position, board2fen(board, fen), MAX_FEN_STRING_LEN);
        if (!position(black) || go(black, evaluations)) {
            fprintf(stderr, "Error in engine communication (Black)\n"); exit(1);
        }
        uci2move_idx(evaluations[0]->bestmove, move);       
        //strcat(sanMoves, move2san(board, move, sanMove));
        //strcat(sanMoves, " ");
        captured = ff_move(board, move);
        fprintf(stdout, "Game %d Move %d: %s, score: %.1f/%d (%.1f%%)\n", game_id, board.moveNumber, evaluations[0]->bestmove, score, GAMES_PER_MATCH, percentage * 100);
        writeDebug(board);
        updateHash(zh, board, move, captured, z);
        if (++position_history[zh.hash] >= 3) { repetition = true; break; }
        // Clear movesFromSquares for next SAN generation
        //memset(movesFromSquares, 0, sizeof(movesFromSquares));
        //generateMoves(board, movesFromSquares);
        isCheckMateStaleMate(board);
    }

    // Result
    double result = 0.5;
    if (board.isMate) {
        result = (board.sideToMove == ColorWhite) ? 0.0 : 1.0; // White lost (0.0) or Black lost (1.0 -> White won)
    } else if (board.isStaleMate || repetition || board.halfmoveClock >= 100) {
        result = 0.5;
    } else if (__builtin_popcountl(board.side[ColorWhite] | board.side[ColorBlack]) <= 5 && evaluations[0]->scorecp == 0) {
        result = 0.5; // Insufficient material heuristic
    }
    
    return result;
}

// Plays a match and returns the score for the Challenger (Engine 2)
// Challenger plays White in even games, Black in odd games
double play_match(int n_games, FILE* pgnFile) {
    double challenger_score = 0;
    
    for (int i = 1; i <= n_games; ++i) {
        double result; // Score from White's perspective
        
        // Pick an opening based on pair index. 
        // Games 1 & 2 use opening 0. Games 3 & 4 use opening 1.
        int fen_idx = ((i - 1) / 2) % opening_fens.size();
        const char* current_fen = opening_fens[fen_idx];

        if (i % 2 != 0) {
            // Odd Game: Challenger is White
            result = play_one_game(challenger, champion, i, current_fen, challenger_score);
            challenger_score += result;
            printf("W"); // Challenger White Win/Draw/Loss indicator could go here
        } else {
            // Even Game: Challenger is Black
            result = play_one_game(champion, challenger, i, current_fen, challenger_score);
            // Result is from White's perspective. If White (Champion) won (1.0), Challenger gets 0.
            challenger_score += (1.0 - result);
            printf("B");
        }        
    }
    return challenger_score;
}

int main(int argc, char ** argv) {
    init_magic_bitboards();
    zobristHash(z);
    evaluations[0] = &evaluation;
    evaluations[0]->maxPlies = 1;

    // Init Engines
    initChessEngine(champion, CREATICA_PATH, MOVETIME, DEPTH, HASH, THREADS, SYZYGY_PATH, 1, false, false, 2300);
    initChessEngine(challenger, CREATICA_PATH, MOVETIME, DEPTH, HASH, THREADS, SYZYGY_PATH, 1, false, false, 2300);

    FILE* logFile = fopen(PGN_FILE, "a");
    fprintf(logFile, "{Starting Tuning Session}\n");

    bool improvement_found = true;
    int pass_num = 0;

    // Outer Loop: Keep iterating over all parameters until no improvements are found in a full pass
    while (improvement_found) {
        improvement_found = false;
        pass_num++;
        printf("\n=== STARTING TUNING PASS %d ===\n", pass_num);
        fprintf(logFile, "\n=== STARTING TUNING PASS %d ===\n", pass_num);

        for (auto& param : params) {
            // Apply current best params to Champion
            apply_params(champion, params);

            printf("\n--- Tuning Parameter: %s (Current: %d) ---\n", param.name.c_str(), param.value);
            fprintf(logFile, "\n--- Tuning Parameter: %s (Current: %d) ---\n", param.name.c_str(), param.value);

            bool local_improvement = false;
            
            // Try INCREASING
            if (param.value + param.step <= param.max) {
                int original_val = param.value;
                int test_val = param.value + param.step;
                
                // Configure Challenger
                std::vector<TunableParam> test_params = params;
                // Find the param in the test set to update
                for(auto& p : test_params) { if(p.name == param.name) p.value = test_val; }
                apply_params(challenger, test_params);

                printf("Testing %s = %d vs Baseline %d... ", param.name.c_str(), test_val, original_val);
                fprintf(logFile, "Testing %s = %d vs Baseline %d... ", param.name.c_str(), test_val, original_val);
                double score = play_match(GAMES_PER_MATCH, logFile);
                double percentage = score / GAMES_PER_MATCH;
                printf("Score: %.1f/%d (%.1f%%)\n", score, GAMES_PER_MATCH, percentage * 100);
                fprintf(logFile, "Score: %.1f/%d (%.1f%%)\n", score, GAMES_PER_MATCH, percentage * 100);

                if (percentage >= WIN_THRESHOLD) {
                    printf(">>> SUCCESS! Upgrade %s to %d\n", param.name.c_str(), test_val);
                    fprintf(logFile, ">>> SUCCESS! Upgrading %s to %d\n", param.name.c_str(), test_val);
                    param.value = test_val; // Update the "Best" struct
                    improvement_found = true;
                    local_improvement = true;
                    // Optimization: If we found an improvement, try going further in this direction?
                    // For now, let's stick to one step per pass to avoid overshooting.
                } 
            }

            // Try DECREASING (Only if increasing didn't work)
            if (!local_improvement && (param.value - param.step >= param.min)) {
                int original_val = param.value;
                int test_val = param.value - param.step;

                std::vector<TunableParam> test_params = params;
                for(auto& p : test_params) { if(p.name == param.name) p.value = test_val; }
                apply_params(challenger, test_params);

                printf("Testing %s = %d vs Baseline %d... ", param.name.c_str(), test_val, original_val);
                fprintf(logFile, "Testing %s = %d vs Baseline %d... ", param.name.c_str(), test_val, original_val);
                double score = play_match(GAMES_PER_MATCH, logFile);
                double percentage = score / GAMES_PER_MATCH;
                printf("Score: %.1f/%d (%.1f%%)\n", score, GAMES_PER_MATCH, percentage * 100);
                fprintf(logFile, "Score: %.1f/%d (%.1f%%)\n", score, GAMES_PER_MATCH, percentage * 100);

                if (percentage >= WIN_THRESHOLD) {
                    printf(">>> SUCCESS! Downgrade %s to %d\n", param.name.c_str(), test_val);
                    fprintf(logFile, ">>> SUCCESS! Downgrade %s to %d\n", param.name.c_str(), test_val);
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
    cleanup_magic_bitboards();
    return 0;
}