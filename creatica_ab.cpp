// cmake . && make creatica_ab      (or: c++ -std=c++20 -Wall -O3 -I . -L . creatica_ab.cpp uci_frontend.cpp -o creatica_ab -Wl,-lchess,-rpath,.)
//
// CREATICA AB -- the alpha-beta engine.
//
// creatica's search was MCTS. This is the other engine: the same NNUE evaluator, the same library,
// and an alpha-beta search instead. The MCTS engine is preserved unchanged as creatica_mcts, and
// neither of them is this file's business -- the point of having both as targets is that they can
// play each other, and against Stockfish, and on lichess, which is the only way to settle what the
// position test sets disagree about.
//
// PROVENANCE. The search below is the prober abprobe5.cpp, which is where every number in
// ENGINE_SETTINGS and in the session logs came from. It is copied close to verbatim ON PURPOSE:
// it is the thing that was measured, and rewriting it for tidiness would mean the engine is not
// the thing the measurements describe. The comments explaining WHY each rule is shaped the way it
// is -- and which rules measured negative and are therefore off -- come with it.
//
// WHAT IS ON BY DEFAULT is the reference configuration from the measurement logs, plus SEE
// pruning. Note that the prober's own env defaults are NOT that configuration: they leave the
// quiescence table, PVS, aspiration and SEE ordering off, and score 115 where the reference scores
// 131. An engine that inherited the env defaults would be measurably weaker than everything in the
// logs, which is why apply_options() sets them explicitly rather than trusting the initialisers.
//
// WHAT IS DELIBERATELY ABSENT, all of it known and none of it hidden:
//   - no Syzygy tablebases and no online probes. creatica_mcts has both, so endgames are the one
//     place this engine is handicapped in a head-to-head; add tbprobe.c and the root probe if the
//     tournament shows it losing endings it should hold.
//   - no MultiPV, no searchmoves, no 50-move-rule awareness in the search.
//   - mate distances coming out of QUIESCENCE are unadjusted (the prober's own note says so), so a
//     mate found there can be reported at the wrong distance. Mates found in the main search are
//     correct.
//   - Lazy SMP is here but buys about +0.8 ply at four threads, because the NNUE evaluator itself
//     only scales 3.43x on this machine. Threads defaults to 1.
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <thread>
#include <sstream>
#include <algorithm>
#include "nnue/nnue/nnue_accumulator.h"
#include "nnue/bitboard.h"
#include "libchess.h"
#include "tbprobe.h"
#include "policy_net.h"

//NNUEContext and the nnue wrapper functions come from libchess.h as of 36d447b. This block used to
//be a private copy of them -- one of four in the tree, which is why a signature change there was a
//link-time surprise rather than a compile error.

//PER-THREAD SEARCH STATE. Lazy SMP gives every thread its own board, its own NNUE context and its
//own hash, and shares exactly ONE structure -- the transposition table. That is the whole of the
//communication between threads, and it is why alpha-beta SMP is so much less machinery than
//creatica's shared MCTS tree, which needs atomic edges, virtual loss, a per-node expanding flag and
//a collector that coordinates with live searchers.
//
//`z` (the Zobrist tables) and `pnet` (the policy net) stay SHARED because nothing writes them after
//init. Everything below is written during a search, so a shared copy would be a data race.
static thread_local Board board; static thread_local NNUEContext ctx;
static Zobrist z; static thread_local ZobristHash zh;
static PolicyNet pnet; static thread_local float pctx[512];
static thread_local std::vector<unsigned char> pfeat;
//NNUE_CHECK_SENTINEL is gone: nothing here compares against it any more. evaluate_nnue() is only
//ever called with in_check == false, because the one place that could be in check resolves the
//position by searching instead. The compiler flagging it as unused is the refactor's point.

static const int MATE_CP   = 30000;
static const int MAX_PLY   = 64;
//Anything at least this large is a mate score rather than an evaluation, so it has to be stored in
//the table relative to the ROOT and read back relative to the current ply -- otherwise a mate in 3
//found at ply 6 comes back out of the table as a mate in 3 at ply 2.
static const int MATE_IN_MAX = MATE_CP - MAX_PLY;
static int  policy_plies   = 3;
static int  qdepth         = 6;
//REPCHECK: scoring the first recurrence of a position as a draw. It is creatica's own rule, and it is
//needed once a table exists because a score computed inside a cycle must not escape it -- but it is
//also far more aggressive inside alpha-beta than inside MCTS, and combined with a table it invites the
//graph-history error, where a 0 stored because THIS path repeated is reused on a path that does not.
//Switchable so the table's own contribution can be measured apart from it. REPCHECK=2 keeps the rule
//but refuses to store anything in the table from a subtree that used it, which is the sound version.
static int  repcheck       = 1;
//Set when the subtree below the current node scored a repetition, so its score is not written to the
//table under REPCHECK=2.
static thread_local bool rep_tainted = false;
static const int MAX_MOVES = 256;
struct Mv { uint8_t src, dst, promo; };

//STABLE insertion sort, descending by score. Stability is not a nicety here: the sorts it replaces
//are std::stable_sort, so ties keep generation order, and an unstable sort would reorder equal-scored
//moves and change the search tree -- which would make the "same nodes" check fail for a reason that
//has nothing to do with speed. Shifting only while score[j] < s is what preserves it. n is about 35,
//so the constant matters more than the order of growth.
static inline void sort_desc(Mv * mv, int * score, int n) {
    for (int i = 1; i < n; ++i) {
        const Mv m = mv[i]; const int s = score[i];
        int j = i - 1;
        while (j >= 0 && score[j] < s) { mv[j + 1] = mv[j]; score[j + 1] = score[j]; --j; }
        mv[j + 1] = m; score[j + 1] = s;
    }
}
static inline bool same(const Mv& a, const Mv& b) {
    return a.src == b.src && a.dst == b.dst && a.promo == b.promo;
}

//ORDERING: 0 keeps the original rule, captures first by the value of the piece captured and quiet
//moves in generation order. 1 adds KILLERS and HISTORY.
//
//Why these two first, before null-move pruning or reductions. They are the cheapest thing on the list
//-- a table lookup, nanoseconds, against the 13.76 us the policy head costs -- and they are the part
//of Stockfish's ordering that is NOT hand-crafted chess knowledge. A killer is simply "this move
//caused a cutoff at this ply a moment ago"; a history score is "this from-to square pair has been
//causing cutoffs". Both are statistics the search accumulates about itself at runtime, in the same
//sense that MCTS's visit counts are. Nothing about chess is encoded in either.
//
//The target is concrete. Stockfish reaches depth 12 on these positions in 25,546 nodes and depth 16
//in 200,553, at an effective branching factor near 2.1, and scores 73.1% and 89.5%. This probe sits
//at a branching factor of 12.5. Creatica already spends 600,000 nodes a move, so the budget for depth
//16 is there three times over; what is missing is the cost per ply.
static int  ordering       = 1;
//PRUNE, as bit flags: 1 = null-move pruning, 2 = late move reductions, 3 = both, 0 = neither.
//
//WHY THIS STEP IS THE ONE THAT MATTERS, and why ordering could never have got there. The mean position
//in this set has 32.3 legal moves, so alpha-beta that searches EVERY move has a hard floor of
//sqrt(32.3) = 5.68 on its effective branching factor, reached only with perfect ordering. Killers and
//history took this probe from 12.54 to 11.38. Stockfish runs at about 2.1 -- well BELOW that floor,
//which is only possible by not searching everything to full depth.
//
//AND WHY THIS IS NOT THE GATING THAT WAS RULED OUT FOR CREATICA. A gate drops a move from a node
//permanently on the strength of a weak estimate, with no way back, and under tree reuse that decision
//outlives the move. Both mechanisms here are REVERSIBLE and bound-checked: a reduced move that
//nevertheless beats alpha is re-searched at full depth, and a null-move cutoff is a claim about beta
//that the surrounding window still has to accept. The error is caught by the search rather than
//carried by it.
static int  prune          = 3;
//R = nullr_base + (nullr_div ? depth/nullr_div : (depth > 6)). The defaults reproduce the original
//2 + (depth > 6) exactly; Stockfish's shape is base 7, div 3.
static int  nullr_base     = 2;
static int  nullr_div      = 0;
static thread_local long long null_tries = 0, null_cuts = 0, lmr_tries = 0, lmr_researches = 0;
//HOW GOOD IS THE ORDERING, measured where it matters: at a node that cuts off, was it the first move
//tried that did it? Stockfish's whole pruning cascade rests on late moves really being bad, so this
//is the number that says whether this search has earned the right to prune harder. cut_first counts
//cutoffs on move index 0, cut_nodes every cutoff, and cut_index_sum the mean index.
static thread_local long long cut_nodes = 0, cut_first = 0, cut_index_sum = 0;

//Two killers a ply. A move that caused a beta cutoff at this ply in a sibling subtree is very likely
//to cause one here too, and it costs nothing to try it early.
static thread_local Mv killers[MAX_PLY][2];
//From-square to to-square, per side. Incremented by depth*depth on a quiet cutoff, so a cutoff found
//deep in the tree counts for more than a shallow one, and halved wholesale when it would overflow.
static thread_local int history[2][64][64];

static inline void history_bump(int stm, const Mv& m, int depth) {
    int& h = history[stm][m.src][m.dst];
    h += depth * depth;
    if (h > (1 << 20)) {                       //keep it bounded; scale the whole table, not one entry
        for (int c = 0; c < 2; ++c)
            for (int a = 0; a < 64; ++a)
                for (int b = 0; b < 64; ++b) history[c][a][b] >>= 1;
    }
}

static thread_local long long nodes = 0, qnodes = 0, policy_orders = 0;
static thread_local long long killer_first = 0, history_cutoffs = 0;
static std::chrono::steady_clock::time_point deadline;
//SHARED and atomic: when the clock runs out every thread has to stop, not just the one that
//noticed. A thread_local flag would leave the helpers running past the deadline.
static std::atomic<bool> out_of_time{false};
//FIXNODES: stop after this many nodes instead of on the clock; 0 is off. The marginal cost of a
//ply is a NODE quantity, so the instrument that measures it should be one too. It also makes a run
//deterministic again -- a timed arm's result depends on how fast the machine felt, which is why an
//identical repeat moved by 2 positions at SINGDEPTH=4 -- and a deterministic instrument can resolve
//the 1-to-3 position differences a learned rule will produce, which the clock cannot.
//It is also the HONEST budget for comparing search rules that differ in cost per node: a rule that
//thinks harder at each node pays for it in nodes here, exactly as it would on the clock, but without
//the jitter.
static long long fixnodes = 0;
//LMRPROBE: sample this many parts per thousand of the LMR decisions and find out whether the
//reduction was RIGHT, by searching the same move both ways and comparing the verdicts. LMRPROBE_OUT
//names the TSV the rows go to.
//
//WHY THIS LABEL AND NOT ANOTHER. The search already catches one kind of mistake at runtime: when the
//reduced search comes back above alpha it re-searches at full depth, so an under-reduction costs
//time and is self-correcting. The dangerous mistake is the opposite one -- the reduced search fails
//low and a full-depth search would NOT have -- and the search never finds out, because nothing ever
//re-searches a move that failed low. That error is invisible at runtime by construction, it is the
//one that costs accuracy rather than time, and this probe exists to count it.
//
//The probe does not change what the search plays: the normal path's result is used exactly as
//before and the extra search's value is thrown away. It does cost time, so a probe run is for
//collecting labels, not for measuring strength.
static int lmrprobe = 0;
//LMRPLUS: add this much to every late-move reduction. It exists because the headroom probe says the
//current rule `r = 1 + (move_index >= 8)` is safe on 99.6% of its decisions and that one MORE ply of
//reduction would agree with full depth on 99.92% of them -- which is a rule with slack in it, not a
//rule making mistakes. Whether that LOCAL slack converts into a cheaper tree is a different question,
//because reducing harder everywhere compounds: shallower subtrees feed worse values upward. This
//knob is how the question gets answered empirically rather than argued.
static int lmrplus = 0;
static int lmrsel  = 0;
//LMRCOST flips the probe's search order so the FULL-depth search runs first. That makes the accuracy
//labels worthless -- it is exactly the table contamination described at the probe -- but it makes the
//node COSTS honest, because in the ordinary order the three shallower searches have already filled the
//table the full search then reads. One arm cannot measure both; this is the cost arm.
static int lmrcost = 0;
//MOVE COUNT PRUNING (late move pruning), the first of the forward-pruning mechanisms creatica does
//not have. The ladders on the Arasan suite measured the per-ply node multiplier m at 2.72 against
//Stockfish 18's 1.53, with the LEAF cost constant equal on both (266 against 260) -- so the whole
//13.6-ply deficit at equal nodes is how many children survive to be searched, and nothing to do
//with what a leaf costs. m is also flat across creatica's depths 6-10 while Stockfish's keeps
//falling, so the gap widens with depth. This knob attacks m directly.
//
//MCP=0 off, 1 quadratic limit (base + depth*depth), 2 linear limit (base + 2*depth). MCPDEPTH caps
//the depth at which it fires at all; MCPBASE shifts both limits. The shape is a knob rather than a
//constant because the quadratic form barely bites above depth 5 and only the ladder can say which
//is right here.
static int mcp       = 0;
static int mcp_base  = 3;
static int mcp_depth = 8;
//MCPMIN exists because the first arm answered the question badly in an informative way. A
//`3 + depth*depth` limit is 4 moves at depth 1 and 7 at depth 2, and almost every node in the tree
//sits at the frontier, so the rule was overwhelmingly "search four quiet moves at depth 1 and throw
//the rest away" -- blind to the position. It saved 2.8x the nodes at equal depth and cost 8-13
//positions, a net loss of 5-7 at every budget. This gate keeps the frontier unpruned so the middle
//of the tree can be tested on its own.
static int mcp_min   = 1;
static thread_local long long mcp_tested = 0, mcp_pruned = 0;

//FUTILITY PRUNING, the value-aware counterpart. Where move-count pruning asks how late a move is,
//this asks whether the position can still be saved: if the static evaluation is already a margin
//below alpha and the move is quiet, no quiet move is expected to make up the difference, so the
//subtree is skipped. This is how Stockfish can afford to prune hard at the frontier -- its rule
//consults the position, and a count does not.
//
//The margin scales with depth because the remaining search has more room to recover the deficit the
//deeper the node is. The static evaluation costs one NNUE call per node where the cheap gates pass,
//which is not free -- ~90% of this probe's runtime is evaluation -- so it is computed lazily and
//only when the arm is on.
static int fut        = 0;
static int fut_margin = 61;
static int fut_depth  = 4;
static thread_local long long fut_tested = 0, fut_pruned = 0;

//REVERSE FUTILITY PRUNING (Stockfish's Step 8, "futility pruning: child node"). The mirror image of
//the rule above and the more valuable of the two: where that one skips a single quiet move whose
//position is already too far BELOW alpha, this returns the ENTIRE NODE when the static evaluation is
//far enough ABOVE beta that the node is expected to fail high anyway. One test, and the whole subtree
//including its first child is never searched.
//
//Stockfish's form is `eval - 76 * depth * (improving terms) >= beta` for depth < 14, returning
//(2 * beta + eval) / 3. This has no `improving` flag -- that needs a static evaluation at every node
//and a per-ply stack to compare against -- so the margin is plain cp per ply, and the return is beta
//rather than the blended value, which is the conservative choice when the eval is not corrected by
//any history.
//TTSE: store the quiescence stand-pat evaluation in the table so interior nodes can read it. Off by
//default so every arm recorded before this existed stays reproducible.
static int ttse        = 0;
//EVMODE decides where a node's static evaluation comes from: 0 none (the original search), 1 CACHED
//ONLY -- use the stand-pat the previous iteration stored and never evaluate, 2 cached else compute.
//Mode 1 is the one that costs nothing, which is the whole point of storing them.
static int evmode      = 0;
//RAZORING, Stockfish Step 7: an evaluation far enough BELOW alpha means the node is not going to
//reach it, so the whole node is handed to quiescence instead of searched.
static int razor       = 0;
//STOCKFISH'S CONSTANTS ARE NOT CENTIPAWNS. `types.h:185` has `PawnValue = 208`, so its internal unit
//is about half a centipawn and every margin lifted from search.cpp must be divided by 2.08. Porting
//them verbatim made razoring demand being 7.7 pawns below alpha at depth 1 -- it fired on 508 of 13.5
//million nodes -- and made the improving bonus and the null-move gate twice as aggressive as intended,
//which is why both measured negative. Defaults below are the converted values: 485 -> 233, 281 -> 135,
//76 -> 37, 2474*76/1024 = 184 -> 88, 350 -> 168, 18 -> 9.
static int razor_base  = 233;
static int razor_quad  = 135;
//NULLGATE, the condition Stockfish puts on its null move and this probe does not: only try it when
//the evaluation already suggests a fail-high. The null move measured as a NULL here precisely
//because it fires blind on 0.56% of nodes; gated, it fires where it can pay, which is also what
//makes a large R defensible.
static int nullgate    = 0;
static int nullgate_base  = 168;
static int nullgate_slope = 9;
//IMPROVING scales the pruning margins. Stockfish subtracts (2474 * improving) * mult / 1024 from the
//reverse-futility margin, which with mult 76 is about 183 cp -- so a node whose evaluation has risen
//since our last turn is pruned MORE readily. It needs the evaluation from two plies up, which is
//exactly what a per-ply stack of cached evals provides.
static int rfut_imp    = 0;
static const int EVAL_UNKNOWN = -32767;
static thread_local int ss_eval[MAX_PLY];
static thread_local long long razor_tested = 0, razor_pruned = 0;
static thread_local long long nullgate_skipped = 0;
//FUT/RFUT mode 2 means CACHED ONLY: prune using an evaluation the table already holds and never
//compute one. That makes interior-node pruning free in evaluations, which is the whole point -- a
//flat eval at every interior node would cost 2.04x the evaluations this probe performs today.
static int rfut        = 0;
static int rfut_margin = 37;
static int rfut_depth  = 8;
static thread_local long long rfut_tested = 0, rfut_pruned = 0;
//THE NUMBER THAT DECIDES THE IDEA: how often an interior node finds its own static evaluation
//already in the table. Every node at ply p was a quiescence leaf at iteration p+1, so in a search
//with no pruning this would be ~100%; the shortfall is nodes reached on a path the earlier iteration
//never took, plus slots lost to collisions.
static thread_local long long ev_lookups = 0, ev_hits = 0;

//=== CORRECTION HISTORY (Stockfish's correction_value / update_correction_history) ===
//THE POINT. A static evaluation is not wrong at random. It is wrong in ways that repeat: the same
//pawn structure, the same minor-piece configuration, the same material balance get mis-valued the
//same way every time the search meets them. Correction history is the running record of that
//error. After every node the search compares what it actually found with what the evaluation had
//predicted, and writes the signed residual into a few integer tables keyed on cheap features of
//the position. The next node whose features hash to the same entry is handed the evaluation plus
//the accumulated correction.
//
//This is the consumer the "NNUE error is predictable but unusable" result never had. The signal
//measured there was r = 0.52 at position level; here reading it costs four array lookups and no
//evaluation at all, which is the only price this search can afford.
//
//WHY IT MATTERS HERE SPECIFICALLY. Every eval-driven pruning rule in this file -- razoring, the
//null-move gate, reverse futility, per-move futility, the improving flag -- takes the static
//evaluation as if it were the truth. Each one measured as hollow depth: it reached further at the
//same node count and played worse. A systematically wrong evaluation is one explanation that the
//measurements so far cannot rule out, and this is the instrument that tests it.
//
//UNITS. The bonus is computed from (best - static), which in this probe is centipawns and in
//Stockfish is 2.08 of its own units per centipawn. Because the same quantity is divided back out
//when the correction is applied, the 131072 divisor carries over unchanged and the loop stays
//self-consistent. The one thing that does not carry over is the saturation limit: a table entry
//here is driven by bonuses 2.08x smaller, so it saturates 2.08x more slowly.
static int corrhist   = 0;        //0 off, 1 the pawn/minor/material tables, 2 also the continuation
static int corrq      = 1;        //also correct the quiescence stand-pat, as Stockfish does
static int corr_div   = 131072;   //what the weighted sum is divided by to become centipawns
static const int CORR_LIMIT = 1024;      //ceiling on the magnitude of one table entry
static const int CORR_SIZE  = 1 << 16;   //a power of two, so the mask keeps the whole key
//Stockfish's four weights, carried over unchanged: pawn, minor piece, non-pawn material (applied
//to both colours' entries), continuation.
static int corr_w_pawn = 10347, corr_w_minor = 8821, corr_w_np = 11665, corr_w_cont = 7841;
//ONE BUNDLE PER SLOT, FOUR FIELDS. Stockfish hashes all four keys into the SAME table and reads a
//different field of the bundle for each, so a pawn-key collision with a material key cannot
//corrupt the other's statistic. That is cheaper than four separate tables and is why the field
//names below have to match the key that wrote them.
struct CorrBundle { int16_t pawn, minor, npw, npb; };
static thread_local CorrBundle corr_tab[2][CORR_SIZE];
//[piece and destination of the move two or four plies back][piece and destination of the move
//into this node]. Piece index 12 is the "no move" row, which Stockfish keeps and updates like any
//other rather than skipping.
static thread_local int16_t corr_cont[13][64][13][64];
//The move played AT each ply, which is the move leading into ply+1. Needed by the continuation
//table, and recorded as the piece standing on the destination AFTER the move so a promotion is
//keyed on the piece it became.
static thread_local uint8_t ss_mv_pc[MAX_PLY], ss_mv_to[MAX_PLY];
static thread_local long long corr_applied = 0, corr_updates = 0, corr_abs_sum = 0;
//THE MEASUREMENT THAT JUDGES THE IDEA, and it is free. Correction history makes exactly one claim:
//that it predicts the gap between what the evaluation says and what the search finds. At every
//node that writes a residual, the gap is known for both the raw and the corrected evaluation -- and
//the correction was read BEFORE this node's residual was written, so it is a genuine one-step-ahead
//prediction, not a fit. If the mean absolute residual falls, the table has real predictive power;
//if it does not, the mechanism is noise and no amount of tuning the margins will rescue it.
//This is deterministic and has no accuracy noise floor, which is why it is the judge rather than
//the 197-position score.
static thread_local long long corr_res_n = 0, corr_res_raw = 0, corr_res_cor = 0;
//The same measurement over every node whose score is EXACT -- alpha_orig < best < beta -- with the
//sign test and the no-capture rule dropped. Those two conditions are what the table is trained on,
//so the population above is the one most likely to flatter it; this one is selected only on `best`
//being a value rather than a bound, which it has to be for the residual to mean anything at all.
//(The first version of this counter took every node, and reported a 305 cp raw residual against the
//selected population's 79. That was not an evaluation error: at a null-window node `best` is clipped
//to the window, so it was measuring the distance to alpha.)
static thread_local long long corr_all_n = 0, corr_all_raw = 0, corr_all_cor = 0;
//=== WHERE THE NODES ACTUALLY ARE ===
//m, the per-ply node multiplier, is the whole gap against Stockfish, and m^2 is the effective
//number of moves explored per node: 7.4 here against 2.3 there. The 89.6% first-move-cutoff
//figure measured earlier CANNOT explain that, because it was measured at cut nodes -- and a cut
//node searches about one move, so cut nodes are not where the nodes are. An ALL-node has no
//cutoff and must search every move it generates, so the tree's size is the number of all-nodes
//times their width. This counts the three node types separately, with the moves generated and the
//moves actually searched at each, which is the measurement that says what m is made of.
//=== SEE PRUNING (Stockfish Step 14), THE RULE AIMED AT ALL-NODES ===
//WHY THIS AND NOT MOVE COUNT. 61.5% of every move this search looks at is searched at an ALL-node,
//which by definition found no cutoff and must try everything it generates -- 16.65 of 16.7 moves.
//That width is what m is made of. Move-count pruning aimed at the same nodes and lost 5 to 7
//positions, and the reason is visible in what it used: a move's RANK in the ordering, which at an
//all-node is the one signal already known to be wrong, because the ordering's own first choice
//just failed to cut. SEE prunes by what the move IS -- whether the material it puts en prise comes
//back -- which is a measurement of the move rather than a guess from its position in a list.
//
//Two rules, both Stockfish's, with the margins converted out of its 2.08-per-centipawn units:
//  quiets:   prune when SEE < -12 * lmrDepth^2   (25 there), lmrDepth being the REDUCED depth the
//            move would actually get, so a move about to be searched shallowly is held to a
//            stricter standard than one about to be searched deeply. At lmrDepth 0 it prunes any
//            quiet that loses material at all; by lmrDepth 6 the margin is -432 and inert.
//  captures: prune when SEE < -80 * depth       (166 there). Loose by design -- this only throws
//            out wild sacrifices, and it is the quiet rule that reaches all-node width.
//THE FIX THIS DEPENDS ON. get_see() had three defects until 2026-10-13 -- pinned defenders
//recaptured, king recaptures went unvetted, and an unsound early break -- so this rule could not
//have been written before today without pruning on wrong numbers. That is what the fix was for.
static int seeprune     = 0;    //0 off, 1 quiets, 2 quiets and captures
//=== QUIESCENCE PRUNING (Stockfish qsearch Step 6) -- HALF THE TREE, UNTOUCHED UNTIL NOW ===
//WHY THIS IS THE BIGGEST REMAINING ITEM. Quiescence is 49.6% of all nodes, and the measured
//per-iteration leaf constant is 168 against Stockfish's 90 -- a 1.87x factor sitting in half the
//tree. Every pruning rule tried so far has been in the main search, which is the other half.
//
//Stockfish prunes quiescence four ways, all gated on not already being lost, and all EXEMPTING
//checks, recaptures (the move lands where the previous one did) and promotions -- the three cases
//where a static material count is least trustworthy:
//  1. moveCount > 2 -- after the first two tries, stop. This is the one with real reach: it caps
//     the WIDTH of every quiescence node at 2 regardless of how many captures exist.
//  2. futility: stand-pat + 169 + what the capture wins <= alpha, and raise bestValue to it.
//  3. SEE below (alpha - futilityBase), i.e. this capture cannot climb to alpha even after trades.
//  4. and for every move, capture or not: SEE < -38, a capture that simply loses material.
//Margins converted out of Stockfish's 2.08-per-centipawn units: 351 -> 169, -80 -> -38.
static int qprune     = 0;     //0 off, 1 the moveCount cap and the SEE filter, 2 also futility
static int q_mcount   = 2;     //moveCount > this is skipped
static int q_futbase  = 169;   //cp added to the stand-pat before comparing with alpha
static int q_see      = -38;   //a capture with SEE below this is not searched at all
static thread_local long long qp_tested = 0, qp_mcount = 0, qp_fut = 0, qp_see = 0;

//=== CONTINUATION HISTORY AND A SHAPED REDUCTION ===
//WHY THIS IS WHAT IS LEFT. The flat reduction has an interior optimum: on Arasan, LMRPLUS 0/1/2/3
//scores 68/70/72/67, so +2 is worth about a node doubling and +3 falls off a 5-position cliff.
//Stockfish reduces 3.4 to 4.5 plies on average, which a flat rule cannot reach -- and the reason is
//that its average is not a uniform large reduction. It is this:
//
//    statScore = 2*mainHistory + contHist[0] + contHist[1]
//    r -= statScore * 850 / 8192          (r in 1024ths of a ply)
//
//statScore spans roughly +-74000 there, so that ONE line moves the reduction by several plies: a
//quiet move with a good track record is barely reduced, a bad one is reduced enormously. A constant
//has to pick one value for both populations, which is exactly what the cliff at +3 looks like.
//
//WHAT A CONTINUATION HISTORY IS. Not "was this move good" but "was this move good AFTER THAT MOVE".
//Keyed on the pair (previous move's piece and destination, this move's piece and destination), so it
//learns replies rather than moves. creatica has one butterfly table on [side][from][to] and nothing
//pairwise, which is why no history-shaped rule could be ported before this.
//
//TWO LEVELS, not Stockfish's six. Levels 1 and 2 carry its heaviest weights (1133 and 683 of 1024)
//and the plies they need are already tracked -- ss_mv_pc/ss_mv_to exist for correction history.
//Capture history, pawn history and low-ply history are separate items and are NOT here.
static int conthist  = 0;    //0 off, 1 populate and use in ordering, 2 also shape the reduction
static int lmrshape  = 0;    //0 the flat integer rule, 1 the 1024ths rule with statScore shaping
static int lmrbase   = 0;    //0 keep the measured flat base, 1 Stockfish's log(depth)*log(moveno)
static int shape_div = 8192; //r -= statScore * shape_mul / shape_div
static int shape_mul = 850;
static const int CONT_LIMIT = 30000;        //Stockfish's PieceToHistory cap, unchanged
//[level][previous piece][previous to][this piece][this to]. Level 0 is the move INTO this node,
//level 1 the move two plies back. Index 12 is the "no move" row, as in correction history.
static thread_local int16_t cont_hist[2][13][64][13][64];
static thread_local long long shape_n = 0, shape_sum = 0, shape_absum = 0;

//Stockfish's StatsEntry operator<<, with the continuation cap: step toward the bonus with a pull
//back proportional to how far the entry already is, so a repeated bonus saturates instead of
//running away.
static inline void cont_add(int16_t& e, int bonus) {
    const int b = std::clamp(bonus, -CONT_LIMIT, CONT_LIMIT);
    const int v = e;
    e = (int16_t)(v + b - v * std::abs(b) / CONT_LIMIT);
}

//The piece index this engine uses for history keys: colour * 6 + type - 1, so 0..11, with 12 free
//for "no move".
static inline int hist_pc(int stm, uint8_t src) {
    return stm * 6 + (int)PC_TYPE((Piece)board.piecesOnSquares[src]) - 1;
}

static inline int16_t * cont_slot(int level, int ply, int pc, int to) {
    const int back = level + 1;
    const int ppc = (ply >= back) ? (int)ss_mv_pc[ply - back] : 12;
    const int pto = (ply >= back) ? (int)ss_mv_to[ply - back] : 0;
    return &cont_hist[level][ppc][pto][pc][to];
}

//THE SCALE PROBLEM, and how it is handled. Stockfish caps mainHistory at 7183 and continuation at
//30000, and weights the main term by 2. creatica's butterfly history is a different animal: it
//grows by depth*depth with a wholesale halving at 2^20, so its numbers are two orders of magnitude
//larger and mean something else. Rather than change the ordering -- which is measured, and at 83%
//first-move cutoffs is not the problem -- the main term is shifted into a comparable range
//(2^20 >> 7 is 8192, against Stockfish's 7183) and used only for the shaping.
//TAKES THE PIECE INDEX, NOT THE SQUARE, because the only place that needs it is AFTER the move has
//been made -- the reduction depends on whether the move gives check, so it cannot be decided
//earlier -- and by then m.src is empty and hist_pc() would read whatever is there. ss_mv_pc[ply]
//was recorded before the move for exactly this kind of use. Quiet moves only, so there is no
//promotion to disagree about.
static inline int stat_score(int ply, int pc, uint8_t to, uint8_t from, int stm) {
    if (pc < 0 || pc > 11) return 0;
    return 2 * (history[stm][from][to] >> 7)
         + (int)*cont_slot(0, ply, pc, to)
         + (int)*cont_slot(1, ply, pc, to);
}

//Stockfish's bonus and the per-level weights, applied to the move that caused the cutoff and, with
//the opposite sign, to the quiet moves at the same node that did not.
static inline void cont_update(int ply, const Mv& m, int stm, int bonus) {
    //Called with the board RESTORED, so the piece is back on m.src and hist_pc() is correct here.
    const int pc = hist_pc(stm, m.src);
    if (pc < 0 || pc > 11) return;
    const int b = bonus * 896 / 1024;
    cont_add(*cont_slot(0, ply, pc, m.dst), b * 1133 / 1024 + 88);
    cont_add(*cont_slot(1, ply, pc, m.dst), b *  683 / 1024);
}
static int see_quad     = 12;   //cp per lmrDepth^2, quiet rule
static int see_cap_base = 80;   //cp per depth, capture rule
static thread_local long long seep_q_tested = 0, seep_q_pruned = 0;
static thread_local long long seep_c_tested = 0, seep_c_pruned = 0;
static thread_local long long nt_cut = 0, nt_all = 0, nt_pv = 0;
static thread_local long long nt_cut_gen = 0, nt_all_gen = 0, nt_pv_gen = 0;
static thread_local long long nt_cut_srch = 0, nt_all_srch = 0, nt_pv_srch = 0;

//The four keys, built from the bitboards. About thirty table lookups and XORs -- next to an NNUE
//evaluation this does not register, and unlike the main hash these are not maintained
//incrementally because ZobristHash carries only the combined hash. If the idea pays, splitting
//updateHash() to maintain them is the obvious follow-up.
struct CorrKeys { uint64_t pawn = 0, minor = 0; uint64_t np[2] = {0, 0}; };
static inline CorrKeys corr_keys(const Board& b) {
    CorrKeys k;
    for (int c = 0; c < 2; ++c) {
        uint64_t pw = b.side[c] & b.pieceTypes[Pawn - 1];
        while (pw) { const int sq = lsBit(pw); pw &= pw - 1; k.pawn ^= z.pawns[c][sq - 8]; }
        for (int pt = Knight; pt <= King; ++pt) {
            uint64_t bb = b.side[c] & b.pieceTypes[pt - 1];
            while (bb) {
                const int sq = lsBit(bb); bb &= bb - 1;
                const uint64_t r = z.nonPawns[c][pt - 2][sq];
                k.np[c] ^= r;
                if (pt == Knight || pt == Bishop) k.minor ^= r;   //minor piece configuration
            }
        }
    }
    return k;
}

//Stockfish's StatsEntry operator<<: step toward the bonus, with a pull back proportional to how
//far the entry already sits from zero. A bonus repeated forever saturates at the limit instead of
//running away, and a single outlier moves the entry only a little.
static inline void corr_add(int16_t& e, int bonus) {
    const int b = std::clamp(bonus, -CORR_LIMIT, CORR_LIMIT);
    const int v = e;
    e = (int16_t)(v + b - v * std::abs(b) / CORR_LIMIT);
}

//The two continuation rows this node reads, with the out-of-range plies mapped onto the "no move"
//row rather than skipped -- that row is a real statistic about nodes near the root.
static inline void corr_cont_idx(int ply, int& p1, int& t1, int& p2, int& t2, int& p4, int& t4) {
    p1 = ply >= 1 ? ss_mv_pc[ply - 1] : 12; t1 = ply >= 1 ? ss_mv_to[ply - 1] : 0;
    p2 = ply >= 2 ? ss_mv_pc[ply - 2] : 12; t2 = ply >= 2 ? ss_mv_to[ply - 2] : 0;
    p4 = ply >= 4 ? ss_mv_pc[ply - 4] : 12; t4 = ply >= 4 ? ss_mv_to[ply - 4] : 0;
}

//THE WEIGHTED SUM, read only. Four array reads plus two more for the continuation table; no
//evaluation, no search, nothing that can be wrong by more than its own history.
static inline int corr_value(const CorrKeys& k, int stm, int ply) {
    const CorrBundle& pb = corr_tab[stm][k.pawn  & (CORR_SIZE - 1)];
    const CorrBundle& mb = corr_tab[stm][k.minor & (CORR_SIZE - 1)];
    const CorrBundle& wb = corr_tab[stm][k.np[0] & (CORR_SIZE - 1)];
    const CorrBundle& bb = corr_tab[stm][k.np[1] & (CORR_SIZE - 1)];
    int cv = corr_w_pawn * (int)pb.pawn + corr_w_minor * (int)mb.minor
           + corr_w_np * ((int)wb.npw + (int)bb.npb);
    if (corrhist >= 2) {
        int p1, t1, p2, t2, p4, t4;
        corr_cont_idx(ply, p1, t1, p2, t2, p4, t4);
        //Stockfish substitutes 8 when there is no previous move at all, rather than reading a row.
        const int cnt = (p1 == 12 && t1 == 0 && ply < 1)
                        ? 8
                        : (int)corr_cont[p2][t2][p1][t1] + (int)corr_cont[p4][t4][p1][t1];
        cv += corr_w_cont * cnt;
    }
    return cv;
}

//The correction applied. Clamped away from the mate range for the same reason Stockfish clamps
//away from the tablebase range: a corrected static evaluation must stay a static evaluation and
//must never be mistaken for a proof.
static inline int corr_apply(int v, int cv) {
    return std::clamp(v + cv / corr_div, -MATE_IN_MAX + 1, MATE_IN_MAX - 1);
}

//The residual, written. Each key gets its own field of its own slot; the relative weights are
//Stockfish's and say that material and continuation residuals are worth slightly more than the
//pawn-structure one.
static inline void corr_update(const CorrKeys& k, int stm, int ply, int bonus) {
    corr_add(corr_tab[stm][k.pawn  & (CORR_SIZE - 1)].pawn,  bonus);
    corr_add(corr_tab[stm][k.minor & (CORR_SIZE - 1)].minor, bonus * 156 / 128);
    corr_add(corr_tab[stm][k.np[0] & (CORR_SIZE - 1)].npw,   bonus * 178 / 128);
    corr_add(corr_tab[stm][k.np[1] & (CORR_SIZE - 1)].npb,   bonus * 178 / 128);
    if (corrhist >= 2) {
        int p1, t1, p2, t2, p4, t4;
        corr_cont_idx(ply, p1, t1, p2, t2, p4, t4);
        corr_add(corr_cont[p2][t2][p1][t1], bonus * 127 / 128);
        corr_add(corr_cont[p4][t4][p1][t1], bonus *  59 / 128);
    }
}
//The index of the position being searched, written into every probe row. Without it the labelled
//rows cannot be split by position, and a plain row split leaks: ~160 decisions come from the same
//position, and pieces/n_moves/depth identify that position well enough for a model to memorise it
//rather than learn the rule. Set once per position by the main loop, read by the probe only.
static int pos_index = 0;
static std::FILE * lmrprobe_fp = nullptr;
static thread_local long long lmr_probed = 0, lmr_wrong = 0, lmr_over = 0;
static thread_local long long lmr_r1_safe = 0, lmr_r2_safe = 0;
//THE PROBER'S CROSS-POSITION ACCUMULATORS ARE GONE. They summed each thread_local counter
//over a whole test set so the harness could print one summary line at the end; an engine has
//no test set and no end. The per-thread counters they fed are still here and still counted,
//because the search reads some of them, and because they cost one increment each.
//THREE WAYS A SEARCH ENDS, and an engine needs all of them: its own clock, a limit the GUI set,
//and a "stop" arriving mid-search. The probe only had the first two, and only ever stopped itself.
//`stop_requested` is separate from `out_of_time` because stop() has to be able to interrupt a
//search that still has time left, and because a ponder search has no deadline at all.
static std::atomic<bool> stop_requested{false};
static bool unlimited = false;        //go infinite / go ponder: never stop on the clock
static inline bool timed_out() {
    if (out_of_time) return true;
    if (stop_requested.load(std::memory_order_relaxed)) { out_of_time = true; return true; }
    if (fixnodes > 0 && nodes >= fixnodes) { out_of_time = true; return true; }
    if (unlimited) return false;
    if ((nodes & 1023) == 0 && std::chrono::steady_clock::now() >= deadline) out_of_time = true;
    return out_of_time;
}

//THE TRANSPOSITION TABLE. The single biggest thing missing from the first version of the probe, and
//the reason it is worth adding: iterative deepening re-searches the same positions over and over, and
//without a table every re-search starts from nothing. The table gives back two quite different things
//-- a score that can cut the node off outright, and, when it cannot, the move that was best there last
//time, which is the cheapest and strongest ordering hint available.
//
//The incremental hash it is keyed on was verified against a fresh getHash() over 567,436 nodes with
//zero mismatches before this was written. An unverified hash does not produce a weaker search, it
//produces a wrong one: entries from unrelated positions collide and are believed.
enum { TT_NONE = 0, TT_EXACT = 1, TT_LOWER = 2, TT_UPPER = 3 };
//THE TABLE, MADE SAFE TO SHARE. Several threads write 16-byte entries with no lock, so a reader can
//see one thread's key beside another thread's score -- a TORN entry, which looks valid and is not.
//
//The fix is Hyatt's: the payload goes in one 64-bit word and `key ^ payload` in the other. A writer
//stores the payload first, then the xored key; a reader recomputes `stored_key ^ payload` and only
//believes the entry if it equals the hash it was looking for. A tear mixes words from two different
//writes, the xor fails, and the entry is simply ignored. Eight-byte aligned loads and stores are
//individually atomic on arm64, so no locking is needed for this to hold.
//
//Most engines skip this and accept torn entries, on the grounds that a wrong score is re-derived
//soon enough. It is here because every expensive mistake in this project has been a SILENT one, and
//a few instructions per probe is a cheap way to make this class of failure impossible rather than
//merely improbable.
//THE STATIC EVAL LIVES IN THE TABLE, which is what makes interior-node pruning free. Arkadi's
//point: iterative deepening starts at depth 1, so a node at ply p was a QUIESCENCE LEAF at iteration
//p+1 and was evaluated there. At every later iteration it is an interior node whose evaluation has
//already been paid for -- it only needs storing. Stockfish does exactly this, writing
//`unadjustedStaticEval` into the entry and reading `ttData.eval` back on a hit rather than
//evaluating again.
//
//It fits in the existing 16-byte entry because the old packing was far looser than the fields need:
//src and dst are squares (6 bits each, not 8), promo is a PieceType (3), and flag has four values
//(2). That frees 15 bits of the 64, and with score at 16 and depth at 8 the total is 41, leaving
//room for a 16-bit eval. The entry stays 16 bytes, so the table still holds 4M positions in 64 MB.
static const int16_t EVAL_NONE = INT16_MIN;
struct TTData {
    int16_t  score = 0;
    int16_t  eval  = EVAL_NONE;     //static eval of this position, or EVAL_NONE
    uint8_t  depth = 0;
    uint8_t  flag  = TT_NONE;
    uint8_t  src   = 0, dst = 0, promo = 0;
};
static inline uint64_t tt_pack(const TTData& d) {
    return  (uint64_t)(uint16_t)d.score
         | ((uint64_t)d.depth << 16)
         | ((uint64_t)(d.flag  & 3u)  << 24)
         | ((uint64_t)(d.src   & 63u) << 26)
         | ((uint64_t)(d.dst   & 63u) << 32)
         | ((uint64_t)(d.promo & 7u)  << 38)
         | ((uint64_t)(uint16_t)d.eval << 41);
}
static inline TTData tt_unpack(uint64_t w) {
    TTData d;
    d.score = (int16_t)(uint16_t)(w & 0xffff);
    d.eval  = (int16_t)(uint16_t)((w >> 41) & 0xffff);
    d.depth = (uint8_t)(w >> 16);
    d.flag  = (uint8_t)((w >> 24) & 3u);
    d.src   = (uint8_t)((w >> 26) & 63u);
    d.dst   = (uint8_t)((w >> 32) & 63u);
    d.promo = (uint8_t)((w >> 38) & 7u);
    return d;
}
struct TTEntry {
    uint64_t key = 0;      //hash ^ payload, so a torn pair cannot pass the check below
    uint64_t data = 0;
};
//Returns true and fills `out` only if the entry is both present and internally consistent.
static inline bool tt_read(const TTEntry * e, uint64_t hash, TTData& out) {
    const uint64_t d = e->data;              //payload first, key second: the reverse of the write
    const uint64_t k = e->key;
    if (d == 0 && k == 0) return false;      //never written
    if ((k ^ d) != hash) return false;        //wrong position, or a torn pair
    out = tt_unpack(d);
    return out.flag != TT_NONE;
}
//THE EVAL, READ. Deliberately independent of `flag`: an entry may hold a static eval and no bound at
//all (written by the quiescence stand-pat below), which tt_read() rejects because it has nothing to
//say about the score. Returns true only if the key is consistent AND an eval was actually stored.
static inline bool tt_read_eval(const TTEntry * e, uint64_t hash, int& out) {
    const uint64_t d = e->data;
    const uint64_t k = e->key;
    if (d == 0 && k == 0) return false;
    if ((k ^ d) != hash) return false;
    const int16_t ev = (int16_t)(uint16_t)((d >> 41) & 0xffff);
    if (ev == EVAL_NONE) return false;
    out = ev;
    return true;
}
//THE EVAL, WRITTEN. Only into a slot that is free or already holds this position -- never over
//another position's entry. That keeps the replacement policy exactly as it was, so an arm using this
//differs from the control in its PRUNING alone and not in what the table evicted.
static inline void tt_store_eval(TTEntry * e, uint64_t hash, int ev) {
    const uint64_t raw = e->data;
    const uint64_t k   = e->key;
    const bool empty = (raw == 0 && k == 0);
    const bool mine  = ((k ^ raw) == hash);
    //An entry carrying ONLY an eval and no bound is fair game for any position: losing a cached eval
    //costs one evaluation, while overwriting a searched entry costs a subtree. A searched entry
    //belonging to another position is still never touched.
    const bool evalonly = !empty && !mine && ((raw >> 24) & 3u) == TT_NONE;
    if (!empty && !mine && !evalonly) return;
    if (evalonly) { TTData fresh; fresh.eval = (int16_t)ev;
                    const uint64_t pk = tt_pack(fresh); e->data = pk; e->key = hash ^ pk; return; }
    TTData d;
    if (mine) d = tt_unpack(raw);
    d.eval = (int16_t)ev;
    const uint64_t packed = tt_pack(d);
    e->data = packed;
    e->key  = hash ^ packed;
}
static inline void tt_write(TTEntry * e, uint64_t hash, const TTData& in) {
    const uint64_t d = tt_pack(in);
    e->data = d;
    e->key  = hash ^ d;                       //published last
}
//TT=0 turns the table off. It exists because the original probe had no switch: ab_p3.tsv and
//ab_tt_p3.tsv were produced by two different BUILDS twenty minutes apart, which means the 2.9-point
//accuracy drop between them cannot be attributed to the table rather than to anything else edited
//that morning. An arm that needs a recompile is not an arm.
static int tt_on = 1;
//QTT=1 extends the table into quiescence; 0 is the incumbent behaviour exactly.
static int qtt_on = 0;
//DELTA: the delta-pruning margin in centipawns. -1 is OFF and is the incumbent search exactly.
static int delta_margin = -1;
//PVS=1 searches every move after the first on a null window; 0 is the incumbent full-window search.
static int pvs_on = 0;
//SEEORD: 0 = MVV-LVA as before, 1 = SEE in the main search, 2 = SEE in quiescence as well.
static int see_ord = 0;
static thread_local long long pvs_nulls = 0, pvs_researches = 0;
//ASP: the aspiration half-window in centipawns at the ROOT. 0 is OFF -- the root searches
//(-MATE, +MATE) as before.
static int asp_window = 0;
static thread_local long long asp_researches = 0;
static thread_local long long delta_pruned = 0;
static const size_t TT_BITS = 22;                      //4,194,304 entries, 64 MB
static const size_t TT_SIZE = (size_t)1 << TT_BITS;
static std::vector<TTEntry> tt;
static thread_local long long tt_probes = 0, tt_hits = 0, tt_cutoffs = 0, tt_move_hits = 0;

//SIZED AT RUNTIME from the Hash option, so the mask is a variable rather than the prober's
//compile-time (TT_SIZE - 1). It is always a power of two minus one -- apply_hash() rounds the
//requested megabytes DOWN to a power-of-two entry count, because this mask is the only thing
//keeping the index in range.
static size_t tt_mask = TT_SIZE - 1;
static inline TTEntry * tt_slot(uint64_t key) { return &tt[(size_t)key & tt_mask]; }
//Mate scores are stored root-relative and read back ply-relative; see MATE_IN_MAX.
static inline int tt_to_store(int sc, int ply) {
    if (sc >=  MATE_IN_MAX) return sc + ply;
    if (sc <= -MATE_IN_MAX) return sc - ply;
    return sc;
}
static inline int tt_from_store(int sc, int ply) {
    if (sc >=  MATE_IN_MAX) return sc - ply;
    if (sc <= -MATE_IN_MAX) return sc + ply;
    return sc;
}

//REPETITION, which only becomes dangerous once a table exists. Without one, a cycle merely wasted
//nodes; with one, a score from inside a cycle can be stored and then believed outside it. The search
//path's own hashes are kept and the FIRST recurrence is scored as a draw, which is the same rule
//creatica uses deliberately -- a threefold-correct rule lets the opponent force the draw.
static thread_local uint64_t path_hash[MAX_PLY + 8];
//THE POSITIONS THE GAME ALREADY VISITED, newest last, same side to move as the root on even
//offsets. The probe never needed this -- in a test set the root is the whole world -- but an
//engine that cannot see a repetition already on the board will repeat into a draw while winning,
//and will not claim one while losing. Written once per `position` command, read-only in search.
static std::vector<uint64_t> game_hashes;
static inline bool repeated(int ply) {
    for (int i = ply - 2; i >= 0; i -= 2) if (path_hash[i] == zh.hash) return true;
    //Walk the game backwards in steps of two: only positions with the same side to move can
    //repeat this one. `ply` parity decides where in the game history that stride starts.
    for (int i = (int)game_hashes.size() - 1 - (ply & 1); i >= 0; i -= 2)
        if (game_hashes[(size_t)i] == zh.hash) return true;
    return false;
}

//The canonical legal-only generator. `captures_only` keeps the quiescence search to captures and
//promotions, which is what stops a fixed depth from being evaluated in the middle of an exchange.
//TEMPORARY GUARD. kingMoves() dereferences a null pointer when the side to move has no king, which
//means the board has been corrupted before we get here. Catch it with the position in hand rather
//than reading a stack trace.
static thread_local int guard_ply = -1, guard_depth = -1;
static thread_local const char * guard_where = "?";
static void board_guard() {
    const uint64_t k = board.side[board.sideToMove] & board.pieceTypes[King - 1];
    if (k) return;
    char fen[MAX_FEN_STRING_LEN];
    std::fprintf(stderr, "BOARD CORRUPT at %s, ply %d depth %d: side to move %d has no king\n"
                 "  fen %s\n  white %016llx black %016llx kings %016llx\n",
                 guard_where, guard_ply, guard_depth, (int)board.sideToMove,
                 board2fen(board, fen),
                 (unsigned long long)board.side[0], (unsigned long long)board.side[1],
                 (unsigned long long)board.pieceTypes[King - 1]);
    std::fflush(stderr);
    std::abort();
}

//WRITES INTO A CALLER-OWNED ARRAY. It used to push_back into a std::vector, which meant a heap
//allocation at every node of the search -- 361,189 of them per position, 57% of those in quiescence.
//MAX_MOVES is 256 because that is the ceiling the rest of libchess uses (leaf_search.inc does the
//same with `Move mv[256]`). The count is returned; nothing else about the order changes, so the
//search tree must come out bit-identical.
static int generate(Mv * out, bool captures_only) {
    int n = 0;
    auto push = [&](uint8_t a, uint8_t b, uint8_t c) { if (n < MAX_MOVES) out[n++] = {a, b, c}; };
    board_guard();
    auto [kmoves, pinned, pinning, checkers, ksq] = kingMoves(board);
    const uint64_t them = board.side[1 - board.sideToMove];
    Move move = {}; move.src = ksq; move.promoType = PieceTypeNone;
    for (uint64_t m = kmoves; m; m &= m - 1) {
        const Square d = (Square)lsBit(m);
        if (captures_only && !((them >> d) & 1ULL)) continue;
        push((uint8_t)ksq, (uint8_t)d, (uint8_t)PieceTypeNone);
    }
    if (bitCount(checkers) > 1) return n;
    auto [cm, em] = checkers ? checkMask(board, ksq, checkers) : std::make_pair(~0ULL, 0ULL);
    for (PieceType pt = Queen; pt >= Pawn; --pt) {
        uint64_t occ = board.side[board.sideToMove] & board.pieceTypes[pt - 1];
        while (occ) {
            move.src = (Square)lsBit(occ);
            uint64_t mv = piece_moves(board, pt, move.src, ksq, pinned, pinning, cm, em);
            while (mv) {
                move.dst = (Square)lsBit(mv);
                PieceType s = PieceTypeNone, e = PieceTypeNone;
                if (promoMove(board, move)) { s = Knight; e = Queen; }
                const bool is_cap = ((them >> move.dst) & 1ULL) != 0;
                if (!captures_only || is_cap || s != PieceTypeNone) {
                    if (s == PieceTypeNone) push((uint8_t)move.src,(uint8_t)move.dst,(uint8_t)PieceTypeNone);
                    else for (PieceType q = s; q <= e; q = (PieceType)(q + 1))
                        push((uint8_t)move.src,(uint8_t)move.dst,(uint8_t)q);
                }
                move.promoType = PieceTypeNone; mv &= mv - 1;
            }
            occ &= occ - 1;
        }
    }
    return n;
}

//IS THE SIDE TO MOVE IN CHECK, computed rather than remembered.
//
//Computed from the board. There is no cached alternative any more -- the four derived flags were
//removed from Board in 36d447b, which is why STALECHECK and QSTALE are gone from this file.
//as a side effect, so it is only valid for the position kingMoves() was last called on. Reading it at
//the top of a node -- before generating that node's moves -- returns the parent's or a sibling's
//answer. That stale read let null-move pruning fire while the side to move was in CHECK: the opponent
//then captured the king, and the next generator call dereferenced a null pointer. It also fed the
//in-check depth extension and the mate-versus-stalemate decision at terminal nodes.
//EXT: the check extension. `if (in_check) ++depth` adds a ply every time the side to move is in
//check, and nothing bounds it -- in a forcing sequence the depth never decreases, so the search can
//spend without limit on checks. While the check test was reading a stale cache this line almost never
//fired, which is why the bug looked beneficial. EXT=0 turns the extension off, EXT=N allows at most N
//of them on one path, EXT=-1 is the old unbounded behaviour.
static int ext_limit = -1;
//THREADS: Lazy SMP thread count including thread 0. 1 is the single-threaded search exactly.
static int threads = 1;
//STAGGER: the Lazy SMP depth offset. Thread `tid` begins its iterative deepening at depth
//`1 + (tid % stagger)` instead of at depth 1, so the helpers are not repeating thread 0's exact
//iteration sequence. This is not an optimisation bolted onto Lazy SMP -- it is one of its
//load-bearing parts, and the baseline measured without it is therefore NOT a verdict on Lazy SMP.
//Without a stagger all threads do very nearly the same work, which is what the first smoke test
//showed: 3.47x the nodes at 4 threads for one ply LESS depth and branching 4.67 -> 5.47. The
//helpers were not adding search, they were adding memory traffic.
//STAGGER=1 is no offset (the behaviour measured so far). 2 puts every other thread a ply ahead.
//Thread 0 always starts at depth 1 whatever this is set to, because it is the thread that reports
//and it must complete every iteration to have a PV at the shallow depths.
//Forced to 1 under FIXDEPTH: a staggered helper could start above the fixed depth and never run,
//and the fixed-depth instrument is only useful while it stays comparable across builds.
static int stagger = 1;
//File scope because root_search() runs on every thread and needs both. Written once in main()
//before any thread starts, read-only afterwards.
static int fixdepth = 0;
static thread_local int ext_used = 0;
//THE EXTENSION SET, measured one at a time. Every one of these answers the same question -- "does
//this move deserve a ply the depth counter was not going to give it?" -- and they differ only in
//what evidence they accept. Each has its own knob and its own counter so that a gain can be
//attributed; the shared `ext_used` budget above bounds all of them together on one line, because
//any two extensions that can both fire on the same move can together stop the depth decreasing.
//
//ONEREPLY: the side to move has exactly one legal move. Extending is nearly free -- the node had
//one child whatever the depth -- and it stops a forced sequence from eating the horizon.
static int oner_on = 0;
//RECAP: the move captures on the square the previous move captured on. An exchange is a single
//tactical event and the depth counter cuts it in half; the extension lets it resolve.
static int recap_on = 0;
//PPEXT: a pawn pushed to the 7th rank (6th counts as well at PPEXT=2) with no enemy pawn able to
//stop it. Promotion is a material swing the static evaluation sees only once it happens.
static int pp_on = 0;
//MTEXT: the null-move search came back with a MATE score against us. The opponent having a mate
//if we do nothing is a threat, and the moves that answer a threat are usually not the ones the
//ordering put first -- so the node needs depth rather than less of it. Note this fires where
//null-move pruning FAILED, so it costs nothing where the pruning was already paying.
static int mt_on = 0;
static thread_local long long ext_oner = 0, ext_recap = 0, ext_pp = 0, ext_mt = 0;
//Accumulated across positions; the thread_local counters above are reset per position.
//Where the previous move captured, per ply, or 64 for "the previous move was quiet". Set by the
//move loop for its children, so the recapture test is a comparison and not a re-derivation.
static thread_local uint8_t prev_cap_sq[MAX_PLY];
//SINGEXT: the SINGULAR EXTENSION, in centipawns of margin; 0 is off. The reasoning is different in
//kind from the four above, which all recognise a SHAPE on the board. This one asks a question about
//the SEARCH: if every move except the table's best move fails to come within `SINGEXT` of it, then
//the table move is the only move holding the position up, and the line it leads to is worth a ply
//because getting it wrong loses the position rather than a tempo.
//
//It is answered by a VERIFICATION SEARCH: the node is searched again at half depth on a null window
//just below the table score, with the table move EXCLUDED. If that fails low, the move is singular.
//The cost is a second search of nearly the whole node, which is why it is gated on depth.
static int sing_margin = 0;
static int sing_depth = 6;        //SINGDEPTH: the minimum depth at which the test is worth its cost
//SINGPLIES: restrict BOTH singular variants to plies below this, -1 for all plies. It exists so the
//verification search and the policy margin can be compared on exactly the same set of nodes -- the
//policy only runs for ply < POLICY_PLIES, so an unrestricted comparison would be comparing two
//different populations of node and calling the difference a model result.
static int sing_plies = -1;
//POLSING: the POLICY singular extension, the substitution test. Same extension, same nodes, but the
//evidence is the policy head's top-two score margin instead of a verification search -- one forward
//pass that the ordering already paid for, against a second search of the node. Units are the
//policy's own score scale times 1000; 0 is off.
static int polsing_margin = 0;
//SINGNOVER: THE CONTROL for the singular extension, in parts per thousand; 0 is off. It grants the
//same extension on the same node population at a chosen RATE, with NO verification search and no
//evidence of any kind -- the decision is a hash of the position, which is uncorrelated with whether
//the move is actually singular. Set it to the rate the verification search grants at and the
//comparison isolates what the SEARCH contributed: if the control scores the same, the test is
//decoration and the gain belongs to "extend the table move near the root", which costs nothing.
//Deterministic, so the control is as reproducible as the arm it controls for.
static int singnover = 0;
static thread_local long long ext_sing = 0, sing_tests = 0, ext_polsing = 0, ext_nover = 0;
//The move excluded from the search at this ply, for the verification search. Only one move can be
//excluded at a ply, which is all the test needs.
static thread_local Mv sing_excl[MAX_PLY];
static thread_local bool sing_excl_set[MAX_PLY];
//QSTALE isolates the OTHER half of the stale-flag effect. In quiescence the check test decides two
//quite different things: whether to stand pat at all, and whether to generate every legal move or
//only captures. With the cache reading false, an in-check leaf was stand-patted with eval_cp(), and
//evaluate_nnue() returns its NNUE_CHECK sentinel in check, which eval_cp() maps to 0 -- so every
//position in check was scored a DEAD DRAW and never searched. Correct detection searches the evasions
//instead, which is right but far more expensive.
static inline bool stm_in_check() {
    const uint64_t k = board.side[board.sideToMove] & board.pieceTypes[King - 1];
    if (!k) return false;                       //should not happen; board_guard() reports it
    return getCheckers(board, (Square)lsBit(k)) != 0;
}

//EVALUATING A POSITION, INCLUDING ONE IN CHECK.
//
//evaluate_nnue() cannot be called while the side to move is in check -- it returns the NNUE_CHECK
//sentinel instead -- and this function used to map that sentinel to 0, so every position in check was
//scored a dead draw. Measured, that mistake was worth +13.5 points on this set against the thing I
//replaced it with, because "evade, then evaluate statically" is a horizon effect in the middle of a
//tactical sequence while "a check is worth 0" at least does not pretend to know.
//
//Both are wrong. The engine already has the right answer: process_check() takes a ONE-PLY MINIMAX over
//the evasions, which is a real value rather than a guess or a bound. That is what this does, with a
//small recursion budget so a perpetual check cannot run away.
static int eval_cp_depth(int budget, int alpha, int beta);
//Defined below with the ordering rules; declared here because the in-check search orders its
//evasions by it.
static inline int victim_value(const Mv& m);
static inline bool stm_in_check();
//WINDOWED. The unwindowed version of this took a full maximum over every legal evasion, which cost
//29.5 evaluations per in-check node and 84.5% of ALL evaluation work in the probe -- with the
//evaluation itself being ~90% of runtime, roughly three quarters of the search's time.
//
//The pattern came from creatica's MCTS, where `process_check()` scores a checked node as a 1-ply
//minimax over its children. There it is right and unavoidable: MCTS has no window to cut against.
//Ported into alpha-beta it is simply an unpruned search sitting inside a pruned one, and the fix is
//to hand it the window the caller already has. A beta cutoff then makes most evasions provably
//irrelevant, exactly as it does everywhere else.
static inline int eval_cp(int alpha, int beta) { return eval_cp_depth(2, alpha, beta); }
//For the stand-pat and other call sites that are NOT in check, where the window is irrelevant
//because evaluate_nnue() answers on the first line.
static inline int eval_cp() { return eval_cp_depth(2, -MATE_CP - 1, MATE_CP + 1); }

//WHERE THE EVALUATIONS GO. The profile says ~90% of runtime is inside evaluate_nnue, so an
//evaluation is the unit of cost and these counters are the only way to see which call site spends
//them. ev_check_children counts the ones spent on the in-check recursion below, which costs one
//evaluation per legal evasion instead of one for the node.
static thread_local long long ev_total = 0, ev_check_children = 0, ev_check_nodes = 0;
static int eval_cp_depth(int budget, int alpha, int beta) {
    ++ev_total;
    //COMPUTED, not cached. Before 36d447b this read evaluate_nnue()'s sentinel and treated it as a
    //check test -- but it guarded on board.isCheck, a cache that described whatever position
    //kingMoves() last saw. It fired on quiet positions, and the workaround below then spent ~29
    //evaluations resolving a check that was not there: 84.5% of every evaluation this probe
    //performed, a 4.3x slowdown and ten points of accuracy. The Board no longer carries the flag
    //and evaluate_nnue() takes it as an argument, so there is nothing left to go stale.
    const bool in_check = stm_in_check();
    if (!in_check) return (int)std::lround(evaluate_nnue(board, ctx, in_check) * 100.0);
    //In check, so the net cannot score it: resolve it by searching the evasions, with the caller's
    //window so a cutoff still ends it early. Out of budget, say so honestly rather than returning 0
    //or a bound -- a side in check with no resolution left is in trouble, and half a pawn down is a
    //far better guess than level.
    if (budget <= 0) return -50;
    ++ev_check_nodes;
    Mv ev[MAX_MOVES];
    const int nev = generate(ev, false);
    if (nev == 0) return -MATE_CP;                          //mate, since we are in check
    //Ordered so the cutoff happens on the first move at most nodes. Capturing the checking piece is
    //usually the best evasion, and victim value is what the rest of the probe orders by.
    {
        int sc[MAX_MOVES];
        for (int i = 0; i < nev; ++i) sc[i] = victim_value(ev[i]);
        sort_desc(ev, sc, nev);
    }
    int best = -MATE_CP;
    for (int ei = 0; ei < nev; ++ei) {
        const Mv& m = ev[ei];
        Move mm = {}; mm.src = (Square)m.src; mm.dst = (Square)m.dst; mm.promoType = (PieceType)m.promo;
        StateInfo st = {};
        auto [dp, dts] = accumulator_stack_push(ctx);
        do_move_dp(board, mm, st, dp, dts);
        const int r = -eval_cp_depth(budget - 1, -beta, -alpha);
        undo_move(board, mm, st); accumulator_stack_pop(ctx);
        ++ev_check_children;                                //counted per evasion ACTUALLY searched
        if (r > best) best = r;
        if (best > alpha) alpha = best;
        //Fail-soft: a value at or above beta is a bound the caller can already act on, and the
        //remaining evasions cannot change what it does with it.
        if (alpha >= beta) break;
    }
    return best;
}

//Captures-first by the value of the piece captured. The one hand-crafted rule in the probe; see the
//note at the top of the file.
static const int PIECE_CP[7] = {0, 100, 320, 330, 500, 900, 0};
//SEE comes from the library: get_see() in board.cpp, revived 2026-10-12, with attackers_to()
//beside it. A private copy lived here briefly and was deleted -- it mishandled en passant, reading
//the victim off a destination square that an ep capture leaves empty. see_test.cpp covers the
//library version on eight positions with unambiguous material answers.
//get_see() takes libchess's Move; the probe carries its own 3-byte Mv.
static inline Move to_move(const Mv& m) {
    Move mm = {};
    mm.src = (Square)m.src; mm.dst = (Square)m.dst; mm.promoType = (PieceType)m.promo;
    return mm;
}

static inline int victim_value(const Mv& m) {
    const int p = board.piecesOnSquares[m.dst];
    if (p == PieceNone) return 0;
    return PIECE_CP[p & 7];
}

//Set by order_by_policy() for the node it was called on, and only valid immediately after that
//call -- the policy runs at a node, and any recursion invalidates it.
static thread_local float pol_margin = 0.0f;
static thread_local bool pol_margin_valid = false;

//Order by the policy head: one feature pass, one spatial forward, then a score per move. Used only
//near the root, because it costs about ten evaluations a node.
static void order_by_policy(Mv * mv, int n) {
    if (n < 2) return;
    ++policy_orders;
    nnue_features(board, ctx, pfeat.data());
    policy_context(pnet, pfeat.data(), pctx);
    const bool flip = (board.sideToMove == ColorBlack);
    if (pnet.has_wl) {
        size_t rows[MAX_MOVES];
        for (int i = 0; i < n; ++i)
            rows[i] = policy_row(pnet, board.piecesOnSquares[mv[i].src] & 7, mv[i].src, mv[i].dst, flip);
        std::sort(rows, rows + n);
        const int nr = (int)(std::unique(rows, rows + n) - rows);
        policy_apply_legal_bias(pnet, pctx, rows, nr);
    }
    static PolicySpatialScratch scr;
    const PolicySpatialScratch * sp = nullptr;
    if (pnet.spatial.loaded) {
        int fr[MAX_MOVES], to[MAX_MOVES], pt[MAX_MOVES];
        for (int i = 0; i < n; ++i) { fr[i] = flip ? (mv[i].src ^ 56) : mv[i].src;
                                      to[i] = flip ? (mv[i].dst ^ 56) : mv[i].dst;
                                      pt[i] = board.piecesOnSquares[mv[i].src] & 7; }
        static unsigned char planes[26 * 64];
        policy_build_planes(board, pnet.spatial, fr, to, pt, n, planes);
        policy_spatial_forward(pnet.spatial, planes, scr);
        sp = &scr;
    }
    float sc[MAX_MOVES];
    for (int i = 0; i < n; ++i)
        sc[i] = policy_score(pnet, pctx, board.piecesOnSquares[mv[i].src] & 7,
                             mv[i].src, mv[i].dst, flip, sp);
    //Stable, descending, same as the std::stable_sort it replaces -- float scores, so it cannot
    //share sort_desc()'s int array.
    for (int i = 1; i < n; ++i) {
        const Mv m = mv[i]; const float s2 = sc[i];
        int j = i - 1;
        while (j >= 0 && sc[j] < s2) { mv[j + 1] = mv[j]; sc[j + 1] = sc[j]; --j; }
        mv[j + 1] = m; sc[j + 1] = s2;
    }
    //THE TOP-TWO MARGIN, kept for the policy singular extension. It is a margin in the policy's own
    //SCORE units (a logit difference), not a probability difference -- the scores are never put
    //through a softmax here, so the threshold has to be swept rather than read as a confidence.
    //This is free: the scores were already computed and sorted for the ordering.
    pol_margin = sc[0] - sc[1];
    pol_margin_valid = true;
}

static thread_local Mv prev_pv[MAX_PLY];   //the previous iteration's PV, for ordering
static thread_local bool prev_pv_set[MAX_PLY];
static thread_local Mv pv_line[MAX_PLY][MAX_PLY];
static thread_local int pv_len[MAX_PLY];

//QTT=1 lets the transposition table see the QUIESCENCE nodes. It could not before: the probe and
//the store both sit behind `depth > 0` in search(), and quiescence is 56% of every node this probe
//visits -- so the table was working on 44% of the tree, which is the most likely reason it buys only
//half a ply here when a transposition table usually transforms an engine.
//
//Soundness, which is the whole reason this is an arm and not a patch:
//  - a qsearch entry is stored at depth 0, and search() probes with `e->depth >= depth` where depth
//    is at least 1, so a capture-only result can never satisfy a full-width node. That asymmetry is
//    what makes this safe.
//  - depth-preferred replacement means a depth-0 store only ever evicts another depth-0 entry, so
//    quiescence cannot flush the deep entries the main search paid for.
//  - MATE SCORES ARE NOT STORED from quiescence. `ply` is now a parameter -- correction history
//    needs it for the continuation keys -- so the root-relative adjustment tt_to_store() needs is
//    available, but it is deliberately still not applied here: `-MATE_CP` is unadjusted and
//    changing what quiescence stores would change every arm measured against this baseline.
//=== SYZYGY INSIDE THE SEARCH ===
//WHAT THIS BUYS over the root probe alone. The root probe only fires when the ending is ALREADY on
//the board. This one fires on every node that reaches the tables, so a search five plies from a
//won or drawn ending gets the exact verdict instead of an NNUE guess -- which is how an engine
//knows to walk into a drawn rook ending rather than avoid it, or to avoid one it thinks is won.
//Fathom's own note says engines should use tb_probe_wdl() during search.
//
//PLY-INDEPENDENT ON PURPOSE. A win is a win, so the score carries no distance and needs no
//root-relative adjustment -- which matters because tt_to_store()/tt_from_store() only adjust the
//MATE band, so a ply-relative score below it would be stored at one ply and read at another and be
//silently wrong. The cost of carrying no distance is that the search has no reason to prefer a
//faster win, and the answer to that is the ROOT probe: it uses DTZ and always plays the
//distance-optimal move when the position is in the tables, so conversion is handled where it can
//be handled exactly.
//
//TWO TRAPS IN THE API, both of which bite silently:
//  - tb_probe_wdl() RETURNS TB_RESULT_FAILED WHENEVER rule50 != 0. So 0 must be passed, and it is
//    not a shortcut: passing the real halfmove clock -- which this search does maintain correctly
//    through do_move/undo_move -- would have disabled the probe on nearly every position. The
//    consequence to know about is that the WDL value therefore IGNORES the fifty-move rule, so in
//    a position with a high clock a reported win may in truth be a draw.
//  - castling rights also force a failure, because Syzygy cannot express castling. Excluded here
//    rather than probed and discarded.
static int tb_search = 1;                    //use the tables inside the search
static unsigned tb_pieces_max = 0;           //TB_LARGEST once initialised; 0 means no tables
static const int TB_SCORE_CP = 20000;        //well below MATE_IN_MAX, so no mate arithmetic applies
static thread_local long long tbhits = 0;

static inline bool tb_probe_node(int& out) {
    if (!tb_search || tb_pieces_max == 0 || board.castlingRights) return false;
    const unsigned pieces = (unsigned)bitCount(board.side[ColorWhite] | board.side[ColorBlack]);
    if (pieces > tb_pieces_max) return false;
    const Square ep = legalEnPassantMove(board);
    const unsigned wdl = tb_probe_wdl(
        board.side[ColorWhite], board.side[ColorBlack], board.pieceTypes[King - 1],
        board.pieceTypes[Queen - 1], board.pieceTypes[Rook - 1], board.pieceTypes[Bishop - 1],
        board.pieceTypes[Knight - 1], board.pieceTypes[Pawn - 1],
        0, 0, ep == SquareNone ? 0 : (unsigned)ep,
        board.sideToMove == ColorWhite ? 1 : 0);
    if (wdl == TB_RESULT_FAILED) return false;
    ++tbhits;
    //0 loss, 4 win, 1..3 draw. 1, 2 and 3 are blessed loss, draw and cursed win -- decided on the
    //board but drawn by the fifty-move rule -- and all three really are draws for our purposes.
    out = wdl == 4 ? TB_SCORE_CP : (wdl == 0 ? -TB_SCORE_CP : 0);
    return true;
}

static int qsearch(int alpha, int beta, int ext, int ply) {
    ++nodes; ++qnodes;
    if (timed_out()) return 0;
    const bool in_check = stm_in_check();
    const int alpha_orig = alpha;
    Mv qtt_move{0, 0, 0}; bool have_qtt_move = false;
    if (tt_on && qtt_on) {
        ++tt_probes;
        const TTEntry * e = tt_slot(zh.hash);
        TTData d;
        if (tt_read(e, zh.hash, d)) {
            ++tt_hits;
            qtt_move = {d.src, d.dst, d.promo}; have_qtt_move = true;
            //Any stored depth is acceptable HERE: a deeper entry is a better answer than quiescence
            //would compute, and a depth-0 one is the same quality.
            const int sc = d.score;
            if (std::abs(sc) < MATE_IN_MAX) {
                if (d.flag == TT_EXACT) { ++tt_cutoffs; return sc; }
                if (d.flag == TT_LOWER && sc >= beta)  { ++tt_cutoffs; return sc; }
                if (d.flag == TT_UPPER && sc <= alpha) { ++tt_cutoffs; return sc; }
            }
        }
    }
    //THE TABLES IN QUIESCENCE TOO. Quiescence is about half the nodes, and an ending reached by a
    //sequence of captures is exactly the shape of position that lands in the tables.
    if (tb_search && ply > 0) {
        int tbv;
        if (tb_probe_node(tbv)) return tbv;
    }
    //Hoisted out of the branch below so delta pruning can use it. -MATE_CP when in check, where
    //there is no stand-pat and no pruning either.
    int stand = -MATE_CP;
    if (!in_check) {
        stand = eval_cp();
        //Stored HERE rather than with the entry at the end of the function: the next line returns on
        //a stand-pat at or above beta, which is a large share of quiescence nodes, and those would
        //otherwise never record the evaluation they just paid for.
        if (tt_on && ttse) tt_store_eval(tt_slot(zh.hash), zh.hash, stand);
        //STORED RAW, USED CORRECTED, exactly as Stockfish does it. The table holds the unadjusted
        //evaluation and the correction is applied after reading, which is the only ordering that
        //stays right when the tables have moved on by the time the entry is read again.
        if (corrhist && corrq) {
            const CorrKeys qk = corr_keys(board);
            const int qcv = corr_value(qk, board.sideToMove, ply);
            if (qcv) {
                const int before = stand;
                stand = corr_apply(stand, qcv);
                ++corr_applied; corr_abs_sum += std::abs(stand - before);
            }
        }
        if (stand >= beta) return stand;
        if (stand > alpha) alpha = stand;
    }
    //eval_cp() now values an in-check position properly, so the horizon no longer needs a special case
    //that returned a BOUND (alpha) dressed up as a value.
    if (ext <= 0) return eval_cp(alpha, beta);
    Mv mv[MAX_MOVES];
    const int n = generate(mv, !in_check);          //in check, every legal move is a reply
    if (in_check && n == 0) return -MATE_CP;
    {
        int score[MAX_MOVES];
        //SEEORD=2 orders quiescence by SEE too. Every move here is a capture, so this is the most
        //expensive place to call it -- quiescence is about half of all nodes -- which is exactly why
        //it is a separate level rather than implied by SEEORD=1.
        if (see_ord >= 2) for (int i = 0; i < n; ++i) score[i] = get_see(board, to_move(mv[i]));
        else              for (int i = 0; i < n; ++i) score[i] = victim_value(mv[i]);
        sort_desc(mv, score, n);
    }
    if (have_qtt_move)
        for (int i = 0; i < n; ++i)
            if (same(mv[i], qtt_move)) { ++tt_move_hits; std::swap(mv[0], mv[i]); break; }
    int best = in_check ? -MATE_CP : alpha;
    //The move that actually produced `best`, for the table. Storing mv[0] instead -- the first move
    //in sorted order -- is what the previous version did: it can only mislead the ordering hint,
    //never a value, but it meant the entry pointed at whatever MVV-LVA happened to rank first rather
    //than at the move that mattered, so the arm was understating itself.
    //NOT mv[0]: with no captures and not in check, n is 0 and mv[] is UNINITIALISED here -- qsearch
    //returns early only for the in-check-and-no-moves case. Reading it stored uninitialised stack
    //bytes into the table as a best-move hint, which made the search non-reproducible: two runs of
    //the same binary at a fixed depth disagreed by 19 nodes on one position. A null move is stored
    //instead, which no generated move matches, so the hint is simply absent.
    Mv qbest{0, 0, 0};
    //MOVES ACTUALLY SEARCHED at this node, which is what the cap counts. Not the loop index: a move
    //that delta pruning or the SEE filter threw away never cost a node and must not use up the
    //allowance of two.
    int q_count = 0;
    for (int i = 0; i < n; ++i) {
        const Mv& m = mv[i];
        //DELTA PRUNING. If the stand-pat score plus everything this capture can possibly win still
        //does not reach alpha, no line beginning with it can either, so it is skipped.
        //
        //THIS IS THE ONE UNSOUND THING IN THE PROBE, and it is unsound in a specific direction: a
        //skipped move is GONE, unlike a late move reduction, which re-searches anything that beats
        //alpha, or the transposition table, which is exact. The moves it discards are the ones that
        //look materially hopeless -- and a sacrifice that wins by force looks exactly like that. So
        //it cannot be judged on node counts: it changes the value the search returns, and the only
        //question is whether the positions it gets wrong outnumber the depth it buys.
        //
        //The margin is a tunable, which is why DELTA sweeps rather than taking one value: a single
        //setting that happened to win would be a tuning artefact reported as an effect.
        if (delta_margin >= 0 && !in_check) {
            int gain = victim_value(m);
            if (m.promo >= Knight && m.promo <= Queen) gain += PIECE_CP[m.promo] - PIECE_CP[Pawn];
            if (stand + gain + delta_margin <= alpha) { ++delta_pruned; continue; }
        }
        //QUIESCENCE PRUNING, Stockfish's Step 6, decided before the move is made. `best` standing
        //at -MATE_CP means we are in check with nothing yet proved, and pruning there could miss
        //the only escape; that is Stockfish's !is_loss(bestValue) gate.
        if (qprune && !in_check && best > -MATE_IN_MAX) {
            ++qp_tested;
            const bool is_promo   = (m.promo >= Knight && m.promo <= Queen);
            //A RECAPTURE IS NEVER PRUNED. The previous move landed on this square, so the exchange
            //is still open and its value is not yet known -- prev_cap_sq is the same array the
            //recapture extension uses, and search() has already set it for this ply.
            const bool recapture  = (ply < MAX_PLY && prev_cap_sq[ply] == m.dst);
            if (!is_promo && !recapture) {
                if (q_count > q_mcount) { ++qp_mcount; continue; }
                if (qprune >= 2) {
                    const int fv = stand + q_futbase + victim_value(m);
                    if (fv <= alpha) {
                        ++qp_fut;
                        if (fv > best) best = fv;      //it is a bound on this move, so it raises best
                        continue;
                    }
                    if (get_see(board, to_move(m)) < alpha - stand - q_futbase) {
                        ++qp_fut;
                        const int cap = std::min(alpha, stand + q_futbase);
                        if (cap > best) best = cap;
                        continue;
                    }
                }
            }
            //Applies to EVERY move including recaptures and promotions: a capture that loses
            //material outright is not worth a node even here.
            if (get_see(board, to_move(m)) < q_see) { ++qp_see; continue; }
        }
        ++q_count;
        Move mm = {}; mm.src = (Square)m.src; mm.dst = (Square)m.dst; mm.promoType = (PieceType)m.promo;
        StateInfo st = {};
        auto [dp, dts] = accumulator_stack_push(ctx);
        const ZobristHash saved = zh;
        //THE CHILD'S RECAPTURE TEST needs to know where this capture landed. search() maintains
        //this for its own children; quiescence has to as well, or every quiescence node below the
        //first would exempt the wrong square.
        if (ply + 1 < MAX_PLY) prev_cap_sq[ply + 1] = (uint8_t)m.dst;
        //The captured type comes from do_move_dp, and the board is post-move when updateHash runs --
        //the convention the engine itself uses, verified by hashcheck.
        updateHash(zh, board, mm, do_move_dp(board, mm, st, dp, dts), z);
        const int v = -qsearch(-beta, -alpha, ext - 1, ply + 1);
        undo_move(board, mm, st); accumulator_stack_pop(ctx); zh = saved;
        if (out_of_time) return 0;
        if (v > best) { best = v; qbest = m; }
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
    }
    if (tt_on && qtt_on && !out_of_time && std::abs(best) < MATE_IN_MAX
        && !(repcheck == 2 && rep_tainted)) {
        TTEntry * e = tt_slot(zh.hash);
        //Depth 0, so this can only replace another quiescence entry, never a searched one.
        TTData old;
        const bool have = tt_read(e, zh.hash, old);
        if (!have || old.depth == 0) {
            TTData d;
            d.score = (int16_t)best;
            d.depth = 0;
            d.flag  = (uint8_t)(best >= beta ? TT_LOWER : (best > alpha_orig ? TT_EXACT : TT_UPPER));
            //n == 0 leaves qbest as the null move, which is what we want in the table.
            d.src = qbest.src; d.dst = qbest.dst; d.promo = qbest.promo;
            tt_write(e, zh.hash, d);
        }
    }
    return best;
}

//IS THIS MOVE A PASSED PAWN PUSH to at least `min_rel` ranks from the mover's own side?
//
//Called on the pre-move board, so src/dst are read from the position the move is made in. It is
//only reached for pawn moves that already got far enough, which is why the file/rank mask is built
//with a loop rather than a precomputed table -- it runs on a tiny fraction of moves.
static bool passed_pawn_push(const Mv& m, int min_rel) {
    const int stm = board.sideToMove;
    if (PC_TYPE(board.piecesOnSquares[m.src]) != Pawn) return false;
    const int r = (int)SQ_RANK((Square)m.dst), f = (int)SQ_FILE((Square)m.dst);
    const int rel = (stm == ColorWhite) ? r : 7 - r;
    if (rel < min_rel) return false;
    //No enemy pawn on this file or either neighbour, on any rank ahead of the destination, can
    //stop or capture it on the way.
    const uint64_t enemy_pawns = board.side[1 - stm] & board.pieceTypes[Pawn - 1];
    uint64_t block = 0;
    for (int ff = f - 1; ff <= f + 1; ++ff) {
        if (ff < 0 || ff > 7) continue;
        for (int rr = 0; rr < 8; ++rr) {
            const bool ahead = (stm == ColorWhite) ? (rr > r) : (rr < r);
            if (ahead) block |= 1ULL << (rr * 8 + ff);
        }
    }
    return (enemy_pawns & block) == 0;
}

static int search(int depth, int alpha, int beta, int ply) {
    ++nodes;
    pv_len[ply] = 0;
    if (timed_out()) return 0;
    if (ply >= MAX_PLY - 2) return eval_cp(alpha, beta);
    guard_ply = ply; guard_depth = depth; guard_where = "search";
    bool granted = false;
    struct ExtGuard { bool& g; ExtGuard(bool& b) : g(b) {} ~ExtGuard() { if (g) --ext_used; } } eg(granted);
    if (repcheck && ply > 0 && repeated(ply)) { rep_tainted = true; return 0; }  //see repeated()
    path_hash[ply] = zh.hash;
    const int alpha_orig = alpha;
    const bool in_check = stm_in_check();
    //Bounded by ext_limit; see the note there. `ext_used` counts the extensions already granted on
    //this path, so the limit is per line rather than per search.
    if (in_check && (ext_limit < 0 || ext_used < ext_limit)) { ++depth; ++ext_used; granted = true; }

    //THE TABLE, read. A stored EXACT score is the answer; a LOWER bound can only cut off against beta
    //and an UPPER bound only against alpha. The root is exempt from cutoffs, because the whole point
    //of searching it is to come back with a move and a principal variation rather than a bound.
    Mv tt_move{0, 0, 0}; bool have_tt_move = false;
    //Kept out of the probe's scope for the singular test below, which needs the stored score and
    //how trustworthy it is, not just the move.
    int tt_score = 0, tt_flag = -1, tt_depth = -1;
    //EXCLUDED: this invocation is a verification search for the singular extension, re-searching
    //the same node with one move left out. Three things change. The stored score cannot cut off,
    //because it was computed WITH the excluded move and so answers a different question. Nothing is
    //written back for the same reason. And the test does not recurse.
    const bool excluded = sing_excl_set[ply];
    if (tt_on && depth > 0) {
        ++tt_probes;
        const TTEntry * e = tt_slot(zh.hash);
        TTData d;
        if (tt_read(e, zh.hash, d)) {
            ++tt_hits;
            tt_move = {d.src, d.dst, d.promo}; have_tt_move = true;
            tt_score = tt_from_store(d.score, ply); tt_flag = (int)d.flag; tt_depth = (int)d.depth;
            if (ply > 0 && !excluded && (int)d.depth >= depth) {
                const int sc = tt_from_store(d.score, ply);
                if (d.flag == TT_EXACT) { ++tt_cutoffs; return sc; }
                if (d.flag == TT_LOWER && sc >= beta)  { ++tt_cutoffs; return sc; }
                if (d.flag == TT_UPPER && sc <= alpha) { ++tt_cutoffs; return sc; }
            }
        }
    }

    if (depth <= 0) return qsearch(alpha, beta, qdepth, ply);

    //NULL-MOVE PRUNING. Give the opponent a free move; if the position is still so good that it fails
    //high at reduced depth, the real move list is not worth searching at full depth either.
    //
    //Guarded against zugzwang by requiring a non-pawn piece for the side to move -- in a pawn endgame
    //"doing nothing" is often better than any move, and the assumption the method rests on fails.
    //
    //The hash is recomputed with getHash() rather than XORed by hand. A null move changes the side to
    //move and clears en passant, and guessing the Zobrist convention for those two would corrupt every
    //table entry below this node; a fresh hash costs a fraction of a microsecond and cannot be wrong.
    //The accumulator is untouched on purpose: no piece moves, and the NNUE keeps both perspectives, so
    //there is nothing to update. After a null move the opponent cannot be in check -- our position was
    //legal and it was our turn, so their king was not attacked.
    //THE NODE'S STATIC EVALUATION, fetched once and shared by every rule below it. In mode 1 it is
    //whatever the previous iteration left in the table when this position was a quiescence leaf, so
    //it costs nothing; in mode 2 a miss is computed and stored, which costs an evaluation.
    int nev = 0; bool have_nev = false;
    if (evmode && !in_check && ply > 0) {
        ++ev_lookups;
        if (tt_read_eval(tt_slot(zh.hash), zh.hash, nev)) { have_nev = true; ++ev_hits; }
        else if (evmode >= 2) {
            nev = eval_cp(); have_nev = true;
            if (ttse) tt_store_eval(tt_slot(zh.hash), zh.hash, nev);
        }
    }
    //THE TABLES, ASKED BEFORE THE EVALUATION. Not at the root: ply 0 has to produce a move, and the
    //root's own DTZ probe already answers that case exactly. Stored at the maximum depth because the
    //answer IS exact -- it can legitimately satisfy any deeper probe, which is the one case where a
    //depth-preferred table should never replace an entry.
    if (tb_search && ply > 0) {
        int tbv;
        if (tb_probe_node(tbv)) {
            if (tt_on && !out_of_time && !excluded) {
                TTEntry * e = tt_slot(zh.hash);
                TTData d;
                d.score = (int16_t)tbv;
                d.depth = 255;
                d.flag  = TT_EXACT;
                tt_write(e, zh.hash, d);
            }
            return tbv;
        }
    }

    //THE CORRECTION, read once and applied before anything consumes the evaluation. Everything
    //downstream -- razoring, the null-move gate, reverse futility, per-move futility, the
    //improving flag, and the residual written back at the end of this node -- sees the corrected
    //value, which is what makes this a closed loop rather than a decoration.
    CorrKeys ck; int cv = 0; const int nev_raw = nev;
    if (corrhist && !in_check) {
        ck = corr_keys(board);
        cv = corr_value(ck, board.sideToMove, ply);
        if (have_nev && cv) {
            nev = corr_apply(nev, cv);
            ++corr_applied; corr_abs_sum += std::abs(nev - nev_raw);
        }
    }
    if (ply < MAX_PLY) ss_eval[ply] = have_nev ? nev : EVAL_UNKNOWN;
    //IMPROVING compares against OUR OWN evaluation two plies ago, which is why the stack is indexed
    //by ply rather than carried down the recursion.
    const bool improving = (have_nev && ply >= 2 && ss_eval[ply - 2] != EVAL_UNKNOWN)
                           ? (nev > ss_eval[ply - 2]) : false;

    //RAZORING (Stockfish Step 7). An evaluation far enough below alpha says this node will not reach
    //it, so quiescence answers it instead of a search. Null window only, and the quiescence value is
    //returned as-is because it is a real value rather than an assumption.
    if (razor && have_nev && ply > 0 && depth >= 1 && beta == alpha + 1
        && alpha > -MATE_IN_MAX) {
        ++razor_tested;
        if (nev < alpha - razor_base - razor_quad * depth * depth) {
            ++razor_pruned;
            return qsearch(alpha, beta, qdepth, ply);
        }
    }

    //REVERSE FUTILITY. Before the null move, as in Stockfish, because it is strictly cheaper: one
    //table read against the null move's whole reduced search. Null window only and not on the root,
    //and never when beta is already a mate bound -- a mate claim must be proved by searching, not
    //assumed from an evaluation.
    if (rfut && have_nev && ply > 0 && depth <= rfut_depth && depth >= 1 && beta == alpha + 1
        && beta < MATE_IN_MAX && beta > -MATE_IN_MAX) {
        ++rfut_tested;
        if (nev - rfut_margin * depth + rfut_imp * improving >= beta) { ++rfut_pruned; return beta; }
    }
    //The gate Stockfish puts on its null move: skip it unless the evaluation already suggests the
    //node fails high. Without an evaluation there is nothing to gate on, so a miss tries it as before.
    const bool null_gated = nullgate && have_nev
                            && nev < beta - nullgate_slope * depth + nullgate_base;
    if (null_gated) ++nullgate_skipped;
    if ((prune & 1) && !null_gated && ply > 0 && !in_check && depth >= 3 && beta < MATE_IN_MAX) {
        const int stm = board.sideToMove;
        const uint64_t non_pawn = board.side[stm]
                                  & ~board.pieceTypes[Pawn - 1] & ~board.pieceTypes[King - 1];
        if (non_pawn) {
            ++null_tries;
            //NULL MOVE REDUCTION, and the largest single discrepancy against Stockfish 18's
            //search.cpp. Stockfish uses `R = 7 + depth / 3` and searches `depth - R`; this used
            //`2 + (depth > 6)` and searches `depth - 1 - R`, so an effective 3 or 4. At depth 9
            //Stockfish spends a quiescence search on its null move where this spends a depth-5
            //search -- and the null move is tried at every interior node that reaches it, so the
            //difference lands directly on the per-ply multiplier m.
            //
            //Stockfish can afford the large R because it only tries the null move when the static
            //evaluation already suggests a fail-high (`staticEval >= beta - 18 * depth + 350`) and
            //verifies it at high depth. This probe has no node static evaluation, so it tries the
            //null move blind; whether a big R survives without the gate is the question the sweep
            //answers rather than assumes.
            const int R = nullr_base + (nullr_div > 0 ? depth / nullr_div
                                                      : (depth > 6 ? 1 : 0));
            const ZobristHash saved = zh;
            const File saved_ep = board.enPassant;
            board.sideToMove = (Color)(1 - stm);
            board.enPassant  = FileNone;
            getHash(zh, board, z);
            //No piece moved, so the child's continuation keys must point at the "no move" row
            //rather than inherit whatever this ply last tried.
            if (ply < MAX_PLY) { ss_mv_pc[ply] = 12; ss_mv_to[ply] = 0; }
            const int v = -search(depth - 1 - R, -beta, -beta + 1, ply + 1);
            board.sideToMove = (Color)stm;
            board.enPassant  = saved_ep;
            zh = saved;
            if (out_of_time) return 0;
            if (v >= beta) { ++null_cuts; return beta; }
            //MATE THREAT. The null move did NOT cut off, and the value that came back says the
            //opponent mates us if we do nothing. The reply has to be found rather than guessed, and
            //the ordering has no reason to put it first, so the node gets a ply instead of less.
            //This costs nothing where null-move pruning was already paying -- it only fires on the
            //searches that failed.
            if (mt_on && v < -MATE_IN_MAX && (ext_limit < 0 || ext_used < ext_limit) && !granted) {
                ++depth; ++ext_used; granted = true; ++ext_mt;
            }
        }
    }

    Mv mv[MAX_MOVES];
    const int n = generate(mv, false);
    if (n == 0) return in_check ? -MATE_CP + ply : 0;
    //ONE REPLY. With a single legal move the node has one child whatever the depth, so the ply is
    //very nearly free, and a forced sequence no longer consumes the horizon it did not choose to
    //enter. `granted` guards against stacking this on top of the check extension on the same node,
    //which is the common case -- a single legal move usually means a check.
    if (oner_on && n == 1 && !granted && (ext_limit < 0 || ext_used < ext_limit)) {
        ++depth; ++ext_used; granted = true; ++ext_oner;
    }
    //Invalidated before the ordering runs, so a margin left behind by a DIFFERENT node can never
    //be read as this node's. order_by_policy() sets it again, and returns early without setting it
    //when there are fewer than two moves -- which is the case where there is no margin to speak of.
    pol_margin_valid = false;
    if (ply < policy_plies) order_by_policy(mv, n);
    else if (!ordering) {
        int score[MAX_MOVES];
        for (int i = 0; i < n; ++i) score[i] = victim_value(mv[i]);
        sort_desc(mv, score, n);
    }
    else {
        //ONE SCORE PER MOVE, in tiers: captures by the piece they win, then the two killers for this
        //ply, then the quiet moves by their history. The tiers are far apart so a tier never leaks
        //into the one below it.
        const int stm = board.sideToMove;
        const Mv k0 = killers[ply][0], k1 = killers[ply][1];
        //Scored in place: the vector of (score, move) pairs this replaces was a second heap
        //allocation per node, plus a copy of every move back into the list afterwards.
        int score[MAX_MOVES];
        for (int i = 0; i < n; ++i) {
            const Mv& m = mv[i];
            const int vv = victim_value(m);
            if (vv > 0 || m.promo != PieceTypeNone) {
                //SEE ORDERING. MVV-LVA sorts by what is captured and ignores what defends it, so
                //RxP defended by a pawn (-400) outranks NxN (0) -- a move that loses a rook ahead of
                //one that loses nothing. SEE answers the question MVV-LVA approximates, so a LOSING
                //capture drops below the quiet moves instead of leading them.
                //
                //The tiers: good captures 4000000+, killers 3000000/2900000, quiets 0..1048576
                //(history is capped at 1<<20, which is what keeps it out of the killer tier), and
                //losing captures below all of them at -1000000+see. Nothing can cross a boundary:
                //see is at most ~1300 and at least ~-1000.
                if (see_ord) {
                    const int sv = get_see(board, to_move(m));
                    score[i] = (sv >= 0) ? 4000000 + sv : -1000000 + sv;
                } else score[i] = 4000000 + vv;
            }
            else if (same(m, k0))                   score[i] = 3000000;
            else if (same(m, k1))                   score[i] = 2900000;
            else                                    score[i] = history[stm][m.src][m.dst];
        }
        sort_desc(mv, score, n);
    }
    //The policy's own first choice, captured BEFORE the table and previous-PV swaps move it. The
    //policy singular extension needs it: the margin says "the policy's top move is clear of its
    //second", which is only evidence about the TABLE move when the two are the same move. Without
    //this check the test would fire on a confident policy and extend a different move entirely.
    const Mv pol_top = mv[0];
    //THE TABLE, used for ordering. This is the half that pays even when the score cannot cut off: the
    //move that was best here on a shallower pass is overwhelmingly the move that is best now.
    if (have_tt_move)
        for (int i = 0; i < n; ++i)
            if (same(mv[i], tt_move)) { ++tt_move_hits; std::swap(mv[0], mv[i]); break; }
    //The previous iteration's move for this ply goes first: that is what makes iterative deepening
    //pay for itself, and it is the cheapest ordering improvement available.
    if (prev_pv_set[ply])
        for (int i = 1; i < n; ++i)
            if (same(mv[i], prev_pv[ply])) { std::swap(mv[0], mv[i]); break; }
    //THE SINGULAR TEST. Run here, after ordering, because the policy variant needs the margin the
    //ordering just computed, and because both variants need the move list to know whether the table
    //move is even present. `sing_ok` means the table move -- which ordering has put at mv[0] -- has
    //earned its extra ply, and the move loop applies it to that move only.
    bool sing_ok = false;
    const bool sing_eligible =
        have_tt_move && !excluded && ply > 0 && !in_check && depth >= sing_depth
        && (sing_plies < 0 || ply < sing_plies)
        && tt_depth >= depth - 3 && (tt_flag == TT_EXACT || tt_flag == TT_LOWER)
        && std::abs(tt_score) < MATE_IN_MAX
        && (ext_limit < 0 || ext_used < ext_limit)
        && same(mv[0], tt_move);
    if (sing_eligible && singnover > 0) {
        ++sing_tests;
        if ((int)((zh.hash >> 29) % 1000u) < singnover) { sing_ok = true; ++ext_nover; }
    } else if (sing_eligible && polsing_margin > 0) {
        //THE SUBSTITUTION. The policy's confidence that its first move is clear of its second,
        //instead of a search proving that the alternatives fail. Free where the policy already ran;
        //it tells us nothing where it did not, so the test simply does not fire there.
        if (pol_margin_valid && same(pol_top, tt_move)
            && pol_margin * 1000.0f >= (float)polsing_margin) {
            sing_ok = true; ++ext_polsing;
        }
        ++sing_tests;
    } else if (sing_eligible && sing_margin > 0) {
        ++sing_tests;
        const int sb = tt_score - sing_margin;
        sing_excl[ply] = tt_move; sing_excl_set[ply] = true;
        const int sv = search(depth / 2, sb - 1, sb, ply);
        sing_excl_set[ply] = false;
        //The verification search ran at THIS ply and overwrote the PV being built here. The move
        //loop only rewrites it when a move improves on `best`, so a node where every move fails low
        //would otherwise report the verification search's line as its own.
        pv_len[ply] = 0;
        if (!out_of_time && sv < sb) { sing_ok = true; ++ext_sing; }
    }
    if (sing_ok) ++ext_used;      //released after the move loop

    //The node's own static evaluation, for futility pruning only, computed once and only when the
    //cheap gates already pass. Not in check, so eval_cp() is a single NNUE call rather than the
    //evasion recursion.
    int fut_static = 0; bool fut_live = false;
    if (fut && !in_check && depth <= fut_depth && depth >= 1 && beta == alpha + 1) {
        if (evmode)        { fut_static = nev; fut_live = have_nev; }
        else if (fut >= 2) { ++ev_lookups; fut_live = tt_read_eval(tt_slot(zh.hash), zh.hash, fut_static);
                             if (fut_live) ++ev_hits; }
        else               { fut_static = eval_cp(); fut_live = true; }
    }

    int best = -MATE_CP - 1;
    Mv best_mv = mv[0];
    //THE QUIET MOVES ACTUALLY SEARCHED at this node. On a cutoff the move that caused it is
    //rewarded and these are penalised, which is what makes the table mean "this reply works after
    //that move" rather than merely "this move appears often". Pruned moves are NOT in here: they
    //were never tried, so they are evidence of nothing.
    Mv q_tried[64]; int n_q_tried = 0;
    const bool tainted_on_entry = rep_tainted;
    rep_tainted = false;
    int move_index = 0;
    for (int mi = 0; mi < n; ++mi) {
        const Mv& m = mv[mi];
        if (excluded && same(m, sing_excl[ply])) continue;   //the move under test, left out
        Move mm = {}; mm.src = (Square)m.src; mm.dst = (Square)m.dst; mm.promoType = (PieceType)m.promo;
        //Quiet or not has to be decided BEFORE the move is made: victim_value() reads the destination
        //square's occupant.
        const bool quiet = (victim_value(m) == 0 && m.promo == PieceTypeNone);
        //Features for the LMR probe that can only be read BEFORE the move is made: SEE and the
        //victim both look at the destination square's occupant, and the killer and table tests
        //compare against this ply's own entries. Computed only when the probe is on, so a normal
        //run pays nothing for them -- get_see() in particular is not free.
        int probe_see = 0, probe_victim = 0, probe_killer = 0, probe_ttmove = 0, probe_hist = 0;
        if (lmrprobe > 0) {
            probe_see = quiet ? 0 : get_see(board, to_move(m));
            probe_victim = victim_value(m);
            probe_killer = same(m, killers[ply][0]) ? 1 : (same(m, killers[ply][1]) ? 2 : 0);
            probe_ttmove = (have_tt_move && same(m, tt_move)) ? 1 : 0;
            probe_hist = history[board.sideToMove][m.src][m.dst];
        }
        //PER-MOVE EXTENSIONS. These extend the CHILD rather than the node: `ext` is added back to
        //the depth passed down, so only the move that earned it gets the ply. They are mutually
        //exclusive by construction (at most one ply) so that two cheap tests cannot together stop
        //the depth decreasing, and they share the per-line `ext_used` budget with the node-level
        //ones. Both are decided BEFORE the move is made -- victim_value() and passed_pawn_push()
        //read the pre-move board.
        int ext = 0;
        if (sing_ok && same(m, tt_move)) ext = 1;            //the budget was taken above
        else if ((recap_on || pp_on) && (ext_limit < 0 || ext_used < ext_limit)) {
            if (recap_on && !quiet && m.dst == prev_cap_sq[ply]) { ext = 1; ++ext_recap; }
            else if (pp_on && passed_pawn_push(m, pp_on >= 2 ? 5 : 6)) { ext = 1; ++ext_pp; }
            if (ext) ++ext_used;      //released after the child returns, below
        }
        //The child's recapture test needs to know where this move captured, or that it did not.
        if (ply + 1 < MAX_PLY) prev_cap_sq[ply + 1] = quiet ? (uint8_t)64 : (uint8_t)m.dst;
        //SEE IS READ HERE AND USED AFTER THE MOVE. get_see() needs the position the move is made
        //FROM, but the decision needs the reduction r and the gives-check test, which both need the
        //position after. So the value is taken now and spent below -- the same split MCP uses, and
        //the reason the pruned move still costs a make and unmake.
        int pre_see = 0; bool pre_see_live = false;
        if (seeprune && ply > 0 && best > -MATE_IN_MAX
            && (quiet ? true : seeprune >= 2)
            && (board.side[board.sideToMove]
                & ~board.pieceTypes[Pawn - 1] & ~board.pieceTypes[King - 1])) {
            pre_see = get_see(board, to_move(m));
            pre_see_live = true;
        }
        //Recorded for the continuation correction table, as the piece that will be STANDING on the
        //destination -- so a promotion is keyed on what it became, which is what Stockfish's
        //piece_on(to_sq()) reads after the move.
        if (ply < MAX_PLY) {
            const int moved = (m.promo >= Knight && m.promo <= Queen)
                              ? m.promo : PC_TYPE(board.piecesOnSquares[m.src]);
            ss_mv_pc[ply] = (uint8_t)(board.sideToMove * 6 + moved - 1);
            ss_mv_to[ply] = m.dst;
        }
        StateInfo st = {};
        auto [dp, dts] = accumulator_stack_push(ctx);
        const ZobristHash saved = zh;
        updateHash(zh, board, mm, do_move_dp(board, mm, st, dp, dts), z);
        //MOVE COUNT PRUNING. LMR searches a late quiet move shallower; this does not search it at
        //all. Both bets are invisible when wrong -- a reduction that hid a good move is never
        //re-searched either -- but the skip returns the whole subtree instead of a fraction of it,
        //which is why it moves m and LMR's reduction depth did not (a constant +1 reduction cost 8
        //positions).
        //
        //Decided AFTER the move is made, which costs a make/unmake per pruned move and buys the
        //gives-check guard. ~90% of this probe's runtime is inside evaluate_nnue, so a make/unmake
        //that returns a whole subtree is very nearly free, and a checking move is exactly the one a
        //move-count rule must not throw away.
        //
        //Null window only: on the PV a wrong skip changes the move the engine plays, while on a null
        //window it can only loosen a bound the caller still has to accept. An extended move is never
        //pruned, for the same reason it is never reduced -- the extension looked at the position and
        //the move count only at the index. `best > -MATE_IN_MAX` keeps it from firing when every
        //move searched so far loses to mate, where the escape may well be the move about to be cut.
        if (mcp && !ext && quiet && !in_check && depth <= mcp_depth && depth >= mcp_min
            && beta == alpha + 1
            && best > -MATE_IN_MAX && move_index >= mcp_base + (mcp == 1 ? depth * depth : 2 * depth)
            && !stm_in_check()) {
            ++mcp_tested; ++mcp_pruned;
            undo_move(board, mm, st); accumulator_stack_pop(ctx); zh = saved;
            continue;
        }
        if (mcp && !ext && quiet && !in_check && depth <= mcp_depth && depth >= mcp_min) ++mcp_tested;
        //FUTILITY PRUNING. Same guards as the move count rule and for the same reasons: null window
        //only, never an extended move, never while every move so far loses to mate, and never a move
        //that gives check. The test itself is the position's, not the move's.
        if (fut_live && !ext && quiet && best > -MATE_IN_MAX) {
            ++fut_tested;
            if (fut_static + fut_margin * depth <= alpha && !stm_in_check()) {
                ++fut_pruned;
                undo_move(board, mm, st); accumulator_stack_pop(ctx); zh = saved;
                continue;
            }
        }
        //LATE MOVE REDUCTIONS. The ordering has already put the moves most likely to be best in front,
        //so the ones at the back are searched shallower on the bet that they are not. Half the bet is
        //checked rather than trusted: anything that still beats alpha is re-searched at full depth. The
        //other half is not checkable at all - when the reduced search fails low, nothing re-searches it,
        //so a reduction that hid a good move is invisible to the search that made it. The probe measures
        //that half at 0.19% of reductions by searching the child a second time at full depth.
        int r = 0;
        const int cd_guard = depth - 1 + ext;      //the child's depth before any reduction
        if ((prune & 2) && depth >= 3 && move_index >= 4 && quiet && !in_check && !stm_in_check()) {
            r = 1 + (move_index >= 8 ? 1 : 0) + lmrplus;
            //SELECTIVE BOLDNESS, from 27,718 labelled reductions. The error in reducing one ply deeper
            //is not spread evenly over the decisions: it concentrates by a factor of ten into two cells
            //of a two-bit table. A null window with a capture on the previous move is 53% of all
            //reductions and costs 0.122% at r+1 against 0.095% at r, so one more ply there is nearly
            //free; a full window with no preceding capture is 6% of reductions and is already wrong
            //2.15% of the time at the current r, so it wants less reduction rather than more. Both bits
            //are free at the decision point - the window is in hand and prev_cap_sq was already kept
            //for the recapture extension.
            const bool nullwin   = (beta == alpha + 1);
            const bool after_cap = (prev_cap_sq[ply] != 64);
            if ((lmrsel & 1) &&  nullwin &&  after_cap)          r += 1;
            if ((lmrsel & 2) && !nullwin && !after_cap && r > 0) r -= 1;
        }
        if (quiet && n_q_tried < 64) q_tried[n_q_tried++] = m;
        //A reduction may not take the child below the horizon: at cd - r <= 0 the child is a
        //quiescence search whatever the reduction, so a larger r past that point buys nothing and
        //only makes the arm harder to read.
        //THE SHAPED REDUCTION, in 1024ths of a ply so the statScore term has somewhere to go. The
        //flat integer path above is left exactly as it was and is what runs when LMRSHAPE is off,
        //which is what keeps every earlier measurement reproducible.
        if (lmrshape && r > 0) {
            int r1024;
            if (lmrbase) {
                //Stockfish's base: reductions[i] = 21.46 * ln(i), scaled by depth AND move number,
                //plus its 1182 offset. Growing with both is the point -- a flat rule reduces the
                //fourth move at depth 20 the same as at depth 4.
                const double ld = std::log((double)std::max(1, depth));
                const double lm = std::log((double)std::max(1, move_index));
                r1024 = (int)(21.46 * ld * 21.46 * lm) + 1182;
            } else {
                r1024 = r * 1024;
            }
            //The whole point of the exercise: a move with a good track record after the move that
            //led here is barely reduced; a bad one is reduced far more than any constant would dare.
            const int ss_sc = (ply < MAX_PLY)
                            ? stat_score(ply, (int)ss_mv_pc[ply], m.dst, m.src,
                                         board.sideToMove ^ 1)
                            : 0;
            r1024 -= ss_sc * shape_mul / std::max(1, shape_div);
            ++shape_n; shape_sum += r1024; shape_absum += std::abs(ss_sc);
            r = r1024 / 1024;
            if (r < 0) r = 0;
        }
        if (r > cd_guard) r = cd_guard;
        //An extended move is never also reduced: the two tests disagree about the same move and the
        //extension is the one that looked at the position rather than at the move's index.
        if (ext) r = 0;
        const int cd = depth - 1 + ext;
        //SEE PRUNING, decided here because this is the first point where the reduced depth the move
        //would actually receive is known. Checking moves are exempt for the same reason they are
        //exempt from move-count pruning: a checking move is exactly the one a static material count
        //cannot be trusted about.
        if (pre_see_live && !ext && !stm_in_check()) {
            const int lmr_depth = std::max(cd - r, 0);
            bool drop = false;
            if (quiet) {
                ++seep_q_tested;
                if (pre_see < -see_quad * lmr_depth * lmr_depth) { ++seep_q_pruned; drop = true; }
            } else if (seeprune >= 2) {
                ++seep_c_tested;
                //Stockfish's guard against pruning the sacrifice of our last piece: allowed when we
                //are not already losing, or when the piece moved is not all the material we have.
                const uint64_t np = board.side[1 - board.sideToMove]
                                    & ~board.pieceTypes[Pawn - 1] & ~board.pieceTypes[King - 1];
                if ((alpha >= 0 || bitCount(np) > 1)
                    && pre_see < -see_cap_base * depth) { ++seep_c_pruned; drop = true; }
            }
            if (drop) {
                undo_move(board, mm, st); accumulator_stack_pop(ctx); zh = saved;
                continue;
            }
        }
        int v;
        if (r) {
            ++lmr_tries;
            const long long n_before_red = nodes;
            v = -search(cd - r, -alpha - 1, -alpha, ply + 1);
            const long long c_red = nodes - n_before_red;   //clean: nothing has searched this child yet
            //THE PROBE. Sampled deterministically off the position hash so a collection run can be
            //repeated exactly. It runs BEFORE the ordinary re-search so that both searches see the
            //same table state as nearly as possible -- the reduced search has already written its
            //own entries either way, which the full search may legitimately use.
            if (lmrprobe > 0 && !out_of_time
                && (int)((zh.hash >> 17) % 1000u) < lmrprobe) {
                //ORDER MATTERS, AND GETTING IT WRONG INVALIDATES THE WHOLE PROBE. The searches
                //run SHALLOWEST FIRST. The first version ran the full-depth search second and the
                //reduced probes after it, so the full search stored the child at depth `cd`, and
                //every shallower probe then satisfied the table's `e->depth >= depth` test and was
                //answered with the full-depth value it had just written. It reported that r+1
                //agreed with full depth 99.92% of the time against the rule's own r at 99.54% --
                //a deeper reduction cannot be MORE accurate than a shallower one, and that
                //impossibility was the tell. Shallowest-first is safe in the other direction: a
                //shallow entry can never satisfy a deeper probe.
                //NODE COST, which is the quantity a reduction is actually bought with. Ranking the
                //decisions by error PER DECISION said the null-window-after-capture cell was the
                //safe place to reduce harder; reducing harder there gained 0.1 ply and lost 3
                //positions, because that cell is half the decisions but only a sixth of the saving
                //-- its subtrees are already cheap. The ratio that matters is error per node saved.
                long long c_full = 0;
                int v_r2, v_r1, v_full;
                if (lmrcost) {
                    const long long n0 = nodes;
                    v_full = -search(cd, -alpha - 1, -alpha, ply + 1);
                    c_full = nodes - n0;
                    v_r2 = -search(cd - r - 2, -alpha - 1, -alpha, ply + 1);
                    v_r1 = -search(cd - r - 1, -alpha - 1, -alpha, ply + 1);
                } else {
                    v_r2 = -search(cd - r - 2, -alpha - 1, -alpha, ply + 1);
                    v_r1 = -search(cd - r - 1, -alpha - 1, -alpha, ply + 1);
                    const long long n0 = nodes;
                    v_full = -search(cd, -alpha - 1, -alpha, ply + 1);
                    c_full = nodes - n0;      //UNDERSTATED here: the two searches above warmed the table
                }
                const int v_red  = v;
                if (!out_of_time) {
                    ++lmr_probed;
                    const bool red_beats  = v_red  > alpha;
                    const bool full_beats = v_full > alpha;
                    const bool r1_beats   = v_r1   > alpha;
                    const bool r2_beats   = v_r2   > alpha;
                    if (r1_beats == full_beats) ++lmr_r1_safe;
                    if (r2_beats == full_beats) ++lmr_r2_safe;
                    const bool wrong = (red_beats != full_beats);
                    //OVER-pruning: the reduction said "this move cannot beat alpha" and the honest
                    //search disagrees. This is the invisible error described above.
                    const bool over = (!red_beats && full_beats);
                    if (wrong) ++lmr_wrong;
                    if (over)  ++lmr_over;
                    if (lmrprobe_fp) {
                        //The features are exactly those available AT THE DECISION, and no others:
                        //anything needing an evaluation is deliberately excluded, because a rule
                        //that cannot be afforded at the decision point is not a candidate.
                        std::fprintf(lmrprobe_fp,
                            "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d"
                            "\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%lld\t%lld\n",
                            wrong ? 1 : 0, over ? 1 : 0, move_index, depth, ply,
                            probe_see, probe_victim, m.promo != PieceTypeNone ? 1 : 0,
                            probe_hist, probe_killer, probe_ttmove, (beta == alpha + 1) ? 1 : 0,
                            n, prev_cap_sq[ply] != 64 ? 1 : 0, stm_in_check() ? 1 : 0,
                            tt_depth, tt_flag, bitCount(board.side[0] | board.side[1]),
                            beta - alpha, r, r1_beats == full_beats ? 1 : 0,
                            r2_beats == full_beats ? 1 : 0, pos_index, c_red, c_full);
                    }
                }
            }
            if (!out_of_time && v > alpha) { ++lmr_researches; v = -search(cd, -beta, -alpha, ply + 1); }
        } else if (pvs_on && move_index > 0) {
            //PRINCIPAL VARIATION SEARCH. The ordering has already put the move most likely to be
            //best first, so every move after it is searched on a NULL WINDOW -- a question with a
            //yes/no answer ("can this beat alpha?") rather than "what is this worth?", which cuts
            //off far sooner. Only a move that answers yes is re-searched with the real window to
            //find out what it is actually worth.
            //
            //Sound, and in the same way LMR is: the null-window result is a BOUND, and a bound that
            //claims the move cannot beat alpha is all the caller needed. `v < beta` guards the
            //re-search because at or above beta the node is cutting off anyway and the exact value
            //would be thrown away. The first move keeps the full window: with nothing yet above
            //alpha there is no bound to test against.
            ++pvs_nulls;
            v = -search(cd, -alpha - 1, -alpha, ply + 1);
            if (!out_of_time && v > alpha && v < beta) {
                ++pvs_researches;
                v = -search(cd, -beta, -alpha, ply + 1);
            }
        } else {
            v = -search(cd, -beta, -alpha, ply + 1);
        }
        ++move_index;
        //The singular grant is released once after the loop, not here -- it was taken once for the
        //node, and this move can be reached only once.
        if (ext && !(sing_ok && same(m, tt_move))) --ext_used;
        undo_move(board, mm, st); accumulator_stack_pop(ctx); zh = saved;
        if (out_of_time) return 0;
        if (v > best) {
            best = v; best_mv = m;
            pv_line[ply][0] = m;
            for (int i = 0; i < pv_len[ply + 1]; ++i) pv_line[ply][i + 1] = pv_line[ply + 1][i];
            pv_len[ply] = pv_len[ply + 1] + 1;
        }
        if (best > alpha) alpha = best;
        if (alpha >= beta) {
            ++cut_nodes; cut_index_sum += move_index - 1;
            if (move_index == 1) ++cut_first;
            //A BETA CUTOFF BY A QUIET MOVE is the only thing worth remembering. A capture that cuts
            //off is already ordered first by its victim, so recording it would teach the tables
            //nothing they do not already know.
            if (ordering && victim_value(m) == 0 && m.promo == PieceTypeNone) {
                if (!same(m, killers[ply][0])) {
                    killers[ply][1] = killers[ply][0];
                    killers[ply][0] = m;
                }
                history_bump(board.sideToMove, m, depth);
                ++history_cutoffs;
                //CONTINUATION HISTORY, written. The move that cut off is rewarded and every quiet
                //move tried at this node that did NOT cut off is penalised -- both halves matter,
                //because a table that only ever goes up cannot tell a good reply from a common one.
                //Stockfish's bonus and malus, unchanged except for the depth cap being in plies.
                if (conthist) {
                    const int bonus = std::min(116 * depth - 81, 1515);
                    const int malus = std::min(848 * depth - 207, 2446) - 17 * move_index;
                    cont_update(ply, m, board.sideToMove, bonus);
                    for (int qi = 0; qi < n_q_tried; ++qi)
                        if (!same(q_tried[qi], m))
                            cont_update(ply, q_tried[qi], board.sideToMove, -malus);
                }
            }
            break;
        }
    }
    if (sing_ok) --ext_used;
    //A VERIFICATION SEARCH WITH NOTHING LEFT TO SEARCH. The excluded move was the only legal move,
    //so the node has no alternatives at all -- which is the strongest possible form of singular.
    //`best` would otherwise be returned as -MATE_CP-1, a value that is not a score of anything.
    //Failing low is the right answer and is what makes the caller extend.
    if (excluded && move_index == 0) return alpha;
    //THE TABLE, written. Depth-preferred replacement: a deeper entry is worth more than a newer one,
    //and the table is cleared between positions anyway, so there is no age to track.
    //CORRECTION HISTORY, WRITTEN. The node has just learned what the position is really worth, and
    //the gap against what the evaluation predicted is the signal. Three conditions, all Stockfish's:
    //  - not in check, where there is no static evaluation to be wrong about;
    //  - the best move is not a capture, because a capture's value comes from material the
    //    evaluation already sees, so the residual would be charged to the wrong feature;
    //  - the sign test (best > static) == (a best move was found). A node that failed low found
    //    nothing better than the evaluation and a node with a best move found something better;
    //    the other two combinations are bounds rather than evidence about the evaluation.
    //The bonus is scaled by depth, because a deep node's verdict is worth more than a shallow one's.
    if (corrhist && !in_check && have_nev && !out_of_time && !excluded) {
        if (std::abs(best) < MATE_IN_MAX && best > alpha_orig && best < beta) {
            ++corr_all_n;
            corr_all_raw += std::abs(best - nev_raw);
            corr_all_cor += std::abs(best - nev);
        }
        const bool have_best = best > alpha_orig;
        const bool best_is_cap = have_best && victim_value(best_mv) != 0;
        if (!best_is_cap && (best > nev) == have_best) {
            const int bonus = std::clamp((best - nev) * depth / (have_best ? 10 : 8),
                                         -CORR_LIMIT / 4, CORR_LIMIT / 4);
            corr_update(ck, board.sideToMove, ply, bonus);
            ++corr_updates;
            //Mate scores are left out: the residual against a mate is not an evaluation error, and
            //one of them would swamp a million ordinary nodes.
            if (std::abs(best) < MATE_IN_MAX) {
                ++corr_res_n;
                corr_res_raw += std::abs(best - nev_raw);
                corr_res_cor += std::abs(best - nev);
            }
        }
    }
    //Classified exactly as the table entry is: a fail-high is a cut node, a fail-low an all-node,
    //and a score inside the window a PV node. `move_index` is the moves actually searched, `n` the
    //moves that were available.
    if (!excluded && !out_of_time) {
        if (best >= beta)             { ++nt_cut; nt_cut_gen += n; nt_cut_srch += move_index; }
        else if (best > alpha_orig)   { ++nt_pv;  nt_pv_gen  += n; nt_pv_srch  += move_index; }
        else                          { ++nt_all; nt_all_gen += n; nt_all_srch += move_index; }
    }
    const bool subtree_tainted = rep_tainted;
    rep_tainted = tainted_on_entry || subtree_tainted;
    if (tt_on && !out_of_time && !excluded && !(repcheck == 2 && subtree_tainted)) {
        TTEntry * e = tt_slot(zh.hash);
        TTData old;
        const bool have = tt_read(e, zh.hash, old);
        if (!have || (int)old.depth <= depth) {
            TTData d;
            d.score = (int16_t)tt_to_store(best, ply);
            d.depth = (uint8_t)std::min(depth, 255);
            d.flag  = (uint8_t)(best >= beta ? TT_LOWER : (best > alpha_orig ? TT_EXACT : TT_UPPER));
            d.src = best_mv.src; d.dst = best_mv.dst; d.promo = best_mv.promo;
            tt_write(e, zh.hash, d);
        }
    }
    return best;
}

static std::string uci_of(const Mv& m) {
    static const char * pl = " nbrq";
    std::string s = std::string(square[m.src]) + square[m.dst];
    if (m.promo >= Knight && m.promo <= Queen) s += pl[m.promo];
    return s;
}

//LAZY SMP. Every thread runs THIS, on its own board, its own NNUE context and its own ordering
//tables, and the threads communicate through ONE shared structure: the transposition table. There
//is no work splitting, no synchronisation and no idle time -- a thread that finishes an iteration
//starts the next one, and the entries its siblings left behind make its search cheaper. That is the
//whole algorithm, and it is why alpha-beta SMP needs a fraction of the machinery creatica's shared
//MCTS tree does (atomic edges, virtual loss, a per-node expanding flag, a collector that has to
//coordinate with live searchers).
//
//The threads are NOT given staggered depths. Pure Lazy SMP lets them diverge on their own: they
//race, so they reach different depths at different moments and overwrite each other's entries, and
//that is the source of the diversity. A stagger is an optimisation to try once the plain version is
//measured, not part of the baseline.
//
//MULTI-THREADED RUNS ARE NOT REPRODUCIBLE. The interleaving decides which entries exist when, so
//the fixed-depth determinism that caught a real bug in this probe today is simply not available
//here. Thread 0's result is the one reported, and the comparison that means anything is 1 thread
//against N at EQUAL TIME, repeated.
struct RootResult {
    std::string best_uci;
    int reached = 0, score = 0;
    long long nodes = 0, qnodes = 0;
};

static thread_local bool ctx_ready = false;

//==================================================================================================
//THE ENGINE LAYER. Everything above is the search as the prober measured it; everything below turns
//it into something a GUI, a tournament or the lichess bot can play against.
//==================================================================================================
#include "uci_engine.h"
#include "json.hpp"
#include <curl/curl.h>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <condition_variable>

//THE ROOT POSITION, owned by the protocol thread and read by the search threads. Only ever written
//between searches -- set_position() stops a running search first, for the same reason creatica's
//does: every one of these is read by a search in flight.
static std::string root_fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1";
//THE SAME 300 ms creatica uses, repeated here rather than included: the constant lives in
//creatica_search.hpp, which is the MCTS engine's private header and pulls in its whole tree of
//MCTS settings. What it pays for is documented at length in creatica.cpp -- the bestmove has to
//reach the driver and the next position come back, and that time comes off the clock without ever
//having been in the allocation.
static const double LATENCY_ALLOWANCE = 300.0;
//Well inside the mate band (MATE_IN_MAX is 29936), so print_score() reports it as a centipawn
//score and no mate-distance arithmetic is applied to it.
static const int TB_WIN_CP = 10000;


static int root_move_number = 1;

//COPIED FROM creatica.cpp's CreaticaEngine, deliberately and with regret.
//
//normalise_castling() and is_legal() are 40 lines of subtle, hard-won move handling -- the
//king-takes-rook castling dialect lichess speaks in standard games, and the refusal of any move
//string that is not legal on the board we actually hold. They are private statics of the MCTS
//engine's class, so there is no way to call them from here.
//
//Duplicating them is exactly the fault uci_engine.h's own header comment complains about: a bug
//fixed in one copy stays unfixed in the other. They belong in a shared header, and the only reason
//they are not in one is that factoring them out edits creatica.cpp, which means relinking the
//deployed ./creatica binary -- not something to do while the bot can run. FACTOR THESE OUT the
//next time creatica is rebuilt on purpose.
static void normalise_castling(const Board& b, Move& move) {
    if (move.promoType != PieceTypeNone) return;
    if (b.isChess960) return;
    const Piece mover = static_cast<Piece>(b.piecesOnSquares[move.src]);
    const Piece onDst = static_cast<Piece>(b.piecesOnSquares[move.dst]);
    if (PC_TYPE(mover) != King || PC_TYPE(onDst) != Rook) return;
    if (PC_COLOR(mover) != PC_COLOR(onDst)) return;
    if (!(b.castlingRooks & (1ULL << move.dst))) return;
    const Square from = static_cast<Square>(move.src), to = static_cast<Square>(move.dst);
    const File f = SQ_FILE(to) > SQ_FILE(from) ? FileG : FileC;
    move.dst = SQ(SQ_RANK(from), f);
}

static bool is_legal_move(Board& b, const Move& m) {
    if (m.src >= Square_NB || m.dst >= Square_NB) return false;
    if (!((b.side[b.sideToMove] >> m.src) & 1ULL)) return false;
    auto [king_moves, pinned, pinning, checkers, kingSq] = kingMoves(b);
    if (m.src == kingSq) return (king_moves >> m.dst) & 1ULL;
    if (bitCount(checkers) > 1) return false;
    auto [check_mask, ep_mask] = checkers ? checkMask(b, kingSq, checkers)
                                          : std::make_pair(0xffffffffffffffffULL, 0ULL);
    const PieceType pt = PC_TYPE(static_cast<Piece>(b.piecesOnSquares[m.src]));
    if (pt < Pawn || pt > Queen) return false;
    const uint64_t mv = piece_moves(b, pt, static_cast<Square>(m.src), kingSq,
                                    pinned, pinning, check_mask, ep_mask);
    return (mv >> m.dst) & 1ULL;
}

//A SCORE AS UCI WANTS IT. Mates are returned by the search root-relative as -MATE_CP + ply, so the
//distance in plies is MATE_CP - |v| and UCI wants it in MOVES, rounded up.
static void print_score(int v) {
    if (v > MATE_IN_MAX)       std::printf("score mate %d",  (MATE_CP - v + 1) / 2);
    else if (v < -MATE_IN_MAX) std::printf("score mate %d", -(MATE_CP + v + 1) / 2);
    else                       std::printf("score cp %d", v);
}

//The per-thread state the probe set up once per test position. Unchanged except that the root comes
//from root_fen rather than a test-set row.
static bool worker_setup(int tid) {
    if (!ctx_ready) {
        init_nnue_context(ctx);
        pfeat.resize((size_t)nnue_feature_dims());
        ctx_ready = true;
    }
    board = Board{};
    if (fen2board(board, root_fen.c_str()) != 0) return false;
    accumulator_stack_reset(ctx);
    getHash(zh, board, z);
    std::memset(prev_pv_set, 0, sizeof prev_pv_set);
    std::memset(path_hash, 0, sizeof path_hash);
    std::memset(killers, 0, sizeof killers);
    std::memset(history, 0, sizeof history);
    nodes = qnodes = policy_orders = 0;
    tbhits = 0;
    rep_tainted = false; ext_used = 0;
    killer_first = history_cutoffs = 0;
    null_tries = null_cuts = lmr_tries = lmr_researches = 0;
    asp_researches = 0;
    ext_oner = ext_recap = ext_pp = ext_mt = 0;
    ext_sing = sing_tests = ext_polsing = ext_nover = 0;
    for (int i = 0; i < MAX_PLY; ++i) ss_eval[i] = EVAL_UNKNOWN;
    std::memset(ss_mv_pc, 12, sizeof ss_mv_pc);
    std::memset(ss_mv_to, 0, sizeof ss_mv_to);
    std::memset(prev_cap_sq, 64, sizeof prev_cap_sq);
    std::memset(sing_excl_set, 0, sizeof sing_excl_set);
    pol_margin_valid = false;
    (void)tid;
    return true;
}

//TOTALS ACROSS THREADS, for the nps the GUI is shown. Each thread counts its own nodes in a
//thread_local, so the helpers publish theirs here as they finish iterations.
static std::atomic<long long> total_nodes{0};
//Published so the info line can carry it: "tbhits" is a standard UCI field, and without it there
//is no way to see from the outside whether the tables are being reached at all.
static std::atomic<long long> total_tbhits{0};

//=== THE ONLINE TABLEBASE, lichess's 7-piece set ===
//WHY IT IS ASYNCHRONOUS, and why a synchronous port would have been a regression. creatica used to
//probe lichess on the move's own clock, and on failure searched afterwards anyway -- so a timed-out
//probe cost its timeout AND a full search. Measured over one self-play session: 26 timeouts in one
//engine's log against 8 in the other's, at three seconds each, in games with a 120 second base
//clock, and one game lost on time. The answer is to spend nothing: post the request, search exactly
//as if there were no tables, and use the answer only if it has arrived by the time the search ends.
//
//NOT PORTED, deliberately: the PREFETCH of the position after our own move. It is an optimisation
//whose failure modes are the subtlest in the original -- it has to reason about whether the
//tablebase leaves the opponent a unique best move before predicting their reply, because
//tablebase.lichess.ovh rate-limits and two bots on one machine share an IP, and it must not call
//fen2board() on the worker thread because that rebuilds libchess's PROCESS-WIDE castling tables
//underneath a running search. Coverage is the same without it; only the latency differs.
static std::mutex              ol_mtx;
static std::condition_variable ol_cv;
static std::string             ol_req_fen;
static uint64_t                ol_req_id = 0;
static bool                    ol_req_live = false;
static bool                    ol_quit = false;
static std::thread             ol_worker;
static bool                    ol_worker_started = false;
//The answer slot, stamped with the request it belongs to so a late reply cannot be played for the
//wrong position -- the failure that would be hardest to notice and worst to make.
static std::mutex              ol_ans_mtx;
static uint64_t                ol_ans_id = 0;
static std::string             ol_ans_move;
static int                     ol_ans_score = 0;
//An endgame revisits positions, and a cached answer costs nothing and no request.
static std::unordered_map<std::string, std::pair<int, std::string>> ol_cache;

static size_t ol_write(void * p, size_t sz, size_t n, void * ud) {
    ((std::string *)ud)->append((char *)p, sz * n);
    return sz * n;
}

//ONE HANDLE, REUSED, owned by the worker thread alone -- a fresh curl_easy_init() per probe throws
//away the warm connection, which is most of the latency on a 7-piece endgame's worth of lookups.
static bool ol_probe(const std::string& fen, int& score, std::string& move) {
    static CURL * curl = nullptr;
    if (!curl) curl = curl_easy_init();
    if (!curl) return false;
    std::string body;
    char * esc = curl_easy_escape(curl, fen.c_str(), (int)fen.size());
    const std::string url = std::string("http://tablebase.lichess.ovh/standard?fen=")
                          + (esc ? esc : fen.c_str());
    if (esc) curl_free(esc);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ol_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    //GENEROUS, because nothing waits for this. The clock no longer needs protecting from it.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    const CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    if (rc != CURLE_OK || http != 200 || body.empty()) return false;
    try {
        const nlohmann::json d = nlohmann::json::parse(body);
        if (d.contains("category")) {
            const std::string c = d["category"];
            //"maybe-win" and "maybe-loss" are win/loss subject to the fifty-move rule, which is the
            //same compromise the local WDL probe makes, so they are treated the same way.
            if (c == "win" || c == "syzygy-win" || c == "maybe-win")        score =  TB_SCORE_CP;
            else if (c == "loss" || c == "syzygy-loss" || c == "maybe-loss") score = -TB_SCORE_CP;
            else                                                            score = 0;
        }
        if (d.contains("moves") && d["moves"].is_array() && !d["moves"].empty()
            && d["moves"][0].contains("uci"))
            move = d["moves"][0].value("uci", "");
    } catch (const nlohmann::json::parse_error&) {
        return false;
    }
    return !move.empty();
}
static std::chrono::steady_clock::time_point search_start;

static long long elapsed_ms() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - search_start).count();
}

//ITERATIVE DEEPENING, one thread's worth. tid 0 is the thread that reports; the helpers exist only
//to fill the shared table, which is the whole of Lazy SMP (see the note on root_search in the
//prober: four threads buy about +0.8 ply, because the NNUE evaluator itself only scales 3.4x).
static std::string worker(int tid) {
    std::string best_uci;
    if (!worker_setup(tid)) return best_uci;
    int reached = 0, score = 0;
    int iters_done = 0;
    const int d0 = 1 + (tid % std::max(1, stagger));
    for (int d = d0; d < MAX_PLY - 4; ++d) {
        int v;
        if (asp_window > 0 && d >= 3 && iters_done >= 2) {
            int w = asp_window, lo = score - w, hi = score + w;
            for (;;) {
                v = search(d, lo, hi, 0);
                if (out_of_time) break;
                if (v > lo && v < hi) break;
                ++asp_researches;
                w *= 2;
                if (v <= lo) lo = (w > 4 * asp_window) ? -MATE_CP - 1 : score - w;
                else         hi = (w > 4 * asp_window) ?  MATE_CP + 1 : score + w;
            }
        } else {
            v = search(d, -MATE_CP - 1, MATE_CP + 1, 0);
        }
        if (out_of_time) break;
        ++iters_done;
        if (pv_len[0] > 0) {
            best_uci = uci_of(pv_line[0][0]); reached = d; score = v;
            for (int i = 0; i < pv_len[0] && i < MAX_PLY; ++i) {
                prev_pv[i] = pv_line[0][i]; prev_pv_set[i] = true;
            }
            //ONE info LINE PER COMPLETED ITERATION, and only from the reporting thread. A line is
            //printed only after the window test above succeeded, because a search that stopped on
            //a bound has a PV whose first move was never established as best.
            if (tid == 0) {
                const long long ms = elapsed_ms();
                const long long n  = total_nodes.load() + nodes;
                std::printf("info depth %d ", d);
                print_score(v);
                std::printf(" nodes %lld time %lld nps %lld tbhits %lld pv",
                            n, ms, ms > 0 ? n * 1000 / ms : n,
                            total_tbhits.load() + tbhits);
                for (int i = 0; i < pv_len[0] && i < MAX_PLY; ++i)
                    std::printf(" %s", uci_of(pv_line[0][i]).c_str());
                std::printf("\n");
                std::fflush(stdout);
            }
        }
        if (v > MATE_CP - 100 || v < -MATE_CP + 100) break;   //a mate is settled
        if (fixdepth && d >= fixdepth) break;
    }
    total_nodes.fetch_add(nodes);
    total_tbhits.fetch_add(tbhits);
    (void)reached;
    return best_uci;
}

class AlphaBetaEngine : public uci::SearchEngine {
public:
    const char * id_name()   const override { return "Creatica AB (alpha-beta, NNUE)"; }
    const char * id_author() const override { return "Arkadi Poliakevitch"; }

    void declare_options(uci::Options& o) override {
        //THE CONFIGURATION THAT WAS MEASURED, as the defaults. The prober's env defaults are NOT
        //this -- they leave QTT, PVS, aspiration and SEE ordering off and score 115 where the
        //reference config scores 131 -- so an engine that inherited them would be measurably
        //weaker than everything in the logs. These are the reference config, plus SEEPrune=2.
        o.spin("Hash",     &o_hash,     64, 1, 4096);
        o.spin("Threads",  &o_threads,   1, 1, 8);
        //Search structure. Off by default means measured negative or unresolved; see the notes at
        //each knob in the search above.
        o.spin("QDepth",      &o_qdepth,     6,  0, 16);
        o.spin("Aspiration",  &o_asp,       25,  0, 500);
        o.spin("SEEOrdering", &o_seeord,     1,  0, 2);
        o.spin("SEEPrune",    &o_seeprune,   2,  0, 2);
        o.spin("SEEQuad",     &o_seequad,   12,  0, 200);
        o.spin("SEECapBase",  &o_seecapbase, 80,  0, 400);
        //QUIESCENCE PRUNING, default 2. On its own it is worth NOTHING -- 68 of 197, exactly the
        //control -- because delta pruning at a zero margin already does most of the same work.
        //Stacked with SEEPrune it is worth +3, and the pair is +10 against the control on Arasan,
        //where FOUR TIMES the node budget is worth +7.
        o.spin("QPrune",      &o_qprune,     2,  0, 2);
        o.spin("QMoveCount",  &o_qmcount,    2,  1, 16);
        o.spin("QFutBase",    &o_qfutbase, 169,  0, 600);
        o.spin("QSEE",        &o_qsee,     -38, -600, 0);
        //CONTINUATION HISTORY AND THE SHAPED REDUCTION. Both default OFF, and the reason is a
        //property of the MEASUREMENT rather than of the mechanism: the prober clears its tables for
        //every test position, because each one has to be an independent search, so statScore never
        //reaches working magnitude there. Measured on one position at increasing search size:
        //    depth 6: mean |statScore|   449      depth 9:  5,528
        //    depth 8:              2,396      depth 10:  6,614
        //still climbing steeply, against the ~74,000 Stockfish's own tables span. At 5,528 the
        //shaping range is +-0.56 plies where Stockfish gets +-3, so the test set cannot show what
        //this does. IN A GAME it is a different quantity entirely, because the tables persist from
        //move to move -- which is why they are cleared here only on "ucinewgame", never between
        //moves. Turn these on for a match, not on the strength of a test-set number.
        o.spin("ContHist",    &o_conthist,   0,  0, 1);
        o.spin("LMRShape",    &o_lmrshape,   0,  0, 1);
        o.spin("LMRBase",     &o_lmrbase,    0,  0, 1);
        o.spin("ShapeMul",    &o_shapemul, 850,  0, 20000);
        o.spin("LMRPlus",     &o_lmrplus,    0, -2, 3);
        o.spin("SingularExt", &o_singext,   50,  0, 400);
        o.spin("CorrHist",    &o_corrhist,   0,  0, 2);
        o.spin("RepCheck",    &o_repcheck,   1,  0, 2);
        //Syzygy. Without it, endgames are played by a search that does not know the result, which
        //in a head-to-head against creatica_mcts -- which has both local tables and online probes
        //-- measures the missing tables rather than the search.
        o.string("SyzygyPath", &syzygy_path_, "", [this] { on_syzygy_path(); });
        //Probing inside the search, not only at the root. On by default: it is exact information
        //for the cost of a table lookup, and the alternative is an NNUE guess about a position
        //whose value is known.
        o.spin("SyzygyInSearch", &o_tbsearch, 1, 0, 1);
        //The online 7-piece set, ON by default -- which is parity, not a preference.
        //creatica_mcts has no option for this at all: it probes lichess unconditionally whenever
        //the local tables cannot answer. Shipping it off here would make the two engines behave
        //differently in exactly the endgames a head-to-head is decided by, so the default has to
        //match. It costs nothing on the clock (the request and the search overlap, and the answer
        //is read only after the search has finished), so there is no case for paying for it in
        //time either. Turn it off for a network-isolated run.
        o.check("OnlineTablebase", &o_online, true);
        //THE POLICY NET, AND WHY IT DEFAULTS TO OFF.
        //
        //The prober loads this net unconditionally and orders the first POLICY_PLIES plies with
        //it, so every measurement in the logs was taken WITH it active. It had to be exposed here
        //or the engine would not be the search those numbers describe -- and worse, the first
        //version of this engine left policy_plies at 3 while never loading the net, so
        //order_by_policy() ran against an uninitialised net: undefined behaviour, and a search
        //2x the size of the prober's on the same position at the same depth.
        //
        //DEFAULTS TO 3, which is what the prober used. The first version defaulted it to 0, on
        //the grounds that policy ordering measured null (+0.02 plies) and costs about 13.76 us per
        //ordered node. That cost argument is not decidable from what has been measured, and the
        //effect is strongly POSITION-DEPENDENT. Same binary, fixed depth:
        //    middlegame, depth 9:   PolicyPlies=3   729,316 nodes  |  =0  1,118,773 nodes
        //    start position, d 10:  PolicyPlies=3   751,637 nodes  |  =0    742,991 nodes
        //A 35% saving in one and nothing in the other, at identical nps either way -- the per-node
        //cost does not show up, because only the first three plies are ordered and those are a
        //vanishing fraction of the tree. Two positions settle nothing; the set measurement is owed.
        //
        //It defaults to 3 for a reason that does not depend on that: every number in the logs was
        //taken with this on, so 3 is the configuration the measurements describe, and an engine
        //running something else is not the engine they are about.
        o.spin("PolicyPlies", &o_polplies, 3, 0, 8);
        o.string("PolicyNet", &policy_net_path_, "nnue_policy_pisp2.bin",
                 [this] { load_policy(); });
    }

    //THE WORKER, started on demand and joined in quit(). Not detached: a detached thread that
    //outlives main() holds a curl handle and a mutex this translation unit owns.
    void start_online_worker() {
        if (ol_worker_started) return;
        ol_worker_started = true;
        curl_global_init(CURL_GLOBAL_DEFAULT);
        ol_worker = std::thread([] {
            for (;;) {
                std::string fen; uint64_t id;
                {
                    std::unique_lock<std::mutex> lk(ol_mtx);
                    ol_cv.wait(lk, [] { return ol_req_live || ol_quit; });
                    if (ol_quit) return;
                    fen = ol_req_fen; id = ol_req_id; ol_req_live = false;
                }
                int score = 0; std::string move;
                bool answered = false;
                {
                    std::lock_guard<std::mutex> lk(ol_ans_mtx);
                    auto it = ol_cache.find(fen);
                    if (it != ol_cache.end()) {
                        score = it->second.first; move = it->second.second;
                        answered = !move.empty();
                    }
                }
                if (!answered) {
                    answered = ol_probe(fen, score, move);
                    std::lock_guard<std::mutex> lk(ol_ans_mtx);
                    ol_cache[fen] = { score, answered ? move : std::string() };
                }
                if (!answered) continue;
                std::lock_guard<std::mutex> lk(ol_ans_mtx);
                //STAMPED WITH THE REQUEST, so an answer that arrives after the engine has moved on
                //is discarded instead of being played for a position that is no longer on the board.
                ol_ans_id = id; ol_ans_move = move; ol_ans_score = score;
            }
        });
    }

    //Posted and forgotten. Nothing waits on it; run() looks in the answer slot when the search is
    //already over.
    void post_online_probe(const std::string& fen) {
        if (!o_online) return;
        start_online_worker();
        std::lock_guard<std::mutex> lk(ol_mtx);
        ol_req_fen = fen;
        ol_req_id = ++online_id_;
        ol_req_live = true;
        ol_cv.notify_one();
    }

    //Loaded on demand, and a FAILURE IS NOT FATAL. The prober aborts when the net is missing,
    //which is right for an instrument and wrong for an engine: a GUI that gets no answer cannot
    //tell a missing file from a crash. Policy ordering is simply turned off instead, and said so.
    void load_policy() {
        if (policy_loaded_ || policy_net_path_.empty()) return;
        char err[256];
        if (!policy_net_load(pnet, policy_net_path_.c_str(), err, sizeof err)) {
            std::printf("info string policy net not loaded (%s); policy ordering off\n", err);
            std::fflush(stdout);
            o_polplies = 0;
            return;
        }
        policy_loaded_ = true;
        std::printf("info string policy net loaded from %s\n", policy_net_path_.c_str());
        std::fflush(stdout);
    }

    //tb_init() is not re-entrant here, the same guard creatica.cpp uses.
    void on_syzygy_path() {
        if (tb_ready_) {
            std::printf("info string SyzygyPath: tablebases already initialised; ignoring\n");
            std::fflush(stdout);
            return;
        }
        if (syzygy_path_.empty()) return;
        tb_init(syzygy_path_.c_str());
        if (TB_LARGEST == 0) {
            std::printf("info string error no tablebases found in %s\n", syzygy_path_.c_str());
        } else {
            tb_ready_ = true;
            //The search reads its own copy rather than TB_LARGEST directly, so that turning the
            //option off actually turns the probing off instead of merely hiding it.
            tb_pieces_max = TB_LARGEST;
            std::printf("info string tablebases initialised in %s, max %u pieces\n",
                        syzygy_path_.c_str(), TB_LARGEST);
        }
        std::fflush(stdout);
    }

    void init() override {
        Stockfish::Bitboards::init();
        zobristHash(z);
        init_nnue();
        apply_hash();
    }

    void new_game() override {
        //A NEW GAME SHARES NOTHING WITH THE LAST ONE. The table is keyed on position hashes, so
        //stale entries are not wrong, but the history and correction tables are statistics about a
        //particular game's tree and carry over badly.
        stop();
        std::fill(tt.begin(), tt.end(), TTEntry{});
        game_hashes.clear();
        std::memset(corr_tab, 0, sizeof corr_tab);
        std::memset(corr_cont, 0, sizeof corr_cont);
        //CLEARED HERE AND NOWHERE ELSE. The prober wipes these per test position; an engine must
        //not wipe them per move, because what a continuation history is FOR is carrying "this reply
        //works after that move" from one move of the game to the next. That difference is the
        //whole reason the mechanism is worth measuring in games and cannot be measured on the set.
        std::memset(cont_hist, 0, sizeof cont_hist);
    }

    void set_position(const std::string& fen, const std::vector<std::string>& moves) override {
        //STOP FIRST. root_fen and game_hashes are read by every search thread.
        stop();
        Board b{};
        if (fen2board(b, fen.c_str()) != 0) {
            std::printf("info string unparseable fen, position ignored\n");
            std::fflush(stdout);
            return;
        }
        ZobristHash h{};
        getHash(h, b, z);
        //REBUILT FROM SCRATCH every time, because the driver replays the whole move list on every
        //position command -- appending instead would count a position once per command and the
        //engine would see repetitions everywhere. The starting position counts: it is a position
        //that has occurred.
        game_hashes.clear();
        game_hashes.push_back(h.hash);
        for (const std::string& ms : moves) {
            Move m = {};
            uci2move_idx(ms.c_str(), m);
            normalise_castling(b, m);
            if (!is_legal_move(b, m)) {
                //STOP AT THE FIRST REFUSED MOVE rather than applying the rest to a board the
                //driver does not have.
                std::printf("info string illegal move %s ignored\n", ms.c_str());
                std::fflush(stdout);
                break;
            }
            updateHash(h, b, m, ff_move(b, m), z);
            game_hashes.push_back(h.hash);
        }
        //The root itself is the last entry; repeated() walks the history BEFORE the root, so drop
        //it to avoid matching the root against itself at ply 0.
        if (!game_hashes.empty()) game_hashes.pop_back();
        char fenbuf[MAX_FEN_STRING_LEN];
        root_fen = board2fen(b, fenbuf);
        root_move_number = b.moveNumber;
    }

    //SYZYGY AT THE ROOT. Not in the search: a WDL probe inside the tree would also be worth
    //having, but it means giving win/draw/loss its own score band so mate-distance handling does
    //not misread it, and getting that wrong is a silent, position-dependent error. The root probe
    //is self-contained, cannot affect any other position, and removes the handicap where it is
    //actually visible in a game -- an ending reached at the root that is won or drawn by rule and
    //would otherwise be played by a search that does not know it.
    //
    //Probed by whether the probe ANSWERS, not by the piece count, which is the lesson creatica.cpp
    //records at length: TB_LARGEST is raised to 6 by a partial 6-piece set, so a piece-count test
    //routes positions to tables that were never downloaded and they are then never searched
    //either. Castling is excluded because Syzygy cannot express it.
    bool probe_tb_root(std::string& best, int& score_cp) {
        if (!tb_ready_ || TB_LARGEST == 0) return false;
        Board b{};
        if (fen2board(b, root_fen.c_str()) != 0) return false;
        if (b.castlingRights) return false;
        const unsigned pieces = (unsigned)bitCount(b.side[ColorWhite] | b.side[ColorBlack]);
        if (pieces > TB_LARGEST) return false;
        const Square ep = legalEnPassantMove(b);
        const unsigned res = tb_probe_root(
            b.side[ColorWhite], b.side[ColorBlack], b.pieceTypes[King - 1],
            b.pieceTypes[Queen - 1], b.pieceTypes[Rook - 1], b.pieceTypes[Bishop - 1],
            b.pieceTypes[Knight - 1], b.pieceTypes[Pawn - 1],
            b.halfmoveClock, 0, ep == SquareNone ? 0 : (unsigned)ep,
            (b.sideToMove ^ 1) == ColorBlack ? 1 : 0, NULL);
        if (res == TB_RESULT_FAILED) return false;      //not in the tables we actually hold
        //STALEMATE AND CHECKMATE ARE NOT FAILURES, and they carry NO MOVE -- the from/to fields are
        //meaningless. Reading them anyway produced "bestmove a1a1", which is not a move: the first
        //test position, 8/8/8/8/8/4k3/4p3/4K3 w, really is stalemate, because the black pawn on e2
        //covers d1 and f1 while the black king covers d2 and f2. Fall through to the search, which
        //handles having no legal moves correctly.
        if (res == TB_RESULT_STALEMATE || res == TB_RESULT_CHECKMATE) return false;
        const unsigned wdl = TB_GET_WDL(res);           //0 loss, 4 win, 1..3 draw
        //A TABLEBASE WIN IS NOT A MATE. The first version reported MATE_CP - 1, which came out as
        //"score mate 1" for a KRvK win at DTZ 27 -- a claim to mate next move that is simply false,
        //and the kind of thing a GUI adjudicates on. DTZ is distance to ZEROING, not to mate, so
        //there is no mate distance to report here. A large cp score, reduced by DTZ so a slower win
        //scores lower, says "won by rule" without claiming to know how fast.
        const int dtz = (int)TB_GET_DTZ(res);
        score_cp = wdl == 4 ? TB_WIN_CP - dtz : (wdl == 0 ? -(TB_WIN_CP - dtz) : 0);
        const unsigned from = TB_GET_FROM(res), to = TB_GET_TO(res), promo = TB_GET_PROMOTES(res);
        best = std::string(square[from]) + square[to];
        if (uciPromoLetter[6 - promo]) best += uciPromoLetter[6 - promo];
        return true;
    }

    void go(const uci::Limits& lim) override {
        stop();
        last_limits_ = lim;
        apply_options();
        apply_hash();
        stop_requested.store(false);
        out_of_time = false;
        total_nodes.store(0);
        total_tbhits.store(0);
        search_start = std::chrono::steady_clock::now();
        fixnodes = lim.nodes;
        fixdepth = lim.depth;
        set_clock(lim);
        if (!lim.searchmoves.empty()) {
            std::printf("info string searchmoves is not implemented; searching all moves\n");
            std::fflush(stdout);
        }
        //POSTED BEFORE THE SEARCH STARTS, so the request and the search overlap. Only for
        //positions the LOCAL tables cannot answer and lichess might: at most 7 pieces, no castling.
        //A ponder search is excluded -- the position it is pondering is a guess.
        if (!lim.ponder && o_online) {
            Board b{};
            if (fen2board(b, root_fen.c_str()) == 0 && !b.castlingRights) {
                const unsigned pc = (unsigned)bitCount(b.side[ColorWhite] | b.side[ColorBlack]);
                if (pc <= 7 && pc > tb_pieces_max) {
                    char fb[MAX_FEN_STRING_LEN];
                    post_online_probe(board2fen(b, fb));
                }
            }
        }
        //ANSWERED BY THE TABLE, so there is nothing to search. A ponder search is NOT short-circuited
        //-- UCI forbids an unsolicited bestmove after "go ponder", and the table's answer will still
        //be there on ponderhit.
        if (!lim.ponder) {
            std::string tb_best; int tb_score = 0;
            if (probe_tb_root(tb_best, tb_score)) {
                std::printf("info depth 1 ");
                print_score(tb_score);
                std::printf(" nodes 0 time 0 pv %s\n", tb_best.c_str());
                std::printf("bestmove %s\n", tb_best.c_str());
                std::fflush(stdout);
                return;
            }
        }
        driver_ = std::thread([this] { this->run(); });
    }

    void stop() override {
        stop_requested.store(true);
        if (driver_.joinable()) driver_.join();
    }

    //A PONDER SEARCH MUST NOT STOP ON ITS OWN -- UCI forbids an unsolicited bestmove after
    //"go ponder". It therefore runs with no deadline at all, and ponderhit simply installs the
    //real one: the search threads re-read `deadline` and `unlimited` on every check, so the
    //running search converts into a timed one with no restart and no lost work.
    void ponderhit() override {
        uci::Limits lim = last_limits_;
        lim.ponder = false;
        last_limits_ = lim;
        set_clock(lim);
    }

    void quit() override {
        stop();
        if (ol_worker_started) {
            { std::lock_guard<std::mutex> lk(ol_mtx); ol_quit = true; }
            ol_cv.notify_all();
            if (ol_worker.joinable()) ol_worker.join();
            curl_global_cleanup();
        }
    }

    bool custom(const std::string& line) override {
        if (line == "eval") {
            //Through the SEARCH's own evaluation path, not evaluate_nnue() directly: eval_cp()
            //is what every node in this engine actually sees, including its handling of the
            //in-check sentinel, and an "eval" that reported something else would be misleading.
            if (!worker_setup(0)) return true;
            std::printf("info string static eval %d cp (side to move)\n", eval_cp());
            std::fflush(stdout);
            return true;
        }
        return false;
    }

private:
    void run() {
        const int n = std::max(1, (int)o_threads);
        std::vector<std::thread> pool;
        std::string best;
        for (int t = 1; t < n; ++t) pool.emplace_back([t] { (void)worker(t); });
        best = worker(0);
        //EVERY HELPER MUST STOP BEFORE WE REPORT, or a thread outlives the search and writes into
        //the table during the next one.
        stop_requested.store(true);
        for (auto& th : pool) th.join();
        //A SEARCH THAT FOUND NOTHING STILL HAS TO ANSWER. A GUI that gets no bestmove hangs
        //forever, so fall back to any legal move rather than going silent.
        //THE ONLINE ANSWER, if it arrived while we were searching. It cost nothing on the clock:
        //the search ran to its own completion either way. A tablebase verdict beats a search, so it
        //wins -- but only if it belongs to THIS move's request.
        {
            std::lock_guard<std::mutex> lk(ol_ans_mtx);
            if (o_online && ol_ans_id == online_id_ && !ol_ans_move.empty()) {
                std::printf("info depth 1 ");
                print_score(ol_ans_score);
                std::printf(" nodes 0 time %lld tbhits 1 pv %s\n",
                            elapsed_ms(), ol_ans_move.c_str());
                best = ol_ans_move;
            }
        }
        if (best.empty()) best = any_legal_move();
        if (best.empty()) std::printf("bestmove 0000\n");
        else              std::printf("bestmove %s\n", best.c_str());
        std::fflush(stdout);
    }

    std::string any_legal_move() {
        Board b{};
        if (fen2board(b, root_fen.c_str()) != 0) return "";
        board = b;
        getHash(zh, board, z);
        Mv mv[MAX_MOVES];
        const int n = generate(mv, false);
        return n > 0 ? uci_of(mv[0]) : std::string();
    }

    //CREATICA'S OWN TIME MANAGEMENT, formula for formula. It is not obvious and it was not cheap:
    //the latency allowance, the capping rather than assigning below MIN_TIME_THRESHOLD, and the
    //deliberate absence of CRITICAL_TIME_FACTOR are each documented at length in creatica.cpp
    //against measurements over 10k searches and 200-move simulations at four latencies. An
    //alpha-beta search has no reason to want a different allocation, and inventing one here would
    //be re-learning all of that by flagging in real games.
    void set_clock(const uci::Limits& lim) {
        if (lim.ponder || lim.infinite) { unlimited = true; return; }
        unlimited = false;
        double alloc;
        if (lim.movetime_ms > 0) {
            alloc = (double)lim.movetime_ms * 0.99;
        } else if (!lim.wtime && !lim.btime) {
            //No clock and no movetime: "go" or "go depth N". Nothing to budget.
            unlimited = true;
            return;
        } else {
            Board b{};
            fen2board(b, root_fen.c_str());
            const int64_t remaining = b.sideToMove == ColorWhite ? lim.wtime : lim.btime;
            const int64_t inc       = b.sideToMove == ColorWhite ? lim.winc  : lim.binc;
            int moves_left = lim.movestogo ? lim.movestogo
                                           : MAX_MOVES_REMAINING - root_move_number;
            if (moves_left < MIN_MOVES_REMAINING) moves_left = MIN_MOVES_REMAINING;
            int64_t r = remaining;
            if (r > TIME_SAFETY_BUFFER) r -= TIME_SAFETY_BUFFER;
            alloc = (double)r / moves_left + (double)inc * 0.5;
            alloc -= LATENCY_ALLOWANCE;
            if (remaining < MIN_TIME_THRESHOLD) alloc = std::min(alloc, (double)remaining * 0.5);
            if (inc < 1000 && alloc < 3000) alloc = 100;
            if (alloc < 100) alloc = 100;
        }
        deadline = search_start + std::chrono::milliseconds((long long)alloc);
    }

    void apply_hash() {
        //ONE ENTRY IS 16 BYTES, and the count has to be a power of two for the mask in tt_slot().
        const size_t want = (size_t)std::max<int64_t>(1, o_hash) * 1024 * 1024 / sizeof(TTEntry);
        size_t n = 1;
        while (n * 2 <= want) n *= 2;
        if (n != tt.size()) { tt.assign(n, TTEntry{}); tt_mask = n - 1; }
    }

    void apply_options() {
        qdepth       = (int)o_qdepth;
        asp_window   = (int)o_asp;
        see_ord      = (int)o_seeord;
        seeprune     = (int)o_seeprune;
        see_quad     = (int)o_seequad;
        see_cap_base = (int)o_seecapbase;
        //ORDERING CANNOT BE ASKED FOR WITHOUT THE NET. order_by_policy() reads pnet
        //unconditionally, so policy_plies must be 0 unless a net is actually in memory.
        if (o_polplies > 0 && !policy_loaded_) load_policy();
        policy_plies = policy_loaded_ ? (int)o_polplies : 0;
        conthist     = (int)o_conthist;
        lmrshape     = (int)o_lmrshape;
        lmrbase      = (int)o_lmrbase;
        shape_mul    = (int)o_shapemul;
        tb_search    = (int)o_tbsearch;
        qprune       = (int)o_qprune;
        q_mcount     = (int)o_qmcount;
        q_futbase    = (int)o_qfutbase;
        q_see        = (int)o_qsee;
        lmrplus      = (int)o_lmrplus;
        sing_margin  = (int)o_singext;
        corrhist     = (int)o_corrhist;
        repcheck     = (int)o_repcheck;
        threads      = (int)o_threads;
        //The pieces of the reference config that are not exposed: they were measured and there is
        //no reason to let a GUI turn them off.
        tt_on = 1; qtt_on = 1; pvs_on = 1; prune = 3; ext_limit = -1;
        delta_margin = 0; sing_depth = 2; sing_plies = 3;
        //ttse STAYS OFF, and setting it to 1 here was a bug caught by checking the engine against
        //the prober: same position, same depth, same score of 381, but 1,426,648 nodes against
        //729,316 and a different move. ttse makes quiescence write its static evaluation into the
        //table, which is only useful when evmode reads it back -- and evmode is 0, because every
        //arm built on the cached eval measured negative or unresolved. On its own it just creates
        //eval-only entries that displace real ones. The reference config has ttse=0; an engine
        //running a configuration that was never measured is not the engine the numbers describe.
        ttse = 0; evmode = 0;
    }

    std::thread driver_;
    uci::Limits last_limits_;
    int64_t o_hash = 64, o_threads = 1;
    int64_t o_qdepth = 6, o_asp = 25, o_seeord = 1;
    int64_t o_seeprune = 2, o_seequad = 12, o_seecapbase = 80;
    int64_t o_qprune = 2, o_qmcount = 2, o_qfutbase = 169, o_qsee = -38;
    int64_t o_conthist = 0, o_lmrshape = 0, o_lmrbase = 0, o_shapemul = 850;
    int64_t o_tbsearch = 1;
    bool o_online = true;
    uint64_t online_id_ = 0;
    int64_t o_lmrplus = 0, o_singext = 50, o_corrhist = 0, o_repcheck = 1;
    std::string syzygy_path_;
    std::string policy_net_path_ = "nnue_policy_pisp2.bin";
    bool policy_loaded_ = false;
    int64_t o_polplies = 3;
    bool tb_ready_ = false;
};

int main(int argc, char ** argv) {
    AlphaBetaEngine engine;
    return uci::uci_main(engine, argc, argv);
}
