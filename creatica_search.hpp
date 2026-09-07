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
//Hard ceiling on the check extension in process_check() -> eval_and_expand().
//
//That recursion had no limit at all. It descends one level for every consecutive check, and each
//level holds an accumulator_stack_push() for the whole descent, so its depth is charged against
//AccumulatorStack::MaxSize (MAX_PLY + 1 = 247) on top of the MCTS path itself. It is genuinely
//unbounded rather than merely deep: process_check() recurses whenever make_child() hands back a
//node whose cp is still NO_MATE_SCORE, and a position repeated inside the chain returns THE SAME
//node, still unevaluated because its eval_and_expand() has not returned yet. A perpetual check
//therefore recurses forever. pos_history does not stop it -- that holds the GAME history, and the
//extension chain never adds to it.
//
//Nothing bounded it. AccumulatorStack::push() guards MaxSize with an assert(), which the release
//dylib compiles out, so overflow would have been a silent heap overwrite rather than a crash at
//the point of failure.
//
//32 keeps the worst case (MAX_DEPTH 100 + 32) far under 247 and under the 512 KB thread stack,
//while being far deeper than any real forcing sequence, so ordinary play is unaffected. Hitting
//the cap returns 0.0, which is also the right answer: a check chain this long is a perpetual, and
//a perpetual is a draw.
#define MAX_CHECK_EXTENSION 32

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
// An OPEN-ADDRESSING hash table keyed by Zobrist hash, holding the transposition DAG.
//
// It replaces std::unordered_map, and the reason is measured rather than stylistic. An
// unordered_map allocates one hash node per entry, so every erase() is an allocator free of a
// small block scattered across a gigabyte of live tree. In an overnight run, one collection that
// dropped 8.1M nodes spent 15.8 SECONDS in the unlink walk -- roughly 2 microseconds per erase --
// against 229 ms of marking. Erasing here is a tombstone write into a contiguous array: no
// allocation and no pointer chase.
//
// Two secondary gains. It is smaller: 16 bytes a slot at a 0.7 load factor, against a node plus a
// next pointer plus malloc overhead plus the bucket array. And make_child() does millions of
// lookups per search, which become a short linear probe over adjacent cache lines instead of a
// chain walk through scattered allocations.
//
// The index is simply the low bits of the key, which is what NoOpHash already did for
// unordered_map -- Zobrist hashes are well distributed, so no mixing is needed.
//
// LOCKING IS UNCHANGED and remains the caller's job: shared_lock on map_mutex to look up, unique
// _lock to insert. One rule is stricter than before, though. A rehash REALLOCATES the slot array,
// so an iterator or a Slot& is only valid while the lock is held -- copy the MCTSNode* out before
// unlocking, never dereference an iterator afterwards.
class NodeMap {
  public:
    // Named first/second so that "for (auto& [h, n] : tree)" and "it->second" both read exactly
    // as they did with unordered_map, keeping every call site unchanged.
    struct Slot { uint64_t first = 0; MCTSNode * second = nullptr; };

    // second == nullptr means the slot was never used and terminates a probe; TOMB means it held
    // an entry that was erased, so a probe must continue THROUGH it.
    // 1 is never a real MCTSNode address: the type's alignment is at least 8.
    static MCTSNode * tomb() noexcept { return reinterpret_cast<MCTSNode *>(uintptr_t(1)); }

    class iterator {
      public:
        iterator() = default;
        iterator(Slot * p, Slot * e) : p_(p), e_(e) { skip(); }
        Slot &     operator*()  const { return *p_; }
        Slot *     operator->() const { return p_; }
        iterator & operator++()       { ++p_; skip(); return *this; }
        bool operator==(const iterator& o) const { return p_ == o.p_; }
        bool operator!=(const iterator& o) const { return p_ != o.p_; }
        Slot * raw() const { return p_; }
      private:
        // begin() and operator++ must land on a LIVE slot; empties and tombstones are skipped.
        void skip() { while (p_ != e_ && (p_->second == nullptr || p_->second == tomb())) ++p_; }
        Slot * p_ = nullptr;
        Slot * e_ = nullptr;
    };

    NodeMap() = default;
    ~NodeMap() { delete[] slots_; }
    NodeMap(const NodeMap&)            = delete;
    NodeMap& operator=(const NodeMap&) = delete;

    size_t size() const noexcept { return size_; }
    bool  empty() const noexcept { return size_ == 0; }

    iterator begin() { return iterator(slots_, slots_ + cap_); }
    iterator end()   { return iterator(slots_ + cap_, slots_ + cap_); }

    void clear() {
        delete[] slots_;
        slots_ = nullptr;
        cap_ = size_ = used_ = 0;
    }

    iterator find(uint64_t key) {
        if (!cap_) return end();
        size_t i = key & (cap_ - 1);
        for (;;) {
            Slot & s = slots_[i];
            if (s.second == nullptr) return end();              // empty: key is absent
            if (s.second != tomb() && s.first == key) return iterator(&s, slots_ + cap_);
            i = (i + 1) & (cap_ - 1);                           // tombstone or collision: keep going
        }
    }

    std::pair<iterator, bool> emplace(uint64_t key, MCTSNode * v) {
        // Grow on PROBE occupancy (live + tombstones), not on size alone: a table full of
        // tombstones probes just as badly as a full one.
        if (!cap_ || (used_ + 1) * 10 >= cap_ * 7) grow();
        size_t  i    = key & (cap_ - 1);
        Slot *  reuse = nullptr;
        for (;;) {
            Slot & s = slots_[i];
            if (s.second == nullptr) {
                Slot * dst = reuse ? reuse : &s;
                if (!reuse) ++used_;          // a tombstone was already counted in used_
                dst->first  = key;
                dst->second = v;
                ++size_;
                return { iterator(dst, slots_ + cap_), true };
            }
            if (s.second == tomb()) { if (!reuse) reuse = &s; }
            else if (s.first == key) return { iterator(&s, slots_ + cap_), false };
            i = (i + 1) & (cap_ - 1);
        }
    }

    iterator erase(iterator it) {
        Slot * p = it.raw();
        p->second = tomb();     // used_ is unchanged: the slot still blocks probes
        --size_;
        return iterator(p + 1, slots_ + cap_);
    }

    // Right-size the table after a collection, which also sweeps out the tombstones.
    //
    // This is not an optimisation, it is the thing that makes an open-addressed table viable
    // here at all. Iteration is O(CAPACITY), not O(size): the sweep scans every slot, live or
    // not. std::unordered_map iterates in O(size) because libc++ threads its elements onto a
    // linked list, so it never pays for the empty space. Measured without this: a collection
    // that freed nothing walked 1.6M live entries in 551 ms because the table still had ~16M
    // slots left over from before the last die-off, while unordered_map walked 4.0M in 390 ms.
    // Per slot the flat table is about three times quicker; it was simply scanning ten times as
    // many of them.
    //
    // Rebuilding is cheap precisely because this table has no per-entry allocation: it is one
    // array allocation plus a linear reinsert of the survivors.
    void compact() {
        if (!cap_) return;
        size_t want = 16;
        while (want * 7 < size_ * 10) want <<= 1;
        // Rehash when the table is mostly empty, OR when tombstones have taken over the probe
        // space even though the live count has not moved much.
        if (want < cap_ || used_ > size_ + (cap_ >> 2)) rehash(want < cap_ ? want : cap_);
    }

    void reserve(size_t n) {
        size_t want = 16;
        while (want * 7 < n * 10) want <<= 1;
        if (want > cap_) rehash(want);
    }

  private:
    void grow() {
        // Sizing off size_ rather than used_ is what makes a rehash also SWEEP the tombstones:
        // a table that is mostly tombstones is rebuilt at the same capacity instead of doubling.
        size_t ncap = cap_ ? cap_ : 1024;
        while ((size_ + 1) * 10 >= ncap * 7) ncap <<= 1;
        rehash(ncap);
    }

    void rehash(size_t ncap) {
        Slot * old = slots_;
        size_t oc  = cap_;
        slots_ = new Slot[ncap];      // Slot's default member initialisers make every slot empty
        cap_   = ncap;
        size_ = used_ = 0;
        for (size_t i = 0; i < oc; ++i) {
            MCTSNode * v = old[i].second;
            if (v && v != tomb()) insert_fresh(old[i].first, v);
        }
        delete[] old;
    }

    // No tombstones exist in a table being rehashed, so the first empty slot is the destination.
    void insert_fresh(uint64_t key, MCTSNode * v) {
        size_t i = key & (cap_ - 1);
        while (slots_[i].second != nullptr) i = (i + 1) & (cap_ - 1);
        slots_[i].first  = key;
        slots_[i].second = v;
        ++size_;
        ++used_;
    }

    Slot * slots_ = nullptr;
    size_t cap_   = 0;   // always a power of two, so the modulo is a mask
    size_t size_  = 0;   // live entries
    size_t used_  = 0;   // live entries + tombstones, i.e. slots that block a probe
};

struct MCTSSearch {
    MCTSNode * root = nullptr;
    NodeMap tree; //Zobrist hash -> node
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
extern bool post_move_collect;
extern int64_t gc_threshold;
//Visit-distribution dump: the AlphaZero-style policy training target, collected as a free
//byproduct of searches that happen anyway. Empty path disables it.
extern std::string visit_dump_path;
extern std::string game_tag;
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
