#include <cstdlib>   //calloc/malloc for the child table
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
//Per-move overhead charged against the time budget, in milliseconds.
//
//A move costs more than its search: the bestmove has to reach lichess and the next position come
//back. That time leaves the clock but never appeared in the allocation, so the engine reliably
//spent more than it believed. Subtracting it makes the accounting honest, and it is what makes
//lifting the panic collapse safe at fast controls -- without it, 60+1 is left with 1.2 s on the
//clock at 400 ms of real latency and flags outright at 600 ms.
//
//300 ms is deliberately generous for a local network. Erring high costs a little search per move;
//erring low risks the flag, and the whole point of this constant is the asymmetry between those.
#define LATENCY_ALLOWANCE 300.0
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

//How much of Hash a collection is allowed to keep, in per-mille, soft limit and hard limit.
//Past the soft limit only nodes holding at least a thousandth of the root's visits keep their
//children; past the hard limit nothing does. See the long note in gc() for why retention has to
//be bounded at all: with tree reuse the collector could mark the entire tree as reachable, free
//nothing, and leave the search permanently unable to expand. 600 leaves 40% headroom, which is
//several moves' growth at observed rates, and 850 bounds how far the PV exemption may overshoot.
#define GC_EVICT_SOFT 600
#define GC_EVICT_HARD 850

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
struct alignas(64) MCTSNode {
    std::atomic<uint64_t> hash{0};
    std::atomic<uint64_t> N{0};  // Atomic for lock-free updates
    std::atomic<double> W{0};
    std::atomic<int> cp {NO_MATE_SCORE}; //position evaluation in centipawns 
    std::atomic<int> num_children{0};
    std::atomic<int> terminal{0}; //0 (not terminal), 1 (mate), 2 (stalemate), 3 (repetition), -1 (check)
    std::atomic<uint8_t> expanding{0};  // expansion gate (test-and-set try-lock): exchange(1, acquire) == 0 acquires it, store(0, release) releases it
    //How many times the search has DESCENDED THROUGH this node, i.e. the sum of its edges' n.
    //It is the numerator of the exploration term, and it has to be edge-consistent with the
    //denominator or the two are on different scales: N below counts every arrival at this
    //POSITION from any parent and across the whole game, which under tree reuse can be millions
    //while a fresh edge is still at zero. Sits in padding after `expanding`, so it did not grow the
    //node; the size is pinned by the static_assert after the struct.
    std::atomic<uint32_t> descents{0};
    //Where this node's children sit in the child table's pool: child i is slot pool[pool_off + i]. A
    //copy of what kids[] holds for the collector, kept here because the SEARCH already has this cache
    //line loaded when it walks a node's children, so reading it costs nothing extra. Written when the
    //expansion is published, before num_children. It must sit HERE, in the four bytes between
    //`descents` and `evidence` -- the only padding left in the node. See the static_assert below.
    std::atomic<uint32_t> pool_off{0};
    //EVIDENCE: visits that actually produced information, as distinct from N, which counts every
    //time the search came here.
    //
    //In textbook MCTS the two are the same number, because every visit runs a fresh rollout and so
    //adds a new sample. Creatica replaced rollouts with a deterministic evaluator, and the moment a
    //node cannot be expanded -- the tree is full, so there is no memory for its children -- every
    //further visit re-backpropagates the SAME evaluation. N climbs, Q does not move, and the visit
    //count stops measuring confidence and starts measuring attention.
    //
    //That is not a rare corner. Observed in a real game: a root child reached 16,025,716 visits
    //with no children and Q frozen at exactly its birth value, the engine ranked by N, played it,
    //and lost a queen from a winning position. Across two bots' logs, 0.1% and 0.6% of all moves
    //were played with no searched continuation at all.
    //
    //So N keeps driving SELECTION -- it must, or the exploration term never decays and the search
    //live-locks on the node it cannot expand -- while `evidence` drives Q and the root ranking.
    //W pairs with evidence, not with N: a visit that learned nothing adds to neither, so Q simply
    //does not move rather than being cemented in place by repetition.
    std::atomic<uint64_t> evidence{0};
    std::atomic<Edge *> children {nullptr}; //array of moves and priors leading to next nodes
};
//64 BYTES, checked. pool_off was first declared after `children`, in what the comment beside it called
//"padding that already sat at the end of the node". There was none: `children` ends at exactly byte
//64, so the fields came to 68 and alignas(64) rounded the node up to 128. That doubled every arena
//slot and raised the occupancy charge per node from 88 to 160 bytes, so Hash 2048 held about 11.7
//million nodes instead of about 18.7 million -- and nothing noticed until a lichess game froze on a
//full tree and lost its queen. The only real gap is the four bytes between `descents` and `evidence`.
static_assert(sizeof(MCTSNode) == 64, "MCTSNode must stay one cache line; see pool_off");
//THE CHILD TABLE stores std::atomic<uint64_t> in calloc'd memory, which is only sound while the atomic
//has the plain integer's size and alignment.
static_assert(sizeof(std::atomic<uint64_t>) == sizeof(uint64_t) && alignof(std::atomic<uint64_t>) == alignof(uint64_t),
              "the child table relies on std::atomic<uint64_t> being laid out as uint64_t");
struct NodeArena {
    MCTSNode * slots = nullptr;
    //THE MARK STAMPS LIVE HERE, NOT IN THE NODE, and that is the difference between a sweep that
    //reads 22 MB and one that reads 358 MB. The stamp used to be an `int generation` field inside
    //MCTSNode, so deciding whether a slot was reachable meant touching its 64-byte cache line.
    //Sequential is not the same as cheap: measured at 183 ns per slot even after the nodes were
    //made contiguous, because a 5.6M-slot arena is a third of a gigabyte and this machine is under
    //memory pressure. Four bytes a slot instead of sixty-four is sixteen times less to move.
    //
    //0 means the slot is free. A live slot carries the generation of the collection that last
    //marked it, and gc() hands out values starting at 2, so the two can never be confused.
    std::atomic<uint32_t> * gen = nullptr;
    //IDENTITY, WHICH IS NOT THE SAME THING AS REACHABILITY, and conflating them was a real bug:
    //the mark rewrites gen[] on EVERY collection, so a map entry validated against gen went stale
    //the first time the collector ran and every lookup in the tree failed at once. tag[] changes
    //only when a slot changes hands -- a fresh value on allocation, 0 when the sweep frees it --
    //so it answers "is this still the node I filed?" while gen[] answers "did the mark reach it?".
    std::atomic<uint32_t> * tag = nullptr;
    //Monotonic, so a recycled slot never reuses a value an old map entry might still hold. It
    //wraps after four billion allocations; an entry would have to survive the whole wrap AND land
    //on the same slot, and every compact() rebuilds the table, so this is not reachable in play.
    std::atomic<uint32_t> next_tag{1};
    uint32_t   cap   = 0;      //how many slots exist; 0 means "not sized yet"
    //Slots never handed out yet. Bump first, free list afterwards: a fresh arena hands out
    //sequential indices, which keeps a young tree contiguous in memory.
    std::atomic<uint32_t> bump{0};
    //Indices returned by the reaper, LIFO. A plain vector under its own mutex: it is touched once
    //per allocation and once per reclaim, both off the per-simulation hot path, so the contention
    //that would justify a lock-free stack is not there.
    std::vector<uint32_t> free_list;
    std::mutex            free_mtx;
    std::atomic<size_t>   released{0};

    //THE CHILD TABLE. What the collector's mark needs, kept where the mark can read it without touching
    //a node or an edge.
    //
    //The mark only needs, for each reachable node, how many children it has and which slots they are.
    //It used to get that from the node's 64-byte line and from every child's 24-byte Edge, which for an
    //11.6M-node, 39M-edge tree is about 1.7 GB of pages. Measured: with those pages in RAM the mark
    //costs about 10.5 ns per node-plus-edge; when the machine has compressed them it cost 67.5 ns, with
    //463,243 decompressions in one run. A page is decompressed if ANY byte of it is read, so a smaller
    //read only helps if it lives in pages of its own -- which is why this is two separate arrays
    //rather than a field added to the node or the edge.
    //
    //  kids[slot]  (pool offset << 32) | child count, 0 when the slot has no children. Per slot, like
    //              gen[] and tag[]. Written when an expansion is PUBLISHED, before num_children, so a
    //              reader that sees a child count also sees where the children are.
    //  pool[off+i] the arena slot of child i, for i < count. Written BEFORE the expansion is published,
    //              never changed afterwards, and returned to pool_free when the node dies.
    //
    //Blocks are recycled by exact size: a chess position has at most 218 legal moves and the sizes in
    //a game cluster tightly, so exact-size free lists reuse well without a general allocator.
    static constexpr uint32_t POOL_MAX_BLOCK = 256;
    std::atomic<uint64_t> * kids = nullptr;
    uint32_t *            pool = nullptr;
    uint64_t              pool_cap = 0;
    std::atomic<uint64_t> pool_bump{0};
    std::vector<uint64_t> pool_free[POOL_MAX_BLOCK + 1];
    std::mutex            pool_mtx;
    //Offset of a block of n entries, or UINT64_MAX when the pool is exhausted, which the caller treats
    //exactly like an exhausted arena: the expansion is abandoned and the node stays a leaf.
    uint64_t pool_alloc(uint32_t n) {
        if (n == 0 || n > POOL_MAX_BLOCK) return UINT64_MAX;
        {
            std::lock_guard<std::mutex> lk(pool_mtx);
            auto& fl = pool_free[n];
            if (!fl.empty()) { const uint64_t off = fl.back(); fl.pop_back(); return off; }
        }
        const uint64_t off = pool_bump.fetch_add(n, std::memory_order_relaxed);
        return off + n <= pool_cap ? off : UINT64_MAX;
    }
    void pool_release(uint64_t off, uint32_t n) {
        if (n == 0 || n > POOL_MAX_BLOCK) return;
        std::lock_guard<std::mutex> lk(pool_mtx);
        pool_free[n].push_back(off);
    }
    void pool_reset() {
        pool_bump.store(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(pool_mtx);
        for (auto& fl : pool_free) fl.clear();
    }
    //Child i of a published node. `parent` must be expanded (num_children > i): that is what guarantees
    //pool_off and the block behind it are in place. Every read of a child goes through here.
    MCTSNode * child_at(const MCTSNode * parent, int i) const {
        return &slots[pool[(uint64_t)parent->pool_off.load(std::memory_order_relaxed) + (uint64_t)i]];
    }
    static uint64_t kids_pack(uint64_t off, uint32_t n) { return (off << 32) | (uint64_t)n; }
    static uint64_t kids_off(uint64_t k) { return k >> 32; }
    static uint32_t kids_count(uint64_t k) { return (uint32_t)(k & 0xFFFFFFFFu); }

    void size_to(size_t bytes_for_nodes) {
        const uint32_t want = (uint32_t)std::min<size_t>(bytes_for_nodes / sizeof(MCTSNode),
                                                         0xFFFFFFFEu);
        if (want == cap) return;
        delete[] slots;
        delete[] gen;
        delete[] tag;
        std::free(kids);
        std::free(pool);
        slots = new MCTSNode[want];
        gen   = new std::atomic<uint32_t>[want];
        tag   = new std::atomic<uint32_t>[want];
        //calloc and malloc, not new[]: a value-initialised new[] writes every element, which would make
        //hundreds of megabytes resident up front. These pages are touched only as the tree grows.
        //std::atomic<uint64_t> has uint64_t's size and layout, and zero bytes are a zero value.
        kids  = static_cast<std::atomic<uint64_t> *>(std::calloc(want, sizeof(std::atomic<uint64_t>)));
        //An Edge is never smaller than 16 bytes, so Hash / 16 bounds the edges that can ever exist.
        pool_cap = std::min<uint64_t>(bytes_for_nodes / 16, 0xFFFFFFFFull);
        pool     = static_cast<uint32_t *>(std::malloc(pool_cap * sizeof(uint32_t)));
        pool_reset();
        for (uint32_t i = 0; i < want; ++i) {
            gen[i].store(0, std::memory_order_relaxed);
            tag[i].store(0, std::memory_order_relaxed);
        }
        cap   = want;
        bump.store(0, std::memory_order_relaxed);
        released.store(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(free_mtx);
        free_list.clear();
    }
    //Returns nullptr when the arena is exhausted. The caller must treat that exactly as it treats
    //a full tree today -- expansion does not happen, the node stays a leaf.
    MCTSNode * alloc() {
        //fetch_add, not a compare-exchange loop. Every node creation goes through here -- of the
        //order of a million a second across four threads -- and a CAS loop makes every thread
        //that loses the race go round again, so the cost grows with the thread count instead of
        //staying flat. fetch_add is wait-free. It can run PAST cap, which is harmless: the
        //overshoot is never dereferenced, and everything that reads bump clamps it.
        const uint32_t i = bump.fetch_add(1, std::memory_order_relaxed);
        if (i < cap) { stamp_fresh(i); return &slots[i]; }
        std::lock_guard<std::mutex> lk(free_mtx);
        if (free_list.empty()) return nullptr;
        const uint32_t idx = free_list.back();
        free_list.pop_back();
        //UN-COUNT IT. live() is handed-out minus given-back, so a slot coming back OUT of the free
        //list has to be taken off that tally or it stays counted as free for the rest of the game.
        //Without this the node count drifts steadily below the truth -- measured as a collection
        //reporting 575,239 live when 811,576 slots were actually in use -- which feeds
        //tree_occupancy() and therefore the expansion gate.
        released.fetch_sub(1, std::memory_order_relaxed);
        stamp_fresh(idx);
        return &slots[idx];
    }
    void release(uint32_t idx) {
        std::lock_guard<std::mutex> lk(free_mtx);
        free_list.push_back(idx);
        released.fetch_add(1, std::memory_order_relaxed);
    }
    uint32_t index_of(const MCTSNode * n) const { return (uint32_t)(n - slots); }
    //What the map files: the slot's identity, not its mark.
    uint32_t stamp_of(const MCTSNode * n) const {
        return tag[index_of(n)].load(std::memory_order_relaxed);
    }
    //A value no live map entry can be holding, given for a slot just handed out. 0 is skipped
    //because 0 is what "this slot is free" means.
    void stamp_fresh(uint32_t i) {
        uint32_t t = next_tag.fetch_add(1, std::memory_order_relaxed);
        if (t == 0) t = next_tag.fetch_add(1, std::memory_order_relaxed);
        tag[i].store(t, std::memory_order_relaxed);
    }
    void stamp(const MCTSNode * n, uint32_t g) {
        gen[index_of(n)].store(g, std::memory_order_relaxed);
    }
    //Stamp and report what was there before, which is how the mark tells "I am the first to reach
    //this node" from "someone already has".
    uint32_t stamp_exchange(const MCTSNode * n, uint32_t g) {
        return gen[index_of(n)].exchange(g, std::memory_order_relaxed);
    }
    //How many slots are live: handed out, minus those given back.
    //Slots ever handed out, clamped because bump overshoots when the arena is exhausted.
    uint32_t high_water() const {
        const uint32_t b = bump.load(std::memory_order_relaxed);
        return b < cap ? b : cap;
    }
    //THE NODE COUNT. It used to come from the map's entry count, which stopped being the same
    //thing when the sweep stopped erasing: stale entries linger until the next rebuild. Handed
    //out minus given back is exact and needs no lock.
    size_t live() const {
        const size_t hw = high_water(), rel = released.load(std::memory_order_relaxed);
        return hw > rel ? hw - rel : 0;
    }
    ~NodeArena() { delete[] slots; delete[] gen; delete[] tag; std::free(kids); std::free(pool); }
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
    // HASH -> NODE AS AN INDEX AND A STAMP, NOT A POINTER.
    //
    // The point is what the collector's sweep has to touch. Holding a pointer meant a dead node's
    // entry could only be removed by looking the node up by hash and erasing -- one cold read of
    // the node plus a probe into this table, per corpse. Once the scan itself became free that was
    // the entire remaining sweep: 1,229 ns per dead node, 6.4 seconds on a 5.2M-corpse collection.
    //
    // With an index and a stamp the sweep invalidates an entry WITHOUT TOUCHING ANYTHING: it zeroes
    // gen[i] in the array it is already scanning, and every entry pointing at slot i is stale from
    // that instant. The cold work -- reading the node, releasing its edges, wiping and returning
    // the slot -- moves to the reaper, off the move's clock.
    //
    // AN ENTRY IS VALID WHEN BOTH HOLD:
    //   gen[idx] == stamp     the slot has not been freed or re-stamped since we recorded it
    //   slots[idx].hash == key   the slot has not been recycled for a different position
    // The stamp alone is not enough: a slot can be freed and handed to a new node within the same
    // generation, which would leave gen[idx] equal to the stamp we stored while the occupant is a
    // different position entirely. The hash check closes that, and it costs nothing where it
    // matters, because lookup() is about to hand that node to a caller who will read it anyway.
    // The stamp alone IS enough to spot a stale slot cheaply, which is all the insert path needs.
    struct Slot { uint64_t key = 0; uint32_t idx = 0; uint32_t stamp = 0; };

    NodeMap() = default;
    ~NodeMap() { delete[] slots_; }
    NodeMap(const NodeMap&)            = delete;
    NodeMap& operator=(const NodeMap&) = delete;

    void bind(NodeArena * a) { arena_ = a; }

    // Entries INSERTED since the last rebuild. Stale ones are not counted down -- nothing walks
    // this table to find them any more -- so this is an upper bound and must not be used as the
    // node count. NodeArena::live() is the authority on that.
    size_t entries() const noexcept { return size_; }

    void clear() { delete[] slots_; slots_ = nullptr; cap_ = size_ = used_ = 0; }

    // nullptr when the key is absent, or its node has been freed, or its slot has been recycled.
    MCTSNode * lookup(uint64_t key) const {
        if (!cap_) return nullptr;
        size_t i = key & (cap_ - 1);
        for (;;) {
            const Slot & s = slots_[i];
            if (s.stamp == 0) return nullptr;          // never written: the key is not here
            if (s.key == key) { MCTSNode * n = resolve(s, key); if (n) return n; }
            i = (i + 1) & (cap_ - 1);                  // collision, or a stale entry for this key
        }
    }

    // Returns (the node now filed under key, whether it was ours). Never fails: it grows first.
    std::pair<MCTSNode *, bool> insert(uint64_t key, MCTSNode * n) {
        if (!cap_ || (used_ + 1) * 10 >= cap_ * 7) grow();
        size_t i = key & (cap_ - 1);
        Slot * reuse = nullptr;
        for (;;) {
            Slot & s = slots_[i];
            if (s.stamp == 0) {
                Slot * dst = reuse ? reuse : &s;
                if (!reuse) ++used_;                   // a reused stale slot was already counted
                dst->key   = key;
                dst->idx   = arena_->index_of(n);
                dst->stamp = arena_->stamp_of(n);      // so n must be stamped BEFORE it is filed
                ++size_;
                return { n, true };
            }
            if (s.key == key) { MCTSNode * w = resolve(s, key); if (w) return { w, false }; }
            // Cheap staleness only -- the gen check, no node read. Missing a recycled-but-
            // same-generation slot costs one unreused entry, which the next rebuild reclaims.
            if (!reuse && arena_->tag[s.idx].load(std::memory_order_relaxed) != s.stamp) reuse = &s;
            i = (i + 1) & (cap_ - 1);
        }
    }

    // Rebuild, dropping stale entries. Called after a collection, which is exactly when a large
    // share of the table has just been invalidated by the sweep zeroing their stamps.
    void compact() {
        if (!cap_) return;
        size_t live = 0;
        for (size_t i = 0; i < cap_; ++i)
            if (slots_[i].stamp &&
                arena_->tag[slots_[i].idx].load(std::memory_order_relaxed) == slots_[i].stamp)
                ++live;
        size_t want = 16;
        while (want * 7 < live * 10) want <<= 1;
        if (want < cap_ || used_ > live + (cap_ >> 2)) rehash(want < cap_ ? want : cap_);
    }

    void reserve(size_t n) {
        size_t want = 16;
        while (want * 7 < n * 10) want <<= 1;
        if (want > cap_) rehash(want);
    }

  private:
    MCTSNode * resolve(const Slot & s, uint64_t key) const {
        if (arena_->tag[s.idx].load(std::memory_order_relaxed) != s.stamp) return nullptr;
        MCTSNode * n = &arena_->slots[s.idx];
        return n->hash.load(std::memory_order_relaxed) == key ? n : nullptr;
    }

    void grow() {
        size_t ncap = cap_ ? cap_ : 1024;
        while ((size_ + 1) * 10 >= ncap * 7) ncap <<= 1;
        rehash(ncap);
    }

    void rehash(size_t ncap) {
        Slot * old = slots_;
        const size_t oc = cap_;
        slots_ = new Slot[ncap];
        cap_   = ncap;
        size_  = used_ = 0;
        for (size_t i = 0; i < oc; ++i) {
            const Slot & s = old[i];
            if (!s.stamp) continue;
            if (arena_->tag[s.idx].load(std::memory_order_relaxed) != s.stamp) continue;  // stale
            size_t j = s.key & (cap_ - 1);
            while (slots_[j].stamp) j = (j + 1) & (cap_ - 1);
            slots_[j] = s;
            ++size_; ++used_;
        }
        delete[] old;
    }

    NodeArena * arena_ = nullptr;
    Slot *  slots_ = nullptr;
    size_t  cap_   = 0;   // always a power of two, so the modulo is a mask
    size_t  size_  = 0;   // entries inserted since the last rebuild
    size_t  used_  = 0;   // probe-occupied slots since the last rebuild
};

//ONE FLAT ARRAY OF NODES, ALLOCATED ONCE.
//
//Nodes used to come from `new MCTSNode()`, millions of times a move, which scattered them across
//the heap in allocation order. The collector's sweep iterates the hash map, so it walked those
//nodes in HASH order -- effectively at random across several gigabytes -- and had to dereference
//every one of them to read its generation stamp. The nodes it wants are by definition the ones the
//search has not touched, so they are the coldest pages in the process. Measured at Hash 4096 on an
//8 GB machine: 83,828 ms for one sweep, 1.09 microseconds per entry over 26.8M entries, charged to
//the clock of the move about to be played. The search that followed got 4 ms and 37 simulations.
//
//With the nodes in one array the sweep iterates by INDEX instead, which is a sequential stride, and
//the frees disappear entirely -- a slot is returned to a free list rather than handed to the
//allocator.
//
//WHAT THIS DELIBERATELY DOES NOT CHANGE: which nodes die. Only nodes unreachable from the root are
//reclaimed, exactly as before. That property is load-bearing and must not be traded away for
//speed: search threads hold raw MCTSNode* for a whole simulation -- down the tree, through a
//~30-evaluation expansion, and back up writing results into every node on the path -- and that is
//safe precisely because a node on a live thread's path is reachable, so the mark reaches it and
//the sweep never frees it. A scheme that recycled live slots (a transposition-table-style arena
//that overwrites the least valuable entry, say) would let one thread overwrite a node another
//thread is standing on, and the second thread's result would land in an unrelated position's
//statistics, silently. The pointers stay raw because nothing live is ever reused.

struct MCTSSearch {
    MCTSSearch() { tree.bind(&arena); }
    MCTSNode * root = nullptr;
    NodeArena  arena;  //declared first: the map validates against its stamps
    NodeMap    tree;   //Zobrist hash -> a slot in `arena`, as an index plus a stamp
};
struct Edge {
    std::atomic<int> move {0};             // The move that leads to the child position
    //N(s,a): how many times THIS edge has been taken, as distinct from how many times the child
    //POSITION has been reached. The two differ for any transposition, and they differ enormously
    //under ReuseTree, where a child keeps every visit it ever earned -- including the visits it
    //earned while it was itself the search root or the ponder root. Ranking and exploration both
    //want the per-edge count; only the value estimate Q wants the per-position one. Occupies the
    //four bytes of padding that already sat between `move` and `P`.
    std::atomic<uint32_t> n {0};
    std::atomic<double> P {0.0};            // Prior probability - model move_probs for a given move in the node
    //NO CHILD POINTER. It was an 8-byte pointer here; the child's slot now lives in the child table's
    //pool, and search.arena.child_at(parent, i) returns it. Keeping both would have been 12 bytes an edge
    //of duplicated state that could disagree, and would have grown the tree's footprint -- the very
    //thing the table exists to shrink. sizeof(Edge) is 16.
};
//WHAT ONE NODE AND ONE EDGE COST AGAINST Hash. Every occupancy figure is built from these, so the child
//table's entries are counted where they are spent: 8 bytes of kids[] per node, 4 bytes of pool per edge.
//The 24 per node is the map entry and the two stamp arrays, as before. With the child pointer gone from
//Edge the per-edge cost fell from 24 to 20, so the table leaves the tree's capacity roughly unchanged:
//about 3% fewer nodes for a tree-like middlegame, about 3% more for a transposition-dense endgame.
inline constexpr size_t NODE_BYTES = sizeof(MCTSNode) + 24 + sizeof(uint64_t);
inline constexpr size_t EDGE_BYTES = sizeof(Edge) + sizeof(uint32_t);


//ASYNC ONLINE TABLEBASE PROBE.
//
//The probe used to run BEFORE the search, blocking, on the move's own clock: a failed one cost its
//full timeout and then a full-length search on top, which is how a probe turned into a loss on
//time. It now runs BESIDE the search instead. The search starts immediately and never waits, so a
//slow or failed probe costs exactly nothing; if an answer arrives first it replaces the search's
//move at emission time, and since a tablebase answer is perfect play the search is stopped early
//rather than spending the rest of its allocation on a question already settled.
//
//`want` is the id of the move currently being searched and `have` the id the stored answer belongs
//to. They must match for the answer to be used, which is what makes a late reply from a previous
//move harmless instead of catastrophic.
struct TbProbe {
    std::atomic<uint64_t> want{0};
    std::atomic<uint64_t> have{0};
    std::mutex            mtx;      //guards move/score, held only to copy a short string
    std::string           move;
    int                   score{0};
};
extern TbProbe tb_probe;
//Repetition counts for the actual game, keyed by the full hash (side to move included).
//rep_count_flipped() asks the same question of the other parity -- see the notes on the definitions.
int rep_count(uint64_t h);
int rep_count_flipped(uint64_t h);
//Enqueue a speculative probe of the position the opponent must now answer, one step ahead of the
//search. Called immediately after our own move is applied to the board; costs nothing on our clock.
void tb_prefetch_after_our_move();

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
