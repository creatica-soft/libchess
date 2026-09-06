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

//Measured, on this 4-performance-core M1 with the machine idle: node throughput peaks at
//4 threads and falls away sharply, 8 threads giving 36% FEWER nodes than 4.
//
//    threads   2        4          6          8
//    nodes     1.92M    4.28M      3.68M      2.73M
//
//It is contention, not core placement. Biasing the threads onto performance cores with
//QOS_CLASS_USER_INTERACTIVE recovers none of it (measured 0.94x at 4 threads, 0.63x at 8),
//so the cost is the shared transposition map's mutex and virtual-loss interference between
//threads, which grow faster with thread count than throughput does. The 5th through 8th
//threads cost more in lock traffic than they contribute in nodes.
//
//The UCI Threads option still accepts 1-8; only the default changed.
#define THREADS 4
#define MULTI_PV 5
#define SYZYGY_PATH_DEFAULT "<empty>"
#define SYZYGY_PATH "/Users/ap/syzygy"
#define HASH 2048 //default, GUI may set it via Hash option (once full, expansion won't happen!)
#define EXPLORATION_MIN 65 // used in formular for exploration constant decay with depth
#define EXPLORATION_MAX 160 //smaller value favor exploitation, i.e. deeper tree vs wider tree
#define EXPLORATION_DEPTH_DECAY 5 //linear decay of EXPLORATION CONSTANT with depth using formula:
                      // C * 100 = max(EXPLORATION_MIN, (EXPLORATION_MAX - seldepth * EXPLORATION_DEPTH_DECAY))
//#define PROBABILITY_MASS 100 //% - cumulative probability - how many moves we consider - 100% seems to be the best, so we don't need it!
#define VIRTUAL_LOSS 36 //this is used primarily for performance in MT to avoid threads working on the same tree nodes
#define EVAL_SCALE 61 //This is a divisor in W = tanh(eval/eval_scale) where eval is NNUE evaluation in pawns. 
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

//--- policy head ---------------------------------------------------------------------
//Where to find the exported weights, overridable with the CREATICA_POLICY env var. The
//file is produced by:  EXPORT_WEIGHTS=nnue_policy.bin ./nnue_policy_train
#define POLICY_WEIGHTS_DEFAULT "nnue_policy.bin"
//Buffer bounds for the trunk. policy_net_load() refuses anything larger, so these are a
//guarantee and not a hope.
#define POLICY_MAX_IN 2048
#define POLICY_MAX_H2 512
//First-play urgency, in the same tanh units as W (so the whole scale is -1..1). An
//unvisited child is assumed to be worth this much less than its parent. 0 would mean
//"assume every unexplored move draws", which is why the incumbent's Q=0 fallback is wrong
//once children are born unevaluated. Override at runtime with CREATICA_FPU.
#define FPU_REDUCTION 0.20
//Share of the prior taken from the policy head; the remainder comes from the 1-ply child
//evaluation. 0 reproduces the incumbent exactly, 1 is policy-only. Measured best around
//0.4-0.5 (see the table in creatica_search.cpp). Override with CREATICA_POLICY_BLEND.
//Ignored in "full" mode, which has no child evaluations to blend with.
#define POLICY_BLEND 0.45
//Restores the blended prior's concentration to the incumbent's ~0.39. Override with
//CREATICA_BLEND_SCALE.
#define POLICY_BLEND_SCALE 1.15

struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;    
};
struct ThreadParams {
    int thread_id;
    uint64_t time_alloc;
    int seldepth;
};
struct Edge;
struct MCTSNode {
    std::atomic<uint64_t> hash{0};
    std::atomic<uint64_t> N{0};  // Atomic for lock-free updates
    std::atomic<double> W{0};
    std::atomic<int> cp {NO_MATE_SCORE}; //position evaluation in centipawns 
    std::atomic<int> num_children{0};
    std::atomic<int> generation{0};
    std::atomic<int> terminal{0}; //0 (not terminal), 1 (mate), 2 (stalemate), 3 (repetition), -1 (check)
    std::atomic<uint8_t> expanding{0};  // expansion gate (test-and-set try-lock): exchange(1, acquire) == 0 acquires it, store(0, release) releases it
    std::atomic<Edge *> children {nullptr}; //array of moves and priors leading to next nodes
};
// Custom hasher that uses the key directly
struct NoOpHash {
    std::size_t operator()(uint64_t key) const noexcept {
        return key; // Directly use the key as the hash
    }
};
struct MCTSSearch {
    MCTSNode * root = nullptr;
    std::unordered_map<uint64_t, MCTSNode *, NoOpHash> tree; //Zobrist hash and node 
};
struct Edge {
    std::atomic<int> move {0};             // The move that leads to the child position
    std::atomic<double> P {0.0};            // Prior probability - model move_probs for a given move in the node
    std::atomic<struct MCTSNode *> child {nullptr}; // Pointer to the child node
};

void runMCTS(NNUEContext& ctx);
//Checks the invariants gc() must preserve; off unless validate_tree_enabled is set. Returns the
//number of violations found and logs each. See the definition for what it checks and why it
//never dereferences a pointer it suspects.
//Tree occupancy in per-mille of Hash, from atomics -- safe to call during a search.
int tree_occupancy();
extern std::atomic<size_t> total_nodes;
extern bool reuse_tree;
extern int64_t gc_threshold;
extern bool validate_tree_enabled;
//`collected` says whether a collection has just run; the reachability invariant only
//holds then, since lazy collection deliberately leaves unreachable nodes in the map.
int validate_tree(const char * where, bool collected);
void cleanup(); //free Hash tree
void mcts_search(ThreadParams& params, NNUEContext& ctx);
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
double eval_and_expand(MCTSNode * node, Board& chess_board, const ZobristHash& board_hash, NNUEContext& ctx, const std::unordered_set<uint64_t>& pos_history, int iter);
std::pair<double, int> position_eval(Board& chess_board, const ZobristHash& board_hash, NNUEContext& ctx, const std::unordered_set<uint64_t>& pos_history, int iter);
std::pair<double, int> make_move(Board& chess_board, const ZobristHash& board_hash, const Move& move, NNUEContext& ctx, uint64_t& child_hash, const std::unordered_set<uint64_t>& pos_history, int iter);
//void init_nnue(const char * nnue_file_big, const char * nnue_file_small);
void init_nnue();
void cleanup_nnue();
void init_nnue_context(NNUEContext& ctx);
void free_nnue_context(NNUEContext& ctx);
double evaluate_nnue(const Board& chess_board, NNUEContext& ctx);
//void accumulator_stack_push(NNUEContext& ctx, Stockfish::DirtyPiece& dp);
std::pair<Stockfish::DirtyPiece&, Stockfish::DirtyThreats&> accumulator_stack_push(NNUEContext& ctx);
void accumulator_stack_pop(NNUEContext& ctx);
void accumulator_stack_reset(NNUEContext& ctx);
std::string nnue_eval(Board& board);
