#include "nnue/nnue/nnue_accumulator.h"

#ifdef _MSC_VER
#include <mutex>
#endif

#ifdef __GNUC__ // g++ on Alpine Linux
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <condition_variable>
#include <queue>
#include <cstdarg>
#endif
#include <cassert>
#include <vector>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <random>
#include <chrono>
#include <algorithm>
#include <math.h>
#include "tbprobe.h"
#include "json.hpp"
#include <curl/curl.h>
#include "libchess.h"

#define THREADS 8
#define MULTI_PV 5
#define SYZYGY_PATH_DEFAULT "<empty>"
#define SYZYGY_PATH "/Users/ap/syzygy"
#define HASH 4096 //default, GUI may set it via Hash option (once full, expansion won't happen!)
#define EXPLORATION_MIN 60 // used in formular for exploration constant decay with depth
#define EXPLORATION_MAX 150 //smaller value favor exploitation, i.e. deeper tree vs wider tree
#define EXPLORATION_DEPTH_DECAY 5 //linear decay of EXPLORATION CONSTANT with depth using formula:
                      // C * 100 = max(EXPLORATION_MIN, (EXPLORATION_MAX - seldepth * EXPLORATION_DEPTH_DECAY))
//#define PROBABILITY_MASS 100 //% - cumulative probability - how many moves we consider - 100% seems to be the best, so we don't need it!
#define EVAL_SCALE 62 //This is a divisor in W = tanh(eval/eval_scale) where eval is NNUE evaluation in pawns. 
                     //W is a fundamental value in Monte Carlo tree node along with N (number of visits) 
                     //and P (prior move probability), though P belongs to edges (same as move) but W and N to nodes.
#define TEMPERATURE 58 //used in calculating probabilities for moves in get_prob() using softmax:
                        // exp((eval - max_eval)/(temperature/100)) / eval_sum
                        //can be tuned so that values < 1.0 sharpen the distribution and values > 1.0 flatten it
#define PV_PLIES 16
#define PONDER false
#define DISPLAY_INTERMITTENT_INFO_LINES true
#define DISPLAY_FINAL_INFO_LINES true
#define MAX_DEPTH 100
#define MAX_NODES 10000000

struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;    
};
struct Edge {   //alignas(64) one edge for 64-byte cache line - not sure if it makes any difference
    int move = 0;           // The move that leads to the child position
    double P = 0.0;         // Prior probability - move probability for a given move in the node
    uint32_t child_idx = 0; //index in vector<MCTSNode>
    //uint32_t _pad[10] = {0};
};
struct MCTSNode {
    uint64_t hash = 0;
    uint32_t N = 0;
    double W = 0;
    int cp = NO_MATE_SCORE; //position evaluation in centipawns 
    std::vector<Edge> children;
    //std::array<Edge, 64> children; //requires too much memory! ~30GB
    //size_t num_children = 0;
};
// Custom hasher that uses the key directly
/*struct NoOpHash {
    std::size_t operator()(uint64_t key) const noexcept {
        return key; // Directly use the key as the hash
    }
};*/
struct ThreadParams {
    int thread_id;
    std::atomic<int> seldepth {0};
    uint64_t time_alloc;
    std::vector<MCTSNode> nodes;
    std::atomic<uint32_t> next_idx {0};        // allocator: 0 reserved for root/invalid
    //std::unordered_map<uint64_t, uint32_t, NoOpHash> tree; //Zobrist hash and node idx - seems to be a waste
    std::atomic<uint64_t> total_children {0};
    std::unordered_map<uint64_t, double> nnue_cache;  // hash -> eval (pawns) - ok, better the global cache
};
struct RootStats {
  std::atomic<uint32_t> N;
  std::atomic<int> child_idx;
};

void runMCTS();
void cleanup(const int thread_id); //free Hash tree
void mcts_search(const int thread_id, NNUEContext& ctx);
void uciLoop();
void handleUCI(void);
void handleIsReady(void);
void handleNewGame();
void handlePosition(char * command);
void handleOption(char * command);
void handleGo(char * command);
void handlePonderhit(char * command);
void handleStop();
void handleQuit(void);
void handlePieces(void); //non-standard UCI commnand "pieces" - returns the number of pieces on board
void handleEval(void); //NNUE eval trace
void init_thread_pool(int num_threads);
void log_file(const char * message, ...);
void print(const char * message, ...);
bool sendGetRequest(const std::string& url, int& scorecp, std::string& uci_move);
//void compute_move_evals(Board& chess_board, const ZobristHash& board_hash, NNUEContext& ctx, const std::unordered_set<uint64_t>& pos_history, std::vector<std::tuple<double, int, int, uint64_t>>& move_evals, double prob_mass, uint64_t * movesFromSquares);
double eval_and_expand(Board& chess_board, const ZobristHash& board_hash, NNUEContext& ctx, std::unordered_set<uint64_t>& pos_history, const int thread_id, const uint32_t node_idx);
double position_eval(Board& chess_board, const ZobristHash& board_hash, NNUEContext& ctx, std::unordered_set<uint64_t>& pos_history, const int thread_id);
double make_move(Board& chess_board, const ZobristHash& board_hash, const Move& move, NNUEContext& ctx, uint64_t& child_hash, std::unordered_set<uint64_t>& pos_history);
void shutdown_thread_pool();
void init_thread_pool(const int num_threads);
void init_nnue(const char * nnue_file_big, const char * nnue_file_small);
void cleanup_nnue();
void init_nnue_context(NNUEContext& ctx);
void free_nnue_context(NNUEContext& ctx);
double evaluate_nnue(const Board& chess_board, NNUEContext& ctx);
void accumulator_stack_push(NNUEContext& ctx, Stockfish::DirtyPiece& dp);
void accumulator_stack_pop(NNUEContext& ctx);
void accumulator_stack_reset(NNUEContext& ctx);
std::string nnue_eval(Board& board);
