//For MacOS using clang
// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -I /Users/ap/libchess  -L /Users/ap/libchess -Wl,-lchess,-lcurl,-rpath,/Users/ap/libchess creatica_search.cpp uci-nnue-policy.cpp tbcore.c tbprobe.c -o creatica

// -O3 -g -fno-omit-frame-pointer - options for profiling
//

// For linux or Windows using mingw
// add -mpopcnt for X86_64
// might need to add -Wno-stringop-overflow to avoid some warnings in tbcore.h
//g++ -std=c++20 -mpopcnt -Wno-deprecated -Wno-write-strings -Wno-deprecated-declarations -Wno-stringop-overflow -O3 -I /home/ap/libchess -L /home/ap/libchess creatica.cpp uci.cpp tbcore.c tbprobe.c -o creatica -lchess

//or with clang in MSYS2 MINGW64 or CLANG64
//clang++ -std=c++20 -mpopcnt -Wno-deprecated -Wno-write-strings -Wno-deprecated-declarations -O3 -flto -I /home/ap/libchess -L /home/ap/libchess creatica.cpp uci.cpp tbcore.c tbprobe.c -o creatica -lchess

#include "creatica_search.hpp"
#include "policy_net.h"

//The policy head replaces the engine's incumbent prior. creatica-shared-root builds its
//priors by playing every legal move and evaluating the child with NNUE -- around 29
//evaluations to expand one node. This variant runs one small network over the position's
//own NNUE feature-transformer output and scores each move with a single dot product, so an
//expansion costs one forward pass instead of 29 evaluations.
//
//Measured on held-out lichess PV shards, against the incumbent it replaces:
//  top-1 32.9% vs 28.5%, top-2 48.9% vs 45.0%, top-6 75.5% vs 77.1%.
//Better where PUCT actually spends its first visits, marginally worse in the tail. The
//tail costs little here because this engine never prunes: every legal move keeps a prior
//and stays reachable.
//
//If the weights cannot be loaded the engine falls back to the incumbent prior rather than
//playing with a random net, and says so in the log.
//Two separable changes live behind this, and they must not be bundled again.
//
//  POLICY_OFF   the incumbent. Every child is evaluated; the priors are a by-product of
//               that work, and so is the node's 1-ply minimax value.
//  POLICY_PRIOR children are still evaluated, but the priors come from the policy head.
//               No speedup at all -- this exists only to answer "are the learned priors
//               better IN PLAY", isolated from everything else.
//  POLICY_FULL  children are not evaluated. This is the speedup, and it also throws away
//               the 1-ply lookahead the incumbent got for free. Two changes, not one.
//
//The first self-match ran POLICY_FULL against POLICY_OFF and lost 0-3. That result cannot
//distinguish "the priors are bad" from "losing the lookahead costs more than the extra
//nodes are worth", which is exactly why POLICY_PRIOR exists.
enum PolicyMode { POLICY_OFF = 0, POLICY_PRIOR = 1, POLICY_FULL = 2 };
PolicyNet     policy_net;
int           policy_mode    = POLICY_OFF;
bool          policy_enabled = false;
//get_prob() divides by `temperature`, which was tuned for evaluations in pawns. The head's
//outputs are CE-trained logits on an unrelated scale, and at the engine's 0.58 they carry
//0.486 of the mass on the top move against the incumbent's 0.402 -- sharper than anything
//that was ever tuned for. Scaling by temperature/policy_temperature on the way in makes
//get_prob() yield exactly softmax(logit / policy_temperature), so 1.0 reproduces the
//distribution the head was trained to emit.
double        policy_temperature = 0.75;
//How much of the prior comes from the policy head, 0..1. The rest comes from the 1-ply
//child evaluation the engine computes anyway in this mode.
//
//These two signals carry DIFFERENT information and measuring them against each other was a
//mistake. The child evaluation sees the position AFTER the move, so a move whose merit only
//becomes visible once it is played is obvious to it; the policy head sees only the position
//before the move and is structurally blind to that case. Measured over 12000 held-out
//positions, agreement with Stockfish's PV1:
//
//     w      Top-1    Top-4    Top-6
//   0.00    27.02%   65.75%   77.76%   child evaluation alone (the incumbent)
//   0.40    35.46%   71.92%   81.46%
//   0.50    36.33%   71.62%   81.32%
//   1.00    32.79%   66.21%   75.89%   policy head alone
//
//The blend beats BOTH on every column, and it is the only setting that beats the incumbent
//at Top-6 -- the column where a pure policy prior loses. Nothing here is free: the child
//evaluations still cost what they always did. This buys ranking, not speed.
double        policy_blend = POLICY_BLEND;
//Overall sharpness of the blended prior. A mixture of two signals that disagree is FLATTER
//than either alone: at blend 0.45 the top move carries 0.356 of the mass against the
//incumbent's 0.3945, about 10% flatter. The exploration constants were fitted to the
//incumbent's concentration, so leaving this at 1.0 would change prior sharpness and prior
//ranking at the same time -- the same mistake that made the first temperature fix land 23%
//flatter than intended. 1.15 measures back to 0.4015. Scaling logits cannot reorder them,
//so this is free of any effect on ranking.
double        policy_blend_scale = POLICY_BLEND_SCALE;
double        fpu_reduction  = FPU_REDUCTION;
//--- decomposing the 1-ply look-ahead -------------------------------------------------
//
//The child evaluations computed at expansion feed THREE separate things, and "full" mode
//removed all three at once, which told us only that the combination matters. These two
//switches, with policy_blend, let each be turned off on its own:
//
//   policy_blend = 1.0   the prior stops using the evaluations (policy only)
//   seed_children = 0    a child is no longer born with N=1, W=tanh(eval)
//   node_minimax = 0     the node's value comes from a static evaluation of itself rather
//                        than the 1-ply minimax over its children
//
//The evaluations are still COMPUTED in every case, so these isolate what the look-ahead
//contributes, not what it costs.
bool          seed_children  = true;
bool          node_minimax   = true;
//--- ProbabilityMass ------------------------------------------------------------------
//
//Keep only the moves whose priors sum to this share of the total, and drop the tail. 1.0
//keeps everything and is an exact no-op: the gate below is skipped entirely, so the default
//path is byte-for-byte what it was.
//
//This existed before and was removed because every setting below 100% played weaker. It is
//back because BOTH of its terms have changed.
//
//What changed in the measurement: the old gate cut the tail of an eval-derived prior whose
//top-1 was 27%. The policy head is a different prior.
//
//What changed in the mechanism, and this is the part that matters: the old gate ran AFTER
//the child evaluations, because the prior WAS the evaluation -- so it could only ever
//discard moves, never save the work of evaluating them. It narrowed the tree at no gain,
//which is a good explanation for why every threshold cost strength. This gate runs BEFORE
//them, on the policy score alone, which is the only prior available at that point. Dropping
//a move here really does skip its evaluation, and those are 94% of the cost of an expansion.
//
//Measured on held-out positions, gating the pure policy prior:
//    mass    Stockfish's PV1 survives    moves kept of ~29
//    0.99            98.5%                     18.9      (~35% of the evaluations skipped)
//    0.999           99.9%                     24.6      (~15% skipped)
//
//The risk this carries is not symmetric. A move with a low policy score that is nevertheless
//best is disproportionately a tactical one, and a mate in 1 dropped here is invisible to the
//mate detection below, which only sees moves that were evaluated. Set it below 1.0 only on
//the evidence of a match, not on the recall table above.
double        probability_mass = 1.0;
//--- ReuseTree ------------------------------------------------------------------------
//
//Keep the search tree between moves instead of destroying it, so the subtree under the move
//actually played is inherited already searched rather than rebuilt from nothing.
//
//This used to be reachable only by turning Ponder on, because the two were the same branch.
//They are separate concerns: pondering is about thinking on the opponent's clock, reuse is
//about not throwing away work. Pondering does REQUIRE reuse -- a ponder search that is wiped
//at the next move has achieved nothing -- so Ponder still implies this, but the reverse no
//longer holds and reuse can be measured on its own.
//
//Default ON since it was measured: 9.5/12 head to head against the same binary with this
//off, about +232 Elo at roughly 3 standard errors. The margin is far larger than the
//known biases in that match (the no-reuse side paid cleanup() on its clock, now fixed,
//and the collector competed for CPU in local self-play), which together account for
//perhaps 15-25 Elo of it.
bool          reuse_tree = true;
//Collect only when the tree is actually filling up, in per-mille of the Hash allocation.
//
//gc() is O(tree) and was measured at 100 ms on 410k nodes rising to 442 ms on 5.1M -- 37% of a
//1200 ms move, growing as the game goes on. Running it every move paid that unconditionally.
//But collection only reclaims MEMORY: an unreachable node is never traversed, so it costs the
//search nothing while it sits there. At 5.1M nodes occupancy was still only ~180 of 1000, so
//almost every one of those collections was work for nothing.
int64_t       gc_threshold = 700;

//Simulations performed by THIS search. The info lines used to report search.root->N for
//"nodes", which the comment at the ponder-output site still calls "total simulations" -- and it
//was one, as long as the tree was destroyed before every move so the root started at zero. With
//ReuseTree the root carries the visits of every earlier search that passed through it, so nodes
//counted work already done and nps divided that inherited total by this search's elapsed time,
//inflating both. Counted here instead, and reset when a search starts.
std::atomic<uint64_t> search_simulations{0};
int           nnue_feature_dims();
int           nnue_features(const Board&, NNUEContext&, unsigned char*);

std::mutex mtx, log_mtx, print_mtx, pool_mutex, search_done_mtx, probe_mutex;
std::shared_mutex map_mutex;
std::condition_variable cv, pool_cv, pool_done_cv, cv_search_done;
std::atomic<bool> searchFlag {false};
std::atomic<bool> stopFlag {false};
std::atomic<bool> quitFlag {false};
std::atomic<bool> ponderHit {false};
std::atomic<bool> pool_quit{false};    // True when engine exits
std::atomic<int> pool_generation{0};   // Increments every new search
std::atomic<int> active_workers{0};    // Count of currently working threads
std::atomic<bool> search_done{false}; // Signals search completion
std::atomic<uint64_t> total_children{0};
std::atomic<uint64_t> tbhits{0};
std::atomic<int> generation{0};
std::atomic<int> hash_full{0};
//Node count, maintained atomically so it can be read DURING a search.
//
//tree_occupancy() and the workers' expansion guard both need to know how full the tree is, but
//search.tree.size() cannot be read while make_child() is inserting under map_mutex. Kept in
//step here instead: incremented on a successful insert, resynced from the map by gc() and
//cleanup(), which only ever run with no search in flight.
std::atomic<size_t> total_nodes{0};
std::atomic<int> depth{0};
std::atomic<int> seldepth{0};

FILE * logfile = nullptr;
char best_move[6] = "";
bool tb_init_done = false;
double timeAllocated = 0.0; //ms
double exploration_min;
double exploration_max;
double exploration_depth_decay;
//double probability_mass;
double virtual_loss;
double eval_scale;
double temperature;

std::string last_move;
std::unordered_set<uint64_t> position_history;
Board board = {};
ZobristHash zh = {};
Zobrist z = {};
Engine chessEngine = {};
MCTSSearch search;
std::vector<std::thread> pool_threads;
std::vector<ThreadParams> pool_params;

void gc_join();         //both defined with the collector below
void cleanup_locked();

//The body, with no join. Callable from the background collector itself, which must not try
//to join the thread it is running on.
void cleanup_locked() {
  for (auto& [h, node] : search.tree) {
      Edge * children = node->children.load(std::memory_order_relaxed);
      delete[] children;
      delete node;
  }
  search.tree.clear();
  search.root = nullptr;
  total_children.store(0, std::memory_order_relaxed);
  total_nodes.store(0, std::memory_order_relaxed);
}

//The public form: stop the background collector first, since it owns the tree while it runs
//and destroying it underneath would be a double free. Reached via new_game().
void cleanup() {
  gc_join();
  cleanup_locked();
}

//we run gc() in runMCTS() before starting search threads, so no locking
//--- tree validation ------------------------------------------------------------------------
//
//gc() has never been trusted: the source has carried a note that "there rarely is some kind of
//contamination or corruption of the tree" for long enough that the tree has been substantially
//rewritten since, and the workaround -- calling cleanup() and rebuilding from scratch every
//move -- has been in place ever since. Nobody has been able to say whether the original defect
//still exists, because the symptom is rare, silent, and surfaces many moves after the damage.
//
//This checks the invariants gc() is supposed to preserve, so a violation reports itself at the
//collection that caused it instead of as an inexplicable evaluation later. It walks the whole
//map, so it is off unless the ValidateTree option is set.
//
//It must never dereference a pointer it suspects. A freed node's memory may be reused or
//unmapped, so following an Edge to ask "are you in the map?" would crash inside the very check
//meant to detect that. Instead it collects the set of live node ADDRESSES from the map first
//and tests membership, which touches nothing.
bool validate_tree_enabled = false;

//--- visit-distribution dump ------------------------------------------------------------------
//
//Writes the root's visit distribution after every completed search: the position, and how many
//visits the search gave each legal move.
//
//This is the training target AlphaZero uses, and it is a far richer signal than the one the
//policy head is trained on today. Training against "did we pick Stockfish's PV1" saturated --
//top-1 went 26.7% -> 33.1% -> 34.8% while Elo went +108 -> +113 -> level, because past a point
//the positions the model newly gets right are ones where several moves were comparable anyway.
//A visit distribution says *how much better*, over every move, and it comes from a search that
//looked deeper than the prior it is teaching.
//
//It is a byproduct of searches that are happening regardless, so it costs nothing to collect.
//Written from select_best_moves(), which runs after the workers have been joined -- single
//threaded, so no locking is needed beyond keeping two engine processes out of one file (give
//each its own path).
std::string   visit_dump_path;   //empty disables it entirely
std::string   game_tag;          //set per game by the driver, so records can be joined to results

//Tree occupancy in per-mille of the configured Hash, from counters already maintained -- O(1),
//no walk, so it is safe to consult before deciding whether a walk is worth doing.
int tree_occupancy() {
  const size_t total_memory = total_nodes.load(std::memory_order_relaxed) * (sizeof(MCTSNode) + 24)
                            + (size_t)total_children.load(std::memory_order_relaxed) * sizeof(Edge);
  const size_t max_capacity = (size_t)chessEngine.optionSpin[Hash].value * 1024 * 1024;
  return max_capacity ? (int)((total_memory * 1000) / max_capacity) : 0;
}

int validate_tree(const char * where, bool collected) {
  if (!validate_tree_enabled) return 0;
  int errors = 0;
  int reported = 0;
  const int MAX_REPORT = 20;
  auto complain = [&](const char * what, uint64_t a, uint64_t b) {
    ++errors;
    if (reported++ < MAX_REPORT) {
      log_file("validate_tree(%s): %s (%llu, %llu)\n", where, what,
               (unsigned long long)a, (unsigned long long)b);
      print("info string validate_tree(%s): %s (%llu, %llu)\n", where, what,
            (unsigned long long)a, (unsigned long long)b);
    }
  };

  //Every node the map holds, by address. Membership tests below use this and never dereference.
  std::unordered_set<const MCTSNode *> live;
  live.reserve(search.tree.size() * 2);
  for (const auto& [key, node] : search.tree) live.insert(node);

  //1. The root must exist and be the node the map holds under its own hash.
  if (!search.root) {
    complain("search.root is null", 0, 0);
  } else if (!live.count(search.root)) {
    complain("search.root is not in the tree map", 0, 0);
  } else {
    const uint64_t rh = search.root->hash.load(std::memory_order_relaxed);
    auto it = search.tree.find(rh);
    if (it == search.tree.end() || it->second != search.root)
      complain("search.root is filed under a different key than its own hash", rh, 0);
  }

  //2. Per node: the key matches the stored hash, the children array agrees with the count, no
  //   node carries a repetition verdict, and every Edge points at a node still in the map.
  uint64_t counted_children = 0;
  for (const auto& [key, node] : search.tree) {
    const uint64_t nh = node->hash.load(std::memory_order_relaxed);
    if (nh != key) complain("node filed under a key that is not its hash", key, nh);

    //Repetition is a property of the path, not the position, so it must never be cached on a
    //shared node. If this fires, something is writing terminal 3 to a node again.
    if (node->terminal.load(std::memory_order_relaxed) == 3)
      complain("node carries terminal 3 (repetition) which is path-dependent", key, 0);

    const int    nc = node->num_children.load(std::memory_order_relaxed);
    const Edge * ch = node->children.load(std::memory_order_relaxed);
    if (nc < 0) complain("negative num_children", key, (uint64_t)nc);
    if ((nc > 0) != (ch != nullptr))
      complain("num_children and children array disagree", key, (uint64_t)nc);
    if (nc > 0 && ch) {
      counted_children += (uint64_t)nc;
      for (int i = 0; i < nc; ++i) {
        const MCTSNode * c = ch[i].child.load(std::memory_order_relaxed);
        if (!c) { complain("null child pointer in an Edge", key, (uint64_t)i); continue; }
        //Membership only -- c may be freed memory, so it is never dereferenced here.
        if (!live.count(c))
          complain("Edge points at a node that is NOT in the tree map (dangling)", key, (uint64_t)i);
      }
    }
  }

  //3. The hashfull accounting must match what is actually allocated.
  const uint64_t recorded = (uint64_t)total_children.load(std::memory_order_relaxed);
  if (counted_children != recorded)
    complain("total_children disagrees with the Edge arrays actually present",
             recorded, counted_children);

  //4. Reachability, but ONLY straight after a collection.
  //
  //This is what gc() claims: nothing survives that the root cannot reach. It is NOT true at
  //other times, and asserting it unconditionally was wrong -- with lazy collection the tree is
  //deliberately left uncollected while occupancy is below the threshold, so the map legitimately
  //holds the previous root, its other children and their subtrees. The first run of this check
  //reported 1.6M nodes with 31 reachable, which was the lazy policy working exactly as intended.
  //Unreachable nodes waste memory; they are never traversed and are not a hazard. The dangling
  //check above is the one that matters, and it applies at all times.
  if (collected && search.root && live.count(search.root)) {
    std::unordered_set<const MCTSNode *> seen;
    seen.reserve(search.tree.size() * 2);
    std::queue<const MCTSNode *> q;
    seen.insert(search.root);
    q.push(search.root);
    while (!q.empty()) {
      const MCTSNode * n = q.front();
      q.pop();
      const int    nc = n->num_children.load(std::memory_order_relaxed);
      const Edge * ch = n->children.load(std::memory_order_relaxed);
      if (nc <= 0 || !ch) continue;
      for (int i = 0; i < nc; ++i) {
        const MCTSNode * c = ch[i].child.load(std::memory_order_relaxed);
        //Only follow pointers already proven live, so a dangling Edge cannot crash the walk.
        if (c && live.count(c) && seen.insert(c).second) q.push(c);
      }
    }
    if (seen.size() != search.tree.size())
      complain("nodes in the map are unreachable from the root",
               (uint64_t)search.tree.size(), (uint64_t)seen.size());
  }

  if (errors) {
    log_file("validate_tree(%s): %d violation(s), %zu nodes\n", where, errors, search.tree.size());
    print("info string validate_tree(%s): %d violation(s), %zu nodes\n", where, errors, search.tree.size());
  }
  return errors;
}

//`from` overrides the root to collect from; null means search.root. Re-rooting onto the move
//just played and collecting from there is what actually frees memory -- see the call after
//bestmove in runMCTS.
//Background collector. Started when bestmove has been sent, stopped and joined the moment
//anything wants the tree again -- a search, a new position, a new game, or quit.
//
//It is never concurrent with a search. Running a collector alongside one would need deferred
//reclamation, because make_child() finds nodes by HASH: a search can resurrect a node the mark
//phase has already passed over, and the sweep would then free it under a live parent. Joining
//before the search starts sidesteps that entirely -- the collector only ever runs while the
//engine is idle, and the abort simply bounds how long the next command has to wait for it.
std::thread       gc_thread;
std::atomic<bool> gc_abort{false};
//Fraction of the tree the most recent collection actually reclaimed.
//
//Measured in a live game: six of twelve consecutive collections freed EXACTLY NOTHING while
//costing 350-870 ms each, and one cost 1.16 s. That is what tree reuse does to a mark-and-
//sweep -- the cost is O(tree) but the yield is O(garbage), and a good prior concentrates the
//search on the move it then plays, so the sibling subtrees that become garbage are nearly
//empty. Declining a collection the last one showed to be futile is not deferring work; there
//is no work there to defer.
std::atomic<double> last_gc_freed{1.0};   //start optimistic so the first collection always runs
#define GC_MIN_YIELD 0.05                 //below this the previous collection was not worth its cost

void gc(MCTSNode * from = nullptr);

void gc_join() {
  gc_abort.store(true, std::memory_order_relaxed);
  if (gc_thread.joinable()) gc_thread.join();
  gc_abort.store(false, std::memory_order_relaxed);
}

//wipe==true means "destroy the whole tree" (the no-reuse path) rather than "collect from the
//root", so BOTH configurations defer the same class of work to the same idle window. Without
//it the no-reuse side paid cleanup() on its own clock -- 64-89 ms a move -- while the reuse
//side deferred its collection, making an A/B between them asymmetric by roughly 8% of search
//time at a 1000 ms movetime.
void gc_start(MCTSNode * from, bool wipe) {
  gc_join();                                   //never two collectors at once
  const size_t before = search.tree.size();
  const auto   t0     = std::chrono::steady_clock::now();
  gc_thread = std::thread([from, before, t0, wipe] {
    if (wipe) cleanup_locked(); else gc(from);
    const size_t after = search.tree.size();
    last_gc_freed.store(before ? (double)(before - after) / (double)before : 1.0,
                        std::memory_order_relaxed);
    //Logged from inside the thread: the caller returns immediately and cannot observe the
    //outcome. log_file() is mutex-guarded, so this is safe from here.
    log_file("info string post-move %s: %zu -> %zu nodes, %d permille, %.1f ms%s\n",
             wipe ? "cleanup" : "gc", before, search.tree.size(), tree_occupancy(),
             std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count(),
             gc_abort.load(std::memory_order_relaxed) ? " (interrupted)" : "");
  });
}

void gc(MCTSNode * from) {
  MCTSNode * const gc_root = from ? from : search.root;
  assert(gc_root);
  //fetch_add() updates generation but returns original value before addition; hence, we add 1
  int current_gen = generation.fetch_add(1, std::memory_order_relaxed) + 1;
  // BFS traversal to mark reachable nodes with the current generation.
  // Use queue to avoid recursion and potential stack overflow in deep trees.
  std::queue<MCTSNode *> q;
  // Update generation for root and push it to the queue
  gc_root->generation.store(current_gen, std::memory_order_relaxed);
  q.push(gc_root);

  while (!q.empty()) {
      //Abandoning during the MARK means nothing may be swept: the marks are incomplete, so a
      //sweep would delete live nodes. Leaving the tree entirely alone is always safe, and the
      //next collection re-marks from scratch (exchange() above makes leftover stamps harmless).
      if (gc_abort.load(std::memory_order_relaxed)) return;
      MCTSNode * node = q.front();
      q.pop();  
      int num_children = node->num_children.load(std::memory_order_relaxed);
      Edge * children = node->children.load(std::memory_order_relaxed);
      for (int i = 0; i < num_children; ++i) {
          MCTSNode * child = children[i].child.load(std::memory_order_relaxed);
          if (child) {
              //Stamp with this generation; push only if we are the first to reach it.
              //
              //This was a compare_exchange expecting EXACTLY current_gen - 1, which assumed every
              //reachable node was marked by the immediately preceding collection. That held only
              //while every collection ran to completion. It breaks the moment a collection is
              //abandoned part way -- some nodes then carry the new stamp and some the old, the
              //CAS fails for the mismatched ones, they are never pushed, and the sweep deletes
              //them although a surviving parent still points at them. exchange() makes the mark
              //independent of what was there before, so a partial mark is harmless and the
              //collector can be interrupted.
              if (child->generation.exchange(current_gen, std::memory_order_relaxed) != current_gen)
                  q.push(child);
          }
      }
  }
  // Now iterate through the map and erase nodes with outdated generations, i.e. nodes that are not reachable
  // Also clean up allocated children arrays.
  //The SWEEP runs to completion once started. It must not be interrupted, and the reason is not
  //obvious, so it is worth writing down -- an earlier version did interrupt it and the validator
  //caught the result immediately, 34 dangling Edges in one search.
  //
  //A survivor never points at garbage: if it did, that node would be reachable from the root and
  //would have been marked. So every dangling Edge left by a partial sweep is garbage pointing at
  //deleted garbage, which the search can never traverse -- and that looks harmless. It is not,
  //because make_child() finds nodes by HASH, not by traversal. A garbage node still sitting in
  //the map can be handed back as a child of a live node, and its Edges point at freed memory.
  //Deleting a parent and its children in the same sweep is what prevents that, so the sweep is
  //all-or-nothing. Interruption is confined to the mark phase above, where abandoning the whole
  //collection frees nothing and leaves the tree exactly as it was.
  for (auto it = search.tree.begin(); it != search.tree.end();) {
    MCTSNode * node = it->second;
    if (node->generation.load(std::memory_order_relaxed) < current_gen) {
      // Clean up dynamically allocated children if any.
      Edge * children = node->children.load(std::memory_order_relaxed);
      int num_children = node->num_children.load(std::memory_order_relaxed);
      if (num_children > 0) {
        total_children.fetch_sub(num_children, std::memory_order_relaxed); //update total_children count
        delete[] children;
      }
      it = search.tree.erase(it);
      delete node;
    } else ++it;
  }
  total_nodes.store(search.tree.size(), std::memory_order_relaxed);
  //update hash_full
  size_t total_memory = search.tree.size() * (sizeof(MCTSNode) + 24) + total_children.load(std::memory_order_relaxed) * sizeof(Edge);
  size_t max_capacity = chessEngine.optionSpin[Hash].value * 1024 * 1024;  // MB to bytes
  int hashfull = max_capacity ? (total_memory * 1000) / max_capacity : 0;
  if (hashfull > 1000) hashfull = 1000;  // Cap at 1000 per UCI spec
  hash_full.store(hashfull, std::memory_order_relaxed);    
}

//no locking, call it before starting other threads (search, etc)
void set_root(NNUEContext& ctx) {
  auto it = search.tree.find(zh.hash);
  MCTSNode * root = (it != search.tree.end()) ? it->second : nullptr;
  if (!root) {
    root = new MCTSNode();
    root->hash.store(zh.hash, std::memory_order_relaxed);
    root->generation.store(generation.load(std::memory_order_relaxed), std::memory_order_relaxed);
    search.tree.emplace(zh.hash, root);
  }
  //Expand whether the root is new or REUSED. The expansion used to sit inside the !root branch,
  //so a root inherited from the previous search was left exactly as it was found. That is fine
  //when it was an interior node, but a root reused from a node the last search created and never
  //visited has no children at all, and mcts_search() only expands a leaf when its terminal is
  //<= 0 -- so a root wrongly marked terminal stayed childless and select_best_moves() then had
  //nothing to report. This path only exists when the tree survives between moves, i.e. under
  //gc(); cleanup() guarantees a fresh root every move.
  if (root->num_children.load(std::memory_order_relaxed) == 0) {
    std::unordered_set<uint64_t> pos_history;
    const double result = eval_and_expand(root, board, zh, ctx, pos_history, 0);
    //tanh, not pawns. eval_and_expand() returns an evaluation in PAWNS; every other writer of W
    //stores tanh(pawns / eval_scale) -- mcts_search() converts on the very next line after its
    //own call, and make_child() stores tanh(cp * 0.01 / eval_scale). Storing the raw pawn value
    //here put the root's W on a different scale from every child's, so the root's Q was wrong by
    //roughly a factor of eval_scale near zero and unbounded instead of confined to [-1, 1].
    root->W.store(tanh(result / eval_scale), std::memory_order_relaxed);
    root->N.store(1, std::memory_order_relaxed);
  }
  search.root = root;    
}

//called from expand_node() and make_move()
//returns new or existing node
MCTSNode * make_child(const uint64_t hash, const int cp, const int terminal) {
  //first, try to find child_hash in the tree
  std::shared_lock search_lock(map_mutex);
  auto it = search.tree.find(hash);
  MCTSNode * child = (it != search.tree.end()) ? it->second : nullptr;
  search_lock.unlock();
  if (!child) { //if the child_hash is not found, create a child
    child = new MCTSNode();
    child->cp.store(cp, std::memory_order_relaxed);
    child->terminal.store(terminal, std::memory_order_relaxed);
    child->hash.store(hash, std::memory_order_relaxed);
    child->generation.store(generation.load(std::memory_order_relaxed));
    //we should probably update N and W as well. We're updating cp, the node is evaluated, meaning it's been visited
    //Terminal children are seeded regardless of seed_children: a mate or tablebase result
    //is exact rather than an evaluation, and leaving a forced mate unseeded would measure
    //something other than the look-ahead.
    if (cp != NO_MATE_SCORE && (seed_children || terminal > 0)) {
      child->N.store(1, std::memory_order_relaxed);
      child->W.store(tanh(cp * 0.01 / eval_scale), std::memory_order_relaxed);
    }
    std::unique_lock insert_lock(map_mutex);
    auto [it, inserted] = search.tree.emplace(hash, child);
    insert_lock.unlock();
    if (!inserted) {
      // Another thread inserted first; use the existing node and clean up ours.
      delete child;
      child = it->second; //it.second - is a pointer to the existing node (it.first is a hash)
    } else {
      total_nodes.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return child; //may not be nullptr
}
  
struct TempEdge {
    int move = 0;
    double P = 0.0; 
    MCTSNode * child = nullptr;
};
//called from mcts_search() and process_check()
//calls make_child()
void expand_node(MCTSNode * parent, const std::vector<std::tuple<double, int, int, int, uint64_t>>& top_moves) {
  //parent's expansion gate is held by the caller - mcts_search(); NOTE process_check() -> eval_and_expand() calls this WITHOUT holding the gate
  if (parent->num_children.load(std::memory_order_relaxed) > 0) return; //already expanded by other threads, perhaps
  int num_moves = top_moves.size();
  assert(num_moves > 0);
  Edge * children = new Edge[num_moves];
  for (int i = 0; i < num_moves; ++i) {
      auto [prior, move_idx, child_cp, terminal, child_hash] = top_moves[i];
      //Same reason as the repetition break in mcts_search: make_move() returns terminal 3 when
      //the child position appears in THIS path's history, and make_child() would store that on
      //the shared node, creating it permanently drawn (cp 0, N 1, W 0). Create it unevaluated
      //instead, so whichever path visits it next evaluates it on its own terms. The prior for
      //this edge still reflects the draw, which is only a heuristic and costs nothing if it is
      //wrong for another path.
      const bool path_repetition = (terminal == 3);
      MCTSNode * child = make_child(child_hash,
                                    path_repetition ? NO_MATE_SCORE : child_cp,
                                    path_repetition ? 0 : terminal);
      children[i].P.store(prior, std::memory_order_relaxed);
      children[i].move.store(move_idx, std::memory_order_relaxed);
      children[i].child.store(child, std::memory_order_relaxed);        
  }
  total_children.fetch_add(num_moves, std::memory_order_relaxed); //update total_children counter
  //Publish with a CAS, not a bare store. process_check() -> eval_and_expand() reaches
  //here WITHOUT holding the expansion gate (see the note above), so two threads can both
  //pass the num_children guard and both allocate; a plain store let the loser's array
  //leak. Now exactly one array is ever published and the loser frees its own. The child
  //MCTSNodes themselves are shared via the tree map, so they must NOT be freed here.
  Edge * expected = nullptr;
  if (!parent->children.compare_exchange_strong(expected, children,
        std::memory_order_release, std::memory_order_relaxed)) {
    total_children.fetch_sub(num_moves, std::memory_order_relaxed);
    delete[] children;
    return;
  }
  parent->num_children.store(num_moves, std::memory_order_release);
}

//no locking, call only when search threads finished
//called from select_best_moves(), which in turn is called from runMCTS()
int most_visited_child(const MCTSNode * parent) {
  uint64_t N = 0;
  int idx = -1;
  std::vector<std::pair<double, int>> priors; //prior, child index
  int num_children = parent->num_children.load(std::memory_order_relaxed); // will be 0 for the last node
  Edge * children = parent->children.load(std::memory_order_relaxed); // will be nullptr for the last node
  for (int i = 0; i < num_children; i++) { //this loop will be skipped for the last node
    MCTSNode * child = children[i].child.load(std::memory_order_relaxed);
    uint64_t n = child->N.load(std::memory_order_relaxed);
    priors.push_back({children[i].P.load(std::memory_order_relaxed), i});
    if (n > N) {
      N = n;
      idx = i;
    } 
  }
  //in case of unexpanded child (node before last, last has no children), return idx of best prior and hence, the best scorecp
  if (idx == -1 && num_children > 0) {
    std::sort(priors.begin(), priors.end(), [](const auto& a, const auto& b) { return a.first > b.first;});
    return priors[0].second;
  }
  return idx; //this will be negative for the last node
}

//no locking, call only when search threads finished
//called from runMCTS()
//One line per search. Tab-separated, because a FEN contains spaces:
//  tag \t fen \t simulations \t rootQ \t rootCP \t ponder \t seldepth \t "move:N:prior ..."
//
//The move list is EVERY legal move with its visit count, including zeros -- a move the search
//refused to visit is as much a part of the target as the one it chose, and dropping those would
//bias the distribution toward flatness.
static void dump_visits(const std::vector<std::tuple<uint64_t, int>>& visits, const Edge * children) {
  if (visit_dump_path.empty() || !search.root) return;
  FILE * f = fopen(visit_dump_path.c_str(), "a");
  if (!f) return;

  //Header once, so the file explains itself.
  fseek(f, 0, SEEK_END);
  if (ftell(f) == 0) fprintf(f, "tag\tfen\tsimulations\trootQ\trootCP\tponder\tseldepth\tvisits\n");

  char fen[MAX_FEN_STRING_LEN];
  const uint64_t rootN = search.root->N.load(std::memory_order_relaxed);
  const double   rootW = search.root->W.load(std::memory_order_relaxed);
  //seldepth is recorded because simulation count alone does not say how far the search
  //actually looked, and depth is what decides whether a record teaches anything a shallow
  //search could not. It lets the trainer weight or filter by the quality of each record.
  fprintf(f, "%s\t%s\t%llu\t%.6f\t%d\t%d\t%d\t",
          game_tag.empty() ? "-" : game_tag.c_str(),
          board2fen(board, fen),
          (unsigned long long)rootN,
          rootN ? rootW / (double)rootN : 0.0,
          search.root->cp.load(std::memory_order_relaxed),
          chessEngine.ponder ? 1 : 0,
          seldepth.load(std::memory_order_relaxed));

  char uci[6];
  bool first = true;
  for (const auto& v : visits) {
    const int idx = std::get<1>(v);
    idx2uci(children[idx].move.load(std::memory_order_relaxed), uci);
    //move:visits:prior. The prior is what the policy head believed BEFORE the search; the visits
    //are what the search concluded after looking deeper. Recording both makes the file answer the
    //question that decides whether distillation is worth doing at all -- how often, and by how
    //much, does the search actually disagree with the prior it started from? If it rarely does,
    //there is nothing here to teach and no amount of training will help.
    fprintf(f, "%s%s:%llu:%.6f", first ? "" : " ", uci,
            (unsigned long long)std::get<0>(v),
            children[idx].P.load(std::memory_order_relaxed));
    first = false;
  }
  fprintf(f, "\n");
  fclose(f);
}

int select_best_moves(std::vector<std::pair<int, std::string>>& pvs) { 
  int num_children = search.root->num_children.load(std::memory_order_relaxed);
  if (!num_children) {
    log_file("select_best_moves() warning: root node has no children!\n");
    return 0;
  }
  char uci_move[6];
  std::vector<std::tuple<uint64_t, int>> visits; //N, child_idx
  Edge * children = search.root->children.load(std::memory_order_relaxed);
  for (int i = 0; i < num_children; i++) {
    MCTSNode * child = children[i].child.load(std::memory_order_relaxed);
    visits.push_back({child->N.load(std::memory_order_relaxed), i});
  }
  //Descending by visits, and on a TIE the lower child index wins.
  //
  //This was std::greater() over the whole tuple, which compares lexicographically: equal visit
  //counts fell through to comparing the index, also descending, so the HIGHEST index won. The
  //children are published in descending prior order (move_evals is sorted by prior before
  //expand_node), so the highest index is the LOWEST-prior move. Any tie therefore resolved to
  //the worst move rather than the best -- including the all-zero tie of a root that was never
  //searched, where it picked the move the prior liked least.
  std::sort(visits.begin(), visits.end(), [](const auto& a, const auto& b) {
    if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) > std::get<0>(b);
    return std::get<1>(a) < std::get<1>(b);
  });
  //Dumped HERE, on the raw search result. The loop below can drop moves (the repetition-avoidance
  //in winning positions), and those edits are a playing decision rather than something the search
  //concluded -- training on them would teach the policy a heuristic instead of an evaluation.
  dump_visits(visits, children);
  while (visits.size() > 1) {
    int idx = std::get<1>(visits[0]); //index of the most visited child
    int next_idx = std::get<1>(visits[1]); //index of the next most visited child
    MCTSNode * child = children[idx].child.load(std::memory_order_relaxed);
    MCTSNode * next_child = children[next_idx].child.load(std::memory_order_relaxed);
    //NNUE static eval is not reliable for deciding whether the position is winning
    //Let's try to use W instead. If it is positive, the position is winning 
    int cp = -child->cp.load(std::memory_order_relaxed);
    int next_cp = -next_child->cp.load(std::memory_order_relaxed);
    double w = -child->W.load(std::memory_order_relaxed);
    double next_w = -next_child->W.load(std::memory_order_relaxed);
    if (w > 0 && next_w > 0) { //check for repetition in winning position
      int global_count = position_history.count(child->hash.load(std::memory_order_relaxed));
      if (global_count) {
          int move = children[idx].move.load(std::memory_order_relaxed);
          int promo = (move >> 12) & 7;
          char fen[MAX_FEN_STRING_LEN];
          log_file("select_best_moves() debug: skipping move %s%s%c (would cause repetition in winning position %s, W %f, nextW %f, cp %d, nextCP %d)\n", square[(move >> 6) & 63], square[move & 63], promo != PieceTypeNone ? uciPromoLetter[promo] : ' ', board2fen(board, fen), w, next_w, cp, next_cp);
          visits.erase(visits.begin());
          continue;
      } else break;
    } else break;
  } // end of while (visits.size() > 1)
  int num_visits = visits.size();
  int multiPV = std::min<int>(num_visits, (int)chessEngine.optionSpin[MultiPV].value);
  int pvLength = chessEngine.optionSpin[PVPlies].value * sizeof(uci_move);
  int maxLen = pvLength - sizeof(uci_move);
  for (int i = 0; i < multiPV; i++) {
    int index = std::get<1>(visits[i]);
    idx2uci(children[index].move.load(std::memory_order_relaxed), uci_move);
    MCTSNode * child = children[index].child.load(std::memory_order_relaxed);      
    int cp = -child->cp.load(std::memory_order_relaxed);
    double parent_N = static_cast<double>(search.root->N.load(std::memory_order_relaxed));
    double prior = children[index].P.load(std::memory_order_relaxed);
    uint64_t N = child->N.load(std::memory_order_relaxed);
    double W = -child->W.load(std::memory_order_relaxed);
    double Q = W / N;
    double U = exploration_max * prior * sqrt(parent_N) / (1 + N);
    std::string pv(uci_move);
    std::string pv2(uci_move);
    pv2 += " (" + std::to_string(N) + ", " + std::to_string(llround(W)) + ", " + std::to_string(cp) + ", " + std::to_string(Q) + " + " + std::to_string(U) + " = " + std::to_string(Q + U) + ")";
    // Build PV by following most visited children
    num_children = child->num_children.load(std::memory_order_relaxed);
    Edge * children2 = child->children.load(std::memory_order_relaxed);
    int depth = 0;
    while (num_children > 0 && pv.size() < maxLen) {
      int idx = most_visited_child(child); 
      if (idx < 0) break;
      depth++;
      parent_N = static_cast<double>(child->N.load(std::memory_order_relaxed));
      prior = children2[idx].P.load(std::memory_order_relaxed);
      idx2uci(children2[idx].move.load(std::memory_order_relaxed), uci_move);
      pv += ' ';
      pv.append(uci_move);  
      pv2 += ' ';
      pv2.append(uci_move);   
      child = children2[idx].child.load(std::memory_order_relaxed);
      int cp2 = -child->cp.load(std::memory_order_relaxed);
      N = child->N.load(std::memory_order_relaxed);
      W = -child->W.load(std::memory_order_relaxed);
      Q = W / N;
      U = std::max(exploration_min, exploration_max - (depth * exploration_depth_decay)) * prior * sqrt(parent_N) / (1 + N);
      pv2 += " (" + std::to_string(N) + ", " + std::to_string(llround(W)) + ", " + std::to_string(cp2) + ", " + std::to_string(Q) + " + " + std::to_string(U) + " = " + std::to_string(Q + U) + ")";
      children2 = child->children.load(std::memory_order_relaxed);
      num_children = child->num_children.load(std::memory_order_relaxed);
    } //end of while (num_children > 0 && pv.size() < maxLen)
    pvs.push_back({cp, pv});
    log_file("select_best_moves() debug: PV[%d] %s\n", i, pv2.c_str());
    //std::sort(pvs.begin(), pvs.end(), std::greater<>()); //this is incorrect because short pv have less accurate score
  } // end for (int i = 0; i < multiPV; i++)
  return multiPV;
}

//called from mcts_search() in selection phase
//returns child index with the best PUCT value  
int select_best_child(MCTSNode * parent, const int depth) {
  int num_children = parent->num_children.load(std::memory_order_acquire);
  Edge * children = parent->children.load(std::memory_order_acquire);
  double best_score = -INFINITY;
  int selected; //it will be initialized in the for loop
  // Dynamic Exploration Constant - linear decay with depth
  // setting exploration_depth_decay to 0 will make exploration constant static = exploration_max
  // Decay: Start at exploration_constant, then for example drop by 0.05 - 0.1 per ply, floor at 0.25 - all tunable
  double C = std::max(exploration_min, exploration_max - (depth * exploration_depth_decay));
  //or square root decay with depth
  //double C = std::max(exploration_min, exploration_max - (sqrt(static_cast<double>(depth)) * exploration_depth_decay));
  
  // OPTIONAL: Bonus for Root Node (Depth 0) to ensure wide scanning
  //if (depth == 0) C = 2.0;
  const uint64_t parentN = parent->N.load(std::memory_order_acquire);
  const double   parentW = parent->W.load(std::memory_order_relaxed);
  const double   parentQ = parentN ? parentW / parentN : 0.0;
  for (int i = 0; i < num_children; i++) {
    double P = children[i].P.load(std::memory_order_relaxed);
    MCTSNode * child = children[i].child.load(std::memory_order_relaxed);
    uint64_t N = child->N.load(std::memory_order_relaxed);
    double W = -child->W.load(std::memory_order_relaxed); //parent perspective
    //First-play urgency. In creatica-shared-root every child was born with N=1 and a real
    //evaluation, so this branch almost never fired and Q=0 for an unvisited child was
    //harmless. Policy expansion creates children unevaluated, so N=0 is now the common
    //case and this value decides the whole search order. Q=0 means "drawn", which is wildly
    //optimistic in a lost position and pessimistic in a won one. Start an unvisited child
    //slightly below its parent instead, which is the honest guess before any evidence.
    double Q = N ? W / N : (parentQ - fpu_reduction);
    //PUCT formula
    double score = Q + C * P * sqrt(static_cast<double>(parentN)) / (1.0 + N);
    if (score > best_score) {
      best_score = score;
      selected = i;
    }
  }
  // Apply virtual loss to selected child to avoid contention among threads for the same node
  //children = parent.children.load(std::memory_order_acquire);
  MCTSNode * child = children[selected].child.load(std::memory_order_acquire);
  child->N.fetch_add(1, std::memory_order_release);
  child->W.fetch_sub(virtual_loss, std::memory_order_release);
  return selected;
}

void get_prob(std::vector<std::tuple<double, int, int, int, uint64_t>>& move_evals) { //, double prob_mass) {
    //size_t n = move_evals.size();
    //if (n == 0) return 0;
    // Loop 1: Find max for stability
    double max_val = -std::numeric_limits<double>::infinity();
    for (const auto& ev : move_evals) {
        if (std::get<0>(ev) > max_val) max_val = std::get<0>(ev);
    }
    // Loop 2: Compute total sum of exp(shifted)
    double total = 0.0;
    for (const auto& ev : move_evals) {
        total += std::exp((std::get<0>(ev) - max_val)/temperature);
    }
    if (total == 0.0) {  // Rare case: all -inf or underflow
        double uniform = 1.0 / move_evals.size();
        for (auto& ev : move_evals) std::get<0>(ev) = uniform;
        return;
    }
    // Loop 3: Normalize to probs, accumulate cum_mass
    //double cum_mass = 0.0;
    //int effective = 0;
    for (auto& ev : move_evals) {
        std::get<0>(ev) = std::exp((std::get<0>(ev) - max_val)/temperature) / total;
        //cum_mass += std::get<0>(ev);
        //++effective;
        //if (cum_mass >= prob_mass) break;
    }
    //return effective;
}

//called from make_move() and run_MCTS()
//calls compute_move_evals(), make_child() and expand_node()
//returns the result in pawns from the temp_board.sideToMove perspective
double process_check(Board& temp_board, const ZobristHash& board_hash, NNUEContext& ctx, const std::unordered_set<uint64_t>& pos_history, int iter) {
  MCTSNode * node = make_child(board_hash.hash, NO_MATE_SCORE, -1);
  int stored_cp = node->cp.load(std::memory_order_relaxed);
  if (stored_cp == NO_MATE_SCORE) { //make_child() returned new node without a parent, let's update its cp and expand it
    /*if (iter >= 1) {
      char fen[MAX_FEN_STRING_LEN];
      printf("process_check() debug: iter %d, fen %s\n", iter, board2fen(temp_board, fen));
    }*/
    return eval_and_expand(node, temp_board, board_hash, ctx, pos_history, iter + 1); 
  } else return stored_cp * 0.01;
}

//called from make_move()
//calls isCheckMateStaleMate(), which checks for terminal state if any
//also calls evaluate_nnue() and process_check() if there is check
//returns a pair of position evaluation in pawns from chess_board.sideToMove perspective and terminal enum: -1 (check), 1 (mate), 2 (stalemate), 3 (repetition), 0 (non-terminal)
std::pair<double, int> position_eval(Board& chess_board, const ZobristHash& board_hash, NNUEContext& ctx, const std::unordered_set<uint64_t>& pos_history, int iter) {
  double res;
  int terminal;
  //char fen[MAX_FEN_STRING_LEN];
  //printf("position_eval() debug: iter %d, fen %s\n", iter, board2fen(chess_board, fen));
	isCheckMateStaleMate(chess_board);
  if (chess_board.isMate) {
    res = -MATE_SCORE * 0.01 + iter; //chess_board.sideToMove loses
    return std::make_pair(res, 1);
  }
  else if (chess_board.isStaleMate) {
    res = 0.0;
    return std::make_pair(res, 2);
  } else if (chess_board.isCheck) {
    terminal = -1;
  } else terminal = 0;
	const int pieceCount = bitCount(chess_board.side[ColorWhite] | chess_board.side[ColorBlack]);
	if (pieceCount > TB_LARGEST || chess_board.castlingRights) {
    //if (!terminal)
    //  res = evaluate_nnue(chess_board, ctx); //evaluate_nnue() returns result in pawns (not centipawns!)
	  //else if (terminal == -1) res = process_check(chess_board, board_hash, ctx, pos_history, iter);
	  if (terminal == -1) res = process_check(chess_board, board_hash, ctx, pos_history, iter);
    else res = evaluate_nnue(chess_board, ctx); //evaluate_nnue() returns result in pawns (not centipawns!)
  } else { //pieceCount <= TB_LARGEST, etc
    unsigned int ep = legalEnPassantMove(chess_board);
    const unsigned int wdl = tb_probe_wdl(chess_board.side[ColorWhite], chess_board.side[ColorBlack], chess_board.pieceTypes[King - 1], chess_board.pieceTypes[Queen - 1], chess_board.pieceTypes[Rook - 1], chess_board.pieceTypes[Bishop - 1], chess_board.pieceTypes[Knight - 1], chess_board.pieceTypes[Pawn - 1], 0, 0, ep == SquareNone ? 0 : ep, chess_board.sideToMove == ColorWhite ? 1 : 0);
    if (wdl == TB_RESULT_FAILED) {
      char fen[MAX_FEN_STRING_LEN];
      log_file("error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, occupations %u, ep %u, halfmoveClock %u, whiteToMove %u, whites %llu, blacks %llu, kings %llu, queens %llu, rooks %llu, bishops %llu, knights %llu, pawns %llu, fen %s, err %s\n", TB_LARGEST, pieceCount, ep, chess_board.halfmoveClock, chess_board.sideToMove == ColorWhite ? 1 : 0, chess_board.side[ColorWhite], chess_board.side[ColorBlack], chess_board.pieceTypes[King - 1], chess_board.pieceTypes[Queen - 1], chess_board.pieceTypes[Rook - 1], chess_board.pieceTypes[Bishop - 1], chess_board.pieceTypes[Knight - 1], chess_board.pieceTypes[Pawn - 1], board2fen(chess_board, fen), strerror(errno));
      //if (!terminal)
      //  res = evaluate_nnue(chess_board, ctx); //evaluate_nnue() returns result in pawns (not centipawns!)
  	  //else if (terminal == -1) res = process_check(chess_board, board_hash, ctx, pos_history, iter);
  	  if (terminal == -1) res = process_check(chess_board, board_hash, ctx, pos_history, iter);
      else res = evaluate_nnue(chess_board, ctx); //evaluate_nnue() returns result in pawns (not centipawns!)
    } else { //tb_probe_wdl() succeeded
      //0 - loss, 4 - win, 1..3 - draw
      if (wdl == 4) res = MATE_SCORE * 0.001;
      else if (wdl == 0) res = -MATE_SCORE * 0.001;
      else res = 0.0;
      tbhits.fetch_add(1, std::memory_order_relaxed);
    }
  } //end of else (pieceCount <= TB_LARGEST)
  return std::make_pair(res, terminal);
}

//called from compute_move_evals()
//makes a move and calls position_eval(), undo the move
//checks for repetition
//returns a pair of eval result in pawns from the perspective of chess_board.sideToMove and terminal state enum
std::pair<double, int> make_move(Board& chess_board, const ZobristHash& board_hash, Move& move, NNUEContext& ctx, uint64_t& child_hash, const std::unordered_set<uint64_t>& pos_history, int iter) {
  ZobristHash tmp_hash = board_hash;
  StateInfo state = {}; //keep track of num_moves and isCheck but not isMate or isStaleMate
  auto [dp, dts] = accumulator_stack_push(ctx); //for incremental NNUE evaluation, which is one order faster than full evaluation; both dp and dts are references!
  updateHash(tmp_hash, chess_board, move, do_move_dp(chess_board, move, state, dp, dts), z); //do the move, update the hash
  child_hash = tmp_hash.hash;
  if (pos_history.count(child_hash) > 0) { //what if this position is terminal?? I suppose repetition cannot be terminal
    //int terminal = 3;
    //so in case of repetition, the leaf node may be in check because we do not expand it
    //if (getCheckers(chess_board, kingSquare(chess_board, static_cast<Color>(chess_board.sideToMove)))) terminal = -1;
    undo_move(chess_board, move, state);
    accumulator_stack_pop(ctx); //this early return used to leak one stack level per repetition
    return std::make_pair(0.0, 3); //repetition
  }
  auto [res, terminal] = position_eval(chess_board, tmp_hash, ctx, pos_history, iter);
  //if position_eval() returns terminal state setting chess_board.isMate or .isStaleMate, then undo_move() will not restore these fields until kingMoves() is called
  undo_move(chess_board, move, state);
  accumulator_stack_pop(ctx);
  return std::make_pair(-res, terminal);
}

//Like make_move(), but never evaluates the child.
//
//The prior comes from the policy net, so the only things this needs from a child are its
//hash and whether it is terminal -- a do_move, a hash update and a mate/stalemate test.
//No accumulator push, no NNUE evaluation. That is the whole saving: an expansion goes from
//around 29 evaluations to one forward pass over the parent.
//
//Returns the child's cp in the CHILD's own perspective, or NO_MATE_SCORE to mean "not
//evaluated", plus the same terminal enum position_eval() uses.
std::pair<int, int> make_move_policy(Board& chess_board, const ZobristHash& board_hash, Move& move,
                                     uint64_t& child_hash,
                                     const std::unordered_set<uint64_t>& pos_history, int iter) {
  ZobristHash tmp_hash = board_hash;
  StateInfo state = {};
  //do_move(), not do_move_dp(): with no evaluation of the child there is no accumulator to
  //update, and leaving the stack alone keeps the parent's features -- which the policy net
  //is reading -- valid across the whole loop.
  updateHash(tmp_hash, chess_board, move, do_move(chess_board, move, state), z);
  child_hash = tmp_hash.hash;
  if (pos_history.count(child_hash) > 0) {
    undo_move(chess_board, move, state);
    return std::make_pair(0, 3); //repetition
  }
  isCheckMateStaleMate(chess_board);
  int cp = NO_MATE_SCORE, terminal = 0;
  if (chess_board.isMate) {
    cp = static_cast<int>(-MATE_SCORE + iter * 100); //the child's side to move is mated
    terminal = 1;
  } else if (chess_board.isStaleMate) {
    cp = 0;
    terminal = 2;
  } else {
    if (chess_board.isCheck) terminal = -1;
    //Tablebase positions are exact and cost one probe, so they are still worth taking here
    //rather than deferring to the child's own first visit.
    const int pieceCount = bitCount(chess_board.side[ColorWhite] | chess_board.side[ColorBlack]);
    if (pieceCount <= TB_LARGEST && !chess_board.castlingRights) {
      unsigned int ep = legalEnPassantMove(chess_board);
      const unsigned int wdl = tb_probe_wdl(chess_board.side[ColorWhite], chess_board.side[ColorBlack], chess_board.pieceTypes[King - 1], chess_board.pieceTypes[Queen - 1], chess_board.pieceTypes[Rook - 1], chess_board.pieceTypes[Bishop - 1], chess_board.pieceTypes[Knight - 1], chess_board.pieceTypes[Pawn - 1], 0, 0, ep == SquareNone ? 0 : ep, chess_board.sideToMove == ColorWhite ? 1 : 0);
      if (wdl != TB_RESULT_FAILED) {
        if (wdl == 4)      cp = static_cast<int>(MATE_SCORE * 0.1);
        else if (wdl == 0) cp = static_cast<int>(-MATE_SCORE * 0.1);
        else               cp = 0;
        tbhits.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  undo_move(chess_board, move, state);
  return std::make_pair(cp, terminal);
}

//generate moves and calls make_move() for each move, which calls position_eval(), which calls isCheckMateStaleMate() and if check, calls process_check(), which calls this function again forming recursion for checks; otherwise, position_eval() calls evaluate_nnue(), which cannot be called in check
//computes and sorts move evaluations and calls expand_node()
//called from mcts_search(), set_root() and process_check()
//returns eval result in pawns from chess_board.sideToMove perspective
double eval_and_expand(MCTSNode * node, Board& chess_board, const ZobristHash& board_hash, NNUEContext& ctx, const std::unordered_set<uint64_t>& pos_history, int iter) {
  std::vector<std::tuple<double, int, int, int, uint64_t>> move_evals; //prior, move_idx, child cp, terminal, hash
  Move move;
  auto [king_moves, pinned, pinning, checkers, kingSquare] = kingMoves(chess_board);

  //In check the incumbent path is kept. evaluate_nnue() cannot be called on a position in
  //check at all, so a checked node's value has to come from the 1-ply minimax over its
  //children -- and that needs the children evaluated. Checks are a small share of nodes, so
  //this costs little and preserves the check extension exactly as it was.
  const bool use_policy = policy_enabled && !checkers;
  float pctx[POLICY_MAX_H2];
  bool  flip = false;
  if (use_policy) {
    unsigned char feat[POLICY_MAX_IN];
    //Free at a search node: the accumulator this reads is the one do_move_dp() has been
    //maintaining incrementally down the path.
    nnue_features(chess_board, ctx, feat);
    policy_context(policy_net, feat, pctx);
    flip = (chess_board.sideToMove == ColorBlack);
  }

  //Cancels get_prob()'s own division so the head's logits reach the softmax at the scale
  //they were trained on.
  const double pscale = temperature / policy_temperature;

  //The gate needs the whole move list before any of it is evaluated, so when it is active
  //emit() only collects. At the default 1.0 it evaluates inline exactly as before, and no
  //vector is allocated -- the no-gate path is unchanged.
  const bool gating = use_policy && probability_mass < 1.0;
  std::vector<Move> legal;
  if (gating) legal.reserve(64);

  auto evaluate = [&](Move& m) {
    uint64_t child_hash = 0;
    if (use_policy && policy_mode == POLICY_FULL) {
      auto [cp, terminal] = make_move_policy(chess_board, board_hash, m, child_hash, pos_history, iter);
      move_evals.push_back({policy_score(policy_net, pctx, m.src, m.dst, flip) * pscale,
                            (m.promoType << 12) | (m.src << 6) | m.dst, cp, terminal, child_hash});
    } else {
      auto [res, terminal] = make_move(chess_board, board_hash, m, ctx, child_hash, pos_history, iter);
      //get_prob() divides by `temperature`, so push temperature * (the logit we want).
      const double prior = use_policy
          ? temperature * policy_blend_scale
                * (policy_blend * policy_score(policy_net, pctx, m.src, m.dst, flip) / policy_temperature
                   + (1.0 - policy_blend) * res / temperature)
          : res;
      move_evals.push_back({prior, (m.promoType << 12) | (m.src << 6) | m.dst,
                            static_cast<int>(-res * 100), terminal, child_hash});
    }
  };
  auto emit = [&](Move& m) { if (gating) legal.push_back(m); else evaluate(m); };

  move.src = kingSquare;
  move.promoType = PieceTypeNone;
  while (king_moves) {
    move.dst = popLSB(king_moves);
    emit(move);
  }
  if (bitCount(checkers) <= 1) {
    auto [check_mask, ep_mask] = checkers ? checkMask(chess_board, kingSquare, checkers) : std::make_pair(0xffffffffffffffffULL, 0ULL);
    for (PieceType pt = Queen; pt >= Pawn; --pt) {
      uint64_t occupations = chess_board.side[chess_board.sideToMove] & chess_board.pieceTypes[pt - 1];
      while (occupations) {
        move.src = popLSB(occupations);
        uint64_t moves = piece_moves(chess_board, pt, move.src, kingSquare, pinned, pinning, check_mask, ep_mask);
        while (moves) {
          move.dst = popLSB(moves);
          PieceType startPiece = PieceTypeNone, endPiece = PieceTypeNone;
          if (promoMove(chess_board, move)) {
            startPiece = Knight;
            endPiece = Queen;
          }
          for (move.promoType = startPiece; move.promoType <= endPiece; ++move.promoType) {
            emit(move);
          }
          move.promoType = PieceTypeNone;
        }
      }
    }
  }
  //The gate. Runs between enumeration and evaluation, so a dropped move costs nothing.
  //
  //Only on the policy path: with the policy off, or in check, the prior IS the evaluation and
  //there is nothing to rank the moves by until the work has already been done. Those paths
  //keep every move, exactly as before.
  if (gating && legal.size() > 1) {
    const size_t n = legal.size();
    std::vector<double> logit(n);
    double mx = -1e300;
    for (size_t i = 0; i < n; ++i) {
      logit[i] = policy_score(policy_net, pctx, legal[i].src, legal[i].dst, flip) / policy_temperature;
      if (logit[i] > mx) mx = logit[i];
    }
    //Softmax over the policy logits alone. Shifted by the maximum before exponentiating, so a
    //large logit cannot overflow to infinity and take the normaliser with it.
    std::vector<std::pair<double, size_t>> prob(n);
    double tot = 0.0;
    for (size_t i = 0; i < n; ++i) {
      const double e = std::exp(logit[i] - mx);
      prob[i] = {e, i};
      tot += e;
    }
    std::sort(prob.begin(), prob.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    double cum = 0.0;
    size_t keep = 0;
    while (keep < n) {
      cum += prob[keep].first / tot;
      ++keep;                       //counted after adding, so at least one move always survives
      if (cum >= probability_mass) break;
    }
    if (keep < n) {
      std::vector<Move> kept;
      kept.reserve(keep);
      for (size_t i = 0; i < keep; ++i) kept.push_back(legal[prob[i].second]);
      legal.swap(kept);
    }
  }
  if (gating)
    for (Move& m : legal) evaluate(m);

  if (chess_board.num_moves == 0) {
    if (checkers) {
      node->cp.store(-MATE_SCORE, std::memory_order_relaxed);
      node->terminal.store(1, std::memory_order_relaxed); //mate
      return -MATE_SCORE * 0.01 + iter;
    } else {
      node->cp.store(0, std::memory_order_relaxed);
      node->terminal.store(2, std::memory_order_relaxed); //stalemate
      return 0;
    }
  }
  // Sort by field 0 descending. On the incumbent path that is the child evaluation, which
  // process_check() and mcts_search() rely on to read the best result off the front. On the
  // policy path it is the prior, and the node's own value is computed below instead.
  std::sort(move_evals.begin(), move_evals.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b);});
  get_prob(move_evals);
  int scorecp;
  if ((!use_policy || policy_mode == POLICY_PRIOR) && node_minimax) {
    //The 1-ply minimax over the children, computed explicitly rather than read off the
    //front of the vector: with policy priors the sort is by prior, so move_evals[0] is the
    //most likely move and not the best-evaluated one.
    scorecp = -std::get<2>(move_evals[0]);
    for (const auto& ev : move_evals) scorecp = std::max(scorecp, -std::get<2>(ev));
  } else if (checkers) {
    //evaluate_nnue() cannot be called in check, so a checked node keeps the minimax value
    //whatever node_minimax says -- there is nothing else available.
    scorecp = -std::get<2>(move_evals[0]);
    for (const auto& ev : move_evals) scorecp = std::max(scorecp, -std::get<2>(ev));
  } else {
    scorecp = static_cast<int>(evaluate_nnue(chess_board, ctx) * 100.0);
    // Mates and tablebase results found while making the children are exact, and a forced
    // mate or a tablebase win one move away is worth at least the static score -- we are
    // never obliged to play it, so max() is the right combination. Without this the policy
    // path would happily expand a node with mate in 1 and score it statically.
    for (const auto& ev : move_evals) {
      const int ccp = std::get<2>(ev);
      const int t   = std::get<3>(ev);
      if (t == 1 || (t == 0 && ccp != NO_MATE_SCORE)) scorecp = std::max(scorecp, -ccp);
    }
  }
  node->cp.store(scorecp, std::memory_order_relaxed); //look-ahead update
  expand_node(node, move_evals);
  return scorecp * 0.01;
}


/*
Overview of the MCTS Logic

MCTS implementation follows the four core phases:

  Selection: Starting from the root, traverse the tree using the PUCT (Predictor + Upper Confidence Bound applied to Trees) formula to select the most promising child node until reaching a leaf or terminal position.

  Expansion: At a leaf node, generate child nodes based on legal moves, using NNUE to assign prior probabilities.

  Evaluation: Evaluate terminal positions (checkmate/stalemate) directly or use NNUE for non-terminal positions, mapping scores to [-1, 1]

  Backpropagation: Update visit counts (N) and total value (W) from the leaf back to the root, alternating the sign of the result to reflect perspective changes.
*/

//called from thread_search()
//calls compute_move_evals() and expand_node()
void mcts_search(ThreadParams& params, NNUEContext& ctx) {
  //One simulation per call, counted at entry so the early return below is still counted -- it
  //consumed time and must appear in the denominator of nps.
  search_simulations.fetch_add(1, std::memory_order_relaxed);
  std::vector<MCTSNode *> path;  // Track the path from root to leaf
  path.reserve(chessEngine.depth);
  int terminal = 0;
  params.seldepth = 0;
  MCTSNode * node = search.root;
  //start from the same initial position given by board (at the root node, i.e. at the top of the tree - the up side down tree)
  //copy the board and the hash to preserve it for subsequent iterations
  Board sim_board = board;
  ZobristHash sim_zh = zh;
  // Selection
  //iterate down the tree updating sim_board by initiating and making moves
  //thread-local map to prevent repetition cycles - it should be global I think but thread-safety may be a problem
  std::unordered_set<uint64_t> pos_history;
  while (node->num_children.load(std::memory_order_relaxed) > 0) { //traversal stops at a leaf or at repetition (mate or stalemate node should not have children)
    //return child node index with the best score using PUCT (Predictor + Upper Confidence Bound)
    //it also adds virtual loss to the node to reduce contention for the same node in multi-threaded engine
    int idx = select_best_child(node, params.seldepth);
    Edge * children = node->children.load(std::memory_order_acquire);
    int move_idx = children[idx].move.load(std::memory_order_relaxed);
    path.push_back(node);  // Add parent node to path
    //continue iterating down the tree by getting next node until no more children
    node = children[idx].child.load(std::memory_order_acquire);
    //init and take edge's move that leads to the child node
    terminal = node->terminal.load(std::memory_order_acquire);
    if (terminal == 3) break; //repetition
    Move move;
    move.dst = (Square)(move_idx & 63);
    move.src = (Square)((move_idx >> 6) & 63);
    move.promoType = (PieceType)((move_idx >> 12) & 7);
    //update Zobrist hash (it is needed so that we can call updateHash() later instead of getHash()
    //char fen[MAX_FEN_STRING_LEN];
    //printf("mcts_search(%d) debug: depth %d, fen %s move %s%s%c\n", params.thread_id, params.seldepth, board2fen(sim_board, fen), square[move.src], square[move.dst], uciPromoLetter[move.promoType]);
		updateHash(sim_zh, sim_board, move, ff_move(sim_board, move), z);
    params.seldepth++;
    int global_count = position_history.count(sim_zh.hash); //actual positions that have occured in the game
    int path_count = pos_history.count(sim_zh.hash); //simulated positions ahead of the current one
    if (path_count == 0) pos_history.insert(sim_zh.hash);
    bool repetition = (global_count + path_count >= 1); 
    if (repetition) {
      //The LOCAL terminal is what this simulation needs: it breaks the descent and the
      //backpropagation below scores it 0, which is correct for this path.
      //
      //It must not be written to the node. Mate and stalemate are properties of a POSITION and
      //are safe to cache on a node shared across the transposition DAG; repetition is a
      //property of the PATH taken to reach it, decided from position_history plus this
      //simulation's own pos_history. Stamping terminal=3 and cp=0 on the node made a
      //path-local verdict permanent -- nothing ever cleared it -- so every later visit from any
      //path saw a dead draw with its whole subtree cut off, and the reported cp became 0
      //instead of the evaluation. cleanup() hid this by destroying the tree every move; with
      //gc() and tree reuse it would persist for the rest of the game.
      terminal = 3;
      break;
    }
  } //end of while(node.num_children > 0) loop
  path.push_back(node);  // Add leaf node to path - sim_board corresponds to this node!
  //Here we are at the bottom of the tree, i.e. at a leaf or at the terminal node (mate, stalemate)
  // Evaluation - the node is already evaluated during previous expansion!
  // We could actually improve the eval by using move_evals calculated later in the code for the children nodes before expansion for evaluating its parent (this node), kind of look ahead eval
  int scorecp = 0;
  double result = 0.0;
  if (terminal <= 0) {
    //A full tree stops the search GROWING, not the search THINKING.
    //
    //The workers' loop used to carry `hash_full < 1000` as a termination condition, so when the
    //tree filled every worker exited and the engine played whatever it had -- in a live blitz
    //game it spent 2 s on a move where its neighbours took 4-6. It did not merely stop
    //expanding, it stopped playing chess. Tree reuse exposed this: before it, the tree was
    //rebuilt every move and never came near the ceiling; with reuse it grows monotonically,
    //because a good prior concentrates the search on the move it then plays and almost nothing
    //ever becomes garbage for the collector to reclaim.
    //
    //Taking the already-evaluated branch below instead keeps visits and backpropagation going
    //over the tree that exists. That is degraded -- no new nodes -- but it still sharpens the
    //statistics of the moves already under consideration, which is enormously better than
    //abandoning the search.
    const bool tree_full = hash_full.load(std::memory_order_relaxed) >= 1000;
    if (!tree_full && node->expanding.exchange(1, std::memory_order_acquire) == 0) { //the leaf node is gated only for expansion
                                //nodes traversed in the selection phase are not leaf nodes, i.e. nodes without children
                                //if the gate is already taken, another thread is expanding this node right now
                                //(unlike try_lock(), an atomic exchange cannot fail spuriously or be blocked by readers)
      if (terminal == -1) { //node in check
        log_file("mcts_search() warning: leaf node in check!\n"); //leaf node should not be in check because of process_check()
      } else {
        evaluate_nnue(sim_board, ctx); //the leaf node is evaluated but we call it to make subsequent evals faster
      }
      result = eval_and_expand(node, sim_board, sim_zh, ctx, pos_history, 0);
      result = tanh(result / eval_scale);
      node->expanding.store(0, std::memory_order_release);
    } //end of if (node.expanding.exchange(1, acquire) == 0)
    else { //unable to lock the node, see if it's already evaluated
      scorecp = node->cp.load(std::memory_order_relaxed);
      if (scorecp == NO_MATE_SCORE) { //node has not been evaluated yet, return without a backprop (a bit of a waste)
        printf("mcts_search() warning: unevaluated child - skipping expansion\n");
        for (size_t j = 1; j < path.size(); ++j) {  // From first child to leaf reverse virtual loss
          MCTSNode * nd = path[j];
          nd->N.fetch_sub(1, std::memory_order_relaxed);
          nd->W.fetch_add(virtual_loss, std::memory_order_relaxed);
        }      
        return;
      }
      result = tanh(scorecp * 0.01 / eval_scale);
    }
  } //end of else if (terminal <= 0) - check or non-terminal
  else { //terminal (mate, stalemate, repetition)
    if (terminal == 1) { //mate
      scorecp = node->cp.load(std::memory_order_relaxed);
      result = scorecp == -MATE_SCORE ? -1 : 1;
    } //for stalemate the result is initiated to 0, so no need for this check    
  }
  // Backpropagation: update node visits and results regardless of whether we expand the node or not
  for (auto n = path.rbegin(); n != path.rend(); ++n) {
    node = *n;
    node->N.fetch_add(1, std::memory_order_relaxed);
    node->W.fetch_add(result, std::memory_order_relaxed);
    result = -result;
  }
  // Revert virtual loss for the selected path (skip root, as no loss was applied to it) regardless of expansion
  // because virtual loss was applied in select_best_child() which is called in the selection phase
  for (size_t j = 1; j < path.size(); ++j) {  // From first child to leaf
    MCTSNode * nd = path[j];
    nd->N.fetch_sub(1, std::memory_order_relaxed);
    nd->W.fetch_add(virtual_loss, std::memory_order_relaxed);
  }      
}

void uci_output_thread() {
  auto iter_start = std::chrono::steady_clock::now();

  while (!stopFlag.load(std::memory_order_relaxed) && !search_done.load(std::memory_order_relaxed)) {
    std::unique_lock<std::mutex> lk(search_done_mtx);
    cv_search_done.wait_for(lk, std::chrono::milliseconds(1000), []{return search_done.load(std::memory_order_relaxed);});
    // Calculate nodes (total simulations)
    MCTSNode * current_node = search.root;
    uint64_t nodes = search_simulations.load(std::memory_order_relaxed);
    // Get elapsed time
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
    // Compute NPS
    double nps = elapsed > 0 ? nodes / elapsed : 0;
    //depth should be calculated by traversing the most visited nodes similar to select_best_moves()      
    int d = 0;
    std::unordered_set<uint64_t> visited; //hash
    std::vector<std::pair<uint64_t, int>> visits; //N, child_idx
    int num_root_children = current_node->num_children.load(std::memory_order_relaxed);
    while (current_node && d < MAX_DEPTH) {
      uint64_t current_hash = current_node->hash.load(std::memory_order_relaxed);
      if (visited.find(current_hash) != visited.end()) {
          log_file("uci_output_thread() debug: cycle detected at depth %d, breaking loop\n", d);
          break;
      }
      visited.insert(current_hash);        
      uint64_t N = 0;
      int next_idx = -1;
      int num_children = current_node->num_children.load(std::memory_order_acquire);
      Edge * children = current_node->children.load(std::memory_order_acquire);
      for (int i = 0; i < num_children; i++) {
        MCTSNode * child = children[i].child.load(std::memory_order_acquire);
        uint64_t n = child->N.load(std::memory_order_relaxed);
        if (current_node == search.root) visits.push_back({n, i});
        if (n > N) {
          N = n;
          next_idx = i;
        } 
      }
      if (next_idx < 0) break; //meaning current_node is a leaf node, i.e. no children
      current_node = children[next_idx].child.load(std::memory_order_acquire);
      d++;
    }
    depth.store(d, std::memory_order_relaxed);
    std::shared_lock lock(map_mutex);
    size_t unique_nodes = search.tree.size();
    lock.unlock();
    size_t total_memory = unique_nodes * (sizeof(MCTSNode) + 24) + total_children.load(std::memory_order_relaxed) * sizeof(Edge);
    size_t max_capacity = chessEngine.optionSpin[Hash].value * 1024 * 1024;  // MB to bytes
    int hashfull = max_capacity ? (total_memory * 1000) / max_capacity : 0;
    if (hashfull > 1000) hashfull = 1000;  // Cap at 1000 per UCI spec
    hash_full.store(hashfull, std::memory_order_relaxed);

    std::sort(visits.begin(), visits.end(), std::greater<>());
    int multiPV = std::min<int>(num_root_children, (int)chessEngine.optionSpin[MultiPV].value);
    Edge * children = search.root->children.load(std::memory_order_acquire);
    for (int i = 0; i < multiPV; i++) {
      const int move_idx = children[visits[i].second].move.load(std::memory_order_relaxed);
      char uci_move[6];
      idx2uci(move_idx, uci_move);
      MCTSNode * child = children[visits[i].second].child.load(std::memory_order_acquire);
      log_file("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", d, seldepth.load(std::memory_order_relaxed), i + 1, -child->cp.load(std::memory_order_relaxed), nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, uci_move);
      print("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", d, seldepth.load(std::memory_order_relaxed), i + 1, -child->cp.load(std::memory_order_relaxed), nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, uci_move);
    }
  }
}

void runMCTS(NNUEContext& ctx) {
  //Captured once, at the top. A ponder search is stopped from another thread, so the flag can
  //change underneath us -- and the collection decision before the search and the one after
  //bestmove must agree about which kind of search this was.
  const bool pondering = chessEngine.ponder;
  double elapsed = 0.0;
  size_t unique_nodes = 0;
  uint64_t nodes = 0;
  int hashfull = 0;
  std::vector<std::pair<int, std::string>> pvs;
  int multiPV = 1;

	isCheckMateStaleMate(board); //it calculates board.num_moves as well as part of all legal moves generation
  if (board.num_moves > 1) { //run MCTS using multiple threads
    tbhits.store(0, std::memory_order_relaxed);
    //it seems there rarely is some kind of contamination or corruption of the tree
    //so let's try cleanup() instead of gc() if UCI Ponder option is false
    //avoid using Ponder option, sometimes called "permanent brain", i.e. thinking during opponent's time
    //Ponder implies reuse: a pondered subtree that gets destroyed before the next search was
    //wasted effort. Reuse no longer implies Ponder.
    //Stop and join any collection still running from the last move. This is the "go" edge of
    //Arkadi's signal pair: bestmove starts the collector, the next search stops it. Whatever it
    //managed to free is kept; whatever it did not is collected next time.
    gc_join();
    const auto collect_start = std::chrono::steady_clock::now();
    //Ponder implies reuse: a pondered subtree destroyed before the next search was wasted.
    const bool reusing_tree = reuse_tree || chessEngine.optionCheck[Ponder].value;
    bool collected = !reusing_tree;   //cleanup() always "collects": it empties the tree entirely
    if (reusing_tree) {
      set_root(ctx);      
      //Normally: only when the tree is actually filling up. Most moves skip the walk entirely,
      //because the collection after bestmove keeps occupancy below the threshold. This is the
      //safety net for what that cannot cover -- a position jump, a takeback, or a root that is
      //not a descendant of the previous one.
      //
      //While PONDERING, always. Pondering removes the idle window the background collector was
      //designed around: the next "go" arrives immediately after bestmove, aborts the collector
      //mid-mark, and nothing is ever freed -- so collection would fall entirely to the
      //threshold, firing as one large synchronous stall at an arbitrary moment. Collecting
      //here instead costs the opponent's time, which is time already being spent on a
      //speculative search, and keeps each collection small.
      collected = pondering || tree_occupancy() >= gc_threshold;
      if (collected) gc();
    } else {
      //Usually a no-op: the background wipe started after the last bestmove has already
      //emptied the tree, and gc_join() above waited for it. This remains as the fallback
      //for the first move of a game and for any move where the wipe was interrupted.
      cleanup_locked();
      set_root(ctx);      
    }
    //Both paths, so a run with ValidateTree on says whether reuse specifically is what breaks
    //the invariants, rather than leaving it ambiguous.
    validate_tree(collected ? (reusing_tree ? "after gc" : "after cleanup")
                            : "before search (collection skipped)", collected);
    //What the collection actually cost. It runs inline, before the search starts, so it is dead
    //time on the clock; logging it turns "gc feels slow" into a number.
    log_file("info string %s took %.1f ms, %zu nodes\n", reusing_tree ? "gc" : "cleanup",
             std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - collect_start).count(),
             search.tree.size());
    search_simulations.store(0, std::memory_order_relaxed);
    //Charge the collection to this move's budget instead of adding to it. iter_start is taken
    //AFTER all of the above, so the search used to run its full allocation on top of however
    //long the collection took: 442 ms of collection against a 1200 ms movetime meant a 1642 ms
    //move, and the overshoot grew with the tree. That is a way to lose on time.
    const double collect_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - collect_start).count();
    const double search_budget = std::max(1.0, timeAllocated - collect_ms);
    int num_threads = chessEngine.optionSpin[Threads].value;
    if (pool_threads.size() != num_threads) {
         log_file("Warning: Pool size mismatch: current threads %d != configured %d. Re-initializing...\n", pool_threads.size(), num_threads);
         init_thread_pool(num_threads);
    }
    if (!chessEngine.depth) chessEngine.depth = MAX_DEPTH;
    depth.store(0, std::memory_order_relaxed);
    seldepth.store(0, std::memory_order_relaxed);
    auto iter_start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        pool_params[i].thread_id = i;
        pool_params[i].time_alloc = search_budget;
        pool_params[i].seldepth = 0;
    }
    //The predicate must change while pool_mutex is HELD, or the wake can be lost. A worker in
    //pool_cv.wait() evaluates its predicate holding the lock and only then atomically releases
    //and blocks; if these stores land in that window and the notify fires before the worker is
    //actually blocked, the notification goes nowhere and the worker sleeps through the search.
    {
      std::lock_guard<std::mutex> lk(pool_mutex);
      active_workers.store(num_threads); // Register how many we expect to run
      pool_generation.fetch_add(1);      // Increment generation ID
    }
    pool_cv.notify_all();              // SIGNAL: "Start Engines!"
        
    if (chessEngine.optionCheck[IntermittentInfoLines].value && !chessEngine.ponder) {
      search_done.store(false);
      std::thread output_thread(uci_output_thread);
      // Wait until all workers are done
      while (active_workers.load()) {
        std::unique_lock<std::mutex> lock(pool_mutex);
        pool_done_cv.wait(lock, [] { return active_workers.load() == 0; });
        lock.unlock();
      }
      // Signal output thread to stop
      {
        std::lock_guard<std::mutex> lk(search_done_mtx);
        search_done.store(true);
      }
      cv_search_done.notify_one();
      output_thread.join();
    } else {
      // Just wait for workers
      while (active_workers.load()) {
        std::unique_lock<std::mutex> lock(pool_mutex);
        pool_done_cv.wait(lock, []{ return active_workers.load() == 0; });
        lock.unlock();
      }
    }
    //Refresh hash_full unconditionally.
    //
    //It used to be updated only inside the two info-line blocks and inside gc(), so with both
    //info-line options off -- which is exactly how a match runs -- and reuse off, it was never
    //updated at all. It stayed 0, the workers' `hash_full < 1000` expansion guard never fired,
    //and the Hash option was simply not enforced. The guard was only ever working in the
    //configuration where someone was watching the output.
    {
      int hf = tree_occupancy();
      if (hf > 1000) hf = 1000;   // UCI caps hashfull at 1000
      hash_full.store(hf, std::memory_order_relaxed);
    }
    multiPV = select_best_moves(pvs);
    if (chessEngine.optionCheck[FinalInfoLines].value) {    
      elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
      nodes = search_simulations.load(std::memory_order_relaxed);
      unique_nodes = search.tree.size();
      // Calculate hashfull (in per-mille) using unique_nodes
      size_t total_memory = unique_nodes * (sizeof(MCTSNode) + 24) + total_children.load(std::memory_order_relaxed) * sizeof(Edge);
      size_t max_capacity = chessEngine.optionSpin[Hash].value * 1024 * 1024;  // MB to bytes
      hashfull = max_capacity ? (total_memory * 1000) / max_capacity : 0;
      if (hashfull > 1000) hashfull = 1000;  // Cap at 1000 per UCI spec
      hash_full.store(hashfull, std::memory_order_relaxed);
    }
  } else if (board.num_moves == 1) { //skip MCTS and just make the move
      auto iter_start = std::chrono::steady_clock::now();      
      int move_idx = 0;
      Move move;
      auto [moves, pinned, pinning, checkers, kingSquare] = kingMoves(board);
      move.src = kingSquare;
      move.promoType = PieceTypeNone;
  	  while (moves && move_idx == 0) {
  	    move.dst = lsBit(moves);
  	    move_idx = (move.promoType << 12) | (move.src << 6) | move.dst;
        moves &= moves - 1;
      }
      auto [check_mask, ep_mask] = checkers ? checkMask(board, kingSquare, checkers) : std::make_pair(0xffffffffffffffffULL, 0ULL);
      for (PieceType pt = Queen; pt >= Pawn && move_idx == 0; pt = (PieceType)(pt - 1)) {
      	uint64_t occupations = board.side[board.sideToMove] & board.pieceTypes[pt - 1]; 
      	while (occupations && move_idx == 0) {
      	  move.src = lsBit(occupations);
  	      moves = piece_moves(board, pt, move.src, kingSquare, pinned, pinning, check_mask, ep_mask);
      	  while (moves && move_idx == 0) {
      	    move.dst = lsBit(moves);
          	PieceType startPiece = PieceTypeNone, endPiece = PieceTypeNone;
          	if (promoMove(board, move)) {
          	  startPiece = Knight;
          	  endPiece = Queen;
          	}
        	  //This loop has two exit states, so the chosen promotion must be captured inside the
        	  //body - the pattern already used at line 508 and in test_pos.cpp:154. For a plain move
        	  //startPiece == endPiece == PieceTypeNone (7) and the final increment would leave
        	  //move.promoType == 8, making do_move() write board.pieceTypes[7] past the end of Board;
        	  //for a real promotion the move_idx == 0 guard would exit one piece past the one that
        	  //move_idx was actually built with (Bishop, while the GUI is told "n").
        	  for (PieceType promo = startPiece; promo <= endPiece && move_idx == 0; promo = (PieceType)(promo + 1)) { //loop over promotions if any
          	  move.promoType = promo;
          	  move_idx = (move.promoType << 12) | (move.src << 6) | move.dst;
        	  }
            moves &= moves - 1;
          }
          occupations &= occupations - 1;
        }
      }
      StateInfo state = {};
      //The hash has to advance WITH the board. This used to call do_move() and then hand
      //process_check() the board after the move together with `zh`, which is still the hash of
      //the position BEFORE it -- nothing between here and line ~1203 updates zh. process_check()
      //keys its node by the hash it is given, so it created a node under hash(P) and expanded it
      //with the legal moves of P', and the child hashes it derived were the correct hashes XOR
      //the forced move's delta, matching no position at all. With cleanup() that node is
      //destroyed before the next search reads it; with gc() it survives under a real position's
      //key, carrying another position's move list. Same idiom as lines 595 and 629.
      ZobristHash tmp_zh = zh;
      updateHash(tmp_zh, board, move, do_move(board, move, state), z);
  		isCheckMateStaleMate(board);
      double res;
      if (board.isMate) res = MATE_SCORE * 0.01;
      else if (board.isStaleMate) res = 0.0;
      else if (board.isCheck) {
        std::unordered_set<uint64_t>pos_history;
        res = -process_check(board, tmp_zh, ctx, pos_history, 0);
      } else {
        res = -evaluate_nnue(board, ctx);
      }
      undo_move(board, move, state);
      
      idx2uci(move_idx, best_move);
      std::string pv(best_move);
      pvs.push_back({static_cast<int>(100.0 * res), pv});
      multiPV = 1;
      if (chessEngine.optionCheck[FinalInfoLines].value) {    
        nodes = 1;
        depth.store(1, std::memory_order_relaxed);
        seldepth.store(1, std::memory_order_relaxed);
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
      }
  } //board.num_moves == 1
  else {
      if (board.isMate) {
          log_file("info depth 0 score mate 0\n");
          log_file("bestmove (none)\n");
          print("info depth 0 score mate 0\n");
          print("bestmove (none)\n");
      }
      else if (board.isStaleMate) {
          log_file("info depth 0 score cp 0\n");
          log_file("bestmove (none)\n");
          print("info depth 0 score cp 0\n");
          print("bestmove (none)\n");
      }
      return;
  }
  if (chessEngine.optionCheck[FinalInfoLines].value) {
    double nps = nodes / elapsed;
    for (int i = 0; i < multiPV; i++) {      
      log_file("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s timeAllocated %.2f\n", depth.load(std::memory_order_relaxed), seldepth.load(std::memory_order_relaxed), i + 1, pvs[i].first, nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, pvs[i].second.c_str(), timeAllocated * 0.001);
      print("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", depth.load(std::memory_order_relaxed), seldepth.load(std::memory_order_relaxed), i + 1, pvs[i].first, nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, pvs[i].second.c_str());
    }
  }
  if (!ponderHit.load(std::memory_order_relaxed)) {
    std::string bestmove;
    std::string ponder;
    if (!pvs.empty()) {
      int pos = pvs[0].second.find(" ");
      int pos2 = pvs[0].second.find(" ", pos + 5);
      bestmove = pvs[0].second.substr(0, pos);
      if (pos != std::string::npos) ponder = pvs[0].second.substr(pos + 1, pos2 - pos - 1);
      //here we need to make bestmove to update position_history
      if (!chessEngine.ponder) {
        Move move = {};
        uci2move_idx(bestmove.c_str(), move);
        updateHash(zh, board, move, ff_move(board, move), z);
        position_history.insert(zh.hash);
      }
    } else bestmove = "(none)"; //pvs is empty! 
    if (!ponder.empty()) {
      log_file("bestmove %s ponder %s\n", bestmove.c_str(), ponder.c_str());
      print("bestmove %s ponder %s\n", bestmove.c_str(), ponder.c_str());
    } else {
      log_file("bestmove %s\n", bestmove.c_str());
      print("bestmove %s\n", bestmove.c_str());      
    }
    //Collect AFTER the move has been sent, not before the next search.
    //
    //Two things make this the right place. The obvious one is that the GUI already has its
    //bestmove, so the time spent here is off the critical path -- the engine would otherwise be
    //idle. The less obvious one is that collecting before the root advances frees almost
    //nothing: everything in the tree is reachable from the current root, so there is no garbage
    //yet. The garbage appears when the root MOVES, and the largest part of it is the sibling
    //subtrees of the move we did not play. So re-root onto the move we just played first.
    //
    //This is safe against a concurrent command because searchFlag is still set: new_game(),
    //set_position() and stop() all wait for it to clear, and that happens only after runMCTS
    //returns. Leaving search.root pointing at the child is fine -- set_root() looks the next
    //root up by hash and overwrites it, and if the next position is not under this move (a
    //takeback, a new game) set_root() simply builds a fresh root and this subtree is collected
    //later by the threshold check.
    const bool reuse_active = reuse_tree || chessEngine.optionCheck[Ponder].value;
    if (pondering) {
      //No background collection after a ponder search: the next "go" is imminent and would
      //abort it before it finished anything. The search that follows collects inline instead.
    } else if (!reuse_active) {
      //No reuse: the tree is discarded every move anyway, so discard it HERE, after the move
      //has been sent, rather than on the clock before the next search.
      gc_start(nullptr, /*wipe=*/true);
    } else if (search.root && !bestmove.empty() && bestmove != "(none)") {
      Move bm;
      uci2move_idx(bestmove.c_str(), bm);
      const int want = (bm.promoType << 12) | (bm.src << 6) | bm.dst;
      const int nc = search.root->num_children.load(std::memory_order_relaxed);
      Edge * ch  = search.root->children.load(std::memory_order_acquire);
      MCTSNode * next_root = nullptr;
      for (int i = 0; i < nc && ch; ++i) {
        if (ch[i].move.load(std::memory_order_relaxed) == want) {
          next_root = ch[i].child.load(std::memory_order_acquire);
          break;
        }
      }
      //Collect only if it is likely to achieve something. The threshold still forces one when
      //occupancy is high enough to matter, so the tree cannot silently run away.
      const bool worth_collecting = last_gc_freed.load(std::memory_order_relaxed) >= GC_MIN_YIELD
                                 || tree_occupancy() >= gc_threshold;
      if (next_root && worth_collecting) {
        //Hand the tree to the background collector and return. The engine is now idle until the
        //opponent replies, so this costs nothing off the clock; if the reply comes first, the
        //"go" edge aborts it part-done rather than making the search wait.
        //
        //Validation moved to the next search, after the join -- inspecting the tree here would
        //race the collector.
        search.root = next_root;
        gc_start(next_root, /*wipe=*/false);
      }
    }
  }
}
