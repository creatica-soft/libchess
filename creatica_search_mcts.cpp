// THE HISTORICAL MCTS ENGINE, FROZEN. This is creatica as it was when its search was Monte Carlo
// tree search with NNUE leaf evaluation: one shared tree as a flat open-addressed NodeMap, virtual
// loss, softmax move priors from the policy head, node value W = tanh(eval / eval_scale), an
// exploration constant decaying with depth, quiescence at leaves, Syzygy at <= 5 pieces, and the
// mark-sweep collector over the flat node arena. It is kept building so it does not rot: the live
// engine's search is alpha-beta now, and without a target of its own this code would stop compiling
// against the library's API and nobody would notice until it was needed.
//
// Copied from creatica_search.cpp / creatica.cpp / creatica_search.hpp. Do not develop it -- fix it
// only when a library API change breaks the build.
//
// uci_frontend.cpp is deliberately NOT duplicated here. It is the one UCI protocol loop shared by
// every engine in the repository, and uci_engine.h records why: there used to be fourteen
// near-identical uci-*.cpp files, and that is how a bug fixed in one copy stays unfixed in the
// others. This engine links the shared frontend.

//For MacOS using clang
// c++ -std=c++20 -Wall -O3 -flto -I $HOME/libchess  -L $HOME/libchess -Wl,-lchess,-lcurl,-rpath,$HOME/libchess creatica_search_mcts.cpp creatica_mcts.cpp uci_frontend.cpp tbprobe.c -o creatica_mcts

// -O3 -g -fno-omit-frame-pointer - options for profiling
//

// For linux or Windows using mingw. Not compile-tested: this machine has no GCC and no Windows.
// GCC may still warn inside the vendored Syzygy code that tbprobe.c #includes; if it does,
// -Wno-stringop-overflow is the flag that silenced it before.
//g++ -std=c++20 -Wall -O3 -march=native -DNDEBUG -I /home/ap/libchess -L /home/ap/libchess creatica_search_mcts.cpp creatica_mcts.cpp uci_frontend.cpp tbprobe.c -o creatica_mcts -lchess -lcurl

//or with clang in MSYS2 MINGW64 or CLANG64
//clang++ -std=c++20 -Wall -O3 -march=native -flto -DNDEBUG -I /home/ap/libchess -L /home/ap/libchess creatica_search_mcts.cpp creatica_mcts.cpp uci_frontend.cpp tbprobe.c -o creatica_mcts -lchess -lcurl

#include "creatica_search_mcts.hpp"
#include "policy_net.h"
#include "err_head.h"
#include <memory>     // std::unique_ptr for the spatial term's re-entry fallback

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
//THE PER-MOVE ERROR HEAD. Predicts, for each child of a node, how far the static NNUE evaluation
//of that child is about to be from what a real search of it would say -- an absolute magnitude in
//pawns, not a judgement of the move. The search uses it to decide which children to resolve with
//a bounded alpha-beta instead of trusting their static value; see err_leaf_depth in
//creatica_search.hpp for what that buys and what it is NOT evidence of.
//
//It shares the policy head's architecture on purpose: it reads the SAME feature vector, the
//accumulator stack's post-activation output, which is already computed at a search node. So its
//trunk costs what the policy's trunk costs and nothing is added per move beyond one dot product.
ErrHead       err_head;
bool          err_head_enabled = false;
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
//REPETITION GUARD, in occurrences.
//
//1 (the default, and today's behaviour) refuses a winning move whose resulting position has ever
//occurred before. That is deliberately stricter than the laws of chess, which draw only on the
//THIRD occurrence: with creatica's priors the search cannot see two full repetition cycles ahead,
//so by the time a threefold-correct rule noticed the danger the opponent would already be able to
//force it.
//
//2 uses the full legal allowance -- refuse only a move handing the opponent an immediate threefold
//claim. The case for it is that the danger 1 protects against requires an opponent who is actively
//hunting for a repetition and knows how to set one up over several moves, which most of the field
//is not. The case against is that when they ARE, this concedes draws 1 would have avoided.
//
//Which is right is a question about opponents, not about chess, so it is an option rather than a
//judgement baked into the code: play the two against each other and let the result decide.
int64_t       repetition_guard = 1;
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

//Whether the move this engine CHOOSES is the move that will actually be played next.
//
//True when playing: the post-move step re-roots onto the chosen move and collects, which frees
//the sibling subtrees and is exactly right.
//
//False when REPLAYING someone else's game, or analysing a position list. Then the next position
//is whatever was actually played, which is a sibling of the engine's choice whenever the two
//disagree -- and the post-move collection frees precisely the subtree the next search needs.
//That is why tree reuse measured worthless during target generation: not because consecutive
//positions are unrelated, but because the collector was throwing away the inheritance.
//
//With this off, nothing collects after the move. set_root() then advances to the position that
//really came next -- it finds nodes by HASH LOOKUP, so it does not care where the root pointed --
//and the threshold collection before the search frees from the correct root. Slower to reclaim,
//but there is no clock during generation.
bool          post_move_collect = true;
//Collect only when the tree is actually filling up, in per-mille of the Hash allocation.
//
//gc() is O(tree) and was measured at 100 ms on 410k nodes rising to 442 ms on 5.1M -- 37% of a
//1200 ms move, growing as the game goes on. Running it every move paid that unconditionally.
//But collection only reclaims MEMORY: an unreachable node is never traversed, so it costs the
//search nothing while it sits there. At 5.1M nodes occupancy was still only ~180 of 1000, so
//almost every one of those collections was work for nothing.
int64_t       gc_threshold = 700;
//TREE RESET: discard the tree, instead of collecting it, when a search is about to start on a nearly
//full tree whose root inherited almost nothing. TreeResetBelow is the root's evidence -- informed
//simulations it inherited -- below which to reset, 0 = off. TreeResetOccupancy is the per-mille of Hash
//at or above which the rule applies. See the reset in runMCTS() for why and for the measurements.
//DEFAULT 1000, was 0 (off). Measured by Arkadi at 154.5 : 145.5 over 300 games with TreeResetHollow
//900 -- 51.5%, roughly +10 Elo against a standard error near 12, so INSIDE THE NOISE. It is a
//default on the strength of a sound mechanism and weeks of live use, not a demonstrated gain: a
//frozen tree learns nothing, so discarding it costs little, and the rule only fires at
//TreeResetOccupancy or above. Do not cite the match as evidence it helps.
int64_t       tree_reset_below = 1000;
//Let the collection run while the engine ponders, instead of paying it inline before every search.
//Safe only because of the write barrier; see mark_barrier(). Off by default.
//DEFAULT TRUE, was false. The best-evidenced of the defaults changed here, and the only one that is
//directly observed rather than inferred from a match: in a lichess game it took the collection off
//the move's own clock entirely -- 0 collections costing 0 ms against 13 costing 5,384 ms with a
//worst case of 778 ms -- and searched about 8% more per move. Safe because of the mark's write
//barrier, and the reset-beside-collection crash it once exposed is fixed and reproduced under ASAN.
bool          collect_during_ponder = true;
int64_t       tree_reset_occupancy = 950;
//TreeResetHollow: also reset when the PREVIOUS search was at least this many per-mille hollow -- the tree
//was frozen, whatever the new root inherited. 0 = off. See the reset in runMCTS().
//DEFAULT 900, was 0 (off). See TreeResetBelow above for the evidence, which is weak.
int64_t       tree_reset_hollow = 900;
//The previous search's hollow simulations, per-mille, written when it finishes. -1 before the first.
static std::atomic<int> last_search_hollow_permille{-1};

//Simulations performed by THIS search. The info lines used to report search.root->N for
//"nodes", which the comment at the ponder-output site still calls "total simulations" -- and it
//was one, as long as the tree was destroyed before every move so the root started at zero. With
//ReuseTree the root carries the visits of every earlier search that passed through it, so nodes
//counted work already done and nps divided that inherited total by this search's elapsed time,
//inflating both. Counted here instead, and reset when a search starts.
std::atomic<uint64_t> search_simulations{0};
//How long the last collection took, and whether the search that followed had to wait for it.
//A search that completed 99 simulations because a 1.8 second collection ate its budget looks
//identical in the log to a search that thought hard and disagreed -- and the two call for
//opposite responses. Recording both makes the difference readable afterwards instead of having
//to be reconstructed from two visit-dump rows.
std::atomic<double> last_gc_ms{0.0};
//Simulations that reached a leaf the tree had no room to expand. If this is not near zero the
//search is spending its time re-reading numbers it already had.
std::atomic<uint64_t> uninformed_sims{0};
//MID-SEARCH HOLLOW ABORT.
//
//TreeResetHollow originally looked only at the PREVIOUS search, so a search that froze had to run to
//the end of its allocation before anything was done about it, and the reset happened at the start of
//the next move. The move that discovered the problem was therefore always lost. Measured over the 14
//lichess games of 2026-09-22: 15 searches ended between 920 and 1000 per-mille hollow, meaning that
//between 92% and 100% of their simulations re-read a number the tree already held and taught it
//nothing, and each of those was one move's entire thinking time spent for no gain.
//
//The workers now notice it themselves. Thread 0 already wakes every 4096 simulations to refresh
//hash_full; it checks the running hollow rate at the same time and, if the search has gone hollow,
//sets this flag. Every worker's loop condition tests it, so they all fall out of the search
//together; runMCTS() then retires the tree, rebuilds the root and runs the REST of this move's
//budget on a fresh tree. Exactly one retry per move -- the tree cannot be frozen twice in a row when
//the second attempt starts empty, and capping it means a pathological position can never spend the
//whole clock resetting.
std::atomic<bool> hollow_abort{false};
//OFF BY DEFAULT, and it must stay that way until a match says otherwise.
//
//This was built on 2026-09-24 and, because ./creatica is the binary the lichess bot launches, it went
//live in rated games the moment it was compiled. It fired 28 times across 22 games, each time throwing
//away a tree of 18 to 19 million nodes in the middle of a move, and the bot scored 52.3% that day
//against 85.7% two days earlier. That is not proof the abort caused the drop -- nothing was controlled
//-- but the direction matches the one thing this engine's measurements agree on: the accumulated tree
//is what compensates for weak priors, and schemes that discard it have lost strength every time they
//have been tried. So the default is off and the option exists to make it measurable, not to ship it.
//
//A second defect was visible in those same log lines, and it is now FIXED. During a ponder search --
//and equally under `go infinite` -- timeAllocated is set to 1e9 ms, so HOLLOW_ABORT_MAX_ELAPSED, the
//"only in the first half of the budget" test, never restrained it. Ten of the 28 aborts were ponder
//searches, so the tree was retired immediately before the real search that would have inherited it on
//a ponder hit. The trigger in persistent_worker_func() now requires a search with a real deadline, so
//neither a ponder search nor an infinite one can abort. That costs nothing: a tree still frozen when
//the next real search starts is caught by the between-moves TreeResetHollow check in runMCTS().
bool hollow_abort_enabled = false;
//Whether the abort may fire at all right now. It is armed for the FIRST attempt of a move and
//disarmed for the retry, and the reason is arithmetic rather than policy: search_simulations and
//uninformed_sims are not cleared between the two attempts, so that the move's final summary reports
//what the whole move cost. The retry therefore starts with the failed attempt's hollow simulations
//already in the numerator, and on a short move that ratio can still be above TreeResetHollow even
//though the fresh tree is expanding normally. Left armed, the retry would abort itself within one
//tick of thread 0 and the move would be searched twice for nothing.
std::atomic<bool> hollow_abort_armed{false};
TbProbe tb_probe;
//EDGE VISITS, at RUNTIME rather than compile time.
//
//The counters are ALWAYS maintained -- measured, they cost nothing: a build that keeps them and
//never reads them ran 2,280,402 simulations in five seconds against 2,170,841 for one without them
//at all. This flag only decides whether they are READ, which is what changes behaviour: ranking the
//root by N(s,a) instead of the child's lifetime N, and using it as the PUCT exploration denominator
//so a child carrying a million inherited visits can still be re-examined from a new parent.
//
//That costs half the simulation rate -- 1,114,828 against 2,170,841 -- not in bookkeeping but
//because the search genuinely explores more and pays for the expansions it used to skip. Whether
//better-directed search at half the volume beats worse-directed search at full volume is a question
//only a long match answers, so it is a switch rather than a decision, and it is OFF by default.
//
//Runtime rather than a #define specifically so both sides of that match can be the same binary:
//two builds risk differing in something other than the flag.
bool          edge_visits = false;


//Rank and value by evidence rather than by attention. See MCTSNode::evidence. Default ON: it
//restores the invariant the algorithm assumes rather than adding a special case around its
//absence. Switchable so it can be measured head to head from one binary.
bool          use_evidence = true;

//The denominator of Q and of the root ranking: N when the split is off, evidence when it is on.
static inline uint64_t ev_of(const MCTSNode * n) {
    return use_evidence ? n->evidence.load(std::memory_order_relaxed)
                        : n->N.load(std::memory_order_relaxed);
}
int           nnue_feature_dims();
int           nnue_features(const Board&, NNUEContext&, unsigned char*);


std::mutex mtx, log_mtx, print_mtx, pool_mutex, search_done_mtx, probe_mutex;
std::condition_variable cv, pool_cv, pool_done_cv, cv_search_done;
std::atomic<bool> searchFlag {false};
std::atomic<bool> stopFlag {false};
std::atomic<bool> quitFlag {false};
std::atomic<bool> ponderHit {false};
//Set while set_position() is quiescing a search it found still running. It suppresses the
//bestmove of the search being torn down, exactly as ponderHit does, but for a different
//reason: the GUI sent "position", so it is not reading for a bestmove, and an extra line in
//the pipe desynchronises every subsequent read.
std::atomic<bool> searchAborted {false};
std::atomic<bool> pool_quit{false};    // True when engine exits
std::atomic<int> pool_generation{0};   // Increments every new search
std::atomic<int> active_workers{0};    // Count of currently working threads
std::atomic<bool> search_done{false}; // Signals search completion
std::atomic<uint64_t> total_children{0};
//Unlinked from the tree but not yet freed by the reaper (declared further down). Defined here
//because tree_occupancy() counts them: that memory is still held by the process.
std::atomic<size_t>   reap_pending{0};
std::atomic<uint64_t> reap_pending_edges{0};
std::atomic<uint64_t> tbhits{0};
//Leaves that were inside TB_LARGEST but whose table is not actually on disk.
//
//TB_LARGEST is the size of the LARGEST table present, not a promise that everything at or below
//that size is covered. That distinction did not exist while the local set was the complete 3-4-5
//piece one; it appears the moment a PARTIAL 6-piece set is installed, and then every leaf in a
//configuration that has not been downloaded reaches the failed-probe path.
//
//That path used to write a full error line through log_file(), which takes a global mutex. At a
//few hundred thousand leaves a second across four threads it would not merely fill the log, it
//would serialise the search on that mutex. So count them instead and log the FIRST one of each
//search, which keeps the diagnostic -- with a complete set a miss still means something is wrong,
//a corrupt or unreadable file -- without putting a lock in the hot path.
std::atomic<uint64_t> tb_probe_misses{0};
//STARTS AT 1, because 0 now means "this arena slot is free". gc() hands out fetch_add(1)+1, so
//the first real generation is 2 and nothing live ever carries 0.
std::atomic<int> generation{1};
std::atomic<int> hash_full{0};
//Node count, maintained atomically so it can be read DURING a search.
//
//tree_occupancy() and the workers' expansion guard both need to know how full the tree is, but
//search.arena.live() counts SLOTS, including nodes the background sweep has not retired yet. Kept in
//step here instead: set from the mark by gc(), zeroed by cleanup(), incremented on every insert.
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
//THE EXPLORATION CONSTANT AT THE ROOT, INDEPENDENTLY OF THE DEPTH SCHEDULE. 0 disables it and the
//schedule applies unchanged at every depth, so the engine behaves exactly as the incumbent does.
//
//WHY THE ROOT IS A SPECIAL CASE. Measured on the 57 test-set positions creatica never solves in four
//repeats -- 44 of which play the SAME wrong move every time, so they are a fixed point rather than
//variance. Root PUCT is Q + C*P*sqrt(parentN)/(1+N), and at the end of the search the correct move's
//exploration term is a median 0.0277 against a Q gap of 0.0438 that it would have to overcome: in 62%
//of them it is arithmetically locked out and cannot be selected again however long the search runs.
//
//THE LOCK CLOSES EARLY, which is why this belongs at the root rather than in a late widening. It is
//permanent from the first 10% of the search in 53% of those positions and from before the halfway point
//in 76%, median 2.8%. The first few per cent of the simulations decide the move.
//
//The constant needed to keep the correct move selectable is FLAT IN TIME -- median 1.88 at the very
//start and 1.99 at the end, 75th percentile between 2.5 and 3.7 throughout -- which is what makes one
//larger value at the root the right shape rather than a schedule. ExplorationMax used to be capped at
//2.0, which is the median requirement exactly and cannot reach the 75th percentile; the cap is 10.0 now.
double exploration_root = 0.0;
//double probability_mass;
double virtual_loss;
double eval_scale;
double temperature;

std::string last_move;
//COUNTS, not membership. This was an unordered_set, which can only answer "has this position
//occurred", never "how many times" -- so the engine could not tell a first recurrence from a third,
//and could not reason about whether a repetition was being forced on it.
//
//Keyed by the FULL hash, side to move included. A turn-neutral key would be wrong: the draw rule
//counts a position only against itself with the same side to move, so merging the two parities
//overcounts. Worse, it overcounts exactly where it hurts -- reaching the same board with the
//opposite side to move is TRIANGULATION, the standard winning method in the king-and-pawn and rook
//endgames this matters for, and an engine that scores its own triangulation as a repetition will
//refuse to play the move that wins.
//
//The other parity is still one XOR away, and rep_count_flipped() below is how to ask for it.
std::unordered_map<uint64_t, int> position_history;

//A child's value from the PARENT's point of view, which is what "is this winning" means at the
//root. Zero visits has no value yet, so it is not winning as far as we know.
//`mm` on Q's scale. The field is centipawns; Q is tanh(pawns / eval_scale), the same mapping the
//leaf values use, so the two are directly blendable.
bool mm_value(const MCTSNode * n, double& out) {
  const int16_t v = n->mm.load(std::memory_order_relaxed);
  if (v == MM_NONE) return false;
  out = tanh((double)v * 0.01 / eval_scale);
  return true;
}

static inline double child_q(const MCTSNode * c) {
    const uint64_t n = ev_of(c);
    return n ? -c->W.load(std::memory_order_relaxed) / (double)n : 0.0;
}

//How many times this exact position has occurred in the actual game.
int rep_count(uint64_t h) {
    auto it = position_history.find(h);
    return it == position_history.end() ? 0 : it->second;
}

Board board = {};
ZobristHash zh = {};
Zobrist z = {};

//How many times the SAME BOARD has occurred with the other side to move. z.blackMove is a single
//key XORed in when Black is to move (zobrist-hash.cpp:69-70), so flipping it changes the side to
//move and nothing else. Defined here rather than beside rep_count() because it needs the Zobrist
//table above. This is not a draw signal -- it is a TEMPO signal: a board recurring with the tempo
//flipped means someone triangulated, and a candidate move landing on a board never seen at EITHER
//parity is a genuine attempt at progress rather than shuffling.
int rep_count_flipped(uint64_t h) {
    return rep_count(h ^ z.blackMove);
}
Engine chessEngine = {};
MCTSSearch search;
std::vector<std::thread> pool_threads;
std::vector<ThreadParams> pool_params;

void gc_join();         //all three defined with the collector below
void reap_drain();      //waits for the deferred frees; defined with the reaper below
void sweep_stop();      //the background sweep: stop it, or let it finish; defined with the collector below
void sweep_finish();
void cleanup_locked();
static void gc_completed_invalidate();   //THE EXACT FUTILITY TEST, defined with the collector below
//Expansions abandoned after children may already have been created -- the only way a node can be left
//allocated but attached to nothing. THE EXACT FUTILITY TEST requires this unchanged since the last
//collection; see the list of abandon points there.
static std::atomic<uint64_t> expansion_abandons{0};
void reap_drain();

//Wipe a slot back to a just-constructed state before it is reused. A recycled slot keeps whatever
//the previous occupant left in it, and two of those fields are lethal: num_children > 0 with a
//`children` pointer to an array that has just been deleted would send the very next descent into
//freed memory. make_child() sets only hash/cp/terminal/generation/N/W/evidence, so the rest has to
//be cleared here. Done on the reaper's thread, off the search's clock.
static void arena_reset_slot(MCTSNode * n) {
  n->children.store(nullptr, std::memory_order_relaxed);
  n->num_children.store(0, std::memory_order_relaxed);
  n->pool_off.store(0, std::memory_order_relaxed);
  n->N.store(0, std::memory_order_relaxed);
  n->W.store(0.0, std::memory_order_relaxed);
  n->evidence.store(0, std::memory_order_relaxed);
  n->descents.store(0, std::memory_order_relaxed);
  n->expanding.store(0, std::memory_order_relaxed);
  n->terminal.store(0, std::memory_order_relaxed);
  n->cp.store(NO_MATE_SCORE, std::memory_order_relaxed);
  n->mm.store(MM_NONE, std::memory_order_relaxed);
  n->hash.store(0, std::memory_order_relaxed);
  //LAST, and in this order: the tag retires the slot's identity so no map entry can resolve to
  //it, then the generation marks the slot itself free. Neither may be seen while the rest of the
  //node is still the previous occupant's.
  std::atomic_thread_fence(std::memory_order_release);
  const uint32_t idx = search.arena.index_of(n);
  search.arena.tag[idx].store(0, std::memory_order_relaxed);
  search.arena.gen[idx].store(0, std::memory_order_relaxed);
}

//Size the arena to the Hash option. Changing Hash throws the tree away, which is the same thing
//the engine has always done, but it must happen with the reaper idle: the reaper returns INDICES
//into the slot array, and resizing frees that array underneath it.
void arena_size_check() {
  const size_t   bytes = (size_t)chessEngine.optionSpin[Hash].value * 1024 * 1024;
  const uint32_t want  = (uint32_t)std::min<size_t>(bytes / ARENA_BYTES_PER_SLOT, 0xFFFFFFFEu);
  if (search.arena.cap == want) return;
  //Resizing frees the slot array, the stamps and the child table, so no mark may be walking them.
  //Reachable once CollectDuringPonder lets a collection outlive the search that started it.
  gc_join();
  sweep_stop();
  reap_drain();
  search.tree.clear();
  search.root = nullptr;
  total_nodes.store(0, std::memory_order_relaxed);
  total_children.store(0, std::memory_order_relaxed);
  search.arena.size_to(bytes);
  gc_completed_invalidate();
  log_file("info string node arena: %u slots of %zu bytes, one per %zu bytes of the %d MB Hash\n",
           search.arena.cap, sizeof(MCTSNode), ARENA_BYTES_PER_SLOT, chessEngine.optionSpin[Hash].value);
}

//The body, with no join. Callable from the background collector itself, which must not try
//to join the thread it is running on.
void cleanup_locked() {
  //Stop the background sweep, then drain: the reaper is about to be told every slot is free, and it
  //must not be holding indices from the previous generation when that happens.
  sweep_stop();
  reap_drain();
  //By INDEX over the arena, not by walking the map. Same sequential-access reason as the sweep,
  //and it also catches a node that was created and then lost its map entry.
  const uint32_t n = search.arena.high_water();
  for (uint32_t i = 0; i < n; ++i) {
    if (search.arena.gen[i].load(std::memory_order_relaxed) == 0) continue;   //already free
    MCTSNode * node = &search.arena.slots[i];
    delete[] node->children.load(std::memory_order_relaxed);
    search.arena.kids[i].store(0, std::memory_order_relaxed);
    arena_reset_slot(node);
  }
  //Every block is free again, so reset the pool the same way the arena is reset below.
  search.arena.pool_reset();
  //Everything is free again, so hand the slots back by resetting the bump pointer rather than by
  //pushing millions of indices onto the free list.
  search.arena.bump.store(0, std::memory_order_relaxed);
  //AND the released counter, which pairs with bump: live() is handed-out minus given-back, so
  //resetting one without the other makes the node count collapse to zero.
  search.arena.released.store(0, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lk(search.arena.free_mtx);
    search.arena.free_list.clear();
  }
  //reset(), not clear(): keep the map's size for the tree that is about to grow again. See NodeMap::reset().
  search.tree.reset();
  search.root = nullptr;
  total_children.store(0, std::memory_order_relaxed);
  total_nodes.store(0, std::memory_order_relaxed);
  gc_completed_invalidate();
}

//The public form: stop the background collector first, since it owns the tree while it runs
//and destroying it underneath would be a double free. Reached via new_game().
void cleanup() {
  gc_join();
  cleanup_locked();
  //Wait for the deferred frees too. The reaper's nodes are disjoint from the map, so this is not
  //needed for safety -- it is needed so that "cleanup" means the memory is actually back before
  //a new game starts filling the tree again.
  reap_drain();
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
//ProveTerminals. See the long note in creatica_search.hpp for what it does and why.
bool prove_terminals = false;
//MinimaxWeight. 0 is the incumbent search exactly; see the note in creatica_search.hpp.
double minimax_weight = 0.0;
//TopKMin / TopKWiden; see the note in creatica_search.hpp. 0 is off.
int64_t topk_min   = 0;
double  topk_widen = 1.0;
//FlattenTopK; see the note in creatica_search.hpp. 0 is off.
int64_t flatten_topk = 0;
std::atomic<uint64_t> flat_nodes{0};
//MinVisitK / MinVisitShare / MinVisitDepth; see the note in creatica_search.hpp. 0 is off.
int64_t min_visit_k     = 0;
double  min_visit_share = MIN_VISIT_SHARE;
int64_t min_visit_depth = 0;
//How often the floor overrode the PUCT argmax, and how many selections it could have applied to. The
//ratio is the share of the search the floor is actually diverting, which is the thing to watch: the
//arithmetic predicts at most k * share, and a measured value far below that means the floor is not
//binding and the option is doing nothing.
std::atomic<uint64_t> min_visit_forced{0}, min_visit_eligible{0};
//How many of this node's children selection may consider yet. Counted in DESCENDS through the node,
//the same quantity the exploration term uses, so the two stay on one scale.
std::atomic<uint64_t> topk_narrowed{0}, topk_considered{0}, topk_available{0}, topk_selections{0};
static inline int topk_width(uint32_t descents, int num_children) {
    if (topk_min <= 0) return num_children;
    const double grow = topk_widen * std::log2(1.0 + (double)descents);
    int k = (int)topk_min + (int)std::floor(grow > 0.0 ? grow : 0.0);
    if (k < 1) k = 1;
    return k < num_children ? k : num_children;
}
//Did the operator move the quantity it targets? `mm_raised` counts the times a node's minimax value
//was pulled ABOVE the value its own mean would have given, which is the only case in which the
//blend can change a decision.
std::atomic<uint64_t> mm_updates{0}, mm_changed_choice{0};

//HOW OFTEN THE INDUCTION ACTUALLY FIRES. A change that cannot be shown to move the quantity it
//targets is not a null result, it is an untested one -- so the counters come before any verdict.
std::atomic<uint64_t> proofs_win{0}, proofs_loss{0}, proof_seeds{0}, tb_seeds{0};


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
//Occupancy of the TREE, deliberately excluding memory already handed to the reaper.
//
//It counted reap_pending, on the reasoning that the memory is genuinely not back yet. That is
//true of the process, and false of the decision this feeds. hash_full drives the expansion gate
//in mcts_search(): at 1000 the search stops creating nodes entirely and merely re-visits what
//exists, which is how a move accumulates visits without ever being searched. Counting the dead
//set there stops the engine growing its tree in order to protect memory that is in the act of
//being returned -- and the inflation lands exactly when it hurts, during the search immediately
//after a collection, when the reaper is at its busiest. A collection that sheds five million
//nodes was charging the gate for five million phantom ones.
//
//Measured in a live match: 9.2% of this engine's searches ran at hashfull >= 1000 with expansion
//blocked outright, against an opponent build with no reaper and therefore exact accounting.
//
//The reaper's backlog is still worth seeing, so it is logged in the gc breakdown line instead of
//being folded into a number that decides whether to keep searching.
//
//THAT EXCLUSION HAD QUIETLY STOPPED WORKING once the sweep stopped reading corpses: nothing subtracted them
//until the reaper reached them. In game QHWLWAbv a collection retired 11.75M nodes and the next search
//reported 618,205 nodes at 928 per-mille and ran 94% hollow.
//
//Now the counts come from the MARK, which is exact: the tree after a collection is precisely the nodes and
//edges the mark reached, so gc() sets total_nodes and total_children to those and the search adds to them
//as it expands. Neither the background sweep nor the reaper touches them. arena.live() is still the
//authority on SLOTS in use, which includes nodes waiting to be retired or released.
size_t live_tree_nodes() {
  return total_nodes.load(std::memory_order_relaxed);
}

int tree_occupancy() {
  const size_t total_memory =
      total_nodes.load(std::memory_order_relaxed) * NODE_BYTES
    + (size_t)total_children.load(std::memory_order_relaxed) * EDGE_BYTES;
  const size_t max_capacity = (size_t)chessEngine.optionSpin[Hash].value * 1024 * 1024;
  return max_capacity ? (int)((total_memory * 1000) / max_capacity) : 0;
}

int validate_tree(const char * where, bool collected) {
  if (!validate_tree_enabled) return 0;
  //WAIT FOR THE REAPER FIRST. Since the sweep stopped reading corpses, the live slot count, the
  //free list and total_children only settle once the reaper has worked through its batch;
  //checking before that reports the backlog as corruption. Free to do here, because this whole
  //function runs only under ValidateTree, which is off in play.
  reap_drain();
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

  //Every LIVE node, by address, taken from the arena rather than the map: the map no longer
  //enumerates -- it only answers lookups -- and a slot with a non-zero stamp is exactly a node
  //that has been handed out and not yet reclaimed. Membership tests below use this set and never
  //dereference.
  std::unordered_set<const MCTSNode *> live;
  live.reserve(search.arena.live() * 2);
  {
    const uint32_t hw = search.arena.high_water();
    for (uint32_t i = 0; i < hw; ++i)
      if (search.arena.gen[i].load(std::memory_order_relaxed) != 0)
        live.insert(&search.arena.slots[i]);
  }

  //1. The root must exist and be the node the map holds under its own hash.
  if (!search.root) {
    complain("search.root is null", 0, 0);
  } else if (!live.count(search.root)) {
    complain("search.root is not in the tree map", 0, 0);
  } else {
    const uint64_t rh = search.root->hash.load(std::memory_order_relaxed);
    if (search.tree.lookup(rh) != search.root)
      complain("search.root is filed under a different key than its own hash", rh, 0);
  }

  //2. Per node: the key matches the stored hash, the children array agrees with the count, no
  //   node carries a repetition verdict, and every Edge points at a node still in the map.
  uint64_t counted_children = 0;
  const uint32_t hw_nodes = search.arena.high_water();
  for (uint32_t si = 0; si < hw_nodes; ++si) {
    if (search.arena.gen[si].load(std::memory_order_relaxed) == 0) continue;
    MCTSNode * node = &search.arena.slots[si];
    const uint64_t key = node->hash.load(std::memory_order_relaxed);
    //The map must agree that this node is the one filed under its own hash. This replaces the old
    //"key matches the stored hash" check, which was free when the loop walked the map's keys.
    if (search.tree.lookup(key) != node)
      complain("live node is not the one the map holds under its own hash", key, 0);

    //Repetition is a property of the path, not the position, so it must never be cached on a
    //shared node. If this fires, something is writing terminal 3 to a node again.
    if (terminal_kind(node->terminal.load(std::memory_order_relaxed)) == 3)
      complain("node carries terminal 3 (repetition) which is path-dependent", key, 0);

    const int    nc = node->num_children.load(std::memory_order_relaxed);
    const Edge * ch = node->children.load(std::memory_order_relaxed);
    if (nc < 0) complain("negative num_children", key, (uint64_t)nc);
    if ((nc > 0) != (ch != nullptr))
      complain("num_children and children array disagree", key, (uint64_t)nc);
    //The child table has two copies of where the children are -- kids[] for the collector, pool_off in
    //the node for the search -- and both must agree with the node's own count.
    {
      const uint64_t k = search.arena.kids[search.arena.index_of(node)].load(std::memory_order_relaxed);
      if (NodeArena::kids_count(k) != (uint32_t)(nc > 0 ? nc : 0))
        complain("kids[] child count disagrees with num_children", key, (uint64_t)NodeArena::kids_count(k));
      else if (nc > 0 && NodeArena::kids_off(k) != (uint64_t)node->pool_off.load(std::memory_order_relaxed))
        complain("kids[] pool offset disagrees with the node's pool_off", key, NodeArena::kids_off(k));
      else if (nc > 0 && NodeArena::kids_off(k) + (uint64_t)nc > search.arena.pool_cap)
        complain("child block runs past the end of the pool", key, NodeArena::kids_off(k));
    }
    if (nc > 0 && ch) {
      counted_children += (uint64_t)nc;
      for (int i = 0; i < nc; ++i) {
        const MCTSNode * c = search.arena.child_at(node, i);
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
    seen.reserve(search.arena.live() * 2);
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
        const MCTSNode * c = search.arena.child_at(n, i);
        //Only follow pointers already proven live, so a dangling Edge cannot crash the walk.
        if (c && live.count(c) && seen.insert(c).second) q.push(c);
      }
    }
    if (seen.size() != search.arena.live())
      complain("nodes in the map are unreachable from the root",
               (uint64_t)search.arena.live(), (uint64_t)seen.size());
  }

  if (errors) {
    log_file("validate_tree(%s): %d violation(s), %zu nodes\n", where, errors, search.arena.live());
    print("info string validate_tree(%s): %d violation(s), %zu nodes\n", where, errors, search.arena.live());
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
extern std::atomic<bool> gc_in_flight;
//THE MARK'S WRITE BARRIER.
//
//A mark walks the tree from the root and stamps what it reaches; everything else becomes invisible
//when it publishes its generation. That is safe while no search runs. With a search running, a thread
//can attach an EXISTING node -- a transposition it found through the map -- under a node the mark has
//already passed, so the mark would never reach it and the sweep would free a node that is reachable.
//
//The barrier closes it: while a mark is running, a node handed back by the map is stamped with the
//mark's generation and pushed onto the grey list, which the mark drains as extra starting points. A
//node can be pushed at most once per collection -- the stamp is the record -- so the mark still ends.
//The mutex is taken only when a mark is active AND the node needs stamping; with no mark running the
//barrier is one relaxed load. mark_active is cleared and the generation published together under the
//same mutex, so a thread either hands its node over or finds the node already invisible; make_child()
//and set_root() re-check visibility afterwards and build a fresh node rather than attach a doomed one.
std::atomic<bool>            mark_active{false};
std::atomic<uint32_t>        mark_generation{0};
static std::mutex            grey_mtx;
static std::vector<uint32_t> grey;
//How many nodes the barrier handed to a mark, in total and in the collection being reported. A run where
//this stays zero has not tested the barrier at all, whatever else it validated.
static std::atomic<uint64_t> barrier_handovers{0};
static std::atomic<uint64_t> barrier_handovers_total{0};

void mark_barrier(MCTSNode * n) {
  if (!mark_active.load(std::memory_order_acquire)) return;
  const uint32_t idx = search.arena.index_of(n);
  std::lock_guard<std::mutex> lk(grey_mtx);
  if (!mark_active.load(std::memory_order_relaxed)) return;       //the mark finished while we waited
  const uint32_t g   = mark_generation.load(std::memory_order_relaxed);
  const uint32_t cur = search.arena.gen[idx].load(std::memory_order_relaxed);
  if (cur == 0 || cur == g) return;                               //free, or the mark already has it
  search.arena.gen[idx].store(g, std::memory_order_relaxed);
  grey.push_back(idx);
  barrier_handovers.fetch_add(1, std::memory_order_relaxed);
  barrier_handovers_total.fetch_add(1, std::memory_order_relaxed);
}
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
//How many nodes the last completed collection freed, as a count rather than a share. Read by the
//CREATICA_VERIFY_FUTILE check, which has to say "the exact test said 0 and the collection freed N".
std::atomic<size_t> last_gc_freed_count{0};

//THE EXACT FUTILITY TEST.
//
//A collection frees precisely the nodes its mark cannot reach from the root. So it can free
//something only if the set reachable from the root has changed since the last collection that ran
//to completion -- and there are exactly three ways that set can change:
//
//  1. The root moves. Nodes reachable from the old root may not be reachable from the new one.
//  2. Nodes are handed out. An expansion that fails part way leaves children it already created in
//     the map but under no published edge; those are unreachable from birth.
//  3. An existing edge is removed or re-pointed. NOTHING does this: expand_node() publishes a
//     children array only onto a parent whose pointer is still null (a CAS expecting nullptr), and
//     the other writers are arena_reset_slot(), which runs only on slots the sweep already retired,
//     and the eviction block in gc(), which is commented out. Edges are only ever ADDED to live
//     nodes, and adding an edge can only make more nodes reachable, never fewer.
//
//So if the root is the same node -- same slot AND same identity tag -- and the arena has not handed
//out a single slot since a collection completed, a new collection would mark exactly the nodes the
//last one marked and free exactly zero. That is not a prediction; it cannot be wrong while (3)
//holds. Anyone reinstating eviction, or adding any other code that clears or re-points an edge,
//must invalidate this (gc_completed_invalidate()) or delete the test.
//
//"Handed out" is read from arena.next_tag, which stamp_fresh() advances on every successful
//allocation, bump and free list alike, and which nothing ever resets -- so it needs no new counter
//on the allocation path.
//
//WHY THIS IS NOT THE FUTILITY GUARD THAT WAS REMOVED. That guard skipped whenever the PREVIOUS
//collection had freed little, which predicted this one's yield from the last one's -- wrong
//whenever the root had since moved into territory that orphaned most of the tree, and the tree it
//kept full blocked expansion. This test skips only a collection that provably frees nothing, so
//the tree after skipping is identical to the tree after collecting: it cannot keep anything fuller
//than the collection would have left it.
//
//The case it exists for, measured in a lost-on-time endgame: Ponder on, the driver sends stop and
//a fresh go rather than ponderhit, the ponder search is stopped before it simulates anything, and
//the real search then re-marks an 11.6-million-node tree it had just marked -- 1.5 s of the move's
//budget, ten times in one game.
struct GcCompleted { bool valid = false; uint32_t root_idx = 0, root_tag = 0, next_tag = 0; uint64_t abandons = 0; };
static std::mutex  gc_completed_mtx;
static GcCompleted gc_completed;
static void gc_completed_invalidate() {
  std::lock_guard<std::mutex> lk(gc_completed_mtx);
  gc_completed.valid = false;
}
//Is `target` reachable from `from` by following edges, within `max_depth` plies and at most
//`max_visits` node visits? Breadth-first, no visited set -- the DAG means some nodes are seen twice,
//which the visit cap bounds. Returns false when the cap is hit, which is always the safe answer.
static bool reachable_within(const MCTSNode * from, const MCTSNode * target, int max_depth,
                             size_t max_visits) {
  if (from == target) return true;
  std::vector<const MCTSNode *> frontier{from}, next;
  size_t visits = 0;
  for (int d = 0; d < max_depth && !frontier.empty(); ++d) {
    next.clear();
    for (const MCTSNode * n : frontier) {
      const int    nc = n->num_children.load(std::memory_order_relaxed);
      const Edge * ch = n->children.load(std::memory_order_relaxed);
      if (!ch) continue;
      for (int i = 0; i < nc; ++i) {
        const MCTSNode * c = search.arena.child_at(n, i);
        if (!c) continue;
        if (c == target) return true;
        if (++visits > max_visits) return false;
        next.push_back(c);
      }
    }
    frontier.swap(next);
  }
  return false;
}

//WHY THE ROOT MAY MOVE. With Ponder on, `go ponder` searches the position after OUR move -- the
//predicted reply is applied only on ponderhit -- and the driver answers with stop and a fresh go on
//the position after the opponent's reply. So the real search's root is a CHILD of the root the
//ponder search collected from, and a test demanding the same root never fires in the one pattern it
//was written for.
//
//It does not need the same root. If the last completed collection marked from R, every live node is
//reachable from R; if R is reachable from the current root R', so is every live node, and a
//collection from R' frees nothing. That is exactly what happens in a shuffling endgame, where two or
//three plies lead back to a position already in the tree -- and it is why 42 collections in the lost
//game freed nothing at all. Still exact: it asserts reachability it has just observed, and it keeps
//the no-allocation condition, because an allocated node is not guaranteed to be attached (an
//expansion abandoned when the arena runs dry keeps the children it already created).
//
//ALLOCATIONS ARE ALLOWED, and that is what makes the test useful after a ponder search that actually
//searched. The first version required that not one node had been allocated since the collection,
//because an allocated node is not guaranteed to be attached. What matters is not allocation but
//ORPHANING, and a node can be left attached to nothing only when an expansion is abandoned after
//children may exist. eval_and_expand() has three exits: checkmate and stalemate, which return before a
//child is evaluated, and expand_node(). So the abandon points are all in expand_node(), plus
//process_check()'s null guard, and each increments expansion_abandons:
//  1. the child table's pool is exhausted (process_check() may already have created in-check children)
//  2. make_child() fails part way through the child list
//  3. the publication CAS is lost -- which should orphan nothing, since make_child() keeps one node per
//     position and the winning array points at it, but is counted anyway
//  4. process_check() cannot get a node at all
//expand_node()'s early return when another thread has already published orphans nothing, by the same
//one-node-per-position argument, and is not counted. CREATICA_VERIFY_FUTILE checks all of this: a hole
//in it would show as a collection the test called futile freeing more than zero.
//
//With no abandon, every node allocated since the collection hangs under a node that was reachable from
//the root the search was running from; edges are never removed; so the collection's root R still
//reaches every live node, and if R is reachable from the current root, so does the current root.
//
//The search is capped, so a failed test costs a few milliseconds against a collection that costs
//hundreds; four plies covers the shortest shuffle back (three) with one to spare.
static const int    GC_FUTILE_REACH_DEPTH  = 4;
static const size_t GC_FUTILE_REACH_VISITS = 60000;

//0 = a collection from search.root now might free something (collect). 1 = certainly frees nothing,
//same root. 2 = certainly frees nothing, the last collection's root is reachable from this one.
//Call with the search threads stopped and any background collection joined.
static int gc_would_free_nothing() {
  if (!search.root) return 0;
  GcCompleted c;
  { std::lock_guard<std::mutex> lk(gc_completed_mtx); c = gc_completed; }
  if (!c.valid) return 0;
  //No expansion abandoned since the collection: every node allocated since then was attached where it
  //was created, under a node reachable from the root the search was running from. See ALLOCATIONS below.
  if (c.abandons != expansion_abandons.load(std::memory_order_relaxed)) return 0;
  //No allocation since, so the old root's slot cannot have changed hands -- but check its identity
  //anyway rather than rest the whole argument on that.
  if (c.root_idx >= search.arena.cap || search.arena.tag[c.root_idx].load(std::memory_order_relaxed) != c.root_tag)
    return 0;
  if (c.root_idx == search.arena.index_of(search.root)) return 1;
  const MCTSNode * old_root = &search.arena.slots[c.root_idx];
  return reachable_within(search.root, old_root, GC_FUTILE_REACH_DEPTH, GC_FUTILE_REACH_VISITS) ? 2 : 0;
}
//CREATICA_VERIFY_FUTILE=1: when the exact test says "free nothing", collect anyway and log what was
//actually freed. Any line reporting more than 0 means the reasoning above has a hole.
static const bool gc_locality_probe = [] {
  const char * e = std::getenv("CREATICA_GC_LOCALITY");
  return e && *e && *e != '0';
}();
static std::atomic<uint64_t> kids_wrong_position{0}, kids_published_edges{0};
static const bool verify_kids = [] {
  const char * e = std::getenv("CREATICA_VERIFY_KIDS");
  return e && *e && *e != '0';
}();
//TEST ONLY (CREATICA_MARK_DELAY_US): sleep this long every 4096 nodes of the mark, to widen the window
//in which a mark is running beside a search. Races that need a mark of seconds -- a tree reset landing
//inside one, for instance -- cannot be reproduced on a small tree without it.
static const long mark_delay_us = [] {
  const char * e = std::getenv("CREATICA_MARK_DELAY_US");
  return e && *e ? std::atol(e) : 0L;
}();
static const bool verify_futile = [] {
  const char * e = std::getenv("CREATICA_VERIFY_FUTILE");
  return e && *e && *e != '0';
}();

void gc(MCTSNode * from = nullptr, bool retire_all = false);

//--- deferred reclamation: the reaper ---------------------------------------------------------
//
//The sweep used to do two unrelated jobs in one pass, and only one of them is urgent.
//
//UNLINKING a dead node from search.tree is urgent: the search must not be able to find it.
//FREEING its memory is not urgent at all, and it is where all the time went. Measured on a live
//pair of bots: 384 collections costing 354 s in total, 102 of them over a second, the worst
//36.6 s -- and that one freed about 8.9 million nodes, which is 8.9 million delete calls plus a
//delete[] per Edge array, all of it inline before the search was allowed to start, on a machine
//already deep in swap. The cost tracks how many nodes DIE, not how big the tree is, so it spikes
//exactly when the game leaves the reused subtree and orphans most of the tree at once.
//
//So the sweep now only unlinks, and hands the corpses to this thread to free while the search
//runs. That is safe for a precise reason, and the reason is worth stating because it is the
//whole justification:
//
//  * A node reaches the reaper only after search.tree.erase(), and make_child() finds nodes by
//    HASH LOOKUP in that map. Once erased it can never be handed out again.
//  * A COMPLETED mark guarantees no surviving node points at a dead one: if one did, the dead
//    node would have been reachable from the root and would have been marked. The mark is still
//    all-or-nothing -- an aborted mark sweeps nothing -- so this holds whenever we get here.
//
//Therefore nothing the search can reach touches these nodes, and freeing them concurrently with
//the search is not a race. What is NOT safe is letting them outlive the map they came from
//unaccounted, so reap_pending keeps their memory in the occupancy figure until it is really gone.
std::mutex                           reap_mtx;
std::condition_variable              reap_cv;
std::vector<std::vector<uint32_t>>   reap_queue;   //slot indices, not pointers
std::thread                          reap_thread;
std::atomic<bool>                    reap_quit{false};

static void reaper_func() {
  for (;;) {
    std::vector<uint32_t> batch;
    {
      std::unique_lock<std::mutex> lk(reap_mtx);
      reap_cv.wait(lk, [] { return reap_quit.load(std::memory_order_relaxed) || !reap_queue.empty(); });
      if (reap_queue.empty()) return;              //asked to quit with nothing left to do
      batch = std::move(reap_queue.front());
      reap_queue.erase(reap_queue.begin());
    }
    //EVERY COLD ACCESS LIVES HERE. Reading a corpse is a cache miss and often a page fault --
    //these are by definition the nodes the search has not touched -- so the sweep hands over bare
    //indices and this thread pays for them while the search runs.
    uint64_t edges = 0;
    for (uint32_t idx : batch) {
      MCTSNode * n = &search.arena.slots[idx];
      //The Edge array is still a real allocation and still has to be given back. The NODE is not:
      //it is a slot in the arena, so it is wiped and its index returned to the free list.
      delete[] n->children.load(std::memory_order_relaxed);
      //And the child table's block, which is a pool entry rather than an allocation of its own. Its
      //count is the same number the sweep charged for this node, which keeps reap_pending_edges exact.
      if (const uint64_t k = search.arena.kids[idx].exchange(0, std::memory_order_relaxed)) {
        edges += NodeArena::kids_count(k);
        search.arena.pool_release(NodeArena::kids_off(k), NodeArena::kids_count(k));
      }
      arena_reset_slot(n);
      search.arena.release(idx);
    }
    //total_children is NOT charged here: gc() sets it from the mark. See tree_occupancy().
    //Under the mutex, because reap_drain() waits on exactly this condition while holding it.
    {
      std::lock_guard<std::mutex> lk(reap_mtx);
      reap_pending.fetch_sub(batch.size(), std::memory_order_relaxed);
      reap_pending_edges.fetch_sub(edges, std::memory_order_relaxed);
    }
    reap_cv.notify_all();
  }
}

void reap_enqueue(std::vector<uint32_t>&& dead, uint64_t edges) {
  if (dead.empty()) return;
  {
    std::lock_guard<std::mutex> lk(reap_mtx);
    if (!reap_thread.joinable()) {
      reap_quit.store(false, std::memory_order_relaxed);
      reap_thread = std::thread(reaper_func);
    }
    reap_pending.fetch_add(dead.size(), std::memory_order_relaxed);
    reap_pending_edges.fetch_add(edges, std::memory_order_relaxed);
    reap_queue.push_back(std::move(dead));
  }
  reap_cv.notify_one();
}

//Block until every deferred free has actually happened. Needed only where the memory itself
//must be gone -- cleanup() and shutdown -- never on the search path, which is the entire point.
void reap_drain() {
  //The background sweep feeds the reaper, so it has to finish first or the drain could end with slots
  //still waiting to be retired.
  sweep_finish();
  std::unique_lock<std::mutex> lk(reap_mtx);
  reap_cv.wait(lk, [] { return reap_pending.load(std::memory_order_relaxed) == 0; });
}

void reap_shutdown() {
  sweep_stop();   //it enqueues, and reap_enqueue() would start the reaper again
  {
    std::lock_guard<std::mutex> lk(reap_mtx);
    reap_quit.store(true, std::memory_order_relaxed);
  }
  reap_cv.notify_all();
  if (reap_thread.joinable()) reap_thread.join();
}

//--- the background sweep -------------------------------------------------------------------------
//
//Everything gc() used to do after its mark, done while the search runs: retire every slot the mark did
//not reach and hand it to the reaper, then drop the map entries that point at them. See NO SWEEP ON THE
//CLOCK in gc() for why this is safe to do late, and to leave unfinished.
//
//It is never needed for correctness, only for memory: a node the mark did not reach is already invisible.
//So it can be stopped at any point. Whatever it has not retired still carries an old generation, and the
//next collection's sweep takes it.
static std::thread       sweep_thread;
static std::atomic<bool> sweep_abort{false};

//One bit per arena slot, set for the slots the running sweep retired; see NodeMap::clean_step(). Owned by the
//sweep thread, and only one sweep ever runs.
static std::vector<uint64_t> retired_bits;
//CREATICA_MAP_CLEAN=0 skips the map walk (experiment: is the walk what slows the search it overlaps?).
static const bool map_clean_enabled = [] { const char * e = std::getenv("CREATICA_MAP_CLEAN"); return !(e && *e == '0'); }();

static void sweep_func(uint32_t threshold, uint32_t n_slots) {
  const auto t0 = std::chrono::steady_clock::now();
  const size_t words = ((size_t)search.arena.cap + 63) / 64;
  if (retired_bits.size() != words) retired_bits.assign(words, 0);
  std::vector<uint32_t> dead;
  uint64_t dead_edges = 0;
  bool aborted = false;
  //Slots: by index over the arena, a sequential stride over gen[], exactly as the old sweep. A slot is dead
  //when it is occupied (gen != 0) and the mark did not reach it (gen < threshold). The compare-exchange
  //makes retirement happen once even if two sweeps ever overlapped.
  for (uint32_t i = 0; i < n_slots; ++i) {
    if ((i & 0xFFFF) == 0 && sweep_abort.load(std::memory_order_relaxed)) { aborted = true; break; }
    uint32_t g = search.arena.gen[i].load(std::memory_order_relaxed);
    if (g == 0 || g >= threshold) continue;
    if (!search.arena.gen[i].compare_exchange_strong(g, 0, std::memory_order_relaxed)) continue;
    search.arena.tag[i].store(0, std::memory_order_relaxed);
    dead_edges += NodeArena::kids_count(search.arena.kids[i].load(std::memory_order_relaxed));
    dead.push_back(i);
    retired_bits[i >> 6] |= 1ull << (i & 63);
  }
  const auto t1 = std::chrono::steady_clock::now();
  //The map: drop the entries for those slots, one lock range at a time, so the search is only ever kept
  //waiting by the one range being cleaned. Not needed at all when nothing died.
  size_t dropped = 0, steps = 0;
  const char * map_state = "nothing to drop";
  if (!aborted && !dead.empty() && map_clean_enabled) {
    const uint64_t epoch = search.tree.epoch();
    std::vector<NodeMap::Slot> keep;
    map_state = "clean";
    for (size_t li = 0; li < NodeMap::LOCKS; ++li) {
      if (sweep_abort.load(std::memory_order_relaxed)) { map_state = "stopped by the next collection"; break; }
      ++steps;
      if (!search.tree.clean_range(li, epoch, dropped, keep, retired_bits.data())) { map_state = "rebuilt meanwhile"; break; }
    }
  }
  //Only now do the slots go back. Until the walk has passed, a retired slot must not become a new node,
  //or that node's fresh map entry would be dropped as if it were one of the stale ones.
  for (uint32_t i : dead) retired_bits[i >> 6] &= ~(1ull << (i & 63));
  const size_t retired = dead.size();
  reap_enqueue(std::move(dead), dead_edges);
  const auto t2 = std::chrono::steady_clock::now();
  log_file("info string background sweep: retired %zu slots in %.1f ms; map: %s, %zu stale entries dropped "
           "in %zu range steps, %.1f ms\n",
           retired, std::chrono::duration<double, std::milli>(t1 - t0).count(),
           map_state, dropped, steps, std::chrono::duration<double, std::milli>(t2 - t1).count());
}

//Stop a sweep in progress and wait for its thread. Quick: it checks for this every 65,536 slots and
//between map steps.
void sweep_stop() {
  sweep_abort.store(true, std::memory_order_relaxed);
  if (sweep_thread.joinable()) sweep_thread.join();
  sweep_abort.store(false, std::memory_order_relaxed);
}

//Let a sweep in progress finish, for callers that need every dead slot gone (a drain, the validator).
void sweep_finish() {
  if (sweep_thread.joinable()) sweep_thread.join();
}

static void sweep_start(uint32_t threshold, uint32_t n_slots) {
  sweep_stop();
  sweep_thread = std::thread(sweep_func, threshold, n_slots);
}

void gc_join() {
  gc_abort.store(true, std::memory_order_relaxed);
  if (gc_thread.joinable()) gc_thread.join();
  gc_abort.store(false, std::memory_order_relaxed);
  gc_in_flight.store(false, std::memory_order_relaxed);
}

//wipe==true means "destroy the whole tree" (the no-reuse path) rather than "collect from the
//root", so BOTH configurations defer the same class of work to the same idle window. Without
//it the no-reuse side paid cleanup() on its own clock -- 64-89 ms a move -- while the reuse
//side deferred its collection, making an A/B between them asymmetric by roughly 8% of search
//time at a 1000 ms movetime.
//True from the moment a background collection is spawned until its thread has finished, so a search
//can tell that one is already doing the work; see the pre-search branch in runMCTS().
std::atomic<bool> gc_in_flight{false};

void gc_start(MCTSNode * from, bool wipe) {
  gc_join();                                   //never two collectors at once
  gc_in_flight.store(true, std::memory_order_relaxed);
  const size_t before = live_tree_nodes();
  const auto   t0     = std::chrono::steady_clock::now();
  gc_thread = std::thread([from, before, t0, wipe] {
    //gc() records its own yield from the mark. arena.live() is no measure of it any more: slots are
    //retired by the background sweep, after this returns.
    if (wipe) { cleanup_locked(); last_gc_freed.store(1.0, std::memory_order_relaxed); }
    else gc(from);
    const size_t after = live_tree_nodes();
    last_gc_ms.store(std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count(),
                     std::memory_order_relaxed);
    //Logged from inside the thread: the caller returns immediately and cannot observe the
    //outcome. log_file() is mutex-guarded, so this is safe from here.
    log_file("info string post-move %s: %zu -> %zu nodes, %d permille, %.1f ms%s\n",
             wipe ? "cleanup" : "gc", before, after, tree_occupancy(),
             std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count(),
             gc_abort.load(std::memory_order_relaxed) ? " (interrupted)" : "");
    gc_in_flight.store(false, std::memory_order_relaxed);
  });
}

#if defined(__APPLE__) && (defined(__clang__) || defined(__GNUC__))
//MARK BENCHMARK (CREATICA_MARK_BENCH=<rounds>, every CREATICA_MARK_BENCH_EVERY searches, default 4).
//macOS only: it reads the Mach VM counters, and the prefetch variants use a GCC/Clang built-in. On any
//other platform run_mark_bench() is an empty stub, so the Windows build is unaffected.
//
//WHAT IT FOUND, so nobody re-runs it to relearn this. On an 8.7M-node, 17.4M-edge endgame tree the mark
//costs about 10.5 ns per node-plus-edge with its pages in RAM, even with the CPU caches flushed before
//every run -- and 67.5 ns on the run right after another process allocated 4 GB, with 463,243
//decompressions during that one run. The slow marks are the tree's idle pages being decompressed, not
//CPU cache misses, and the prefetch variants are 10-15% SLOWER once pages are resident.
//
//Runs the MARK ALONE -- no sweep, nothing freed -- several times over the tree as it stands, flushing
//the CPU caches before each run, and rotates through three versions of the walk:
//It walks the child table exactly as gc() does. (Software prefetch variants and the old pointer traversal
//were both measured here and removed; the numbers are in mark_only() and in ENGINE_SETTINGS.md.)
//It exists to test why the mark cost 17 ns a node early in a game and 125-148 ns late at the same
//tree size: whether the cost follows edges per node, and whether it is memory latency, which
//prefetching would cut. Harmless to the tree: it stamps reachable nodes with a fresh generation,
//exactly as a mark does, so the next real collection simply re-stamps them.
static const int mark_bench_rounds = [] {
  const char * e = std::getenv("CREATICA_MARK_BENCH"); return e ? std::atoi(e) : 0; }();
//CREATICA_MARK_BENCH_PRESSURE_MB=<n>: before every round, allocate and touch n MB, then free it. On a
//machine without n MB to spare that pushes the tree's idle pages into the compressor, so every round
//starts from the same compressed state -- which is what a collection late in a long game faces.
static const size_t mark_bench_pressure_mb = [] {
  const char * e = std::getenv("CREATICA_MARK_BENCH_PRESSURE_MB"); return e ? (size_t)std::atoll(e) : (size_t)0; }();
static const int mark_bench_every = [] {
  const char * e = std::getenv("CREATICA_MARK_BENCH_EVERY"); return e && std::atoi(e) > 0 ? std::atoi(e) : 4; }();
struct MarkBenchStats { size_t nodes = 0, edges = 0, edges_to_marked = 0; double ms = 0.0; };
//The mark alone, through the child table, exactly as gc() does it. (The pointer traversal it was compared
//against no longer exists: under 6 GB of pressure it cost 18.9-20.7 ns per node+edge with ~49,000
//decompressions per run, the child table 12.6-12.8 ns with ~18,600.)
static MarkBenchStats mark_only(MCTSNode * root) {
  MarkBenchStats st;
  const uint32_t g = (uint32_t)(generation.fetch_add(1, std::memory_order_relaxed) + 1);
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<uint32_t> q;
  q.reserve(1u << 20);
  const uint32_t r = search.arena.index_of(root);
  search.arena.gen[r].store(g, std::memory_order_relaxed);
  q.push_back(r);
  for (size_t h = 0; h < q.size(); ++h) {
    const uint64_t k = search.arena.kids[q[h]].load(std::memory_order_relaxed);
    const uint32_t n = NodeArena::kids_count(k);
    const uint64_t off = NodeArena::kids_off(k);
    st.edges += n;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t c = search.arena.pool[off + i];
      if (search.arena.gen[c].exchange(g, std::memory_order_relaxed) != g) q.push_back(c); else ++st.edges_to_marked;
    }
  }
  st.nodes = q.size();
  st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return st;
}
#include <mach/mach.h>
struct VmCounters { uint64_t pageins = 0, decompressions = 0, swapins = 0; };
static VmCounters vm_counters() {
  VmCounters c;
  vm_statistics64_data_t vs;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &count) == KERN_SUCCESS) {
    c.pageins = vs.pageins; c.decompressions = vs.decompressions; c.swapins = vs.swapins;
  }
  return c;
}
static void run_mark_bench() {
  static int calls = 0;
  if (mark_bench_rounds <= 0 || !search.root) return;
  if ((calls++ % mark_bench_every) != 0) return;
  //Flush between runs: rewriting 128 MB evicts the whole last-level cache (12 MB on this machine)
  //and far more pages than the TLB holds, so every run starts from the same cold state instead of
  //the second run inheriting what the first loaded -- which is what swamped the last experiment.
  static std::vector<uint8_t> flush(128u << 20);
  double total_ms = 0.0; int runs = 0;
  MarkBenchStats last;
  for (int r = 0; r < mark_bench_rounds; ++r) {
    if (mark_bench_pressure_mb) {
      const size_t bytes = mark_bench_pressure_mb << 20;
      if (char * hog = static_cast<char *>(std::malloc(bytes))) {
        for (size_t i = 0; i < bytes; i += 4096) hog[i] = (char)(i >> 12);
        std::free(hog);
      }
    }
    std::memset(flush.data(), (r * 37 + 11) & 0xff, flush.size());
    volatile uint64_t sink = 0;
    for (size_t i = 0; i < flush.size(); i += 4096) sink += flush[i];
    const VmCounters c0 = vm_counters();
    last = mark_only(search.root);
    const VmCounters c1 = vm_counters();
    total_ms += last.ms; ++runs;
    log_file("info string MARK BENCH round %d: %.0f ms, %.1f ns per node+edge, during it %llu decompressions, "
             "%llu page-ins, %llu swap-ins\n", r, last.ms,
             (last.nodes + last.edges) ? last.ms * 1e6 / (double)(last.nodes + last.edges) : 0.0,
             (unsigned long long)(c1.decompressions - c0.decompressions),
             (unsigned long long)(c1.pageins - c0.pageins), (unsigned long long)(c1.swapins - c0.swapins));
  }
  const double elems = (double)(last.nodes + last.edges);
  log_file("info string MARK BENCH: %zu nodes, %zu edges (%.2f per node), %.1f%% of edges lead to an already-marked node; "
           "%.1f ns per node+edge, %.0f ms per mark\n",
           last.nodes, last.edges, last.nodes ? (double)last.edges / (double)last.nodes : 0.0,
           last.edges ? 100.0 * (double)last.edges_to_marked / (double)last.edges : 0.0,
           runs && elems > 0 ? total_ms / runs * 1e6 / elems : 0.0, runs ? total_ms / runs : 0.0);
}
#else
static void run_mark_bench() {}
#endif

//retire_all: keep nothing, not even the root -- the tree reset. The caller must drop search.root and call
//set_root() again. It goes through the ordinary sweep and reaper, so it is exactly as safe as a
//collection whose mark happened to reach nothing, and it costs the same sequential sweep.
void gc(MCTSNode * from, bool retire_all) {
  MCTSNode * const gc_root = from ? from : search.root;
  assert(gc_root);
  //The exact futility test's snapshot, taken BEFORE marking and published only if this collection
  //completes. Invalidated first, so a collection abandoned during the mark leaves no claim behind.
  gc_completed_invalidate();
  const GcCompleted gc_snapshot{ true, search.arena.index_of(gc_root), search.arena.stamp_of(gc_root),
                                 search.arena.next_tag.load(std::memory_order_relaxed),
                                 expansion_abandons.load(std::memory_order_relaxed) };
  //fetch_add() updates generation but returns original value before addition; hence, we add 1
  int current_gen = generation.fetch_add(1, std::memory_order_relaxed) + 1;
  //Arm the write barrier for this mark; see mark_barrier(). The guard disarms it on every exit,
  //including the abort inside the loop, so a barrier can never outlive the collection that armed it.
  struct MarkPhase {
    ~MarkPhase() {
      std::lock_guard<std::mutex> lk(grey_mtx);
      mark_active.store(false, std::memory_order_release);
      grey.clear();
    }
  };
  {
    std::lock_guard<std::mutex> lk(grey_mtx);
    grey.clear();
    barrier_handovers.store(0, std::memory_order_relaxed);
    mark_generation.store((uint32_t)current_gen, std::memory_order_relaxed);
    mark_active.store(true, std::memory_order_release);
  }
  MarkPhase mark_phase;
  // BFS traversal to mark reachable nodes with the current generation.
  // Use queue to avoid recursion and potential stack overflow in deep trees.
  const auto gc_t0 = std::chrono::steady_clock::now();
  size_t gc_marked = 0;
  //A previous sweep still running must not overlap this mark's restamping; whatever it left is taken by
  //this collection's own sweep.
  sweep_stop();
  const size_t   nodes_before = total_nodes.load(std::memory_order_relaxed);
  const uint64_t edges_before = total_children.load(std::memory_order_relaxed);
  //A PLAIN FIFO over a vector with a read cursor. This was a binary heap, ordered by visit count
  //under GcBestFirst and by a decreasing counter otherwise -- so BOTH settings paid O(log n) with
  //a sift on every push and every pop, tens of millions of times a game.
  //
  //Nothing reads the order. It existed for the retention budget, which chose WHICH live subtrees
  //to truncate when the tree exceeded a share of Hash, and that eviction is commented out:
  //reachability does not care what order it is discovered in.
  //
  //Breadth-first rather than a stack, and that is deliberate. Depth-first is the other obvious
  //O(1) choice, but nodes now live in one flat arena in creation order, so a level's nodes sit
  //near each other in memory and a level-by-level walk keeps that locality. A stack abandons it.
  //
  //A vector with a head index rather than std::queue: the frontier is read once and never
  //reclaimed as we go, so one contiguous allocation is all it needs.
  //THE MARK READS ONLY THE CHILD TABLE: kids[] for each node's child count and pool position, pool[]
  //for the child slots, gen[] for the stamps. It never reads an MCTSNode or an Edge. See THE CHILD TABLE
  //in NodeArena for the measurements behind this: the pages the mark has to bring into memory are what
  //made it slow late in a game, and those three arrays are a fraction of the size of the nodes and
  //edges they summarise. The queue holds 4-byte slot numbers rather than 8-byte pointers for the same
  //reason.
  std::vector<uint32_t> q;
  q.reserve(1u << 16);
  size_t q_head = 0;
  int64_t loc_prev_idx = -1, loc_prev_off = -1;
  size_t  kids_checked_nodes = 0, kids_checked_edges = 0, kids_bad_count = 0, kids_bad_child = 0;
  size_t  loc_nodes = 0, loc_far_nodes = 0, loc_blocks = 0, loc_far_blocks = 0;
  const std::atomic<uint64_t> * const kids = search.arena.kids;
  const uint32_t * const              pool = search.arena.pool;
  std::atomic<uint32_t> * const       gen  = search.arena.gen;
  if (!retire_all) {
    const uint32_t root_idx = search.arena.index_of(gc_root);
    gen[root_idx].store((uint32_t)current_gen, std::memory_order_relaxed);
    q.push_back(root_idx);
  }

  //The eviction block that once sat in this loop, and is still commented out, predates the child
  //table. It cut a live node's children by clearing `children` and deleting the Edge array; reinstating
  //it now would also have to clear kids[] and return the pool block, and it would break THE EXACT
  //FUTILITY TEST, which relies on edges never being removed from a live node.
  size_t kept_nodes = retire_all ? 0 : 1, kept_edges = 0, truncated = 0;

  //The outer loop takes whatever the barrier handed over while the inner loop was walking, and stops
  //only when both are empty -- decided while holding grey_mtx, so nothing can be handed over after the
  //decision. Publishing current_gen there, rather than after the loop, closes the same window.
  for (;;) {
  while (q_head < q.size()) {
      //Abandoning during the MARK means nothing may be swept: the marks are incomplete, so a
      //sweep would delete live nodes. Leaving the tree entirely alone is always safe, and the
      //next collection re-marks from scratch (exchange() below makes leftover stamps harmless).
      if (gc_abort.load(std::memory_order_relaxed)) return;
      const uint32_t idx          = q[q_head++];
      if (mark_delay_us && (q_head & 0xFFF) == 0)
        std::this_thread::sleep_for(std::chrono::microseconds(mark_delay_us));
      const uint64_t k            = kids[idx].load(std::memory_order_relaxed);
      const uint32_t num_children = NodeArena::kids_count(k);
      const uint64_t off          = NodeArena::kids_off(k);

      //LOCALITY PROBE (CREATICA_GC_LOCALITY=1): how often consecutive nodes are more than 2 MB apart
      //in the arena, and consecutive child blocks more than 2 MB apart in the pool.
      if (gc_locality_probe) {
        if (loc_prev_idx >= 0 && std::llabs((int64_t)idx - loc_prev_idx) > (int64_t)((2u << 20) / sizeof(MCTSNode))) ++loc_far_nodes;
        loc_prev_idx = (int64_t)idx;
        if (num_children) {
          if (loc_prev_off >= 0 && std::llabs((int64_t)off - loc_prev_off) > (int64_t)((2u << 20) / sizeof(uint32_t))) ++loc_far_blocks;
          loc_prev_off = (int64_t)off;
          ++loc_blocks;
        }
        ++loc_nodes;
      }
      //CREATICA_VERIFY_KIDS=1: kids[] (which this mark reads) must agree with the node's num_children and
      //pool_off (which the search reads), and every child slot must be a live slot. Reads the node, so it
      //brings back the cost the table removes -- a check, not a mode.
      if (verify_kids) {
        const MCTSNode * node = &search.arena.slots[idx];
        const int nc = node->num_children.load(std::memory_order_relaxed);
        ++kids_checked_nodes;
        if (num_children != (uint32_t)(nc > 0 ? nc : 0)) ++kids_bad_count;
        else if (nc > 0 && off != (uint64_t)node->pool_off.load(std::memory_order_relaxed)) ++kids_bad_count;
        else
          for (uint32_t i = 0; i < num_children; ++i) {
            ++kids_checked_edges;
            const uint32_t c = pool[off + i];
            if (c >= search.arena.cap || search.arena.tag[c].load(std::memory_order_relaxed) == 0) ++kids_bad_child;
          }
      }

      kept_edges += num_children;
      for (uint32_t i = 0; i < num_children; ++i) {
          const uint32_t c = pool[off + i];
          //Stamp with this generation; push only if we are the first to reach it. exchange(), not a
          //compare-exchange expecting the previous generation: a partial mark left by an abandoned
          //collection must be harmless, and exchange() makes the mark independent of what was there.
          if (gen[c].exchange((uint32_t)current_gen, std::memory_order_relaxed) != (uint32_t)current_gen) {
              ++gc_marked;
              ++kept_nodes;
              q.push_back(c);
          }
      }
  }
  //Both empty? Then the mark is complete, and the barrier is disarmed and the generation published in
  //the same critical section, so no thread can hand over a node afterwards believing the mark will
  //take it. Otherwise take what the barrier collected and keep walking: those nodes are reachable
  //again, and their subtrees carry the previous generation.
  {
    std::lock_guard<std::mutex> lk(grey_mtx);
    if (grey.empty()) {
      search.arena.visible_gen.store((uint32_t)current_gen, std::memory_order_relaxed);
      mark_active.store(false, std::memory_order_release);
      break;
    }
    for (const uint32_t idx : grey) { q.push_back(idx); ++gc_marked; ++kept_nodes; }
    grey.clear();
  }
  }
  if (truncated)
    log_file("info string gc: truncated %zu nodes to fit %d permille of Hash "
             "(kept %zu nodes, %zu edges)\n",
             truncated, GC_EVICT_SOFT, kept_nodes, kept_edges);
  const auto gc_t1 = std::chrono::steady_clock::now();
  //NO SWEEP ON THE CLOCK.
  //
  //The mark above is all of the collection the search waits for. Publishing current_gen as the arena's
  //visible generation makes every node the mark did not reach invisible to the map at once, so make_child()
  //can never hand one back, and no traversal can reach one either -- a survivor never points at an unmarked
  //node, or that node would have been marked. Nothing therefore has to be retired before the search starts.
  //
  //The sweep used to run here to completion. Measured on a replay at Hash 2048 it cost 131-819 ms a
  //collection -- 15-125 ms scanning slots and 112-719 ms compacting the node map -- charged to the search
  //that followed. In lichess game 2VnUZWDh collections before real searches took 25 seconds of Black's
  //clock, and one left the search 2 ms. Both jobs now run on a background thread while the search runs
  //(sweep_func), and neither has to finish.
  //
  //The counters come from the mark, which is exact: the tree is now precisely the kept nodes and edges.
  const size_t   freed_nodes = nodes_before > kept_nodes ? nodes_before - kept_nodes : 0;
  const uint64_t freed_edges = edges_before > (uint64_t)kept_edges ? edges_before - (uint64_t)kept_edges : 0;
  total_nodes.store(kept_nodes, std::memory_order_relaxed);
  total_children.store((uint64_t)kept_edges, std::memory_order_relaxed);
  search.arena.visible_gen.store((uint32_t)current_gen, std::memory_order_relaxed);
  sweep_start((uint32_t)current_gen, search.arena.high_water());
  //Record the yield HERE, not only in gc_start(). last_gc_freed existed already but was written
  //solely by the background collector and read solely by the background decision, so the inline
  //path -- the only one that runs with Ponder on -- had no futile-collection guard at all.
  last_gc_freed.store(nodes_before ? (double)freed_nodes / (double)nodes_before : 1.0,
                      std::memory_order_relaxed);
  last_gc_freed_count.store(freed_nodes, std::memory_order_relaxed);
  //Completed: the mark reached everything reachable from gc_root and everything else is invisible, so
  //the tree is now exactly the set reachable from gc_root. See THE EXACT FUTILITY TEST.
  //Not after a reset: its snapshot names a root that has just been retired.
  if (!retire_all) {
    std::lock_guard<std::mutex> lk(gc_completed_mtx);
    gc_completed = gc_snapshot;
  }
  //One line per collection that actually runs. The sweep reports separately when it finishes.
  log_file("info string gc breakdown: %zu nodes -> %zu, marked %zu, freed %zu nodes + %llu edges, "
           "reaper backlog %zu, mark %.1f ms, sweep in the background; map rehashes so far %llu (%.1f ms), "
           "barrier handovers %llu (%llu since start)\n",
           nodes_before, kept_nodes, gc_marked, freed_nodes, (unsigned long long)freed_edges,
           reap_pending.load(std::memory_order_relaxed),
           std::chrono::duration<double, std::milli>(gc_t1 - gc_t0).count(),
           (unsigned long long)NodeMap::rehashes.load(), NodeMap::rehash_us.load() / 1000.0,
           (unsigned long long)barrier_handovers.load(std::memory_order_relaxed),
           (unsigned long long)barrier_handovers_total.load(std::memory_order_relaxed));
  if (verify_kids)
    log_file("info string KIDS-CHECK: %zu nodes, %zu edges checked; count or offset mismatches %zu, children in free slots %zu; "
             "since start %llu edges published, %llu reached the wrong position%s\n",
             kids_checked_nodes, kids_checked_edges, kids_bad_count, kids_bad_child,
             (unsigned long long)kids_published_edges.load(), (unsigned long long)kids_wrong_position.load(),
             (kids_bad_count || kids_bad_child || kids_wrong_position.load()) ? "  <-- THE CHILD TABLE IS WRONG" : "");
  if (gc_locality_probe && loc_nodes)
    log_file("info string gc locality: %zu nodes marked, %.1f ns/node, far node jumps %.1f%%, "
             "far child-block jumps %.1f%%, arena high water %u of %u slots, free list %zu\n",
             loc_nodes, std::chrono::duration<double, std::nano>(gc_t1 - gc_t0).count() / (double)loc_nodes,
             100.0 * (double)loc_far_nodes / (double)loc_nodes,
             loc_blocks ? 100.0 * (double)loc_far_blocks / (double)loc_blocks : 0.0,
             search.arena.high_water(), search.arena.cap,
             [] { std::lock_guard<std::mutex> lk(search.arena.free_mtx); return search.arena.free_list.size(); }());
  //update hash_full
  size_t total_memory = live_tree_nodes() * NODE_BYTES + total_children.load(std::memory_order_relaxed) * EDGE_BYTES;
  size_t max_capacity = chessEngine.optionSpin[Hash].value * 1024 * 1024;  // MB to bytes
  int hashfull = max_capacity ? (total_memory * 1000) / max_capacity : 0;
  if (hashfull > 1000) hashfull = 1000;  // Cap at 1000 per UCI spec
  hash_full.store(hashfull, std::memory_order_relaxed);    
}

//no locking, call it before starting other threads (search, etc)
void set_root(NNUEContext& ctx) {
  //Cheap when Hash has not moved -- it compares one integer and returns. This is the only place
  //the arena is created, so it has to run before the first lookup.
  arena_size_check();
  MCTSNode * root = search.tree.lookup(zh.hash);
  //Same rule as make_child(): hand the node to a running mark, and treat one the mark has just left
  //behind as absent. set_root() runs on the UCI thread, which a background collection does not stop.
  if (root) {
    mark_barrier(root);
    if (!search.arena.visible(root)) root = nullptr;
  }
  if (!root) {
    root = search.arena.alloc();
    //The arena is sized so the hash_full gate stops expansion long before it runs dry -- the gate
    //charges NODE_BYTES (96) a node and a slot is worth ARENA_BYTES_PER_SLOT (100) of Hash -- so this is
    //a should-not-happen at the root. If it ever does,
    //wiping the tree is better than proceeding without a root, which is not survivable.
    if (!root) {
      log_file("info string node arena exhausted at the root; resetting the tree\n");
      //Same rule as the tree reset: cleanup_locked() destroys every node, so no mark may be walking.
      gc_join();
      cleanup_locked();
      root = search.arena.alloc();
      if (!root) { search.root = nullptr; return; }
    }
    root->hash.store(zh.hash, std::memory_order_relaxed);
    search.arena.stamp(root, (uint32_t)generation.load(std::memory_order_relaxed));
    search.tree.insert(zh.hash, root);
    total_nodes.fetch_add(1, std::memory_order_relaxed);
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
    root->evidence.store(1, std::memory_order_relaxed);
  }

  //THE ROOT MUST HAVE CHILDREN, AND ONE RETRY IS ALLOWED TO GET THEM.
  //
  //Everywhere else an abandoned expansion is harmless -- expand_node() gives the block back and the
  //node stays a leaf, which the search simply walks past. At the ROOT it is a forfeit: with no
  //children select_best_moves() has nothing to rank, the workers spin (27.9 million "simulations" in
  //2.2 seconds, twelve million a second, because every descent returns at once) and the engine
  //answers "bestmove (none)". Seen five times in creatica_creaticachessbot.log, each followed by the
  //bot restarting the engine mid-game, and once in a 321-ply game at ply 76 where the position
  //plainly had legal moves.
  //
  //Which abandon point fires has NOT been established. So this does two things rather than one. It
  //discards the tree and expands again from a clean arena, which turns a lost game into a lost tree;
  //and it logs everything needed to identify the cause next time, since expansion_abandons was
  //previously visible nowhere.
  //
  //WHAT THE TWO LIVE OCCURRENCES SHOWED (2026-09-22, creaticachessbot). The recovery fired twice and
  //worked both times -- 14 and 18 children after discarding the tree -- and no "bestmove (none)" has
  //appeared since. Both were endgames reached after a long middlegame, at 592 and 669 per mille
  //occupancy, so the ARENA was plainly not exhausted. That leaves the child pool, and the reason the
  //pool figures are now printed: its free lists are exact-size, so it can refuse a small request
  //while holding a great deal of free space in the wrong sizes. Fragmentation is a hypothesis that
  //fits both observations; the next occurrence will say whether it is the right one.
  if (root->num_children.load(std::memory_order_relaxed) == 0) {
    char fen[MAX_FEN_STRING_LEN];
    const uint64_t ab = expansion_abandons.load(std::memory_order_relaxed);
    //THE CHILD POOL, reported because occupancy alone cannot tell the two exhaustion stories apart.
    //`used` is how many of the pool's entries the bump pointer has handed out and `cap` how many
    //exist, so used == cap means the pool is full. `free` is how many entries are sitting in the
    //exact-size free lists waiting to be reused, and `free at N` how many are in the ONE list a
    //request for N entries can draw from -- N being the size of the last refused request.
    //
    //Read it like this. used < cap: the pool was not the cause, look at the arena. used == cap and
    //free near zero: genuine exhaustion, the pool is too small for this tree. used == cap with free
    //large but `free at N` zero: FRAGMENTATION, which is the case this instrumentation exists for.
    //The free lists are exact-size, so blocks released by ~30-move middlegame nodes cannot serve a
    //~14-move endgame node, and an endgame reached after a long middlegame can be refused while the
    //pool holds plenty of unusable space.
    uint32_t free_at_n = 0;
    const uint32_t fail_n  = search.arena.pool_fail_n.load(std::memory_order_relaxed);
    const uint64_t pfree   = search.arena.pool_free_entries(&free_at_n, fail_n);
    log_file("info string ROOT HAS NO CHILDREN after expansion: fen %s, tree %zu nodes at %d permille, "
             "expansion abandons so far %llu; child pool used %llu of %llu entries (%d permille), "
             "%llu free in the size lists, %llu refusals, last refused %u entries with %u free at that size "
             "(and %u recorded when it failed); arena %zu of %u slots "
             "-- discarding the tree and retrying once\n",
             board2fen(board, fen), live_tree_nodes(), tree_occupancy(), (unsigned long long)ab,
             (unsigned long long)search.arena.pool_used(),
             (unsigned long long)search.arena.pool_cap,
             search.arena.pool_cap ? (int)(1000 * search.arena.pool_used() / search.arena.pool_cap) : 0,
             (unsigned long long)pfree,
             (unsigned long long)search.arena.pool_fails.load(std::memory_order_relaxed),
             fail_n, free_at_n,
             search.arena.pool_fail_free_n.load(std::memory_order_relaxed),
             search.arena.live(), search.arena.cap);
    gc_join();
    cleanup_locked();
    root = search.arena.alloc();
    if (root) {
      //Exactly the sequence the fresh-root path above uses. stamp() is not optional: it sets the
      //generation and identity the map entry is validated against, so a node published without it
      //is invisible to lookup() and would be handed out as absent on the very next search.
      root->hash.store(zh.hash, std::memory_order_relaxed);
      search.arena.stamp(root, (uint32_t)generation.load(std::memory_order_relaxed));
      search.tree.insert(zh.hash, root);
      total_nodes.fetch_add(1, std::memory_order_relaxed);
      std::unordered_set<uint64_t> pos_history;
      const double r2 = eval_and_expand(root, board, zh, ctx, pos_history, 0);
      root->W.store(tanh(r2 / eval_scale), std::memory_order_relaxed);
      root->N.store(1, std::memory_order_relaxed);
      root->evidence.store(1, std::memory_order_relaxed);
    }
    log_file("info string ROOT RETRY: %d children after discarding the tree\n",
             root ? root->num_children.load(std::memory_order_relaxed) : -1);
  }

  search.root = root;
}

//called from expand_node() and make_move()
//returns new or existing node
MCTSNode * make_child(const uint64_t hash, const int cp, const int terminal) {
  //first, try to find child_hash in the tree
  MCTSNode * child = search.tree.lookup(hash);
  if (child) {
    //See mark_barrier(). Hand the node to a running mark, and if the mark finished without reaching
    //it, build a fresh node for the position instead of attaching one the sweep is about to free.
    mark_barrier(child);
    if (!search.arena.visible(child)) child = nullptr;
  }
  if (!child) { //if the child_hash is not found, create a child
    child = search.arena.alloc();
    //Out of slots. Returning nullptr is safe because expand_node() builds the whole Edge array
    //before publishing it with a CAS, so it can abandon the expansion without ever publishing a
    //PARTIAL child list -- which would silently delete legal moves from the search.
    if (!child) return nullptr;
    child->cp.store(cp, std::memory_order_relaxed);
    //SEEDED FROM THE ONE-PLY VALUE the child was born with, clamped into the field. Without a seed
    //the minimax term would be absent for every freshly expanded child, which is most of the tree,
    //and the blend would reduce to the mean exactly where it is supposed to act.
    child->mm.store(cp == NO_MATE_SCORE ? MM_NONE
                                        : (int16_t)std::clamp(cp, -32767, 32767),
                    std::memory_order_relaxed);
    child->terminal.store(terminal, std::memory_order_relaxed);
    child->hash.store(hash, std::memory_order_relaxed);
    search.arena.stamp(child, (uint32_t)generation.load(std::memory_order_relaxed));
    //we should probably update N and W as well. We're updating cp, the node is evaluated, meaning it's been visited
    //Terminal children are seeded regardless of seed_children: a mate or tablebase result
    //is exact rather than an evaluation, and leaving a forced mate unseeded would measure
    //something other than the look-ahead.
    if (cp != NO_MATE_SCORE && (seed_children || terminal_kind(terminal) > 0)) {
      child->N.store(1, std::memory_order_relaxed);
      child->evidence.store(1, std::memory_order_relaxed);   //the evaluation it was born with
      child->W.store(tanh(cp * 0.01 / eval_scale), std::memory_order_relaxed);
    }
    //Read the winner's pointer WHILE STILL HOLDING THE LOCK. This used to unlock first and then
    //dereference the iterator, which is a use-after-invalidate: a concurrent insert can rehash
    //the table, and a rehash reallocates, so the iterator no longer points at anything. It was
    //already undefined with unordered_map -- rehashing invalidates iterators there too -- and the
    //open-addressed table makes it reliably fatal rather than merely unlucky, because the whole
    //slot array moves. Nothing is dereferenced outside the lock now.
    MCTSNode * winner = nullptr;
    bool inserted = false;
    {
      auto [w, ins] = search.tree.insert(hash, child);
      inserted = ins;
      winner   = w;
    }
    if (!inserted) {
      // Another thread inserted first; use the existing node and give our slot straight back.
      const uint32_t idx = search.arena.index_of(child);
      arena_reset_slot(child);
      search.arena.release(idx);
      child = winner;
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
  //The child table's block, taken before any child is created so every exit below can give it back.
  //An exhausted pool is treated exactly like an exhausted arena: no expansion, the node stays a leaf.
  const uint64_t pool_off = search.arena.pool_alloc((uint32_t)num_moves);
  if (pool_off == UINT64_MAX) {
    //ABANDON POINT 1. The children were evaluated before this call, and process_check() may already
    //have created nodes for the in-check ones; with no publication they stay attached to nothing.
    expansion_abandons.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Edge * children = new Edge[num_moves];
  for (int i = 0; i < num_moves; ++i) {
      auto [prior, move_idx, child_cp, terminal, child_hash] = top_moves[i];
      //Same reason as the repetition break in mcts_search: make_move() returns terminal 3 when
      //the child position appears in THIS path's history, and make_child() would store that on
      //the shared node, creating it permanently drawn (cp 0, N 1, W 0). Create it unevaluated
      //instead, so whichever path visits it next evaluates it on its own terms. The prior for
      //this edge still reflects the draw, which is only a heuristic and costs nothing if it is
      //wrong for another path.
      const bool path_repetition = (terminal_kind(terminal) == 3);
      MCTSNode * child = make_child(child_hash,
                                    path_repetition ? NO_MATE_SCORE : child_cp,
                                    path_repetition ? 0 : terminal);
      //ALL OR NOTHING. A partial child list would remove legal moves from the search for the rest
      //of the game, which is the one thing this engine must never do -- it plays as well as it
      //does precisely because every legal move is expanded. Nothing has been published yet, so
      //abandoning here just leaves the parent a leaf, exactly as a full tree does today. The
      //children already created stay in the map, unreachable from the root, and the next
      //collection takes them.
      if (!child) {
        //ABANDON POINT 2. Children 0..i-1 exist in the map and are published nowhere.
        expansion_abandons.fetch_add(1, std::memory_order_relaxed);
        delete[] children;
        search.arena.pool_release(pool_off, (uint32_t)num_moves);
        return;
      }
      children[i].P.store(prior, std::memory_order_relaxed);
      children[i].move.store(move_idx, std::memory_order_relaxed);
      search.arena.pool[pool_off + (uint64_t)i] = search.arena.index_of(child);
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
    //ABANDON POINT 3, counted conservatively. Losing the race should orphan nothing: make_child() leaves
    //exactly one node per position in the map, and the array that won the CAS points at it. Counted
    //anyway, because it is rare and an uncounted orphan would make the futility test wrong.
    expansion_abandons.fetch_add(1, std::memory_order_relaxed);
    total_children.fetch_sub(num_moves, std::memory_order_relaxed);
    delete[] children;
    search.arena.pool_release(pool_off, (uint32_t)num_moves);
    return;
  }
  //Child table and pool position BEFORE the count: every reader of a child goes through
  //child_at(parent, i), which is only valid for i < num_children, so anyone who sees the count must find
  //both already in place. kids[] is the collector's copy, pool_off the search's.
  search.arena.kids[search.arena.index_of(parent)].store(NodeArena::kids_pack(pool_off, (uint32_t)num_moves),
                                                         std::memory_order_release);
  parent->pool_off.store((uint32_t)pool_off, std::memory_order_release);
  parent->num_children.store(num_moves, std::memory_order_release);
  //CREATICA_VERIFY_KIDS=1: structure is not enough -- a consistent but shifted index would pass every
  //other check and play the wrong move. Each child reached through the table must be the position its
  //move leads to.
  if (verify_kids)
    for (int i = 0; i < num_moves; ++i)
      if (search.arena.child_at(parent, i)->hash.load(std::memory_order_relaxed) != std::get<4>(top_moves[i]))
        kids_wrong_position.fetch_add(1, std::memory_order_relaxed);
  if (verify_kids) kids_published_edges.fetch_add((uint64_t)num_moves, std::memory_order_relaxed);
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
    //By EDGE when enabled. Following the child with the largest lifetime N walks into whichever
    //position the game happens to have visited most, not the line this search preferred.
    uint64_t n = edge_visits ? (uint64_t)children[i].n.load(std::memory_order_relaxed)
                             : search.arena.child_at(parent, i)->N.load(std::memory_order_relaxed);
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

//
//THE SCORE TO REPORT for a child, from the PARENT's point of view.
//
//node->cp is a ONE-PLY value, written once when the node is expanded 
//and never refreshed by anything the search subsequently learns. Under
//ReuseTree a node keeps it for the rest of the game, so the number the engine printed could be
//millions of simulations and several moves out of date. Measured in a won endgame: the search's
//own value was about +335 centipawns while the reported score read 125, and on neighbouring moves
//15, 20 and 26.
//
//That is not only a display problem. lichess_bot.cpp:913 ACCEPTS A DRAW OFFER when the reported
//score is below DRAW_CP, which is 30 centipawns -- so a stale number three pawns below what the
//search actually believed was deciding whether to agree a draw in a winning position.
//
//W stores tanh(pawns / eval_scale) -- see the make_child() and backpropagation stores -- so the
//search's own value inverts as pawns = eval_scale * atanh(Q). Mate and tablebase scores are left
//exactly as they are: they are certainties, and pushing a certainty through atanh would only blur
//it. A child with no visits has nothing better to offer than its stored value.
static int report_cp(MCTSNode * child) {
  const int raw = child->cp.load(std::memory_order_relaxed);
  const int stored = -raw;                       // the caller's point of view
  const bool unset = (raw == NO_MATE_SCORE || raw == -NO_MATE_SCORE);
  if (!unset && std::abs(stored) >= MATE_SCORE) return stored;   // exact; keep it
  const uint64_t n = ev_of(child);
  if (n == 0) return unset ? 0 : stored;
  double q = -child->W.load(std::memory_order_relaxed) / (double)n;
  //atanh is infinite at the ends, and Q can reach them through mate backups.
  if (q >  0.999999) q =  0.999999;
  if (q < -0.999999) q = -0.999999;
  return (int)std::lround(100.0 * eval_scale * std::atanh(q));
}

//=== ROOT TRACE: how each root child's Q evolved during one search =============================
//
// A DIAGNOSTIC, NOT A SEARCH CHANGE. It writes nothing unless RootTraceFile names a path, and it
// changes no value the search uses.
//
// Why it exists. The visit dump records each root child's FINAL visit count and prior, which is
// enough to show that creatica puts a median 84.5% of its visits on the move it blunders with and
// 1.5% on the move it should have played -- but not enough to test any change to how Q is formed,
// because Q = W / evidence is a single number at the end. Snapshotting (N, W, evidence) periodically
// turns that into a time series: between two snapshots, the change in W over the change in evidence
// is the mean of exactly the backups that arrived in that window. Any reweighting over windows --
// flat, exponentially decayed, sliding -- can then be recomputed offline on real searches, and
// scored on whether it would have rejected the 171 recorded mistakes, without touching the search.
//
// Thread 0 only, on a tick it is already paying for, so the cost is a few dozen atomic reads every
// 4096 of its simulations. The root's child array is read the same lock-free way uci_output_thread
// reads it.
std::string root_trace_path;   //empty disables it entirely
static std::atomic<uint64_t> root_trace_seq{0};
void root_trace_snapshot(bool final_line) {
  if (root_trace_path.empty() || !search.root) return;
  const int nc = search.root->num_children.load(std::memory_order_relaxed);
  Edge * children = search.root->children.load(std::memory_order_relaxed);
  if (nc <= 0 || !children) return;
  FILE * f = fopen(root_trace_path.c_str(), "a");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  if (ftell(f) == 0)
    fprintf(f, "#tag\tfen\tseq\tfinal\tsims\tmove:edgeN:N:W:evidence:P ...\n");
  char fen[MAX_FEN_STRING_LEN];
  fprintf(f, "%s\t%s\t%llu\t%d\t%llu\t",
          game_tag.empty() ? "-" : game_tag.c_str(), board2fen(board, fen),
          (unsigned long long)root_trace_seq.fetch_add(1, std::memory_order_relaxed),
          final_line ? 1 : 0,
          (unsigned long long)search_simulations.load(std::memory_order_relaxed));
  char uci[6];
  for (int i = 0; i < nc; ++i) {
    //An Edge carries no child pointer -- the child's slot lives in the pool and is resolved through
    //the arena, exactly as select_best_moves() does it.
    MCTSNode * c = search.arena.child_at(search.root, i);
    if (!c) continue;
    idx2uci(children[i].move.load(std::memory_order_relaxed), uci);
    //Both counts are recorded: the per-EDGE visits, which is what ranking uses, and the child
    //position's own N and evidence, which is what Q is formed from. Under tree reuse they differ by
    //a lot, and an offline reconstruction that confused them would measure the wrong thing.
    //The PRIOR too. Without it the exploration term cannot be reconstructed offline at all: U is
    //C * P * sqrt(parent descents) / (1 + edge n), and substituting a flat 1/num_children for P
    //overstates it by orders of magnitude for exactly the starved moves the trace exists to study.
    fprintf(f, "%s%s:%u:%llu:%.6f:%llu:%.8f", i ? " " : "", uci,
            children[i].n.load(std::memory_order_relaxed),
            (unsigned long long)c->N.load(std::memory_order_relaxed),
            c->W.load(std::memory_order_relaxed),
            (unsigned long long)ev_of(c),
            (double)children[i].P.load(std::memory_order_relaxed));
  }
  fprintf(f, "\n");
  fclose(f);
}

static void dump_visits(const std::vector<std::tuple<uint64_t, int>>& visits, const Edge * children) {
  if (visit_dump_path.empty() || !search.root) return;
  FILE * f = fopen(visit_dump_path.c_str(), "a");
  if (!f) return;

  //Header once, so the file explains itself.
  fseek(f, 0, SEEK_END);
  if (ftell(f) == 0) fprintf(f, "tag\tfen\tsimulations\trootQ\trootCP\tponder\tseldepth\thashfull\tvisits\n");

  char fen[MAX_FEN_STRING_LEN];
  const uint64_t rootN = search.root->N.load(std::memory_order_relaxed);
  const double   rootW = search.root->W.load(std::memory_order_relaxed);
  //seldepth is recorded because simulation count alone does not say how far the search
  //actually looked, and depth is what decides whether a record teaches anything a shallow
  //search could not. It lets the trainer weight or filter by the quality of each record.
  fprintf(f, "%s\t%s\t%llu\t%.6f\t%d\t%d\t%d\t%d\t",
          game_tag.empty() ? "-" : game_tag.c_str(),
          board2fen(board, fen),
          (unsigned long long)rootN,
          rootN ? rootW / (double)rootN : 0.0,
          search.root->cp.load(std::memory_order_relaxed),
          chessEngine.ponder ? 1 : 0,
          seldepth.load(std::memory_order_relaxed),
          //hash_full at the end of the search. A full tree sets tree_full in mcts_search(), which
          //blocks expansion outright: the search keeps accumulating VISITS without adding depth,
          //and select_best_moves() picks by visit count. Without this column the condition has to
          //be inferred from a simulations-to-seldepth ratio, which is only suggestive.
          hash_full.load(std::memory_order_relaxed));

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

//no locking, call only when search threads finished
//called from runMCTS()
//CREATICA_DUMP_NODE=<fen>: after a search, print that position's node as the tree holds it -- every
//child with its move, edge visits, evidence, W, the search's value, the one-ply cp, the terminal flag
//and the prior. Written to answer "is the mate child there, and what does the search think of it?",
//which no root-level output can show: the root reports its own children, not a node three plies down.
static void dump_node_if_asked() {
  const char * fen = std::getenv("CREATICA_DUMP_NODE");
  if (!fen || !*fen || !search.root) return;
  Board b{};
  if (fen2board(b, fen) != 0) { log_file("dump_node: cannot parse %s\n", fen); return; }
  ZobristHash h{};
  getHash(h, b, z);
  MCTSNode * n = search.tree.lookup(h.hash);
  if (!n) { log_file("dump_node: %s is not in the tree\n", fen); return; }
  const int nc = n->num_children.load(std::memory_order_relaxed);
  Edge * ch = n->children.load(std::memory_order_acquire);
  log_file("dump_node: %s\n  node N %llu evidence %llu W %.3f cp %d terminal %d children %d\n",
           fen, (unsigned long long)n->N.load(std::memory_order_relaxed),
           (unsigned long long)ev_of(n), n->W.load(std::memory_order_relaxed),
           n->cp.load(std::memory_order_relaxed), n->terminal.load(std::memory_order_relaxed), nc);
  char uci[6];
  for (int i = 0; i < nc && ch; ++i) {
    MCTSNode * c = search.arena.child_at(n, i);
    idx2uci(ch[i].move.load(std::memory_order_relaxed), uci);
    log_file("    %-6s edge n %8u  child N %10llu evidence %10llu  W %12.3f  Q %+.4f  cp %7d  terminal %d  prior %.4f\n",
             uci, ch[i].n.load(std::memory_order_relaxed),
             (unsigned long long)c->N.load(std::memory_order_relaxed), (unsigned long long)ev_of(c),
             c->W.load(std::memory_order_relaxed), child_q(c),
             c->cp.load(std::memory_order_relaxed), c->terminal.load(std::memory_order_relaxed),
             ch[i].P.load(std::memory_order_relaxed));
  }
}

int select_best_moves(std::vector<std::pair<int, std::string>>& pvs) {
  dump_node_if_asked(); 
  int num_children = search.root->num_children.load(std::memory_order_relaxed);
  if (!num_children) {
    log_file("select_best_moves() warning: root node has no children!\n");
    return 0;
  }
  char uci_move[6];
  std::vector<std::tuple<uint64_t, int>> visits; //N, child_idx
  Edge * children = search.root->children.load(std::memory_order_relaxed);
  for (int i = 0; i < num_children; i++) {
    MCTSNode * child = search.arena.child_at(search.root, i);
    //BY EDGE, not by the child node. Ranking by the child's lifetime N is what put frozen nodes
    //at the top of this list every move: a position already played in the game keeps every visit
    //it banked as the ponder root, so it outranked moves the current search had actually worked
    //on, and the repetition filter below then deleted the top of a ranking history had decided.
    visits.push_back({edge_visits ? (uint64_t)children[i].n.load(std::memory_order_relaxed)
                                  : ev_of(child), i});
  }
  //A PROOF OUTRANKS EVERY VISIT COUNT. See prove_terminals. The root is ranked by visits, and a
  //move that has just been proved to win may carry very few of them -- the whole failure this
  //option addresses is that the visits went elsewhere and could not come back. So the verdict is a
  //tier above the count: proven wins first, then the undecided moves in visit order, then the moves
  //proved to lose. Within a tier nothing changes.
  std::vector<int> tier((size_t)num_children, 0);
  if (prove_terminals)
    for (int i = 0; i < num_children; ++i) {
      MCTSNode * c = search.arena.child_at(search.root, i);
      if (!c) continue;
      const int ct = c->terminal.load(std::memory_order_relaxed);
      //From the ROOT's point of view: a child lost for the side to move there is a win for us.
      if (proven_loss(ct))     tier[(size_t)i] =  1;
      else if (proven_win(ct)) tier[(size_t)i] = -1;
    }
  //Descending by visits, and on a TIE the lower child index wins.
  //
  //This was std::greater() over the whole tuple, which compares lexicographically: equal visit
  //counts fell through to comparing the index, also descending, so the HIGHEST index won. The
  //children are published in descending prior order (move_evals is sorted by prior before
  //expand_node), so the highest index is the LOWEST-prior move. Any tie therefore resolved to
  //the worst move rather than the best -- including the all-zero tie of a root that was never
  //searched, where it picked the move the prior liked least.
  std::sort(visits.begin(), visits.end(), [&tier](const auto& a, const auto& b) {
    const int ta = tier[(size_t)std::get<1>(a)], tb = tier[(size_t)std::get<1>(b)];
    if (ta != tb) return ta > tb;
    if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) > std::get<0>(b);
    return std::get<1>(a) < std::get<1>(b);
  });
  //Dumped HERE, on the raw search result. The loop below can drop moves (the repetition-avoidance
  //in winning positions), and those edits are a playing decision rather than something the search
  //concluded -- training on them would teach the policy a heuristic instead of an evaluation.
  //The final state, marked, so the offline reconstruction has the same numbers the engine decided on
  //rather than the last periodic snapshot, which can be thousands of simulations stale.
  root_trace_snapshot(true);
  dump_visits(visits, children);
  if (flatten_topk > 1)
    log_file("flatten: top %lld given equal mass at %llu expansions\n",
             (long long)flatten_topk, (unsigned long long)flat_nodes.load(std::memory_order_relaxed));
  if (topk_min > 0) {
    const uint64_t cons = topk_considered.load(std::memory_order_relaxed);
    const uint64_t avail = topk_available.load(std::memory_order_relaxed);
    const uint64_t narrowed = topk_narrowed.load(std::memory_order_relaxed);
    const uint64_t sels = topk_selections.load(std::memory_order_relaxed);
    log_file("topk: min %lld widen %.2f -- %llu selections, %llu narrowed (%.1f%%), "
             "mean width %.2f of %.2f available\n",
             (long long)topk_min, topk_widen,
             (unsigned long long)sels, (unsigned long long)narrowed,
             sels ? 100.0 * (double)narrowed / (double)sels : 0.0,
             sels ? (double)cons / (double)sels : 0.0,
             sels ? (double)avail / (double)sels : 0.0);
  }
  if (minimax_weight > 0.0)
    log_file("minimax: weight %.2f, %llu node values recomputed, %llu selections where the blend "
             "chose a different child than the mean would have\n", minimax_weight,
             (unsigned long long)mm_updates.load(std::memory_order_relaxed),
             (unsigned long long)mm_changed_choice.load(std::memory_order_relaxed));
  if (prove_terminals)
    log_file("prove: %llu tablebase verdicts stamped, %llu exact nodes met on a path, "
             "%llu nodes proved won, %llu proved lost\n",
             (unsigned long long)tb_seeds.load(std::memory_order_relaxed),
             (unsigned long long)proof_seeds.load(std::memory_order_relaxed),
             (unsigned long long)proofs_win.load(std::memory_order_relaxed),
             (unsigned long long)proofs_loss.load(std::memory_order_relaxed));

  //A FLOOR ON WHAT THE FILTER MAY LAND ON. The loop below DELETES the leading move rather than
  //comparing it with an alternative, and its only exits are "not a repetition" or "one of the two
  //is not winning" -- neither of which is a quality test. So it can walk the visit-ordered list as
  //far as it likes: in one endgame it stood down two moves in a row and played one holding a tenth
  //of the search of the move it started from, and in another it replaced a move carrying about 1.8
  //million visits with one carrying 4,419.
  //
  //This is CONTAINMENT, not the cure. The reason the filter has so much to delete is that
  //select_best_moves() ranks by N, N is a lifetime total that nothing resets, and under ReuseTree
  //the frozen nodes at the top of that ranking are precisely the positions already in
  //position_history -- they banked their statistics while they were the ponder root, immediately
  //after the engine played that very move. The floor stops the symptom from being catastrophic; it
  //does not stop the ranking from being wrong.
  const uint64_t min_N = std::get<0>(visits[0]) / 100;   //1% of the chosen move's visits

  //CAN THEY FORCE IT? An escape is a move that both keeps the advantage and goes somewhere we have
  //not already been. If one exists, a repetition now is harmless -- we can deviate next move and
  //the opponent cannot stop us -- so refusing it is worth the cost. If every advantage-keeping move
  //has already occurred, the opponent only has to keep answering and the draw is theirs whatever we
  //do; wrecking the position to postpone it by one move buys nothing, so we take the repetition
  //instead. That is the whole forcing test, and it costs one map lookup per root move.
  //
  //rep_count_flipped() is deliberately NOT consulted here. A move that reaches a board seen before
  //with the OTHER side to move is triangulation -- the same position with the obligation to move
  //passed to them -- which is how these endgames are won. Its rep_count is 0, so it already counts
  //as an escape, which is exactly right.
  bool escape_exists = false;
  int n_null = 0, n_winning = 0, n_fresh = 0;
  for (int i = 0; i < num_children; ++i) {
      MCTSNode * c = search.arena.child_at(search.root, i);
      if (!c) { ++n_null; continue; }
      const bool winning = child_q(c) > 0;
      const bool fresh   = rep_count(c->hash.load(std::memory_order_relaxed)) == 0;
      if (winning) ++n_winning;
      if (fresh)   ++n_fresh;
      if (winning && fresh) escape_exists = true;
  }
  log_file("select_best_moves(): %d root moves, %d unexpanded, %d winning, %d never played, "
           "escape %s\n", num_children, n_null, n_winning, n_fresh,
           escape_exists ? "yes" : "NO -- they can force it");
  //Diagnostic only. It does not gate anything: whether the opponent can really force a repetition
  //depends on whether they are playing for one at all, which is not knowable from here. Logged so a
  //real game can be read afterwards.
  if (!escape_exists && n_winning > 0 && visits.size() > 1)
      log_file("select_best_moves(): every winning move has been played before\n");

  //0 disables the filter outright: the search's own choice is played whatever it repeats. Worth
  //having as a measurable arm rather than an assumption, because this filter is what discarded a
  //move holding 907,491 in accumulated value for one holding 2,223 in the game that started all
  //this. Whether it earns its place at all is a question nobody has actually asked.
  //
  //Note the guard has to be explicit: `global_count >= 0` is true of everything, so falling into
  //the loop with a threshold of zero would refuse EVERY move rather than none.
  while (repetition_guard > 0 && visits.size() > 1) {
    int idx = std::get<1>(visits[0]); //index of the most visited child
    int next_idx = std::get<1>(visits[1]); //index of the next most visited child
    MCTSNode * child = search.arena.child_at(search.root, idx);
    MCTSNode * next_child = search.arena.child_at(search.root, next_idx);
    //Q, NOT W.
    //
    //The old guard read the raw accumulated W and the comment above it claimed "if it is positive,
    //the position is winning". It is not: W is a running SUM, and a simulation that ends on the
    //repetition test backs up exactly 0.0. Adding zero never lowers a sum, so once a child's W has
    //gone positive no amount of repetition evidence can bring it down again -- `w > 0` really means
    //"this node scored positively at some point in this game", which in a won endgame is true of
    //nearly every root child. That is why this filter fired 306 and 704 times in a single session.
    //
    //Q = W/N is the value the engine actually computes, and it moves the right way: a repetition
    //adds nothing to W while still incrementing N, so Q decays toward zero exactly as a line turns
    //drawish. It is also the quantity report_cp() prints, so the guard and the log now agree.
    int cp = report_cp(child);
    int next_cp = report_cp(next_child);
    double q      = child_q(child);
    double next_q = child_q(next_child);
    if (q > 0 && next_q > 0 && std::get<0>(visits[1]) >= min_N) {
      //Their ledger. Positions alternate parity and the hash carries side to move, so this count
      //is specifically how often the OPPONENT has faced this position -- and they are the side who
      //would complete a cycle on their own move, which is what makes their count the one that
      //decides. Ours is a different key and we control it.
      int global_count = rep_count(child->hash.load(std::memory_order_relaxed));
      if (global_count >= repetition_guard) {
          int move = children[idx].move.load(std::memory_order_relaxed);
          int promo = (move >> 12) & 7;
          char fen[MAX_FEN_STRING_LEN];
          //N and Q, not W. The floor this filter now applies is on VISITS, and the guard above is
          //on Q, so those are the two numbers needed to check either of them from a log -- printing
          //the raw W was what made the frozen-statistics bug invisible for so long.
          log_file("select_best_moves() debug: skipping move %s%s%c (repetition %d in a winning "
                   "position %s; this move N %llu Q %.3f cp %d, next N %llu Q %.3f cp %d)\n",
                   square[(move >> 6) & 63], square[move & 63],
                   promo != PieceTypeNone ? uciPromoLetter[promo] : ' ',
                   global_count, board2fen(board, fen),
                   (unsigned long long)std::get<0>(visits[0]), q, cp,
                   (unsigned long long)std::get<0>(visits[1]), next_q, next_cp);
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
    MCTSNode * child = search.arena.child_at(search.root, index);
    int cp = report_cp(child);                                   //the search's value, not the stale one-ply cp
    const int stale_cp = -child->cp.load(std::memory_order_relaxed); //kept for the debug line below
    double parent_N = static_cast<double>(search.root->N.load(std::memory_order_relaxed));
    double prior = children[index].P.load(std::memory_order_relaxed);
    uint64_t N = ev_of(child);
    double W = -child->W.load(std::memory_order_relaxed);
    double Q = N ? W / N : 0.0;
    double U = exploration_max * prior * sqrt(parent_N) / (1 + N);
    std::string pv(uci_move);
    std::string pv2(uci_move);
    pv2 += " (" + std::to_string(N) + ", " + std::to_string(llround(W)) + ", cp " + std::to_string(cp)
         + " stale " + std::to_string(stale_cp) + ", " + std::to_string(Q) + " + " + std::to_string(U)
         + " = " + std::to_string(Q + U) + ")";
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
      child = search.arena.child_at(child, idx);   //the old child is the parent; evaluated before the assignment
      int cp2 = report_cp(child);
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
//
//NOTE. This function used to carry two further rules, both removed after measurement: a share cap
//(BalanceShare, -150 Elo) and an evidence floor on the replies to the move about to be played
//(VerifyReplies, which caused a forced mate to be walked into -- 0/12, 4/12 and 12/12 blunders as
//the floor rose, because Q is a MEAN and forcing evidence onto every reply lets the mediocre ones
//outvote the refutation). See ENGINE_SETTINGS.md before reintroducing anything of that shape.
int select_best_child(MCTSNode * parent, const int depth) {
  const int all_children = parent->num_children.load(std::memory_order_acquire);
  Edge * children = parent->children.load(std::memory_order_acquire);
  //PROGRESSIVE WIDENING. The children are in descending prior order, so considering only the first k
  //is the narrowing -- and because k grows with the descents, nothing is removed, only deferred.
  int num_children = all_children;
  if (topk_min > 0) {
    num_children = topk_width(parent->descents.load(std::memory_order_relaxed), all_children);
    topk_selections.fetch_add(1, std::memory_order_relaxed);
    topk_considered.fetch_add((uint64_t)num_children, std::memory_order_relaxed);
    topk_available.fetch_add((uint64_t)all_children, std::memory_order_relaxed);
    if (num_children < all_children) topk_narrowed.fetch_add(1, std::memory_order_relaxed);
  }
  double best_score = -INFINITY;
  //INITIALISED. It used to rely on at least one child beating -INFINITY, which held while every
  //child was scored; the proof filter below can skip children, and a node all of whose moves are
  //proven losses would have left this uninitialised.
  int selected = 0;
  const bool prove = prove_terminals;
  //The blend, and the instrumentation that says whether it did anything. `mean_selected` is what
  //the incumbent search would have picked from the same statistics, so counting the disagreements
  //measures the mechanism directly rather than inferring it from the solve rate.
  const double mmw = minimax_weight;
  double best_mean_score = -INFINITY;
  int    mean_selected   = 0;
  // Dynamic Exploration Constant - linear decay with depth
  // setting exploration_depth_decay to 0 will make exploration constant static = exploration_max
  // Decay: Start at exploration_constant, then for example drop by 0.05 - 0.1 per ply, floor at 0.25 - all tunable
  double C = std::max(exploration_min, exploration_max - (depth * exploration_depth_decay));
  //or square root decay with depth
  //double C = std::max(exploration_min, exploration_max - (sqrt(static_cast<double>(depth)) * exploration_depth_decay));

  //The root gets its own constant when ExplorationRoot is set; see the note on exploration_root. This
  //replaces the commented-out `if (depth == 0) C = 2.0;` that used to sit here, and 0 keeps the
  //schedule so the default is byte-for-byte the incumbent's behaviour.
  if (depth == 0 && exploration_root > 0.0) C = exploration_root;
  const uint64_t parentN = parent->N.load(std::memory_order_acquire);
  //The exploration numerator: descents THROUGH this node, which is the sum of its edges' n and
  //therefore on the same scale as the per-edge denominator below.
  const uint32_t parentD = parent->descents.load(std::memory_order_relaxed);
  const bool     use_edge = edge_visits;   //hoisted out of the child loop below
  const double   parentW = parent->W.load(std::memory_order_relaxed);
  const uint64_t parentE = ev_of(parent);
  const double   parentQ = parentE ? parentW / parentE : 0.0;
  //THE VISIT FLOOR. See the MinVisitK note in creatica_search.hpp. Computed before the PUCT loop and,
  //when it binds, replaces the argmax outright -- the floor is a guarantee, not another term to be
  //out-voted by a million-visit mean, which is the whole point.
  int forced = -1;
  if (min_visit_k > 0 && depth <= (int)min_visit_depth && min_visit_share > 0.0) {
    min_visit_eligible.fetch_add(1, std::memory_order_relaxed);
    const int kk = std::min((int)min_visit_k, num_children);
    //Floored, so a node whose descents are below 1/share has no floor yet and starts on pure PUCT.
    const uint32_t floor_n = (uint32_t)(min_visit_share * (double)parentD);
    if (floor_n > 0) {
      uint32_t worst_n = 0;
      for (int i = 0; i < kk; ++i) {
        //A proven win for the child is a loss here, so the floor must not spend visits on it either.
        if (prove && proven_win(search.arena.child_at(parent, i)->terminal.load(std::memory_order_relaxed)))
          continue;
        const uint32_t en = children[i].n.load(std::memory_order_relaxed);
        if (en < floor_n && (forced < 0 || en < worst_n)) { forced = i; worst_n = en; }
      }
    }
    if (forced >= 0) min_visit_forced.fetch_add(1, std::memory_order_relaxed);
  }
  for (int i = 0; forced < 0 && i < num_children; i++) {
    double P = children[i].P.load(std::memory_order_relaxed);
    MCTSNode * child = search.arena.child_at(parent, i);
    //AN EXACT VERDICT OVERRIDES PUCT, which is the point of the option. See prove_terminals.
    //
    //A child that is lost for the side to move there is a move that WINS here, so it is taken at
    //once and no average can out-vote it -- this is the alpha-beta cutoff, with a proof in place of
    //a heuristic. A child that is won for the side to move there loses here, so it is dropped from
    //consideration; that is not the move gating ruled out elsewhere, because gating discards a move
    //on a weak ESTIMATE of its value while this is exact and the position is already decided.
    if (prove) {
      const int ct = child->terminal.load(std::memory_order_relaxed);
      if (proven_loss(ct)) { selected = i; break; }
      if (proven_win(ct)) continue;
    }
    uint64_t N = child->N.load(std::memory_order_relaxed);   //attention: drives exploration
    const uint64_t Ne = ev_of(child);                        //evidence: drives Q
    //N(s,a) for this edge, which is NOT the child's visit count. See the note on Edge::n.
    const uint32_t en = children[i].n.load(std::memory_order_relaxed);
    double W = -child->W.load(std::memory_order_relaxed); //parent perspective
    //First-play urgency. In creatica-shared-root every child was born with N=1 and a real
    //evaluation, so this branch almost never fired and Q=0 for an unvisited child was
    //harmless. Policy expansion creates children unevaluated, so N=0 is now the common
    //case and this value decides the whole search order. Q=0 means "drawn", which is wildly
    //optimistic in a lost position and pessimistic in a won one. Start an unvisited child
    //slightly below its parent instead, which is the honest guess before any evidence.
    //Q still comes from the POSITION: a node's accumulated value is about the position, and a
    //transposition legitimately shares it. Only the exploration term moves to the edge.
    double Q = Ne ? W / Ne : (parentQ - fpu_reduction);
    //Q_blended. A child states its value from its OWN side to move, so the parent sees the
    //negation, exactly as it does for W above.
    double Qb = Q;
    if (mmw > 0.0) {
      double mv = 0.0;
      if (mm_value(child, mv)) Qb = (1.0 - mmw) * Q + mmw * (-mv);
    }
    //PUCT formula. Numerator and denominator are both edge counts now. Using the child's lifetime
    //N as the denominator meant a child carrying a million inherited visits had an exploration
    //term so small it could never be re-examined from a new parent, which is how a node frozen
    //while it was the ponder root stayed frozen for the rest of the game.
    //One branch on a flag that never changes during a search, so it predicts perfectly.
    const double U = use_edge ? C * P * sqrt((double)parentD) / (1.0 + en)
                              : C * P * sqrt((double)parentN) / (1.0 + N);
    const double score = Qb + U;
    if (score > best_score) { best_score = score; selected = i; }
    if (mmw > 0.0) {
      const double mean_score = Q + U;
      if (mean_score > best_mean_score) { best_mean_score = mean_score; mean_selected = i; }
    }
  }
  if (forced >= 0) selected = forced;
  if (mmw > 0.0 && forced < 0 && selected != mean_selected)
    mm_changed_choice.fetch_add(1, std::memory_order_relaxed);
  // Apply virtual loss to selected child to avoid contention among threads for the same node
  //children = parent.children.load(std::memory_order_acquire);
  MCTSNode * child = search.arena.child_at(parent, selected);
  child->N.fetch_add(1, std::memory_order_release);
  child->W.fetch_sub(virtual_loss, std::memory_order_release);
  //Count the edge and the descent together, so the two stay consistent. Neither is ever undone:
  //unlike virtual loss these are not a temporary reservation, they are the visit itself.
  children[selected].n.fetch_add(1, std::memory_order_relaxed);
  parent->descents.fetch_add(1, std::memory_order_relaxed);
  return selected;
}

//Equal mass across the first k, the tail untouched, the total preserved. Called after get_prob(), so
//the entries are already sorted by descending probability and get<0> holds the probability itself.
void flatten_prior(std::vector<std::tuple<double, int, int, int, uint64_t>>& move_evals) {
  if (flatten_topk <= 1) return;
  const size_t k = std::min((size_t)flatten_topk, move_evals.size());
  if (k < 2) return;
  double sum = 0.0;
  for (size_t i = 0; i < k; ++i) sum += std::get<0>(move_evals[i]);
  const double each = sum / (double)k;
  for (size_t i = 0; i < k; ++i) std::get<0>(move_evals[i]) = each;
  flat_nodes.fetch_add(1, std::memory_order_relaxed);
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
  //A check chain this long is a perpetual, and a perpetual is a draw -- so the cap returns the
  //right value rather than merely a safe one. Without it the recursion is unbounded: see
  //MAX_CHECK_EXTENSION in creatica_search.hpp for why a repeated position inside the chain
  //recurses forever, and what it would have overflowed.
  if (iter >= MAX_CHECK_EXTENSION) return 0.0;
  MCTSNode * node = make_child(board_hash.hash, NO_MATE_SCORE, -1);
  //make_child() returns null when the arena is exhausted, and this used to dereference it at once -- a
  //crash, which on lichess is a forfeit. The occupancy gate keeps the arena from running dry, so this
  //is a should-not-happen, but a check extension without a node is better than a dead process: score it
  //as the incumbent does a check it cannot follow, 0.0, and count it as an abandon so the futility test
  //never trusts a tree built across it.
  if (!node) {
    expansion_abandons.fetch_add(1, std::memory_order_relaxed);
    return 0.0;
  }
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
  //The state is RETURNED now rather than left on the Board. It was only ever valid between this
  //call and the next move, and nothing said so -- see the note on BoardState in libchess.h.
	const BoardState bs = isCheckMateStaleMate(chess_board);
  if (bs.mate) {
    res = -MATE_SCORE * 0.01 + iter; //chess_board.sideToMove loses
    return std::make_pair(res, 1);
  }
  else if (bs.stalemate) {
    res = 0.0;
    return std::make_pair(res, 2);
  } else if (bs.check) {
    terminal = -1;
  } else terminal = 0;
	const int pieceCount = bitCount(chess_board.side[ColorWhite] | chess_board.side[ColorBlack]);
	if (pieceCount > TB_LARGEST || chess_board.castlingRights) {
    //if (!terminal)
    //  res = evaluate_nnue(chess_board, ctx); //evaluate_nnue() returns result in pawns (not centipawns!)
	  //else if (terminal == -1) res = process_check(chess_board, board_hash, ctx, pos_history, iter);
	  if (terminal == -1) res = process_check(chess_board, board_hash, ctx, pos_history, iter);
    //`terminal == -1` is the check state, so it is what gets passed rather than a literal: if this
    //call is ever moved out of the else branch the sentinel still fires instead of a silent number.
    else res = evaluate_nnue(chess_board, ctx, terminal == -1); //returns pawns, not centipawns
  } else { //pieceCount <= TB_LARGEST, etc
    unsigned int ep = legalEnPassantMove(chess_board);
    const unsigned int wdl = tb_probe_wdl(chess_board.side[ColorWhite], chess_board.side[ColorBlack], chess_board.pieceTypes[King - 1], chess_board.pieceTypes[Queen - 1], chess_board.pieceTypes[Rook - 1], chess_board.pieceTypes[Bishop - 1], chess_board.pieceTypes[Knight - 1], chess_board.pieceTypes[Pawn - 1], 0, 0, ep == SquareNone ? 0 : ep, chess_board.sideToMove == ColorWhite ? 1 : 0);
    if (wdl == TB_RESULT_FAILED) {
      char fen[MAX_FEN_STRING_LEN];
      //First miss of this search only; see tb_probe_misses.
      if (tb_probe_misses.fetch_add(1, std::memory_order_relaxed) == 0) {
      log_file("info string tablebase miss (not on disk, invalid or illegal); TB_LARGEST %d, occupations %u, ep %u, halfmoveClock %u, whiteToMove %u, whites %llu, blacks %llu, kings %llu, queens %llu, rooks %llu, bishops %llu, knights %llu, pawns %llu, fen %s, err %s\n", TB_LARGEST, pieceCount, ep, chess_board.halfmoveClock, chess_board.sideToMove == ColorWhite ? 1 : 0, chess_board.side[ColorWhite], chess_board.side[ColorBlack], chess_board.pieceTypes[King - 1], chess_board.pieceTypes[Queen - 1], chess_board.pieceTypes[Rook - 1], chess_board.pieceTypes[Bishop - 1], chess_board.pieceTypes[Knight - 1], chess_board.pieceTypes[Pawn - 1], board2fen(chess_board, fen), strerror(errno));
      }
      //if (!terminal)
      //  res = evaluate_nnue(chess_board, ctx); //evaluate_nnue() returns result in pawns (not centipawns!)
  	  //else if (terminal == -1) res = process_check(chess_board, board_hash, ctx, pos_history, iter);
  	  if (terminal == -1) res = process_check(chess_board, board_hash, ctx, pos_history, iter);
      else res = evaluate_nnue(chess_board, ctx, terminal == -1); //returns pawns, not centipawns
    } else { //tb_probe_wdl() succeeded
      //0 - loss, 4 - win, 1..3 - draw
      if (wdl == 4) res = MATE_SCORE * 0.001;
      else if (wdl == 0) res = -MATE_SCORE * 0.001;
      else res = 0.0;
      tbhits.fetch_add(1, std::memory_order_relaxed);
      //A TABLEBASE VERDICT IS A PROOF. See the matching note in make_move_policy(), and note that
      //THIS is the site that matters: the engine runs with the blended prior, which evaluates every
      //child through position_eval(), while make_move_policy() is only used by the policy-only
      //expansion mode. Stamping only the latter left the counter at exactly zero across 684
      //searches, which is how the mistake was found.
      //
      //Only wdl 4 and 0 are proofs. 1, 2 and 3 are blessed loss, draw and cursed win -- decided on
      //the board but drawn by the fifty-move rule -- and the engine already scores all three as 0.
      if (prove_terminals && (wdl == 4 || wdl == 0)) {
        terminal = (terminal & TERMINAL_KIND_MASK) | (wdl == 4 ? PROVEN_WIN : PROVEN_LOSS);
        tb_seeds.fetch_add(1, std::memory_order_relaxed);
      }
    }
  } //end of else (pieceCount <= TB_LARGEST)
  return std::make_pair(res, terminal);
}

//Defined below, next to leaf_moves(). Declared here because make_move() resolves a flagged child
//with it and is defined first.
static int leaf_search(Board& b, NNUEContext& ctx, int depth);
//How many children the selective leaf search resolved, for the stats line. Separate from
//leaf_ab_calls, which counts LeafDepth's node-level searches: the two options can be on at once
//and one counter could not say which did the work.
extern std::atomic<uint64_t> err_leaf_calls;
extern std::atomic<uint64_t> err_leaf_flagged;

//called from compute_move_evals()
//makes a move and calls position_eval(), undo the move
//checks for repetition
//returns a pair of eval result in pawns from the perspective of chess_board.sideToMove and terminal state enum
//`child_leaf_depth` > 0 replaces this child's STATIC value with a bounded alpha-beta from it. The
//caller passes it only for the children the error head flagged; 0 is the incumbent path exactly.
std::pair<double, int> make_move(Board& chess_board, const ZobristHash& board_hash, Move& move, NNUEContext& ctx, uint64_t& child_hash, const std::unordered_set<uint64_t>& pos_history, int iter, int child_leaf_depth) {
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
  //THE SELECTIVE LEAF SEARCH, on a flagged child only.
  //
  //The guards are LeafDepth's, for the same reasons: leaf_search() cannot run on a position in
  //check (evaluate_nnue() refuses one, and the evasions are what position_eval() already minimaxed
  //over), and inside tablebase range position_eval()'s probe is an exact verdict that an estimate
  //must not displace. The terminal test is the one that is specific to here: a mate, a stalemate,
  //a repetition or a proven tablebase result is not an estimate with an error to predict, and
  //overwriting one would throw away a proof to replace it with a search. PROVEN_MASK is tested as
  //well as the kind: a proven win or loss can arrive with a kind of 0, so masking only the kind
  //would resolve exactly the children whose value is already certain.
  //`terminal` is -1 for a checked child, so the kind test already excludes one; the cache read that
  //used to be here was redundant AND unreliable.
  if (child_leaf_depth > 0 && (terminal & (TERMINAL_KIND_MASK | PROVEN_MASK)) == 0 &&
      bitCount(chess_board.side[ColorWhite] | chess_board.side[ColorBlack]) > TB_LARGEST) {
    err_leaf_calls.fetch_add(1, std::memory_order_relaxed);
    //In the CHILD's frame, which is the frame position_eval() returned `res` in; the caller
    //negates both the same way.
    res = leaf_search(chess_board, ctx, child_leaf_depth) * 0.01;
  }
  //position_eval() no longer leaves anything on the Board: it returns a BoardState, so there is
  //nothing for undo_move() to have to restore and nothing stale for the caller to read.
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
  const BoardState bs = isCheckMateStaleMate(chess_board);
  int cp = NO_MATE_SCORE, terminal = 0;
  if (bs.mate) {
    cp = static_cast<int>(-MATE_SCORE + iter * 100); //the child's side to move is mated
    terminal = 1;
  } else if (bs.stalemate) {
    cp = 0;
    terminal = 2;
  } else {
    if (bs.check) terminal = -1;
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
        //A TABLEBASE VERDICT IS A PROOF, and until now the search had no way to say so.
        //
        //This is what made ProveTerminals inert. The only exact verdict the engine stamped on a
        //node was checkmate, so on the mistakes test set the induction had almost nothing to
        //compose -- instrumented over 21 searches it proved a median of about 60 nodes out of
        //600,000 simulations, roughly one node in ten thousand, and the solve rate did not move
        //(36.1% against 33.9%, zero positions converted from never solved to always solved).
        //Meanwhile the same searches were taking hundreds of thousands of tablebase hits, every
        //one of them exact and every one of them recorded as nothing more than a large centipawn
        //number that Q then averaged like any opinion.
        //
        //Only wdl 4 and 0 are proofs. 1, 2 and 3 are blessed loss, draw and cursed win -- won or
        //lost on the board but drawn by the fifty-move rule -- and the engine already scores all
        //three as 0, which is the honest answer and not something to prove a win from.
        if (prove_terminals && (wdl == 4 || wdl == 0)) {
          terminal = (terminal & TERMINAL_KIND_MASK) | (wdl == 4 ? PROVEN_WIN : PROVEN_LOSS);
          tb_seeds.fetch_add(1, std::memory_order_relaxed);
        }
      }
      //Counted but never logged here: this site has always failed silently, and the count is what
      //tells you a partial set is being probed for tables it does not have.
      else tb_probe_misses.fetch_add(1, std::memory_order_relaxed);
    }
  }
  undo_move(chess_board, move, state);
  return std::make_pair(cp, terminal);
}

//=========================== LEAF ALPHA-BETA ==================================================
//
//WHAT THIS IS FOR. Everything measured on 4-6 October says the failures are not a ranking problem
//and not a prior problem: the failure rate tracks the number of plies the engine's own evaluator
//needs before it prefers the right move -- 3.5% at one ply, 22.6% past four -- and the visits a
//non-leading move ever receives are capped at C*P*sqrt(D)/gap, tens to thousands where the deep
//refutations need tens of thousands. The allocation cannot be loosened: MinVisitK, which forces a
//share onto the leading candidates, made the deep bucket 3.5x worse, because the concentration IS
//the depth mechanism. And the depth cannot be bought from the model, because a value head scores a
//position and a combination is a property of a sequence.
//
//So the remaining move is to resolve the plies where they are cheap. Alpha-beta reaches depth with
//an effective branching factor near 2 against MCTS's 12.5, and the reason is not narrower move
//lists but PROOF: a beta cutoff makes every remaining sibling provably irrelevant, which averaging
//can never do. This puts a bounded alpha-beta search at the leaf, so a leaf knows what is four
//plies beneath it and the MCTS tree only has to walk one or two plies to see a five-ply refutation.
//
//The engine already does a degenerate version of this -- the capture-only extension in
//process_check() and the 1-ply minimax in node_minimax -- so this is a change of degree.
//
//THE PRICE, and it is the whole experiment: a leaf costs its alpha-beta subtree instead of one net
//evaluation, so simulations drop by that factor. Whether a 4-ply-resolved leaf is worth the ~20-40
//static leaves it displaces is what the deep set has to answer. LeafDepth 0 is off and is the
//default.
//
//WHAT IT DELIBERATELY DOES NOT HAVE. No transposition table (shared across four threads it would
//need its own correctness argument, and cutoffs alone are what the branching-factor claim rests
//on), no null-move and no late-move reductions (each of those is a separate confound, and the
//measurement is about whether leaf resolution helps at all), and no repetition or fifty-move
//detection inside the leaf search -- within four plies a repetition needs the path history this
//function does not carry, and the surrounding engine already scores those at the node.
int64_t leaf_depth = LEAF_DEPTH;
std::atomic<uint64_t> leaf_ab_calls{0}, leaf_ab_nodes{0}, leaf_ab_cutoffs{0}, leaf_ab_blind{0};
//The selective leaf search's own counters: searches actually run, and children flagged. The two
//differ because a flagged child that is terminal, in check or inside tablebase range is not
//resolved, and the gap between them is how one sees whether the threshold is doing what the
//offline measurement assumed.
int64_t err_leaf_depth = ERR_LEAF_DEPTH;
int64_t err_threshold  = ERR_THRESHOLD;
std::atomic<uint64_t> err_leaf_calls{0}, err_leaf_flagged{0};

//Legal moves for the leaf search, the same staged generation the rest of the engine uses.
//
//captures_only is the quiescence horizon and is IGNORED IN CHECK: standing pat in check is wrong,
//and evaluate_nnue() cannot be called on a checked position at all, so the evasions are always
//generated in full. One kingMoves() call therefore reports both the move list and the check.
static int leaf_moves(Board& b, Move * out, bool captures_only, bool& in_check) {
  auto [king_moves, pinned, pinning, checkers, kingSquare] = kingMoves(b);
  in_check = (checkers != 0);
  if (in_check) captures_only = false;
  const uint64_t them = b.side[b.sideToMove == ColorWhite ? ColorBlack : ColorWhite];
  int n = 0;
  Move move;
  move.src = kingSquare;
  move.promoType = PieceTypeNone;
  while (king_moves) {
    move.dst = popLSB(king_moves);
    if (!captures_only || ((1ULL << move.dst) & them)) out[n++] = move;
  }
  if (bitCount(checkers) <= 1) {
    auto [check_mask, ep_mask] = checkers ? checkMask(b, kingSquare, checkers)
                                          : std::make_pair(0xffffffffffffffffULL, 0ULL);
    for (PieceType pt = Queen; pt >= Pawn; --pt) {
      uint64_t occupations = b.side[b.sideToMove] & b.pieceTypes[pt - 1];
      while (occupations) {
        move.src = popLSB(occupations);
        uint64_t moves = piece_moves(b, pt, move.src, kingSquare, pinned, pinning, check_mask, ep_mask);
        while (moves) {
          move.dst = popLSB(moves);
          PieceType startPiece = PieceTypeNone, endPiece = PieceTypeNone;
          if (promoMove(b, move)) { startPiece = Knight; endPiece = Queen; }
          //A promotion changes the material on the board as much as a capture does, so it stays in
          //the capture-only list whether or not it takes anything. En passant does not land on an
          //occupied square and so is missed by this test; that is the usual compromise for a
          //capture horizon and it loses nothing a quiet evaluation would have caught.
          const bool noisy = (((1ULL << move.dst) & them) != 0) || startPiece != PieceTypeNone;
          for (move.promoType = startPiece; move.promoType <= endPiece; ++move.promoType)
            if (!captures_only || noisy) out[n++] = move;
          move.promoType = PieceTypeNone;
        }
      }
    }
  }
  return n;
}

//ORDERING IS THE WHOLE COST. Alpha-beta visits about the square root of the full tree only when the
//best move is searched FIRST at every node; with a bad order the cutoffs do not happen and it
//degenerates towards full width. Measured on one deep position, MVV-LVA alone gave 84,337 nodes for
//a 4-ply leaf -- a branching factor near 8 per ply, against the 1.83 abprobe.cpp measures with the
//full set of orderings. None of what is added here prunes anything, so none of it can change the
//value the leaf search returns; it only changes how long that value takes.
#define LEAF_MAX_PLY 64
struct LeafOrderCtx {
  //Best move found at each ply, written as the search goes and read by the next iteration of the
  //deepening -- the job a transposition table would do, without the table. Per call, on the stack
  //of the thread that is searching, so four threads need no agreement about it.
  Move best[LEAF_MAX_PLY];
  bool has_best[LEAF_MAX_PLY];
  //Two killers per ply: quiet moves that caused a cutoff elsewhere at the same distance from the
  //leaf. They cost two slots and catch the common case of one refutation answering many moves.
  Move killer[LEAF_MAX_PLY][2];
  bool has_killer[LEAF_MAX_PLY][2];
  LeafOrderCtx() {
    for (int i = 0; i < LEAF_MAX_PLY; ++i) {
      has_best[i] = false; has_killer[i][0] = false; has_killer[i][1] = false;
    }
  }
};

static inline bool leaf_same(const Move& a, const Move& b) {
  return a.src == b.src && a.dst == b.dst && a.promoType == b.promoType;
}

static const int LEAF_PIECE_VALUE[7] = {0, 100, 320, 330, 500, 900, 0}; //index by PieceType

static void leaf_order(Board& b, Move * mv, int n, const LeafOrderCtx& lc, int ply) {
  int score[256];
  const bool have_ply = ply < LEAF_MAX_PLY;
  for (int i = 0; i < n; ++i) {
    const int victim = b.piecesOnSquares[mv[i].dst] == PieceNone
                       ? 0 : LEAF_PIECE_VALUE[b.piecesOnSquares[mv[i].dst] & 7];
    const int mover  = LEAF_PIECE_VALUE[b.piecesOnSquares[mv[i].src] & 7];
    int s = victim * 16 - mover
          + (mv[i].promoType != PieceTypeNone ? LEAF_PIECE_VALUE[mv[i].promoType] * 16 : 0);
    if (have_ply) {
      //Ahead of every capture, because it is the move the previous, shallower iteration actually
      //chose here and so is the best single guess available.
      if (lc.has_best[ply] && leaf_same(mv[i], lc.best[ply])) s += 1 << 20;
      else if (lc.has_killer[ply][0] && leaf_same(mv[i], lc.killer[ply][0])) s += 1 << 14;
      else if (lc.has_killer[ply][1] && leaf_same(mv[i], lc.killer[ply][1])) s += 1 << 13;
    }
    score[i] = s;
  }
  //Insertion sort: n is about 35 and this runs at every node of the leaf search, so the constant
  //matters more than the order of growth.
  for (int i = 1; i < n; ++i) {
    const Move m = mv[i]; const int s = score[i];
    int j = i - 1;
    while (j >= 0 && score[j] < s) { mv[j + 1] = mv[j]; score[j + 1] = score[j]; --j; }
    mv[j + 1] = m; score[j + 1] = s;
  }
}

//A quiet move that cut: remember it for the next node at this ply.
static inline void leaf_store_killer(LeafOrderCtx& lc, const Move& m, int ply, bool quiet) {
  if (!quiet || ply >= LEAF_MAX_PLY) return;
  if (lc.has_killer[ply][0] && leaf_same(m, lc.killer[ply][0])) return;
  lc.killer[ply][1] = lc.killer[ply][0]; lc.has_killer[ply][1] = lc.has_killer[ply][0];
  lc.killer[ply][0] = m;                 lc.has_killer[ply][0] = true;
}

#define LEAF_QS_CHECK_EXT 3   //check extensions allowed inside the capture search

//The capture horizon. Fail-soft, centipawns, from b.sideToMove's perspective.
static int leaf_qs(Board& b, NNUEContext& ctx, int alpha, int beta, int ext, int ply, LeafOrderCtx& lc) {
  leaf_ab_nodes.fetch_add(1, std::memory_order_relaxed);
  Move mv[256];
  bool in_check = false;
  const int n = leaf_moves(b, mv, true, in_check);
  if (n == 0) {
    //No evasion in check is mate. No capture out of check is simply a quiet position, which is
    //what the stand-pat score is for.
    if (in_check) return -MATE_SCORE + ply;
    return static_cast<int>(evaluate_nnue(b, ctx, in_check) * 100.0);
  }
  int best;
  if (in_check) {
    //OUT OF CHECK EXTENSIONS, and the net cannot score a checked position at all. The first version
    //returned the window's alpha here, which was wrong in a way worth recording: it made this leaf's
    //value depend on the window it happened to be searched with, so the leaf search was not a
    //function of the position. That also makes it unverifiable -- a cutoff-free negamax searches with
    //a full window and would disagree by construction -- which is how the problem was found.
    //
    //The answer instead is the 1-ply static minimax over the evasions, which is exactly what the
    //rest of the engine does in check (node_minimax, and process_check() before it), and depends on
    //nothing but the position.
    if (ext <= 0) {
      leaf_ab_blind.fetch_add(1, std::memory_order_relaxed);
      int mm = 0;
      bool any = false;
      for (int i = 0; i < n; ++i) {
        StateInfo st;
        auto [dp, dts] = accumulator_stack_push(ctx);
        do_move_dp(b, mv[i], st, dp, dts);
        bool child_check = false;
        Move tmp[256];
        const int cn = leaf_moves(b, tmp, false, child_check);
        int v;
        //No reply to the evasion: the evasion mates them, or stalemates them.
        if (cn == 0)          v = child_check ? MATE_SCORE - ply : 0;
        //An evasion that gives check back cannot be scored by the net either, and there is no ply
        //left to spend resolving it. Scored as 0, which is a guess -- it is the one remaining
        //inexactness in the leaf search, and leaf_ab_blind counts how often it is reached.
        else if (child_check) v = 0;
        else                  v = -static_cast<int>(evaluate_nnue(b, ctx, child_check) * 100.0);
        undo_move(b, mv[i], st);
        accumulator_stack_pop(ctx);
        if (!any || v > mm) { mm = v; any = true; }
      }
      return mm;
    }
    best = -MATE_SCORE * 2;   //no stand-pat in check
    --ext;
  } else {
    best = static_cast<int>(evaluate_nnue(b, ctx, in_check) * 100.0);
    if (best >= beta) { leaf_ab_cutoffs.fetch_add(1, std::memory_order_relaxed); return best; }
    if (best > alpha) alpha = best;
  }
  leaf_order(b, mv, n, lc, ply);
  for (int i = 0; i < n; ++i) {
    StateInfo st;
    auto [dp, dts] = accumulator_stack_push(ctx);
    do_move_dp(b, mv[i], st, dp, dts);
    const int sc = -leaf_qs(b, ctx, -beta, -alpha, ext, ply + 1, lc);
    undo_move(b, mv[i], st);
    accumulator_stack_pop(ctx);
    if (sc > best) best = sc;
    if (best > alpha) alpha = best;
    if (alpha >= beta) { leaf_ab_cutoffs.fetch_add(1, std::memory_order_relaxed); break; }
  }
  return best;
}

//Plain fail-soft negamax with alpha-beta. Centipawns, from b.sideToMove's perspective.
static int leaf_ab(Board& b, NNUEContext& ctx, int depth, int alpha, int beta, int ply, int ply_cap,
                   LeafOrderCtx& lc) {
  leaf_ab_nodes.fetch_add(1, std::memory_order_relaxed);
  Move mv[256];
  bool in_check = false;
  const int n = leaf_moves(b, mv, false, in_check);
  if (n == 0) return in_check ? -MATE_SCORE + ply : 0;   //mate, or stalemate
  //The check extension, bounded by ply_cap so a long forcing sequence cannot make the leaf search
  //unbounded. It also keeps evaluate_nnue() away from a checked position here: a checked node is
  //never at depth 0 after this line, so it never reaches the horizon below.
  if (in_check && ply < ply_cap) ++depth;
  if (depth <= 0) return leaf_qs(b, ctx, alpha, beta, LEAF_QS_CHECK_EXT, ply, lc);
  leaf_order(b, mv, n, lc, ply);
  int best = -MATE_SCORE * 2;
  for (int i = 0; i < n; ++i) {
    StateInfo st;
    const bool quiet = b.piecesOnSquares[mv[i].dst] == PieceNone
                       && mv[i].promoType == PieceTypeNone;
    auto [dp, dts] = accumulator_stack_push(ctx);
    do_move_dp(b, mv[i], st, dp, dts);
    const int sc = -leaf_ab(b, ctx, depth - 1, -beta, -alpha, ply + 1, ply_cap, lc);
    undo_move(b, mv[i], st);
    accumulator_stack_pop(ctx);
    if (sc > best) best = sc;
    if (best > alpha) {
      alpha = best;
      if (ply < LEAF_MAX_PLY) { lc.best[ply] = mv[i]; lc.has_best[ply] = true; }
    }
    if (alpha >= beta) {
      leaf_ab_cutoffs.fetch_add(1, std::memory_order_relaxed);
      leaf_store_killer(lc, mv[i], ply, quiet);
      break;
    }
  }
  return best;
}

//ITERATIVE DEEPENING inside the leaf. Each shallower pass is cheap and leaves behind the best move
//at every ply, which the next pass searches first -- so the deep pass gets its cutoffs on the first
//move at most nodes instead of after half of them. The total is dominated by the last iteration,
//which is why re-searching from depth 1 every time is not the waste it looks like.
static int leaf_search(Board& b, NNUEContext& ctx, int depth) {
  LeafOrderCtx lc;
  //Depth 0 is the quiescence alone -- the capture horizon with no full-width ply above it. The
  //engine never asks for it (LeafDepth 0 is off and takes the old path entirely), but a caller
  //measuring the leaf search on its own needs it to mean something rather than 0.
  if (depth <= 0) return leaf_qs(b, ctx, -MATE_SCORE * 2, MATE_SCORE * 2, LEAF_QS_CHECK_EXT, 0, lc);
  int sc = 0;
  for (int d = 1; d <= depth; ++d)
    sc = leaf_ab(b, ctx, d, -MATE_SCORE * 2, MATE_SCORE * 2, 0, depth * 3, lc);
  return sc;
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
  //THE SELECTIVE LEAF SEARCH's trunk output for this node, read by every err_score() below.
  //
  //Only on the path that evaluates children statically. In POLICY_FULL there are no static child
  //values, so there is no error to predict and nothing to replace -- the children's values come
  //from their own expansions later. And not in check: a checked node's children are its evasions,
  //whose values already come from process_check()'s minimax rather than from a static evaluation.
  const bool use_err = err_head_enabled && err_leaf_depth > 0 && !checkers &&
                       !(policy_enabled && policy_mode == POLICY_FULL);
  float ectx[POLICY_MAX_H2];
  bool  eflip = false;
  //The spatial forward's output for this position, set once the legal move list is known (see THE
  //SPATIAL TERM below) and read by every policy_score() call in this function. Declared here so
  //evaluate() captures it; it is null whenever the net has no spatial term.
  const PolicySpatialScratch* sp_buf = nullptr;
  if (use_policy || use_err) {
    unsigned char feat[POLICY_MAX_IN];
    //Free at a search node: the accumulator this reads is the one do_move_dp() has been
    //maintaining incrementally down the path. ONE extraction serves both heads -- they were
    //trained on the same vector, which is the whole reason the error head is affordable.
    nnue_features(chess_board, ctx, feat);
    if (use_policy) {
      policy_context(policy_net, feat, pctx);
      flip = (chess_board.sideToMove == ColorBlack);
    }
    if (use_err) {
      err_context(err_head, feat, ectx);
      eflip = (chess_board.sideToMove == ColorBlack);
    }
  }

  //Cancels get_prob()'s own division so the head's logits reach the softmax at the scale
  //they were trained on.
  const double pscale = temperature / policy_temperature;

  //The gate needs the whole move list before any of it is evaluated, so when it is active
  //emit() only collects. At the default 1.0 it evaluates inline exactly as before, and no
  //vector is allocated -- the no-gate path is unchanged.
  const bool gating = use_policy && probability_mass < 1.0;
  //COLLECT BEFORE SCORING when the net carries the legality term, because that term is a function
  //of the WHOLE legal move set: it adds Wl * (mean embedding row over the legal moves) to ctx, so
  //it has to be applied once, with every move known, before a single move is scored. Scoring a
  //move against a partially-accumulated mean would give it a different value from the same move
  //scored later.
  //The SPATIAL term needs the whole set for the same reason: planes 12 and 13 mark every square a
  //legal move starts from or lands on, so they cannot be built until every move is known.
  const bool need_legal_set = use_policy && (policy_net.has_wl || policy_net.spatial.loaded);
  const bool collect_first  = gating || need_legal_set;
  std::vector<Move> legal;
  if (collect_first) legal.reserve(64);

  auto evaluate = [&](Move& m) {
    uint64_t child_hash = 0;
    //Read src/dst BEFORE the move is made. make_move() and make_move_policy() take Move& and
    //hand it to do_move_dp()/do_move(), which REWRITE move.dst for castling: in Chess960 the
    //generator produces the king-takes-rook form (dst = the rook's square) and do_move rewrites
    //it to the standard king square, g8/c8 (board.cpp: "if (board.isChess960) move.dst =
    //castlingKingSquare[...]"). Storing the rewritten value put the STANDARD notation into the
    //edge, so the engine answered a 960 game with "e8g8" where lichess wanted "e8f8" and
    //rejected it with {"error":"Piece on e8 cannot move to g8"} -- every castling move in every
    //960 game. perft.cpp documents this trap for exactly the same reason.
    //
    //Harmless in standard chess, which is why it survived: there isChess960 is false, the
    //generator already emits g1/c1, and do_move's rewrite is skipped, so dst never changes.
    const Square   pre_dst  = static_cast<Square>(m.dst);
    //And the MOVING PIECE, for the same reason and one more: after make_move() the from-square is
    //empty, so reading it later yields PieceTypeNone. A piece-indexed policy net keys its move
    //table on this, so getting it wrong scores every move against an unrelated embedding row.
    const int      pre_pt   = chess_board.piecesOnSquares[m.src] & 7;
    const int      move_idx = (m.promoType << 12) | (m.src << 6) | m.dst;
    if (use_policy && policy_mode == POLICY_FULL) {
      auto [cp, terminal] = make_move_policy(chess_board, board_hash, m, child_hash, pos_history, iter);
      move_evals.push_back({policy_score(policy_net, pctx, pre_pt, m.src, pre_dst, flip, sp_buf) * pscale,
                            move_idx, cp, terminal, child_hash});
    } else {
      //WHICH CHILDREN GET THE PLIES. A fixed centipawn threshold on the predicted error, not a
      //per-node top-k: the offline measurement that this option exists to reproduce flagged the
      //worst tenth of moves by a GLOBAL threshold, so a node full of quiet moves resolves none of
      //them and a node full of sharp ones resolves most. A per-node share would spend the same
      //budget everywhere, which is exactly the allocation the deciles say is wrong.
      //
      //It also means no move list has to be collected first, so this adds nothing to the
      //expansion's structure -- one dot product per move against the trunk output above.
      int child_depth = 0;
      if (use_err &&
          err_score(err_head, ectx, m.src, pre_dst, eflip) * 100.0f >= (float)err_threshold) {
        err_leaf_flagged.fetch_add(1, std::memory_order_relaxed);
        child_depth = static_cast<int>(err_leaf_depth);
      }
      auto [res, terminal] = make_move(chess_board, board_hash, m, ctx, child_hash, pos_history, iter, child_depth);
      //get_prob() divides by `temperature`, so push temperature * (the logit we want).
      const double prior = use_policy
          ? temperature * policy_blend_scale
                * (policy_blend * policy_score(policy_net, pctx, pre_pt, m.src, pre_dst, flip, sp_buf) / policy_temperature
                   + (1.0 - policy_blend) * res / temperature)
          : res;
      move_evals.push_back({prior, move_idx,
                            static_cast<int>(-res * 100), terminal, child_hash});
    }
  };
  auto emit = [&](Move& m) { if (collect_first) legal.push_back(m); else evaluate(m); };

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
  //The legality term, applied to ctx once the full move list is known and BEFORE the gate, which
  //ranks by policy score and so must see the corrected scores.
  //
  //DEDUPLICATED, because the trainer's legal list holds one entry per piece/from/to while this list
  //holds four for every promotion (knight, bishop, rook, queen), all mapping to the same row. Without
  //it a promotion counts four times in the mean the term is built from -- a different function from
  //the one trained, in exactly the positions where a promotion is available. Order does not matter
  //to a mean, so sort-and-unique is enough, and on ~35 rows it is noise beside an expansion.
  if (need_legal_set && policy_net.has_wl && !legal.empty()) {
    std::vector<size_t> rows;
    rows.reserve(legal.size());
    for (const Move& m : legal)
      rows.push_back(policy_row(policy_net, chess_board.piecesOnSquares[m.src] & 7,
                                m.src, m.dst, flip));
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    policy_apply_legal_bias(policy_net, pctx, rows.data(), (int)rows.size());
  }

  //THE SPATIAL TERM (formats 7 and 8). Builds the 8x8 planes from the board and the legal list,
  //runs the convolution once, and leaves u and v in sp_buf so every move below scores
  //dot(u[from], v[to]) on top of its embedding score. Before the gate, which ranks by that score.
  //
  //The output buffer is per thread, which is safe because a policy-using expansion never runs inside
  //another one: the only recursion is process_check() expanding a position IN CHECK, and use_policy
  //is false in check. That invariant is not enforced by anything else, so it is checked here -- if
  //this thread's buffer is already in use by an outer frame, this frame takes its own instead of
  //overwriting the outer frame's u and v halfway through its scoring.
  static thread_local PolicySpatialScratch sp_tls;
  static thread_local int sp_tls_busy = 0;
  struct SpBusy { int& busy; bool held = false; ~SpBusy() { if (held) --busy; } } sp_hold{sp_tls_busy};
  std::unique_ptr<PolicySpatialScratch> sp_own;
  if (use_policy && policy_net.spatial.loaded && !legal.empty()) {
    PolicySpatialScratch* buf = &sp_tls;
    if (sp_tls_busy) { sp_own = std::make_unique<PolicySpatialScratch>(); buf = sp_own.get(); }
    else             { ++sp_tls_busy; sp_hold.held = true; }
    //Consumed by the forward straight away, with nothing in between that can recurse, so these
    //can be per thread too.
    static thread_local unsigned char planes[26 * 64];
    static thread_local std::vector<int> sq_from, sq_to, sq_pt;
    sq_from.clear(); sq_to.clear(); sq_pt.clear();
    for (const Move& m : legal) {
      sq_from.push_back(flip ? (m.src ^ 56) : m.src);
      sq_to.push_back(flip ? (m.dst ^ 56) : m.dst);
      sq_pt.push_back(chess_board.piecesOnSquares[m.src] & 7);
    }
    policy_build_planes(chess_board, policy_net.spatial, sq_from.data(), sq_to.data(), sq_pt.data(),
                        (int)legal.size(), planes);
    policy_spatial_forward(policy_net.spatial, planes, *buf);
    sp_buf = buf;
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
      logit[i] = policy_score(policy_net, pctx, chess_board.piecesOnSquares[legal[i].src] & 7,
                              legal[i].src, legal[i].dst, flip, sp_buf) / policy_temperature;
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
  if (collect_first)
    for (Move& m : legal) evaluate(m);

  //move_evals holds one entry per move this function emitted, with promotions already expanded into
  //their four pieces -- so it IS the legal move count, and a local one. It used to read
  //board.num_moves, which piece_moves() had been accumulating as a side effect during the
  //generation loop above: a count arriving through the Board from three different functions.
  if (move_evals.empty()) {
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
  //Discard the ordering inside the top k, keeping the total mass; see flatten_prior().
  if (flatten_topk > 1) flatten_prior(move_evals);
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
    scorecp = static_cast<int>(evaluate_nnue(chess_board, ctx, checkers != 0) * 100.0);
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
  //LEAF ALPHA-BETA replaces this leaf's value with a bounded search from it. See the block above
  //leaf_moves() for why, and for what it costs.
  //
  //Not in check: a checked node's value already comes from the 1-ply minimax over its evasions
  //(evaluate_nnue() cannot be called on it), and leaf_ab() would only repeat that work one ply
  //later. Not in tablebase range either: the leaf search does not probe, so under TB_LARGEST it
  //would replace an exact verdict with an estimate -- position_eval()'s probe is strictly better
  //there, and keeping those positions on the old path also keeps endgames out of the comparison.
  if (leaf_depth > 0 && !checkers &&
      bitCount(chess_board.side[ColorWhite] | chess_board.side[ColorBlack]) > TB_LARGEST) {
    leaf_ab_calls.fetch_add(1, std::memory_order_relaxed);
    int ab = leaf_search(chess_board, ctx, static_cast<int>(leaf_depth));
    //Kept from the static path: a mate or a tablebase verdict found while making the children is
    //exact, and we are never obliged to play into it, so max() is the right combination. The
    //tablebase case is the one that matters -- leaf_ab() cannot find it on its own.
    for (const auto& ev : move_evals) {
      const int ccp = std::get<2>(ev);
      const int t   = std::get<3>(ev);
      if (t == 1 || (t == 0 && ccp != NO_MATE_SCORE)) ab = std::max(ab, -ccp);
    }
    scorecp = std::clamp(ab, -MATE_SCORE, MATE_SCORE);
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
//THE MINIMAX BACKUP. See minimax_weight.
//
//A node's minimax value is the best its own side can reach: the maximum over its children of the
//NEGATION of theirs, since a child's value is stated from the child's side to move. Recomputed in
//full rather than kept as a running maximum, because a max that only ever rises is a high-water
//mark -- when a child's value falls, the parent's has to fall with it, and an incremental maximum
//cannot do that. One pass over the children per node on the simulation's path.
static void mm_backup(MCTSNode * n) {
  const int nc = n->num_children.load(std::memory_order_acquire);
  if (nc <= 0) return;
  int  best = 0;
  bool any  = false;
  for (int i = 0; i < nc; ++i) {
    MCTSNode * c = search.arena.child_at(n, i);
    if (!c) continue;
    const int16_t v = c->mm.load(std::memory_order_relaxed);
    if (v == MM_NONE) continue;
    const int cand = -(int)v;
    if (!any || cand > best) { best = cand; any = true; }
  }
  if (!any) return;
  n->mm.store((int16_t)std::clamp(best, -32767, 32767), std::memory_order_relaxed);
  mm_updates.fetch_add(1, std::memory_order_relaxed);
}

//Stamp an exact verdict on a node without disturbing its kind. The kind never changes once the node
//has been expanded, so a plain load-compose-store is enough; two threads proving the same node
//compose the same word.
static void set_proof(MCTSNode * n, int proof) {
  if (proof == PROVEN_WIN) proofs_win.fetch_add(1, std::memory_order_relaxed);
  else                     proofs_loss.fetch_add(1, std::memory_order_relaxed);
  const int t = n->terminal.load(std::memory_order_relaxed);
  if (proof_bits(t) & proof) return;                      //already carries it
  //`t & 0xff` is what normalises the legacy -1 of an in-check node into 0xff, so the word it
  //composes is non-negative and proof_bits() will read it. See terminal_kind().
  n->terminal.store((t & TERMINAL_KIND_MASK) | proof, std::memory_order_release);
}

//BACKWARD INDUCTION UP THE PATH THIS SIMULATION TOOK. See prove_terminals.
//
//Two rules, and they are the whole of it. A node whose side to move can reach a position that is
//LOST for the side to move there has a winning move, so it is won. A node is lost only when EVERY
//move reaches a position won for the opponent -- one child that is drawn, or merely unproven, is
//enough to leave the question open.
//
//The walk stops at the first node it cannot prove, so in a quiet position it does nothing beyond
//one comparison. The "every child" scan is the only part that costs anything, and it runs only when
//the child just visited was itself proven won, which is rare.
static void prove_up(const std::vector<MCTSNode *>& path) {
  for (size_t i = path.size(); i-- > 1; ) {
    MCTSNode * parent = path[i - 1];
    MCTSNode * child  = path[i];
    const int ct = child->terminal.load(std::memory_order_acquire);
    //A SEED is an exact verdict that did not come from the induction itself -- today that means
    //checkmate and nothing else. Counted separately, because if the seeds are zero the induction
    //has nothing to compose and the whole option is inert for reasons that have nothing to do with
    //whether propagating proofs is a good idea.
    if (terminal_kind(ct) == 1 || proof_bits(ct)) proof_seeds.fetch_add(1, std::memory_order_relaxed);
    if (proven_loss(ct)) { set_proof(parent, PROVEN_WIN); continue; }
    if (!proven_win(ct)) break;          //nothing exact here, and nothing above can follow
    const int nc = parent->num_children.load(std::memory_order_acquire);
    if (nc <= 0) break;
    bool all_won = true;
    for (int j = 0; j < nc; ++j) {
      MCTSNode * c = search.arena.child_at(parent, j);
      //An unexpanded or missing child is an open question, not a win for the opponent.
      if (!c || !proven_win(c->terminal.load(std::memory_order_relaxed))) { all_won = false; break; }
    }
    if (!all_won) break;
    set_proof(parent, PROVEN_LOSS);
  }
}

void mcts_search(ThreadParams& params, NNUEContext& ctx) {
  //One simulation per call, counted at entry so the early return below is still counted -- it
  //consumed time and must appear in the denominator of nps.
  search_simulations.fetch_add(1, std::memory_order_relaxed);
  std::vector<MCTSNode *> path;  // Track the path from root to leaf
  path.reserve(chessEngine.depth);
  int terminal = 0;
  int node_terminal_word = 0;
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
    node = search.arena.child_at(node, idx);   //the parent is the node being left; evaluated before the assignment
    //init and take edge's move that leads to the child node
    //The raw word carries the proof bits; `terminal` itself stays the kind, so every test below
    //reads exactly what it always did. See terminal_kind().
    node_terminal_word = node->terminal.load(std::memory_order_acquire);
    terminal = terminal_kind(node_terminal_word);
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
    //Now a real count rather than 0-or-1. The test below is deliberately unchanged: it still fires
    //on the FIRST recurrence, because with creatica's priors a threefold-correct rule would need
    //the search to see two full cycles ahead and it will not, so the opponent could simply force
    //the draw. The count is here for the forcing test to reason with, not to relax the threshold.
    int global_count = rep_count(sim_zh.hash); //actual positions that have occured in the game
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
  bool informed = true;      //see the note at the leaf below
  if (terminal <= 0) {
    //A full tree stops the search GROWING, not the search THINKING.
    //
    //The workers' loop used to carry `hash_full < 1000` as a termination condition, so when the
    //tree filled every worker exited and the engine played whatever it had -- in a live blitz
    //game it spent 2 s on a move where its neighbours took 4-6. It did not merely stop
    //expanding, it stopped playing chess. Tree reuse exposed this: before it, the tree was
    //rebuilt every move and never came near the ceiling; with reuse it grew monotonically,
    //because a good prior concentrates the search on the move it then plays and almost nothing
    //ever becomes garbage for the collector to reclaim.
    //
    //That monotonic growth is now bounded: gc() evicts down to GC_EVICT_SOFT after every move
    //rather than keeping everything it can reach, so a full tree is a condition the engine
    //recovers from between moves instead of one it stays in for the rest of the game. This
    //branch therefore covers a single search running out of room, not a permanent state. What
    //the permanent state did to play, and why reachability was the wrong retention rule, is
    //written up in gc().
    //
    //Taking the already-evaluated branch below instead keeps visits and backpropagation going
    //over the tree that exists. That is degraded -- no new nodes -- but it still sharpens the
    //statistics of the moves already under consideration, which is enormously better than
    //abandoning the search.
    //DID THIS SIMULATION LEARN ANYTHING? Expanding a leaf produces new evaluations, and a terminal
    //or tablebase value is exact -- both inform. Re-reading a stored evaluation because the node
    //could not be expanded informs nothing: it backpropagates a number already counted. Decided
    //once, here at the leaf, and applied to the whole path, because a simulation that ends on a
    //hollow leaf taught the nodes it passed through nothing either.
    const bool tree_full = hash_full.load(std::memory_order_relaxed) >= 1000;
    if (!tree_full && node->expanding.exchange(1, std::memory_order_acquire) == 0) { //the leaf node is gated only for expansion
                                //nodes traversed in the selection phase are not leaf nodes, i.e. nodes without children
                                //if the gate is already taken, another thread is expanding this node right now
                                //(unlike try_lock(), an atomic exchange cannot fail spuriously or be blocked by readers)
      if (terminal == -1) { //node in check
        //NOT a warning, and not impossible. The old text claimed process_check() made this
        //unreachable; it fired about 350,000 times per bot in an overnight run, each time taking
        //a mutex and an fflush() inside the search.
        //
        //Where it comes from, confirmed by logging the position: TABLEBASE nodes. position_eval()
        //calls process_check() for an in-check child only on the paths that do not have an exact
        //answer -- when the probe is out of range, or when it FAILS. On a successful probe it
        //takes the WDL result and returns, so the child is created by make_child() with a real cp
        //and no children, and it is in check. Every hit sampled looked like this:
        //
        //   8/6K1/8/3k1nP1/8/8/8/8 w   pieces 4  castling 0  nodeCp 0      terminal -1
        //   8/4r1K1/8/5kP1/8/8/8/8 w   pieces 4  castling 0  nodeCp -2000  terminal -1
        //
        //-- four pieces, no castling rights, cp already set. process_check() then declines to
        //expand it a second time, because its "already evaluated" branch returns the stored cp.
        //
        //That is CORRECT. A tablebase result is exact; searching underneath it can only lose
        //information, and checks are ubiquitous in a four-piece endgame, which is why the count
        //is so large. The node is expanded on its first visit here anyway, so nothing is stuck.
        //Kept as a rate-limited counter purely because a sharp change in the rate would mean
        //something else had started producing unexpanded in-check leaves.
        {
          static std::atomic<uint64_t> in_check{0};
          const uint64_t k = in_check.fetch_add(1, std::memory_order_relaxed);
          if ((k % 1000000) == 0)
            log_file("mcts_search() note: leaf node in check, %llu so far (expected: tablebase nodes)\n",
                     (unsigned long long)(k + 1));
        }
      } else {
        //`terminal == -1` is the in-check case and is handled in the branch above, so this is the
        //quiet one. Passed rather than hard-coded false for the same reason as above.
        evaluate_nnue(sim_board, ctx, terminal == -1); //re-evaluated to keep later evals incremental
      }
      result = eval_and_expand(node, sim_board, sim_zh, ctx, pos_history, 0);
      result = tanh(result / eval_scale);
      node->expanding.store(0, std::memory_order_release);
    } //end of if (node.expanding.exchange(1, acquire) == 0)
    else {
      //ONLY A FULL TREE MEANS NOTHING CAN BE LEARNED HERE.
      //
      //This branch is reached for two quite different reasons and I originally treated them the
      //same, which was wrong and cost real games. Losing the try-lock means ANOTHER THREAD IS
      //EXPANDING THIS NODE RIGHT NOW and will finish in microseconds -- with four threads MCTS
      //deliberately converges on the same promising leaves, which is the entire reason virtual
      //loss exists, so contention here is normal and frequent rather than exceptional. The value
      //backpropagated is a real evaluation and counting it is what the engine has always done.
      //
      //Marking those uninformed thinned the statistics on exactly the busiest lines -- and because
      //the flag applies to the WHOLE path, one contended leaf discarded the backpropagation for
      //every ancestor up to the root. Q then saturated on far fewer samples: measured against the
      //control arm in the same match, cp 2799 where the control said 2039 from the same one-ply
      //value of 830.
      //
      //A full tree is the opposite case: the node can NEVER be expanded, so every visit re-reads
      //the same number forever. That, and only that, is a visit which teaches nothing.
      if (tree_full) { informed = false; uninformed_sims.fetch_add(1, std::memory_order_relaxed); }
      scorecp = node->cp.load(std::memory_order_relaxed);
      if (scorecp == NO_MATE_SCORE) { //node has not been evaluated yet, return without a backprop (a bit of a waste)
        //NEVER printf() from the search. stdout is the UCI channel: this line goes into the
        //protocol stream, and worse, this branch can fire on EVERY simulation. Two threads
        //spinning on it fill the pipe, the engine blocks on write(), the search thread stops,
        //stop() never returns, readyok never arrives, and the driver declares the engine dead
        //after 60 s. That is the stall.
        //
        //It became far more likely today. Repetition children are now created unevaluated
        //(cp == NO_MATE_SCORE) rather than born permanently drawn, which is this exact
        //condition; and the worker loop no longer ends when the tree is full, so a full tree
        //keeps reaching this branch instead of stopping the search.
        //
        //Logged, and only once in a while: a counter that fires every 100000th occurrence says
        //the same thing without the flood.
        {
          static std::atomic<uint64_t> warned{0};
          const uint64_t k = warned.fetch_add(1, std::memory_order_relaxed);
          if ((k % 100000) == 0)
            log_file("mcts_search() warning: unevaluated child - skipping expansion (%llu so far)\n",
                     (unsigned long long)(k + 1));
        }
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
      //THE SIDE TO MOVE HERE IS MATED, always: position_eval() sets terminal 1 from isMate, and
      //eval_and_expand() sets it when a node has no legal moves while in check. So the value is -1
      //from this node's point of view, and the backpropagation below alternates the sign up the path.
      //
      //This used to read `scorecp == -MATE_SCORE ? -1 : 1`, an exact test against -20000 -- but a mate
      //found inside a check extension is stored with its depth folded in (-MATE_SCORE * 0.01 + iter,
      //so -19800 two plies down), and every one of those took the `: 1` branch and was backed up as a
      //WIN for the side being mated. Measured in a real game: the node one move from mating held
      //W +168 over 170 visits, so the mating move scored -0.99 from the mating side's view and the
      //search spent 11,417 of its 11,802 visits avoiding it -- the engine walked into mate in 2 from a
      //drawn position (lichess iP6FoytR, move 41). Mates found at the root's own expansion were stored
      //as exactly -MATE_SCORE and were unaffected, which is why this survived: the deeper the mate, the
      //more certainly the sign was wrong.
      scorecp = node->cp.load(std::memory_order_relaxed);
      if (scorecp > 0)
        log_file("mcts_search() warning: mate node with a positive cp %d -- check the terminal flag\n", scorecp);
      result = -1;
    } //for stalemate the result is initiated to 0, so no need for this check    
  }
  //AN EXACT LEAF BACKS UP AS A CERTAINTY, not as an evaluation. Without this the proof would steer
  //selection while Q went on averaging the static evaluation of a position whose outcome is already
  //known, which is the dilution the whole option exists to remove. terminal 1 already did this for
  //mate; this covers the verdicts the induction established.
  if (prove_terminals && terminal != 1) {
    if (proven_loss(node_terminal_word))      result = -1.0;
    else if (proven_win(node_terminal_word))  result =  1.0;
  }
  // Backpropagation: update node visits and results regardless of whether we expand the node or not
  for (auto n = path.rbegin(); n != path.rend(); ++n) {
    node = *n;
    node->N.fetch_add(1, std::memory_order_relaxed);      //attention, always
    if (informed) {
      node->evidence.fetch_add(1, std::memory_order_relaxed);
      node->W.fetch_add(result, std::memory_order_relaxed);
    }
    //The minimax value is refreshed on the same upward walk, leaf first, so a node is recomputed
    //only after the child it was reached through already carries this simulation's result.
    if (minimax_weight > 0.0) mm_backup(node);
    result = -result;
  }
  //Proofs are established AFTER the values have been backed up, so a node that becomes exact on this
  //simulation is already carrying the visit that proved it.
  if (prove_terminals) prove_up(path);
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
        //Same edge-versus-position distinction as most_visited_child() and select_best_moves().
        MCTSNode * ch = search.arena.child_at(current_node, i);
        uint64_t n = edge_visits ? (uint64_t)children[i].n.load(std::memory_order_relaxed)
                                 : (ch ? ch->N.load(std::memory_order_relaxed) : 0);
        if (current_node == search.root) visits.push_back({n, i});
        if (n > N) {
          N = n;
          next_idx = i;
        } 
      }
      if (next_idx < 0) break; //meaning current_node is a leaf node, i.e. no children
      current_node = search.arena.child_at(current_node, next_idx);   //evaluated before the assignment
      d++;
    }
    depth.store(d, std::memory_order_relaxed);
    size_t unique_nodes = live_tree_nodes();
    size_t total_memory = unique_nodes * NODE_BYTES + total_children.load(std::memory_order_relaxed) * EDGE_BYTES;
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
      MCTSNode * child = search.arena.child_at(search.root, visits[i].second);
      log_file("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", d, seldepth.load(std::memory_order_relaxed), i + 1, report_cp(child), nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, uci_move);
      print("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", d, seldepth.load(std::memory_order_relaxed), i + 1, report_cp(child), nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, uci_move);
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

  //The root's state, held for the whole function. It used to be read back off the Board several
  //hundred lines apart from this call, with a do_move() in between at the num_moves == 1 branch --
  //which is exactly the window in which the cached flags stop describing the position.
	const BoardState root_bs = isCheckMateStaleMate(board); //also counts all legal moves
  if (root_bs.num_moves > 1) { //run MCTS using multiple threads
    tbhits.store(0, std::memory_order_relaxed);
    tb_probe_misses.store(0, std::memory_order_relaxed);
    //Stop and join any collection still running from the last move. This is the "go" edge of
    //signal pair: bestmove starts the collector, the next search stops it. Whatever it
    //managed to free is kept; whatever it did not is collected next time.
    //
    //With CollectDuringPonder no search joins it: the mark is safe beside a search (see
    //mark_barrier()), so it finishes while the engine thinks rather than being aborted here and paid
    //again inline. Joining only in the ponder case was not enough -- in a real game 111 background
    //collections started and 41 collections still ran on the clock, because the real search aborted
    //the mark that was still going and then had to do it itself.
    if (!collect_during_ponder) gc_join();
    //Before the timer, so the benchmark is not charged to the collection line. Test builds only.
    run_mark_bench();
    const auto collect_start = std::chrono::steady_clock::now();
    //Ponder implies reuse: a pondered subtree destroyed before the next search was wasted.
    const bool reusing_tree = reuse_tree || chessEngine.optionCheck[Ponder].value;
    bool collected = !reusing_tree;   //cleanup() always "collects": it empties the tree entirely
    bool futile_skip = false;         //skipped because it provably frees nothing
    bool tree_reset  = false;         //discarded instead of collected; see TREE RESET below
    int  futile_why  = 0;             //gc_would_free_nothing()'s answer
    double set_root_ms = 0.0;
    if (reusing_tree) {
      {
        const auto t = std::chrono::steady_clock::now();
        set_root(ctx);
        set_root_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t).count();
      }
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
      //Threshold only. This used to read "pondering || occupancy >= gc_threshold", which forced
      //a full collection before EVERY ponder search -- and with Ponder on by default that is every
      //move. It was added when a collection meant millions of inline delete calls and the
      //background collector had to be disabled to avoid an unbounded gc_join(); collecting inline
      //on the opponent's clock was the lesser evil. The reaper removes that premise: the inline
      //part is now a map walk with no frees in it. Measured cost of the old behaviour, over one
      //session of two bots: 384 collections and 354 s of collection time per bot.
      //Threshold only. A futile-collection guard used to sit here, skipping the walk when the
      //previous one freed less than GC_MIN_YIELD. It showed NO measurable benefit in an A/B, and
      //it has a cost that was not appreciated at the time: skipping keeps the tree FULL, and a
      //full tree sets tree_full, which blocks expansion entirely (see mcts_search). A search that
      //cannot expand still accumulates visits, and select_best_moves() picks by VISIT COUNT -- so
      //the engine can pour 38M visits into a move whose refutation it never expanded and then
      //play it. That is exactly what happened in a live game: f5f3 took 98% of the visits with a
      //2.3% prior, at seldepth 16 after 38M simulations, and the engine's own cp for it was -322.
      //EXCEPT when the collection is certain to free nothing -- see THE EXACT FUTILITY TEST. The
      //tree after skipping is identical to the tree after collecting, so this cannot reintroduce
      //the full-tree problem described above.
      //TREE RESET (TreeResetBelow > 0, off by default).
      //
      //The collector keeps everything reachable from the root, and during a run of reversible moves that
      //is the whole tree: every earlier root can be reached again by moving pieces back, almost always
      //without repeating an exact game position (measured at 99-100% of a full tree in game QHWLWAbv).
      //So the tree fills, nothing can be freed, and every simulation of the next search is hollow.
      //
      //That is usually survivable, because the root still carries what earlier searches learned about
      //it. It is not survivable when the opponent plays a move the last search barely looked at: the
      //new root inherits a handful of informed simulations and the full tree lets it gain none. In
      //QHWLWAbv move 32 the root inherited 16, every root move finished the search with one visit, and
      //the engine played the highest-prior move and lost its queen. Across 22 games, 189 searches began
      //on a tree at 950 per-mille or more; the move chosen had fewer than 1,000 informed simulations
      //behind it in 7 of them, and the median across those 189 was over 100,000.
      //
      //So: when the tree is at TreeResetOccupancy or more AND the root inherited fewer than
      //TreeResetBelow informed simulations, keeping the tree buys almost nothing and costs the whole
      //search. Retire every node, root included, and start this search on an empty tree. A new root
      //that set_root() just created counts as having inherited 1.
      //
      //TreeResetHollow is the second trigger, and does not look at the inheritance: the previous search was
      //itself hollow, so the tree is frozen and this search would learn nothing either. Measured on 50 frozen
      //positions from real games, searched again from an empty tree for the same time and scored by
      //Stockfish: the fresh search's move was equivalent in 39, better in 7 and worse in 4, and lost a pawn
      //or more in 5 against 7 -- a reset in a frozen tree costs nothing measurable. Fewer pieces make it
      //cheaper still: a fresh search runs about 1.8 times as fast with ten pieces or fewer as on a full board.
      const int last_hollow = last_search_hollow_permille.load(std::memory_order_relaxed);
      if ((tree_reset_below > 0 || tree_reset_hollow > 0) && search.root) {
        const int      occ       = tree_occupancy();
        const uint64_t inherited = search.root->evidence.load(std::memory_order_relaxed);
        const bool starved = tree_reset_below > 0 && inherited < (uint64_t)tree_reset_below;
        const bool frozen  = tree_reset_hollow > 0 && last_hollow >= tree_reset_hollow;
        if (occ >= tree_reset_occupancy && (starved || frozen)) {
          //NEVER BESIDE A BACKGROUND COLLECTION. The reset retires the whole tree, so running it while a
          //mark is walking that same tree puts two collectors on it: in a lichess game with
          //CollectDuringPonder on, a reset fired while a post-move collection was still marking, and a
          //search worker then walked into freed memory (SIGSEGV in king_moves() inside an expansion).
          //Whatever the mark had reached is abandoned, which costs nothing: the reset frees all of it.
          //-DCREATICA_TEST_NO_RESET_GUARD builds the broken version, to show the crash is this and not
          //something else; see the validation note in ENGINE_SETTINGS.md.
#ifndef CREATICA_TEST_NO_RESET_GUARD
          gc_join();
#endif
          const size_t before = live_tree_nodes();
          gc(search.root, /*retire_all=*/true);
          search.root = nullptr;
          const auto t = std::chrono::steady_clock::now();
          set_root(ctx);
          set_root_ms += std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - t).count();
          tree_reset = true;
          log_file("info string tree reset (%s): root inherited %llu informed simulations (TreeResetBelow %lld), "
                   "previous search %d permille hollow (TreeResetHollow %lld), tree at %d permille "
                   "(TreeResetOccupancy %lld); retired %zu nodes, now %d permille\n",
                   starved && frozen ? "starved and frozen" : starved ? "starved" : "frozen",
                   (unsigned long long)inherited, (long long)tree_reset_below, last_hollow,
                   (long long)tree_reset_hollow, occ, (long long)tree_reset_occupancy, before, tree_occupancy());
        }
      }
      //With CollectDuringPonder the collection started after the last bestmove may still be running
      //beside this search, ponder or real. Collecting inline as well would put two marks on the tree
      //at once, and the one already running is doing the same work without spending this move's clock.
      const bool background_running = collect_during_ponder &&
                                      gc_in_flight.load(std::memory_order_relaxed);
      const bool wanted = !tree_reset && !background_running && tree_occupancy() >= gc_threshold;
      futile_why  = wanted ? gc_would_free_nothing() : 0;
      futile_skip = futile_why != 0;
      //Under CREATICA_VERIFY_FUTILE, say which condition failed whenever a collection is NOT judged
      //futile, so a test that expects the skip can see why it did not happen.
      if (verify_futile && wanted && !futile_skip && search.root) {
        std::lock_guard<std::mutex> lk(gc_completed_mtx);
        log_file("info string FUTILE-CHECK: not futile: snapshot %s, root slot %u vs %u, root tag %u vs %u, "
                 "next_tag %u vs %u\n", gc_completed.valid ? "valid" : "INVALID",
                 gc_completed.root_idx, search.arena.index_of(search.root),
                 gc_completed.root_tag, search.arena.stamp_of(search.root),
                 gc_completed.next_tag, search.arena.next_tag.load(std::memory_order_relaxed));
      }
      collected = tree_reset || (wanted && (!futile_skip || verify_futile));
      if (collected && !tree_reset) {
        gc();
        if (futile_skip)
          log_file("info string FUTILE-CHECK: the exact test said nothing could be freed; "
                   "the collection freed %zu nodes%s\n",
                   last_gc_freed_count.load(std::memory_order_relaxed),
                   last_gc_freed_count.load(std::memory_order_relaxed) ? "  <-- THE TEST IS WRONG" : "");
      }
    } else {
      //Usually a no-op: the background wipe started after the last bestmove has already
      //emptied the tree, and gc_join() above waited for it. This remains as the fallback
      //for the first move of a game and for any move where the wipe was interrupted.
      cleanup_locked();
      const auto t = std::chrono::steady_clock::now();
      set_root(ctx);
      set_root_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t).count();
    }
    //Both paths, so a run with ValidateTree on says whether reuse specifically is what breaks
    //the invariants, rather than leaving it ambiguous.
    const auto validate_start = std::chrono::steady_clock::now();
    validate_tree(collected ? (reusing_tree ? "after gc" : "after cleanup")
                            : "before search (collection skipped)", collected);
    //Reported separately for the same reason set_root is: with ValidateTree on, walking every edge of
    //a seven-million-node tree took up to 1.15 s a move in a real game, and it was being read as the
    //cost of collection. It is a check, not part of the engine's normal work.
    const double validate_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - validate_start).count();
    //What the collection actually cost. It runs inline, before the search starts, so it is dead
    //time on the clock; logging it turns "gc feels slow" into a number.
    //Report set_root SEPARATELY. This line used to time the whole block and call the result
    //"gc", which is three different things added together: set_root() -- which expands a childless
    //root, so it can run a full NNUE expansion -- the collection, and validate_tree(), which walks
    //every edge in the tree when ValidateTree is on. A run where gc() did not execute at ALL still
    //reported 700 ms per move under that label. Anyone reading the log to decide whether
    //collection is expensive needs these apart.
    //Say WHOSE clock this was on. A collection at the start of a ponder search is paid out of
    //the opponent's thinking time, and on a ponder hit that same search becomes the real one, so
    //it is never paid for again. A collection at the start of a real search comes straight out
    //of this move's own budget. The two are the same number of milliseconds and completely
    //different in what they cost, and the line could not tell them apart.
    log_file("info string %s took %.1f ms (set_root %.1f ms, validate %.1f ms, collect %s, %s), %zu nodes\n",
             reusing_tree ? "gc" : "cleanup",
             std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - collect_start).count(),
             set_root_ms, validate_ms,
             tree_reset ? "RESET" :
             collected ? (futile_skip ? "yes (VERIFY_FUTILE)" : "yes")
                       : (futile_skip ? (futile_why == 1 ? "SKIPPED-EXACT, same root, no expansion abandoned since the last collection"
                                                           : "SKIPPED-EXACT, last collection's root reachable from this one, no expansion abandoned since")
                                      : "SKIPPED"),
             pondering ? "ponder search" : "real search",
             live_tree_nodes());
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
    //THE OUTPUT THREAD LIVES AND DIES INSIDE ONE ATTEMPT, never across the reset between them. It
    //walks the tree from search.root with no lock at all -- that is what it is for, a cheap reader
    //of a structure only the workers write -- so it cannot be running while gc(retire_all) frees
    //every node under it. Starting and joining it per attempt costs one thread creation on the rare
    //move that aborts and keeps the reset in a window where nothing is reading the tree.
    const bool live_output = chessEngine.optionCheck[IntermittentInfoLines].value && !chessEngine.ponder;
    //THE SEARCH, WITH ONE RETRY ON A FROZEN TREE. See hollow_abort for why the abort exists at all.
    //Attempt 0 runs the whole budget in the normal case and this loop turns once. If the workers
    //come back early because thread 0 found the search hollow, the tree is retired, the root rebuilt
    //and attempt 1 runs whatever is left of the budget on an empty tree. There is never an attempt 2:
    //a tree that starts empty cannot be full, so a second abort would mean something other than a
    //frozen tree, and an uncapped loop could spend the entire move collecting.
    double attempt_budget = search_budget;
    for (int attempt = 0; attempt < 2; ++attempt) {
      hollow_abort.store(false, std::memory_order_relaxed);
      hollow_abort_armed.store(attempt == 0, std::memory_order_relaxed);
      for (int i = 0; i < num_threads; ++i) {
          pool_params[i].thread_id = i;
          pool_params[i].time_alloc = (uint64_t)attempt_budget;
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

      std::thread output_thread;
      if (live_output) {
        search_done.store(false);
        output_thread = std::thread(uci_output_thread);
      }
      // Wait until all workers are done
      while (active_workers.load()) {
        std::unique_lock<std::mutex> lock(pool_mutex);
        pool_done_cv.wait(lock, [] { return active_workers.load() == 0; });
        lock.unlock();
      }
      if (live_output) {
        {
          std::lock_guard<std::mutex> lk(search_done_mtx);
          search_done.store(true);
        }
        cv_search_done.notify_one();
        output_thread.join();
      }
      //Not an abort: either the search ran its allocation, or `stop` arrived, or this was already
      //the retry. Either way there is nothing more to do for this move.
      if (!hollow_abort.load(std::memory_order_relaxed) ||
          stopFlag.load(std::memory_order_relaxed) || attempt == 1) break;

      //EVERY WORKER HAS EXITED. That is what makes the reset safe here and impossible inside a
      //worker: gc(retire_all) frees the whole tree, and a thread still walking it would be reading
      //freed slots. gc_join() covers the other collector -- the background one started after the
      //last bestmove, which under CollectDuringPonder can still be marking this same tree.
      const auto reset_start = std::chrono::steady_clock::now();
      const uint64_t sims_before   = search_simulations.load(std::memory_order_relaxed);
      const uint64_t hollow_before = uninformed_sims.load(std::memory_order_relaxed);
      const size_t   nodes_before  = live_tree_nodes();
      gc_join();
      gc(search.root, /*retire_all=*/true);
      search.root = nullptr;
      set_root(ctx);
      //Both counters belong to the tree that has just been thrown away. seldepth is the deepest line
      //reached in it and depth the length of its principal variation, and the retry starts from a
      //single node; leaving either in place would report the dead tree's numbers for the rest of the
      //move, and with `go depth N` a stale depth could even stop the retry before it began.
      seldepth.store(0, std::memory_order_relaxed);
      depth.store(0, std::memory_order_relaxed);
      {
        int hf = tree_occupancy();
        if (hf > 1000) hf = 1000;
        hash_full.store(hf, std::memory_order_relaxed);
      }
      //Charged against this move's budget, exactly as the pre-search collection is: the elapsed time
      //includes both the wasted attempt and the reset, so the retry gets what is genuinely left and
      //the move as a whole still finishes inside its allocation.
      const double used_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - iter_start).count();
      attempt_budget = search_budget - used_ms;
      log_file("info string hollow abort: %llu of %llu simulations (%d permille) taught the tree nothing "
               "at %zu nodes; retired the tree in %.1f ms, now %zu nodes at %d permille; "
               "re-searching with %.0f ms of this move's %.0f ms left\n",
               (unsigned long long)hollow_before, (unsigned long long)sims_before,
               sims_before ? (int)(1000 * hollow_before / sims_before) : 0,
               nodes_before,
               std::chrono::duration<double, std::milli>(
                 std::chrono::steady_clock::now() - reset_start).count(),
               live_tree_nodes(), tree_occupancy(), attempt_budget, search_budget);
      //Too little left to rebuild anything worth having, or set_root() could not build a root at all.
      //Nothing is lost by stopping in either case: the reset has already happened, so the NEXT search
      //starts on the clean tree rather than the frozen one. The null check is not decoration -- every
      //worker dereferences search.root on its first line, so starting attempt 1 without one would be
      //a crash, and on lichess a crash is a forfeited game.
      if (!search.root || attempt_budget < 1.0) break;
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
    //THE SEARCH'S OWN SUMMARY. Printed unconditionally, not behind FinalInfoLines, because the
    //case worth catching is a search that barely ran -- and that is exactly the case where the
    //other lines carry stale numbers and look normal.
    {
      const double secs = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - iter_start).count();
      const uint64_t sims = search_simulations.load(std::memory_order_relaxed);
      const uint64_t hollow = uninformed_sims.exchange(0, std::memory_order_relaxed);
      last_search_hollow_permille.store(sims ? (int)(1000 * hollow / sims) : 0, std::memory_order_relaxed);
      //The repetition history's shape, which is the cheap way to see that the counts are real and
      //that the per-command replay is not multiplying them: `positions` should track the ply count
      //and `max rep` should stay small. `flipped` is how many of those boards have also occurred
      //with the other side to move -- the triangulation signal.
      int max_rep = 0, flipped = 0;
      for (const auto& kv : position_history) {
          if (kv.second > max_rep) max_rep = kv.second;
          if (position_history.count(kv.first ^ z.blackMove)) ++flipped;
      }
      log_file("info string search: %llu simulations in %.0f ms (%.0f/s), "
               "last collection %.1f ms, tree %zu nodes, %d permille, "
               "history %zu positions, max rep %d, %d also seen with the tempo flipped, "
               "%llu hollow (%.1f%%), %llu tablebase misses\n",
               (unsigned long long)sims, secs * 1000.0,
               secs > 0 ? sims / secs : 0.0,
               last_gc_ms.load(std::memory_order_relaxed),
               live_tree_nodes(), tree_occupancy(),
               position_history.size(), max_rep, flipped,
               (unsigned long long)hollow, sims ? 100.0 * hollow / sims : 0.0,
               (unsigned long long)tb_probe_misses.load(std::memory_order_relaxed));
      //The leaf alpha-beta's own cost, which is the number the experiment turns on: nodes per leaf
      //is the exchange rate against the simulations it displaces, and the cutoff share says whether
      //the ordering is good enough for the search to be paying 2-per-ply rather than 35-per-ply.
      const uint64_t lcalls = leaf_ab_calls.load(std::memory_order_relaxed);
      if (lcalls) {
        const uint64_t lnodes = leaf_ab_nodes.load(std::memory_order_relaxed);
        log_file("info string leaf alpha-beta: depth %lld, %llu leaves searched, %llu nodes "
                 "(%.1f per leaf), %llu cutoffs (%.1f%% of nodes), %llu refused in check\n",
                 (long long)leaf_depth, (unsigned long long)lcalls,
                 (unsigned long long)lnodes, (double)lnodes / (double)lcalls,
                 (unsigned long long)leaf_ab_cutoffs.load(std::memory_order_relaxed),
                 lnodes ? 100.0 * leaf_ab_cutoffs.load(std::memory_order_relaxed) / lnodes : 0.0,
                 (unsigned long long)leaf_ab_blind.load(std::memory_order_relaxed));
      }
      //The selective leaf search, reported separately: the two options can run at once, and the
      //flagged-versus-resolved gap is how one checks that the threshold is selecting the share the
      //offline measurement assumed (a tenth of children, at ErrThreshold 371).
      const uint64_t eflag = err_leaf_flagged.load(std::memory_order_relaxed);
      if (eflag) {
        const uint64_t ecalls = err_leaf_calls.load(std::memory_order_relaxed);
        log_file("info string selective leaf search: depth %lld, threshold %lld cp, %llu children "
                 "flagged, %llu resolved (%.1f%% of flagged; the rest were terminal, in check or "
                 "in tablebase range)\n",
                 (long long)err_leaf_depth, (long long)err_threshold,
                 (unsigned long long)eflag, (unsigned long long)ecalls,
                 100.0 * (double)ecalls / (double)eflag);
      }
    }
    if (chessEngine.optionCheck[FinalInfoLines].value) {    
      elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
      nodes = search_simulations.load(std::memory_order_relaxed);
      unique_nodes = live_tree_nodes();
      // Calculate hashfull (in per-mille) using unique_nodes
      size_t total_memory = unique_nodes * NODE_BYTES + total_children.load(std::memory_order_relaxed) * EDGE_BYTES;
      size_t max_capacity = chessEngine.optionSpin[Hash].value * 1024 * 1024;  // MB to bytes
      hashfull = max_capacity ? (total_memory * 1000) / max_capacity : 0;
      if (hashfull > 1000) hashfull = 1000;  // Cap at 1000 per UCI spec
      hash_full.store(hashfull, std::memory_order_relaxed);
    }
  } else if (root_bs.num_moves == 1) { //skip MCTS and just make the move
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
      //The state AFTER the forced move -- a different position from root_bs, which is why reading
      //the Board here used to work only by accident of ordering.
  		const BoardState after_bs = isCheckMateStaleMate(board);
      double res;
      if (after_bs.mate) res = MATE_SCORE * 0.01;
      else if (after_bs.stalemate) res = 0.0;
      else if (after_bs.check) {
        std::unordered_set<uint64_t>pos_history;
        res = -process_check(board, tmp_zh, ctx, pos_history, 0);
      } else {
        res = -evaluate_nnue(board, ctx, after_bs.check);
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
  } //root_bs.num_moves == 1
  else {
      if (root_bs.mate) {
          log_file("info depth 0 score mate 0\n");
          log_file("bestmove (none)\n");
          print("info depth 0 score mate 0\n");
          print("bestmove (none)\n");
      }
      else if (root_bs.stalemate) {
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
  if (!ponderHit.load(std::memory_order_relaxed) && !searchAborted.load(std::memory_order_relaxed)) {
    std::string bestmove;
    std::string ponder;
    if (!pvs.empty()) {
      int pos = pvs[0].second.find(" ");
      int pos2 = pvs[0].second.find(" ", pos + 5);
      bestmove = pvs[0].second.substr(0, pos);
      if (pos != std::string::npos) ponder = pvs[0].second.substr(pos + 1, pos2 - pos - 1);
      //THE ASYNC PROBE'S ANSWER, if one arrived for THIS move. It is taken in preference to the
      //search's choice because it is exact rather than estimated -- and in a simplified endgame
      //the search has very little to say anyway. The id check is what makes a reply that arrives
      //too late merely useless rather than wrong.
      {
        const uint64_t want = tb_probe.want.load(std::memory_order_acquire);
        if (want != 0 && tb_probe.have.load(std::memory_order_acquire) == want) {
          std::lock_guard<std::mutex> lk(tb_probe.mtx);
          if (!tb_probe.move.empty()) {
            log_file("bestmove: taking the tablebase move %s over the search's %s\n",
                     tb_probe.move.c_str(), bestmove.c_str());
            bestmove = tb_probe.move;
            ponder.clear();          //no PV behind a tablebase move
            //SAY THAT THIS IS A TABLEBASE ANSWER, in the same shape run_go() uses for a LOCAL probe
            //(report_tb_move): one node, one tbhit, depth 1. Without it an answer from lichess is
            //indistinguishable from an ordinary search result to anything reading the UCI stream,
            //because the substitution happens after the search has already printed its own info
            //lines. lichess_bot's tablebaseVerdict() keys on exactly this triple to tell a certainty
            //from an estimate, and with WDL-only local tables most SIX-piece roots are answered by
            //lichess rather than locally -- so without this the draw offer would almost never fire
            //where it matters. tb_probe.score is 0 for a draw: sendGetRequest() only moves it off
            //zero for a win or a loss category.
            print("info depth 1 seldepth 1 multipv 1 score cp %d nodes 1 nps 1 hashfull 0 tbhits 1 "
                  "time 0 pv %s\n", tb_probe.score, bestmove.c_str());
            log_file("info depth 1 seldepth 1 multipv 1 score cp %d nodes 1 nps 1 hashfull 0 tbhits 1 "
                     "time 0 pv %s\n", tb_probe.score, bestmove.c_str());
          }
        }
      }
      //here we need to make bestmove to update position_history
      if (!chessEngine.ponder) {
        Move move = {};
        uci2move_idx(bestmove.c_str(), move);
        updateHash(zh, board, move, ff_move(board, move), z);
        ++position_history[zh.hash];
        //The board now holds the position the opponent must answer, and the engine is idle until
        //they do. Spend that time asking the tablebase what happens next, so the answer is already
        //cached when our own clock is short.
        tb_prefetch_after_our_move();
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
    if (!post_move_collect) {
      //Replaying: leave the tree entirely alone. The next set_root() will find whatever position
      //actually follows, and the pre-search threshold collection reclaims from there.
    } else if (pondering || chessEngine.optionCheck[Ponder].value) {
      //No background collection whenever pondering is in play -- not just after a ponder search,
      //but after a REAL one too, because the ponder "go" follows within milliseconds.
      //
      //gc_start() spawns a collection over a multi-million-node tree; the next go calls
      //gc_join(); and the SWEEP cannot be interrupted, because stopping it half way leaves a
      //parent pointing at a freed child. So a search could block for a whole sweep with nothing
      //bounding the wait -- measured at up to 11.9 s on an 8.3M-node tree.
      //
      //With Ponder on, the ponder go follows a real search within milliseconds, so that join
      //happens EVERY move. Before today only one bot pondered, so the go did not arrive
      //immediately and the collection finished on idle time, which is why a full night of games
      //ran clean and today did not.
      //
      //Honest limit: an 11.9 s worst case does not by itself reach the driver's 60 s watchdog,
      //so this removes a real unbounded wait but is not proven to be the whole story. The
      //engines now log separately, so the next occurrence can be attributed rather than guessed.
      //
      //The ponder path collects INLINE before its search instead (see the pre-search branch), so
      //the tree is still bounded; it is paid on the opponent's clock rather than on a join.
      //No background collection after a ponder search: the next "go" is imminent and would
      //abort it before it finished anything. The search that follows collects inline instead.
      //
      //CollectDuringPonder changes that: the collection is started here and the ponder search that
      //follows does not join it, so the mark runs beside the ponder instead of before it. The sweep
      //was already a background thread that can be stopped, so the unbounded join described above is
      //gone; what remained was the mark, which the write barrier now makes safe to run beside a search.
      if (collect_during_ponder && reuse_active && search.root && !bestmove.empty() && bestmove != "(none)") {
        Move bm;
        uci2move_idx(bestmove.c_str(), bm);
        const int want = (bm.promoType << 12) | (bm.src << 6) | bm.dst;
        const int nc = search.root->num_children.load(std::memory_order_relaxed);
        Edge * ch  = search.root->children.load(std::memory_order_acquire);
        MCTSNode * next_root = nullptr;
        for (int i = 0; i < nc && ch; ++i)
          if (ch[i].move.load(std::memory_order_relaxed) == want) { next_root = search.arena.child_at(search.root, i); break; }
        const bool worth_collecting = last_gc_freed.load(std::memory_order_relaxed) >= GC_MIN_YIELD
                                   || tree_occupancy() >= gc_threshold;
        if (next_root && worth_collecting) {
          search.root = next_root;
          gc_start(next_root, /*wipe=*/false);
        }
      }
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
          next_root = search.arena.child_at(search.root, i);
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
