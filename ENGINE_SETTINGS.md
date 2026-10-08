# Creatica engine settings

Reference for the tunable settings of **`creatica`** — the deployed engine: MCTS over a
shared root, NNUE leaf evaluation, and a learned policy head blended into the move priors
— together with what has actually been measured about them over the board.

It is built from `creatica.cpp` (options and UCI adapter) and `creatica_search.cpp` (the
search), on the shared protocol layer in `uci_frontend.cpp`. `creatica-shared-root` is kept
as a pre-policy baseline for comparison.

Three separate mechanisms control the engine, and they are easy to confuse:

| mechanism | when it takes effect |
|---|---|
| UCI options | at any time, via `setoption` |
| `#define`s in the `.hpp` | requires a rebuild |

Everything tunable is a UCI option. An engine declares what it has once, in
`declare_options()`, and the block sent in reply to `uci`, the `setoption` dispatch and the
bounds checking all follow from that declaration — so adding a knob is one line in the
engine and needs no library rebuild.


## UCI options

These are what the engine advertises in response to `uci`, and they apply to both
binaries. They can be changed at run time with `setoption name <X> value <Y>`.

| option | type | default | range | what it does |
|---|---|---|---|---|
| `Hash` | spin | 2048 | 128–4096 | MB for the MCTS tree. This is the tree itself, not a transposition table — **once it is full, expansion stops**, so a long search with too little hash quietly stops growing. |
| `Threads` | spin | **4** | 1–8 | Search threads, each with its own tree and `NNUEContext`. Default changed from 8 to 4 on measurement — see below. |
| `PerformanceCores` | check | false | — | Bias the search threads onto the M1's performance cores via `QOS_CLASS_USER_INTERACTIVE`. Measured to make no difference; kept because the answer may differ on other hardware. `creatica` only. |
| `MultiPV` | spin | 5 | 1–8 | How many principal variations are reported. |
| `ExplorationMin` | spin | 65 | 0–100 | Floor for the PUCT exploration constant, scaled by 1/100. |
| `ExplorationMax` | spin | 160 | 0–200 | Exploration constant at the root, scaled by 1/100. Lower favours exploitation: a deeper, narrower tree. |
| `ExplorationDepthDecay` | spin | 5 | 0–10 | Linear decay of the exploration constant per ply, scaled by 1/100. |
| `PVPlies` | spin | 16 | 1–32 | Maximum length of a reported PV. |
| `Temperature` | spin | 58 | 1–200 | Divisor (scaled by 1/100, so 0.58) in the softmax that turns move scores into priors. Lower makes the prior sharper. |
| `VirtualLoss` | spin | 36 | 0–100 | **Scaled by 1/10, not 1/100** — the advertised 36 means the search uses **3.6**. Discourages threads from piling onto the same node. Performance device, not a strength knob. |
| `SyzygyPath` | string | `<empty>` | — | Tablebase directory. The advertised default is empty but `creatica` falls back to the compiled-in `SYZYGY_PATH` at startup, so the local tables are used whether or not a GUI sets it. See below — empty does **not** mean "no tablebases". |
| `ProbabilityMass` | spin | 1000 | 900–1000 | **Per-mille, not percent.** Keep only the moves whose policy priors sum to this share and drop the tail, gated before the child evaluations. `1000` keeps every move and is an exact no-op. `creatica` only — see below before changing it. |
| `ReuseTree` | check | **true** | — | Keep the search tree between moves instead of rebuilding it, so the subtree under the move played is inherited already searched. `Ponder` implies this; this does not imply `Ponder`. Default changed to on after measurement — see below. `creatica` only. |
| `GcThreshold` | spin | 700 | 0–1000 | Per-mille of `Hash` at which the tree is collected. It reclaims only memory — an unreachable node is never traversed — so collecting every move used to pay a large cost for nothing. Since the arena that cost is roughly 100 ms at `Hash` 1024, and the value is now set by the *expansion ceiling* rather than by collection cost: occupancy climbs a p99 of 299 per-mille during one search, so 700 is about the highest value that keeps the tree off the 1000 ceiling. Raise it and roughly one search in ten overshoots; lowering it is now affordable. See *The node arena*. `creatica` only. |
| `TreeResetBelow` | spin | **1000** | 0–1000000000 | **On by default; 0 disables.** Before a search, if the tree is at `TreeResetOccupancy` or more and the root inherited fewer than this many informed simulations (its evidence), discard the whole tree instead of collecting it. Aimed at the one case a full tree turns into a blunder: a root the last search barely looked at, on a tree nothing can be freed from. Default since September 2026, on a sound mechanism and weeks of live use rather than a demonstrated gain — the only match, 154.5 : 145.5 over 300 games, is inside the noise. See *A full tree that cannot be collected*. |
| `TreeResetOccupancy` | spin | 950 | 0–1000 | Per-mille of `Hash` at or above which `TreeResetBelow` applies. Below 950 no search in 22 games froze, so resetting there would only throw knowledge away. |
| `TreeResetHollow` | spin | **900** | 0–1000 | **On by default; 0 disables.** A second trigger for the same reset: discard the tree when the *previous* search was at least this many per-mille hollow — the tree is frozen, whatever the root inherited. Applies at `TreeResetOccupancy` or more. See *Which reset threshold* — and note the supporting match is inside the noise. |
| `CollectDuringPonder` | check | **true** | — | Run the collection beside the ponder search instead of inline before every search. Safe because of the mark's write barrier. Default since September 2026: measured in a lichess game at 0 collections on the move's own clock against 13 costing 5,384 ms, and about 8% more simulations a move. See *The mark beside the search*. |
| `VisitDumpFile` | string | `<empty>` | — | Append the root visit distribution after every search to this file. The AlphaZero-style policy training target, collected as a free byproduct of searches that happen anyway. Empty disables it. `creatica` only. |
| `GameTag` | string | `<empty>` | — | Written on every visit-dump line, so records can be joined back to a game and its result. Set per game by `lichess_bot`. `creatica` only. |
| `ValidateTree` | check | false | — | Diagnostic. After every collection, check the invariants the collector must preserve and report violations. Costs a full walk of the tree per move; for runs asking whether reuse is sound, not for playing. `creatica` only. |
| `Ponder` | check | false | — | Think on the opponent's clock. Implies `ReuseTree`, since a pondered subtree that is destroyed before the next search was wasted effort. |
| `FinalInfoLines` | check | true | — | |
| `IntermittentInfoLines` | check | true | — | |

### Threads: measured, and the default changed to 4

This machine is an Apple M1 with **4 performance and 4 efficiency cores**. Node throughput
was measured on three positions at 5 s each, best of three runs per cell, machine idle:

| threads | 2 | 4 | 6 | 8 |
|---|---|---|---|---|
| total nodes | 1.92M | **4.28M** | 3.68M | 2.73M |
| relative | 0.45x | **1.00x** | 0.86x | 0.64x |

**Throughput peaks at 4 and falls away sharply: 8 threads searches 36% fewer nodes than 4.**
The default is now 4 in both engine headers. The UCI option still accepts 1–8.

**It is contention, not core placement.** The obvious explanation was that threads 5–8 land
on efficiency cores, which are three to four times slower. That is testable: biasing the
pool onto performance cores with `QOS_CLASS_USER_INTERACTIVE` should then recover some of
the loss. It recovers none — 0.94x at 4 threads and 0.63x at 8, both within noise of the
same configuration without it. So the cost is the shared transposition map's mutex and
virtual-loss interference between threads, which grow faster with thread count than
throughput does. The fifth through eighth threads cost more in lock traffic than they
contribute in nodes, wherever they run.

That option (`PerformanceCores`) is left in place, defaulting to off. It is a negative
result rather than a feature.

**The match was run, and it agrees.** More nodes is not automatically more strength, so
this needed testing directly rather than inferring it from throughput. Four threads against
eight, head to head, finished **14–10 over 24 games** — about +58 Elo, but only p≈0.22, so
on its own that result is not conclusive.

It is worth more than its p-value, for one specific reason: the direction was predicted in
advance from the throughput measurement, so this is a confirmation of a stated prediction
rather than a search through outcomes for a significant one. Combined with a 36% node
deficit at 8 threads and no mechanism pointing the other way, 4 is the right default. A
larger match would tighten the number but is unlikely to change the decision, and the
machine time is better spent elsewhere.

**In a match it used to compound.** Two engines at 8 threads each is 16 threads on 8 cores,
and every match recorded below ran that way. At the current default it is 8 threads on 8
cores, and only one side is searching at a time, so a match no longer oversubscribes the
machine on its own.

### Tablebases: what happens when they are missing

The engine does not fail if the tables cannot be found. It prints one `info string` and
carries on — but the fallback is worse than it sounds, because of how the root handler is
structured:

```cpp
if (numberOfPieces > 7)               start_search();      // normal search
else if (numberOfPieces > TB_LARGEST) /* ask lichess online */
else                                  /* local tb_probe_root */
```

With the tables loaded `TB_LARGEST` is 5, so only six- and seven-piece positions go online.
**Without them `TB_LARGEST` is 0, and every position from seven pieces down to a bare king is
queried from the online lichess tablebase.** A missing path therefore *increases* network
dependence rather than removing it, in the phase of the game where it hurts most, and those
queries are slow and rate limited.

Those queries no longer block the search — see *The online tablebase probe: from blocking to
concurrent* under **What has been measured** — but they are still rate limited, so a missing path
is still the wrong kind of problem to have.

The tables also help during the search, not only at the end of a game: `position_eval()`
probes whenever the piece count drops to `TB_LARGEST` or below, so a middlegame line that
simplifies into a five-piece ending gets an exact result instead of an evaluation. Removing
them degrades the search, not just the endgame.

`creatica` distinguishes the two failures — a path that is not a directory from a directory
holding no tables — and states the consequence in the message rather than only the fact.
Because tablebase initialisation is not re-entrant, a `SyzygyPath` sent after startup is
refused with `tablebases already initialised; ignoring`, so the compiled-in path is what
actually matters.

### The scaling is not uniform

UCI has no floating-point type, so real-valued settings are advertised as integers and
divided at the point of use. **The divisor is not the same for all of them**, which is easy
to get wrong and impossible to see from the advertised value:

| option | divisor | advertised | what the search uses |
|---|---|---|---|
| `ExplorationMin` | 100 | 65 | 0.65 |
| `ExplorationMax` | 100 | 160 | 1.60 |
| `ExplorationDepthDecay` | 100 | 5 | 0.05 |
| `Temperature` | 100 | 58 | 0.58 |
| `VirtualLoss` | **10** | 36 | **3.6** |
| `EvalScale` | **10** | 61 | **6.1** |

`EVAL_SCALE 61` in the header therefore means the divisor in `W = tanh(eval / eval_scale)`
is **6.1 pawns**, not 61. An earlier version of this document had both of these wrong by a
factor of ten.

### ProbabilityMass

Keep only the moves whose priors sum to this share of the total; drop the tail. Per-mille, so
`1000` keeps every move. **Leave it at 1000.**

At `1000` it is an exact no-op — the gate is skipped and the expansion runs the code it always
ran, so the option costs nothing at its default.

**It has now been tested twice and lost both times.** The first test gated an eval-derived
prior *after* the child evaluations, where it could only discard moves and never save the work
of producing them; every setting below 100% played weaker. The second test fixed both of those
objections — it gated the learned policy score *before* the evaluations, so a dropped move
genuinely skipped work, and held-out positions said a 990 gate retained **98.5%** of
Stockfish's best moves for about **35% fewer child evaluations**. It still lost, visibly,
within a few games of self-play.

Two things explain the gap between that recall figure and the result, and both generalise:

**Recall is not strength.** The 1.5% of positions where the gate drops the best move are not a
random sample. A move with a low prior that is nevertheless best is disproportionately a
tactical one — precisely where losing it decides the game. An accuracy figure averaged over
positions cannot see that the errors concentrate where they cost most.

**Gating deletes rather than deprioritises.** In MCTS a low-prior move can accumulate visits
over time as the search corrects its own prior, but only while it remains a child of the node.
A gate at expansion removes it permanently. With `ReuseTree` on this is worse than it used to
be: the truncated node now survives across moves instead of being rebuilt every move, so one
bad gating decision persists for the rest of the game.

The option is kept, at 1000, as a record of a measured decision rather than a live tunable.

### RepetitionGuard

How many prior occurrences of a position make the engine refuse a winning move that returns to
it. **Leave it at 1** unless you are running the comparison below.

| value | behaviour |
|---|---|
| 0 | filter off — play whatever the search chose, repetition or not |
| 1 | default — refuse a winning move returning to a position seen once |
| 2 | refuse only a move handing the opponent an immediate threefold claim |

**Why 1 rather than the legal 2.** The laws of chess draw on the third occurrence, so `1` is
deliberately stricter than the rules require. The reason is search depth: with creatica's priors
the search cannot see two full repetition cycles ahead, so by the time a threefold-correct rule
noticed the danger the opponent would already be able to force it. Steering away one cycle early
is the safe direction for the side that is winning. Stockfish scores a twofold repetition inside
its search as a draw for the same reason.

**Why not relax it, having built the machinery to.** The counting history makes `2` expressible,
and the question was reopened deliberately. It was closed again on this argument: when we are
winning, refusing a repetition costs at most a wasted tempo *whether or not* the opponent is
actually playing for a draw. Knowing their intent buys nothing, so the strict rule is close to
free insurance. The damage in the endgame that prompted all of this was never caused by the
threshold — see the diagnosis under *What has been measured*.

**What this option does NOT control.** Three repairs in the same function are unconditional,
because they fix things that were wrong rather than expressing a policy:

- the winning guard reads **Q**, not raw `W` (a sum repetitions can never lower);
- a **visit floor** stops the filter landing on a move with under 1% of the chosen move's visits;
- the **initial position is counted**, so a move returning to the position a game started from
  is now recognised as a recurrence.

**The 0 arm exists to be measured.** This filter is what discarded a move holding 907,491 in
accumulated value in favour of one holding 2,223. Whether it earns its place at all has been
assumed rather than tested, and `0` makes that answerable from one binary.

### EdgeVisits

Rank the root's moves by *N(s,a)* — how often each **edge** was taken — instead of by the child
node's lifetime visit count, and use the same figure as the PUCT exploration denominator.
**Off by default.**

**What it fixes is real.** `select_best_moves()` ranks by visits, and under `ReuseTree` a node's
`N` is a lifetime total including everything it earned while it was itself the search root or the
ponder root. So the ranking that chooses the move played is partly decided by numbers no
simulation in the current search produced. The exploration term had the same problem from the
other side: its denominator was the child's lifetime `N`, so a child carrying a million inherited
visits had a bonus so small it could never be re-examined from a new parent.

**What it costs is measured**, on the same position, same binary, five seconds each:

| | simulations |
|---|---|
| `EdgeVisits=false` | 2,095,429 |
| `EdgeVisits=true` | 892,901 |

**The bookkeeping is not the cost.** A build that maintained both counters and never read them
ran 2,280,402 simulations against 2,170,841 for one without them at all — free, within noise. The
halving is the search genuinely exploring more and paying for expansions it used to skip, at
roughly 33 NNUE child evaluations each. Both counters live in padding that already existed, so
they did not change the collector's accounting. (The child table later removed the child pointer
from `Edge`, which is now 16 bytes; see *The child table*.)

**Whether better-directed search at half the volume beats worse-directed search at full volume is
not known.** It is a switch rather than a decision for that reason, and it is runtime rather than
compile-time specifically so both sides of a match can be the same binary — two builds risk
differing in something other than the flag. Set `CREATICA_EDGE_VISITS=1` on one bot of a pair.

### Options that are gone

`EvalScale`, `EvalDepth`, `MaxNodes` and `NegamaxDepth` are in the deprecated
`EngineSpinOptions` enum in `libchess.h` and are not options of `creatica`. `EvalScale` in
particular was set up in the old `lichess_bot.cpp` but sat at enum index 10, past the end of
the list `creatica-nnue-policy` advertises, so it was never sent and the engine ran its own
default. See **Setting options from a driver** below.


## Policy options

The settings of the policy head. These are ordinary UCI options like the ones above — set
them with `setoption`, at any time, and see them listed in response to `uci`.

| option | type | default | range | meaning |
|---|---|---|---|---|
| `PolicyWeights` | string | **`nnue_policy_pisp2.bin`** | — | The exported policy net. Changing it reloads the net; if the load fails the previous net is kept and an `info string` says why, so the engine never runs on a half-loaded net. A reload is refused while a search is running, because the worker threads read the net without a lock. The `info string` names the row count, which is how you check which net is actually in use: **4096 = the narrow table, 24576 = piece-indexed**, and then lists `legality term` and `spatial term: …` when the net has them. See *Policy file formats*. |
| `PolicyMode` | spin | 1 | 0–2 | 0 = off, 1 = prior, 2 = full. See below. |
| `PolicyBlend` | spin | 45 | 0–100 | Share of the prior taken from the policy head, in hundredths; the rest comes from the 1-ply child evaluations. 0 reproduces the incumbent prior exactly, 100 is policy-only. Ignored when `PolicyMode` is 2. |
| `PolicyTemp` | spin | 75 | 0–300 | Temperature the policy logits are read at, in hundredths. Chosen so the resulting prior has the concentration the exploration constants were tuned for. |
| `BlendScale` | spin | **102** | 0–300 | Overall sharpness of the blended prior, in hundredths. A mixture of two disagreeing signals is flatter than either alone; this restores it. **Calibrated per net** — 1.02 belongs to `pisp2`, 1.05 to `pisp`, 1.15 to the old 256/128 net — so it must move whenever `PolicyWeights` does. |
| `FpuReduction` | spin | 20 | 0–100 | First-play urgency in hundredths, in the same tanh units as `W`. An unvisited child is assumed to be worth this much less than its parent. Only bites when children are unevaluated — `PolicyMode 2`, or `SeedChildren` off. |
| `SeedChildren` | check | true | — | Off stops a child being born with `N=1, W=tanh(eval)`. Diagnostic; see the measurements. |
| `NodeMinimax` | check | true | — | Off makes a node take a static evaluation of itself instead of the 1-ply minimax over its children. Diagnostic. |

`PolicyBlend`, `PolicyTemp` and `BlendScale` are **calibrated per net**. The 256/128 net
wants 45 / 75 / 115; the 512/256 net wants 50 / 75 / 108. Changing `PolicyWeights` without
changing the others leaves the prior at the wrong sharpness, which silently confounds any
comparison.

The engine prints every setting at startup, which is the reliable way to confirm what a
match actually ran with:

```
info string settings: Hash 2048, Threads 8, ..., PolicyMode 1, PolicyBlend 0.45,
PolicyTemp 0.75, BlendScale 1.02, FpuReduction 0.2, SeedChildren true, NodeMinimax true
```

There is also a `settings` command, outside the UCI protocol, that prints the same values
in a readable column with both the engine-side number and the UCI integer.

### `ExplorationRoot`: the search commits in its first 3% and cannot reconsider

**The failure is a fixed point, not variance.** Re-running the test set four times per position: 57 of
141 are solved in **zero** of four repeats, and in 44 of those 57 — 77% — the engine plays the *same
wrong move every time*, median one distinct wrong move across four repeats. Quoting a solve rate hides
this; always / never / sometimes is the split that means something.

**It is not tree reuse.** Accumulated visits are a median **1.00x** of the current search's simulations
across 40,763 live searches, above 1.2x in only 7.9%. Top-move concentration is 89.9% on a fresh tree
against 93.4% live, so reuse contributes about 3.5 points of it and nothing else.

**It is PUCT closing, correctly, on an ordering formed too early.** Root PUCT is
`Q + C*P*sqrt(parentN)/(1+N)` with `C = ExplorationMax = 1.6` at depth 0. On the never-solved positions:

| | median |
|---|---|
| Q gap the correct move must overcome | +0.0438 |
| its exploration term U at the end | +0.0277 |
| **U < gap — it can never be selected again** | **34 of 55 (62%)** |
| the correct move's share of visits | 0.29% |
| its evidence at the end | 2,348 of ~767,000 |

And the lock closes almost at once: permanent from the first 10% of the search in **53%** of them and
from before the halfway point in 76%, **median 2.8%**. The remaining 97% of the search confirms a
decision already irreversible, which is exactly why more time does not help and why the same wrong move
returns every run.

The constant needed to keep the correct move selectable is **flat in time** — median 1.88 at the very
start and 1.99 at the end, 75th percentile 2.5 to 3.7 throughout — so one larger value at the root is
the right shape rather than a schedule. `ExplorationMax` was capped at **2.0**, the median requirement
exactly and unable to reach the 75th percentile, so the cap was itself blocking the fix; it is 10.0 now.

**`ExplorationRoot`** sets C at depth 0 independently, **0 meaning "use the depth schedule"**, so the
default is the incumbent exactly. It replaces the `//if (depth == 0) C = 2.0;` that sat commented out in
`select_best_child` — and the measurement explains why that value would have looked like it did nothing.

**Measured, 171 positions from an empty tree at 2 s, three repeats per setting** (equal repeats, because
more repeats mechanically produce fewer of both `always` and `never`):

| `ExplorationRoot` | always | never | sometimes | rate | mean per-position change | paired bootstrap p |
|---|---|---|---|---|---|---|
| 0 (today) | 42 | 89 | 40 | 35.7% | — | — |
| 2.0 | 44 | 104 | 23 | 32.5% | **−3.0 points** | 0.046 *(worse)* |
| 3.5 | 53 | 91 | 27 | 38.9% | +3.5 | 0.056 |
| 6.0 | 48 | 85 | 38 | 38.4% | +3.3 | 0.061 |
| **10.0** | **55** | **78** | 38 | **41.7%** | **+6.3 points** | **0.0065** |

Of the 82 positions never solved at 0, **10 are solved in every repeat at 10.0**; of the 38 always
solved at 0, 1 becomes never. The trend is monotone — 2.0 worse, 3.5 and 6.0 marginally better, 10.0
clearly better — which is what makes it more than a post-hoc pick among four settings.

**The arithmetic predicted 2.0 and 2.0 makes things worse.** The trace said the median C needed was
1.95, but that calculation assumed a *fixed trajectory*: raising C changes the whole visit distribution,
so the extra visits spent on losing moves come out of the moves that needed them. A constant just large
enough to unlock on the old trajectory is too small to pay for the trajectory it creates, and it takes
roughly 3x the predicted value before unlocking outweighs dilution. Any offline reweighting of a fixed
trace is subject to the same error.

**The frontier, three repeats each, 0 re-run in the same batch as an in-run control:**

| `ExplorationRoot` | always | never | sometimes | rate | vs 0 | bootstrap p | agrees with the prior's top move |
|---|---|---|---|---|---|---|---|
| 0 (today) | 40 | 85 | 46 | 35.7% | — | — | 21.0% |
| 2 | 44 | 104 | 23 | 32.5% | −3.2 | 0.057 | 21.2% |
| 3.5 | 53 | 91 | 27 | 38.9% | +3.2 | 0.071 | 22.4% |
| 6 | 48 | 85 | 38 | 38.4% | +2.7 | 0.106 | 22.5% |
| 10 | 50 | 83 | 38 | 40.4% | +4.7 | 0.041 | 22.0% |
| 15 | 50 | 80 | 41 | 42.3% | +6.6 | 0.0039 | 23.5% |
| 20 | 59 | 73 | 39 | 44.6% | +9.0 | 0.0008 | 27.8% |
| 30 | 60 | 76 | 35 | 45.8% | +10.1 | 0.0003 | 27.3% |
| **50** | **68** | **70** | 33 | **49.1%** | **+13.5** | **<0.0001** | 31.8% |

**No ceiling in range.** Monotone except the dip at 2, and still rising at 50, which is the current cap.
`always` goes 40 to 68 and `never` 85 to 70: 28 positions converted from a fixed wrong answer to a fixed
right one.

**It is not the search degenerating into its policy head**, which is the obvious confound, since a large
C makes root allocation tend to `argmax(P/(1+N))` and ignore Q. The prior's own top move is acceptable in
**21.2%** of these positions; at 50 the engine scores 49.1% while agreeing with that move only 31.8% of
the time, so the extra solves are not the prior's.

**What it costs, measured.** `RootTraceFile` at both settings:

| | top move's share of visits | root moves with ≥1% of the top move's visits | simulations |
|---|---|---|---|
| `ExplorationRoot` 0 | 89.9% | **2** | 676,288 |
| `ExplorationRoot` 50 | 55.5% | **10** | 578,895 |

**Today's search effectively examines two root moves out of a median 34.** At 50 it examines ten, still
concentrating 55% on one rather than going uniform, and pays **14% of its simulation throughput** — a
wider root reuses less of the deep principal variation and expands more distinct subtrees.

**THE LIMITATION THAT MATTERS MORE THAN THE p-VALUE.** This test set is built from positions where the
current visit allocation *failed*. It is therefore the best available instrument for showing that the
allocation is the problem, and close to the worst for estimating what changing it is worth: the set holds
no position where creatica already plays well and a wide root would make it play worse, and that is
exactly where the 14% throughput cost lands. The 40 positions it always solves today are the only
counterweight, and 50 breaks few of them, but 40 positions are not a sample of normal play. **Expect a
match to show materially less than +13.5 points' worth, and do not rule out a loss.** The mechanism is
established; the magnitude is not.

### Where the visits go, and whether a recency-weighted Q would help

`RootTraceFile` is a diagnostic, empty by default and writing nothing unless set: on the tick thread 0
already pays for, it appends every root child's `(edge visits, N, W, evidence)`, plus one final line per
search. Between two snapshots, `-(ΔW)/Δevidence` is the mean of exactly the backups that arrived in that
window, so the evolution of `Q` can be reconstructed and alternative weightings scored **offline on real
searches**, without changing the search.

**What the visit dump already showed**, over the 170 mistakes matched to a live search:

| | median |
|---|---|
| the blunder's share of visits | **84.5%** |
| the best acceptable move's share | **1.5%** |
| visit ratio, blunder / acceptable | **41.9x** |
| the acceptable move's rank by visits | 3rd of 34 legal moves |
| prior on the blunder | 0.0615 |
| prior on the best acceptable move | 0.0565 |
| prior *favoured* the blunder in | **82 of 170 — a coin flip** |

So the 42x visit gap is produced by the search's own value estimates, not inherited from the policy.
That is an independent reason the three policy experiments were nulls.

**Concentration does not distinguish a blunder**, which was the first hypothesis and is wrong: the top
move's share of visits is a median 90.3% in positions creatica blundered in against **93.5%** in all
40,590 other searched positions. Blunder positions are slightly *less* concentrated. (Two data traps
here: under `ReuseTree` visit counts accumulate across moves while the `simulations` column is
per-search, so shares must be taken against the visit sum — dividing by `simulations` gives impossible
figures above 100%.)

**The test of a recency-weighted Q.** All 171 positions re-searched from an empty tree at 2 s with the
bot's settings, median 767,554 simulations. Every criterion scored the way the engine scores — argmax
over *all* root moves — and asked whether the move it picks is Stockfish-acceptable:

| criterion | acceptable move picked, of 171 |
|---|---|
| **engine today: most edge visits** | 65 — **38.0%** |
| flat Q, the final `W/evidence` | 69 — 40.4% |
| recency-decayed Q, λ=0.99 | 71 — 41.5% |
| recency-decayed Q, λ=0.95 | 73 — 42.7% |
| **recency-decayed Q, λ=0.90** | 74 — **43.3%** |
| recency-decayed Q, λ=0.70 | 70 — 40.9% |
| Q over the last 50% of evidence | 70–74 — 40.9–43.3% |
| Q over the last 25% of evidence | 71–74 — 41.5–43.3% |
| Q over the last 10% of evidence | 71 — 41.5% |

Paired against the engine's own criterion at λ=0.90: both acceptable 62, **only recency 12, only the
engine 3**, neither 94 — 15 discordant pairs, exact two-sided McNemar **p = 0.035**.

**Four things temper that.** λ was chosen from nine candidates on the same 171 positions, so the effect
size is an upper estimate — though every one of the nine beats the engine's criterion, so the direction
is not a single lucky point. Reweighting a fixed trace shows only that the information was *present* by
the end; it cannot show what the search would have done, because Q feeds PUCT and a different Q would
have sent the visits elsewhere. 94 of 171 are unfixed by any criterion tested. And a position test set
has moved in the opposite direction to playing strength before.

**How it would be implemented, and why the sliding window is the one to build.** A per-backup
exponential decay is `W ← W·γ + v`, `evidence ← evidence·γ + 1`, with γ = 1 recovering today's behaviour
exactly. The measured λ=0.90 per window corresponds to a half-life of about 69,000 evidence, i.e.
γ ≈ 0.99999. But `W` and `evidence` are atomics updated with `fetch_add` on the hottest path in the
engine, and a multiplicative decay needs a compare-and-swap loop instead. The sliding window scores the
same (43.3% at the last 25–50% of evidence) and keeps `fetch_add`: carry a second pair
`(W_recent, evidence_recent)`, reset periodically, and read Q from it once it has enough evidence.
Two more atomic adds per backup and a periodic reset.

### The leaf value: `NodeMinimax` is optimistic, and there is no capture quiescence

Three candidate leaf values, measured against the shard's own Stockfish evaluation over 60,000
held-out positions that are not in check (91.6% of them), with mate scores clamped to ±1000 cp. `W`
is `tanh(cp/610)`, the unit the search averages, on a scale two wide.

| | mean \|err\| | median | 90th pct | sign ok | ≥200cp | ≥400cp | **confidently wrong side** |
|---|---|---|---|---|---|---|---|
| static `evaluate_nnue()` | 154.7 cp / 0.162 W | 60 | 461 | 83.9% | 25.5% | 12.5% | 0.91% |
| **1-ply minimax (what the engine uses)** | **181.8 cp / 0.212 W** | **109** | 463 | **76.3%** | **31.7%** | 13.4% | **2.13%** |
| all-captures quiescence | **134.4 cp / 0.135 W** | **49** | **410** | **86.9%** | **21.8%** | **10.4%** | **0.29%** |

"Confidently wrong side" means the truth and the estimate are both at least 150 cp and disagree on who
is better — the failure that loses games rather than a point of accuracy.

**`NodeMinimax` is worse than not looking at all.** With `node_minimax` true and `PolicyMode 1` — both
defaults — `eval_and_expand()` does not use the static value: the node's `cp` is
`max over children of -position_eval(child)`. That is worse than the static evaluation on every
measure, and 2.3× worse on confidently-wrong-side. The mechanism is maximisation bias, and it shows up
directly when the signed error is measured from the **side to move's** point of view rather than
White's, which is where a White-relative average cancels it out:

| mean signed error, side-to-move relative | |
|---|---|
| static evaluation | **−42.2 cp** (slightly pessimistic) |
| 1-ply minimax | **+74.3 cp** (optimistic) |
| quiescence | **−15.7 cp** (nearly unbiased) |

Taking a maximum over ~30 child estimates that each carry about 150 cp of noise overestimates
systematically. In MCTS that matters more than it would in alpha-beta, because `Q = W / evidence` is a
**mean** of leaf values: random error averages away over visits and a bias does not. And the bias
alternates sign with depth, so every node believes it is doing better than it is — which is the shape
of a search that walks into a blunder.

**THAT CONCLUSION IS WRONG, AND THE TEST SET SAYS SO.** Absolute accuracy against Stockfish is not
the quantity that decides a move. What decides a move is whether the estimator prefers a good move to
the blunder, and that is a *relative* question, on which an optimism shared by every sibling largely
cancels. Measured directly on all 171 rows of `mistakes/testset.tsv` — real positions where a
2-second Stockfish says creatica's move lost at least 100 cp — by scoring the child after the played
move against the child after each acceptable move and asking whether the estimator prefers an
acceptable move by at least 100 cp:

| estimator applied to each candidate's child | blunders rejected, of 171 | quiescence nodes / position |
|---|---|---|
| **1-ply minimax (the current default)** | **54 — 31.6%** | 0 |
| 2-ply minimax | 30 — 17.5% | 0 |
| minimax over children quiesced to depth 2 | 26 — 15.2% | 792 |
| minimax over children quiesced to depth 4 | 25 — 14.6% | 2474 |
| static `evaluate_nnue()` | 23 — 13.5% | 0 |
| quiescence depth 2 | 22 — 12.9% | 22 |
| quiescence depth 4 | 24 — 14.0% | 63 |
| quiescence depth 6 | 26 — 15.2% | 125 |
| quiescence depth 8 | 25 — 14.6% | 180 |
| quiescence depth 10 | 25 — 14.6% | 230 |

At a stricter 200 cp bar the gap widens: the 1-ply minimax rejects 28 and the static value 9.

**`NodeMinimax true` is the best leaf value of the ten tested, by more than a factor of two.** Leaving
it at its default is right and `NodeMinimax false` would make the engine worse — the opposite of what
the absolute-accuracy table above predicts.

**One principle explains every row.** An estimator whose last ply is the *blunderer's own move* takes a
maximum there, so it rescues the blunder with an optimistic reply and hides it. An estimator whose last
ply is the *opponent's* move reveals the punishment. `-minimax1(child)` ends on the opponent's reply, so
it is conservative and sees the refutation; `-minimax2(child)` adds the mover's follow-up and the
optimism returns; quiescence from a child at even depth also ends on the mover's recapture. That is why
1-ply beats 2-ply, why depth 2 quiescence is *worse* than no quiescence at all, and why the
odd-and-even alternation shows up in every one of these tables.

**So a capture quiescence at the leaves is not worth building.** It does not reject creatica's real
mistakes better than the static value it would replace, it makes the existing minimax markedly worse
when combined with it, and it costs 22% of the node rate at depth 2 rising to 119% at depth 8
(measured over the 171 test-set positions: 6.7 quiescence nodes per call at depth 2 and 57.3 at depth 8,
against a PolicyMode 1 expansion of about 150 µs, of which 137 µs is evaluating every child).

**What the residual says.** Even the best leaf estimator flags only 31.6% of these mistakes. For the
other two thirds no 1-ply or 2-ply value distinguishes the blunder from a good move at all — they need
real tree search, the search *did* run for thousands of simulations, and it still chose the blunder. So
for most of creatica's mistakes the lever is neither the evaluation's accuracy nor the leaf's
resolution: it is how the search spends its visits. `Q = W / evidence` is a mean, and a mean is slow to
abandon a move that started out looking fine.

**A caution about the two hand-picked examples.** `QlmMRJGE` and `qgaqG0C8` were chosen *because* they
looked like value errors, and they are not representative: quiescence needs depth 8 to catch the first
and never fully catches the second, while the aggregate said depth 2 was sufficient. Two examples
selected for a hypothesis cannot test it.

**Do not confuse this with `MinimaxBackup`**, which was a different proposal — backing minimax values
up through the tree rather than choosing the leaf's own value — and which failed in a match.

**There is no capture quiescence in creatica.** What `CLAUDE.md` calls "quiescence at leaves" is
`process_check()`, a check *extension*: `evaluate_nnue()` returns a sentinel in check, so a checked leaf
is expanded and minimaxed over its replies instead. Captures are never resolved. The quiescence row
above is a plain stand-pat-plus-all-captures alpha-beta written only to measure the gap; it does 40.4
nodes per position, so at roughly 4 µs an evaluation it cannot simply be dropped in at every leaf.

**Spot-checked on the two blunders that actually cost games**, which is what makes the aggregate
believable. The root evaluations were nearly right; it was the child position after the chosen move
that was misjudged, and quiescence is what catches it — while leaving the correct move untouched:

| | Stockfish | static | quiescence | nodes |
|---|---|---|---|---|
| `QlmMRJGE` after the blunder Ng6 | +516 | **+10** | **+496** | 138 |
| `QlmMRJGE` after the correct Ne6 | +78 | +68 | +68 | 5 |
| `qgaqG0C8` after the blunder Rc7 | +561 | **+58** | **+203** | 37 |
| `qgaqG0C8` after the correct Re8 | 0 | +1 | +1 | 19 |

(All White-relative; both games had creatica as Black.) In `QlmMRJGE` quiescence would have moved that
child from +10 to +496 at its very first expansion, which is enough to reject the move outright.

### The stored PV move was decoded wrongly, and it cost the policy castling entirely

**Every policy figure recorded before 28 September 2026 was measured on a set that silently excluded
the positions the policy is worst at.** Two independent encoding mismatches, both in the readers:

- **Castling is stored king-to-ROOK.** `lichess_evals_bin_writer.cpp`'s `parse_uci_move()` keeps the
  PV move as its UCI text reads, and the source data gives `e1h1`, `e1a1`, `e8h8`, `e8a8`. Every legal
  move list generated here has castling as king-to-`g1`/`c1`. Nothing ever matched.
- **Promotions are stored `0=Q, 1=N, 2=B, 3=R`** — see that function's `'q'` case, which assigns 0.
  Every reader had `{none, N, B, Q}`, so a queen promotion looked like *no* promotion and matched
  nothing, and a rook promotion was silently relabelled a queen. `promo=0` is also the default for a
  non-promotion, but the board resolves it: a pawn arriving on the last rank promotes, and there 0
  means queen.

Of 3,296 records in 203,296 whose stored PV1 matched no generated move, **71% were castling and 29%
promotions, and nothing else at all.** With both decoded, the figure is zero.

**In the trainer this was worse than losing the example.** Under `PIECE_INDEX` the promotion code is
never read, so promotions were unaffected there — but the castling row `(King, e1, h1)` is not in the
legal list, so the loss treated PV1 as illegal. For the 56% of records carrying one PV the whole
position contributed nothing. For the 44% carrying more, PV1 was dropped and the remaining PVs were
**renormalised**, so the model was actively trained to believe the second-best move is best in every
position where castling is best. The row `(King, e1, g1)` appeared in the softmax as a legal move and
never once as the target, so it was only ever pushed down.

**The measured consequence in `nnue_policy_pisp2.bin`**, over the 2,292 test positions where castling
is Stockfish's own best move:

| | Top-1 | Top-6 |
|---|---|---|
| 1-ply child evaluations alone | 22.4% | **92.0%** |
| policy alone | 0.0% | **0.0%** |
| blended at `PolicyBlend 45` | 0.0% | **0.1%** |

The policy does not merely rank castling poorly; it never has it in its top six, and because the blend
is 45% policy it destroys a signal the child evaluations had. Spot-checked by hand with a
single-position ranking tool: in `r3k2r/ppp1qpb1/2n1bnpp/3pp3/4P3/2PP1NNP/PPB2PP1/R1BQK2R w KQkq - 0 1`
castling ranks **34th of 37** legal moves, and in
`r1b1kb1r/pp2pppp/2n2n2/2q5/4P3/2NB4/PPP2PPP/R1BQK2R w KQkq - 0 1` it ranks **37th of 37**.

**So every published number is optimistic.** Validating the deployed net with the fixed decoder, on
20,000 positions rather than the 19,772 that used to survive:

| `nnue_policy_pisp2.bin` | Top-1 | Top-4 | Top-6 |
|---|---|---|---|
| old decoder (what the tables above record) | 39.46 | 74.95 | 84.04 |
| **fixed decoder** | **39.02** | **74.10** | **83.09** |

`bench_blend` agrees: policy alone 39.25 / 74.17 / 82.89 and blended at 0.45 38.32 / 75.72 / 84.76 over
200,000 positions, with zero records skipped. **Treat every policy Top-k in this document as measured
on the easier, truncated set**, and compare new runs only against the fixed-decoder figures.

**The input cannot see castling rights**, and that is deliberately not being changed yet. Two positions
differing only in castling rights reach the model as byte-identical 1024 NNUE features *and*
byte-identical planes — verified directly — because the king's square is already marked as a legal
source in plane 12 and `g1`/`c1` are already marked as legal destinations in plane 13 by the rooks, so
the castling move's `(from, to)` pair is entirely absorbed by the marginals. The halfmove clock and
the fullmove number are invisible too; the en passant square shows up as a single extra cell in plane
13. But the model is only ever asked to score moves that are *in* the legal list, so it is only asked
about castling where castling is legal and never has to infer availability. Fix the labels first —
`L1C8pi3_run.sh` — and see whether anything is still missing.

### Policy file formats

The exported `.bin` records its own layout in a version field, and the engine refuses anything it
cannot apply rather than silently running a net it is only partly executing. Two things vary
independently: whether the move table is indexed by the moving piece, and whether the legality term
`Wl` is present.

| version | tensors | move index | rows |
|---|---|---|---|
| 1 | W1 b1 W2 b2 emb | `from*64 + to` | 4096 |
| 2 | + `Wl` [h2,h2] | `from*64 + to` | 4096 |
| 3 | + `Cl` [out,h2] | `from*64 + to` | 4096 |
| 4 | W1 b1 W2 b2 emb | `(piece-1)*4096 + from*64 + to` | 24576 |
| 5 | + `Wl` [h2,h2] | piece-indexed | 24576 |
| 6 | + `Cl` [out,h2] | piece-indexed | 24576 |
| 7 | 2 + spatial block | `from*64 + to` | 4096 |
| 8 | 5 + spatial block | piece-indexed | 24576 |
| 9 | 8 with **one spatial head per moving piece type** | piece-indexed | 24576 |

The engine implements 1, 2, 4, 5, 7, 8 and 9. Versions 3 and 6 use the untied `Cl` table and are
refused. An engine older than versions 7 and 8 refuses them too, which is deliberate: read as a 2 or
a 5, a spatial net would run without its spatial term. Version 9 is a separate version for the same
reason: read as an 8, its per-square vectors would be truncated to their first `dim` numbers, which
is the pawn head, and every knight, bishop, rook, queen and king move would be scored with it. Its
spatial block also carries a ninth header integer, so its header magic is `SPA2` rather than `SPAT`
and a misread fails on the magic instead of on arithmetic.

**The piece index** exists because `emb[from*64 + to]` gave a rook moving e1–e4 and a queen moving
e1–e4 the same row, leaving the model to tell them apart through the 128-dim global position code.
Measured on matched 28-shard runs: **Top-1 33.45 → 34.65, Top-6 76.07 → 78.02**. It helps *quiet*
moves roughly six times more than captures (best-move-in-top-6: captures 89.75 → 90.16, quiet
65.45 → 67.88), which is the blind spot it was aimed at — the NNUE `FullThreats` input indexes a
move by `(from, to)` only when the destination is occupied, so among legal moves it names captures
and nothing else. Inference cost is unchanged: `policy_score()` does `h2` multiply-adds per move
whatever the row count.

**The legality term** is `score(i) = dot(ctx + Wl * mean over legal moves of emb[j], emb[i])`. It is
*not* a constant offset: expanding it, move *i* picks up `dot(Wl * mean_j emb[j], emb[i])`, a
learned interaction between each move and the set of moves available, so it reorders them. Worth
+0.97 Top-4 and +1.19 Top-6 on a matched pair, for 16,384 numbers — 2% of the model. Because it
depends on the *whole* legal set, the engine collects the moves before scoring any of them, and any
offline tool that scores move-by-move during enumeration will silently omit it. That bug made
`bench_blend` report policy-only Top-1 of 29.89% for a net the trainer measures at 34.65%, and would
have moved a deployed parameter the wrong way.

**A consistency check worth keeping.** Every offline harness should have one number that must match
an independent measurement. Here it is policy-only Top-1 against the trainer's own validation: it
agreed for the old net and was five points out for the new one, which is what exposed the missing
term.

**The spatial term** (versions 7, 8 and 9) adds `dot(u[from], v[to])` to each move's score, where `u`
and `v` are per-square vectors computed from 8x8 planes by 3x3 convolutions. It is what lets the
policy see the board as a board: which squares are defended, and by what. The planes, all oriented
from the side to move's point of view, are: own pieces by type (0–5), enemy pieces (6–11), squares a
legal move starts from (12), squares a legal move lands on (13), and squares attacked by each piece
type of each side, empty squares included (14–25) — the part the NNUE threat features cannot supply,
because they only index attacks on occupied squares.

The spatial block after `Wl` is an 8-integer header — magic `SPAT`, planes, channels, dim, layers,
scale, plane layout, attack planes — followed by `Wc1`, `bc1`, `Wc2`/`bc2` if there are two layers,
`Wu` and `Wv`. In format 9 it is a 9-integer header under the magic `SPA2`, the ninth integer being
the head count. The engine reads only shapes it implements (14 or 26 planes, up to 64 channels, dim
up to 32, one or two layers, 1 or 6 heads and 6 only on a piece-indexed net) and refuses the rest.

**Per-piece spatial heads** (format 9, the trainer's `SPATIAL_HEADS=6`). With one shared `u`/`v` pair
the spatial term `dot(u[from], v[to])` is a function of the two squares and of nothing else, and
under piece indexing it is added to all six per-piece blocks of the move table unchanged. So it
cannot express the most ordinary fact about a destination square: that it is a good square for a
knight and a bad one for a queen. Six heads give each moving piece type its own `u` and `v`, chosen
at scoring time by the piece on the from-square.

**THE MEASUREMENT THAT MOTIVATED THIS WAS WRONG. Read this before repeating the idea.**

The argument was: over 60,000 held-out positions with `pisp2`, the blended prior holds Stockfish's own
best *move* in its top six 85.7% of the time but holds *some move of that move's piece* 93.8% of the
time, with the gap reaching 14.7 points for queen moves and 2.0 for pawn moves. That was read as "the
model picks the right piece and misses the right square", and it is **an artefact of comparing two
metrics with completely different chance baselines**.

"Some move of the right piece is in the top six" is a far easier question than "the right move is in
the top six", and it gets easier the more legal moves that piece has. A piece with *k* of *n* legal
moves lands in a random six-move subset with probability `1 - C(n-k,6)/C(n,6)`, against `6/n` for one
specific move. Measured on the same 60,000 positions, with the floor subtracted and expressed as the
share of *available* headroom the model captures — `lift / (100 - floor)`:

| Stockfish's best move is a… | move: got / floor / %headroom | piece: got / floor / %headroom |
|---|---|---|
| capture | 98.2 / 27.7 / **98%** | 99.2 / 70.0 / 97% |
| quiet move | 81.7 / 30.8 / **74%** | 92.0 / 69.9 / 73% |
| pawn move | 86.3 / 26.3 / 81% | 88.3 / 35.6 / 82% |
| knight move | 88.4 / 21.9 / 85% | 95.8 / 66.2 / 88% |
| bishop move | 85.9 / 22.0 / 82% | 94.5 / 69.8 / 82% |
| rook move | 82.6 / 25.5 / 77% | 95.0 / **82.6** / 71% |
| queen move | 81.5 / 20.7 / 77% | 96.2 / **89.3** / 64% |
| king move | 90.7 / 65.8 / 73% | 94.8 / 87.0 / 60% |
| all positions | 85.7 / 30.0 / **79%** | 93.8 / 69.9 / **79%** |

The two columns capture the **same 79%** of their available headroom. There is no piece-versus-square
gap. For queens the model is in fact *worse* at the piece question (64% of headroom) than at the move
question (77%), and the entire 14.7-point "gap" is the 89.3% floor — a queen with many legal moves is
in any random top six almost by default.

Two overnight runs were spent on that misreading before the floors were computed: `SPATIAL_HEADS=6`
and `SPATIAL_DIM=32`, both nulls, both exactly what an intervention aimed at a non-existent deficit
should produce. **Any claim about where the policy is weak must carry the chance baseline for the
metric it is measured in.** The scratchpad breakdown tool now prints `rndMove6` and `rndPiece6`
beside every row for this reason.

What the corrected table does say is the thing that was already known and already acted on: **captures
are essentially solved at 98% of available headroom and quiet moves sit at 74%**. That is the gap the
by-piece-type attack planes (14–25) were added for, and it is not a new lever pointing at any
particular reshaping of the spatial term.

The receptive-field explanation was also tested and also does not hold: splitting quiet bishop, rook
and queen moves by distance travelled gives 70%, 71%, 72%, 73% and 74% of available headroom at
distances 1 to 5. Flat. One 3x3 convolutions is not the fix.

**Cost.** `Wu` and `Wv` go from `[ch, dim]` to `[ch, 6*dim]`, which at 8 channels and dim 16 is 1,280
more numbers — 0.04% of the model. The `u` and `v` scratch grows to 48 KB each, held in a
`thread_local` per search thread. Six heads cost about **+2.8 µs per expanded node** after the
projection rewrite described below, all of it in `spatial_forward` and none in `spatial_score` — the
expected shape, since the head count changes the per-square projection, run once per position, and not
the per-move dot product, which still reads `dim` numbers. Before the rewrite it was +8.3 µs.

### The projection rewrite

`policy_spatial_forward_impl()` ends by projecting the convolution's `[ch][64]` output into the
per-square vectors `u` and `v`. The obvious way to write that is channel-outer, accumulating straight
into `s.u` and `s.v` — and that reads and rewrites every one of the `64*heads*dim` output numbers once
per channel, so eight channels make eight passes over a 48 KB pair of arrays to perform 98,000
multiply-adds. Measured at dim 16, the projection cost **1.65 µs** a position with one head and
**9.9 µs** with six: six times the arithmetic at six times the price, from a base that was already
slow for what it computes.

It is now square-outer: one square's whole `heads*dim` block is built in a local buffer and stored
once, which does exactly the same arithmetic, summed over channels in exactly the same order, with an
eighth of the traffic. The relu on `u` folds into that single store, removing a separate sweep over
`64*heads*dim` and both `memset`s.

```
spatial_forward, ns per expanded node    before    after
  8 channels, dim 16, 1 head             7,341.7   6,872.9
  8 channels, dim 16, 6 heads           15,586.9   9,632.8
```

The projection itself goes from 1.65 µs to **0.55 µs**, and six heads from 9.9 µs to 3.3 µs.

**It is exact, not merely close.** Summing over channels in the same order makes this a memory-traffic
change rather than a numerics change, and `check_policy_export` confirms it against logit dumps written
*before* the rewrite: three nets — `pisp2` (one head), `pisp2h6` (six trained heads) and a
deliberately-diverged six-head test net — each reproduce their pre-rewrite figures to the last digit
(max 1.12e-05 / 1.07e-05 / 9e-06, mean 9.98e-07 / 8.97e-07 / 6.7e-07), with no top-move disagreements.

### The width frontier

With the projection no longer dominating, `bench_policy_spatial` takes `SPATIAL_CH`, `SPATIAL_DIM`,
`SPATIAL_LAYERS` and `SPATIAL_HEADS` from the environment, so the whole frontier can be measured in one
session. 37 positions of `test_fen_strings`, 40 repeats, idle machine, one head, as ns added per
expanded node and as a multiple of what `pisp2` costs today:

| channels | dim | layers | conv + projection | per move | total added | vs `pisp2` |
|---|---|---|---|---|---|---|
| 8 | 16 | 1 | 6,987 | 82 | 7,272 | **1.00x** (`pisp2`) |
| 8 | 32 | 1 | 7,416 | 182 | 7,797 | 1.07x |
| 16 | 16 | 1 | 14,016 | 86 | 14,325 | 1.97x |
| 16 | 32 | 1 | 14,704 | 181 | 15,094 | 2.08x |
| 32 | 16 | 1 | 28,319 | 92 | 28,646 | 3.94x |
| 32 | 32 | 1 | 30,066 | 184 | 30,478 | 4.19x |
| 8 | 16 | 2 | 8,212 | 82 | 8,473 | 1.17x |
| 16 | 16 | 2 | 19,525 | 84 | 19,810 | 2.72x |
| 32 | 32 | 2 | 50,479 | 188 | 50,876 | 7.00x |

**`dim` is nearly free and `ch` is not.** Doubling `dim` costs 7% of the spatial term, about 1% of an
expansion; doubling `ch` doubles it, because the first convolution's cost is `planes * ch * 9` per
square and 26 planes make it the dominant term. Doubling `dim` also doubles the per-move dot product,
82 ns to 182 ns, which is still negligible against the rest.

The order tried was `dim` first, `ch` only if `dim` moved the destination gap — see `L1C8D32_run.sh`.

**Result: `dim` 16 → 32 is a null too, and `ch` should NOT be tried on this reasoning.**
`nnue_policy_pisp2d32.bin`, trained 27–28 September 2026 on pisp2's recipe with `SPATIAL_DIM=32` and
one shared head, 8 h 06 min, exports clean (1.01e-05 over 56,373 moves, no top-move disagreements):

| | policy alone: Top-1 | Top-4 | Top-6 | blended at 0.45: Top-1 | Top-4 | Top-6 |
|---|---|---|---|---|---|---|
| `pisp2` (dim 16) | 39.58 | 74.72 | 83.73 | 38.81 | 76.50 | 85.73 |
| `pisp2d32` (dim 32) | **39.66** | **74.93** | **83.81** | **39.00** | **76.60** | **85.98** |

Consistently positive but by 0.1 to 0.25 of a point, which is inside run-to-run variation, for 1.07x
the spatial term's cost. More to the point, the destination-side gap it was aimed at went 8.1 → 8.0,
and that gap was a measurement artefact in the first place (see the correction under *Per-piece
spatial heads*). **Three interventions on the spatial term's destination pathway — per-piece heads,
double width — have each moved held-out accuracy by less than a quarter of a point.** Paying 2x for
`ch` 8 → 16 has no evidence behind it and should not be the next thing tried.

If `pisp2d32` were ever deployed it would need `BlendScale 108` rather than 102: at `PolicyBlend 0.45`
it puts 0.3984 on its top move at scale 1.00 against pisp2's 0.4025.

A `SPATIAL_HEADS=6` build can be fine-tuned from a one-head checkpoint: the loader copies the single
trained pair into all six heads and says so, and the model then scores every move exactly as that
checkpoint did, so the six heads can only diverge by learning something. Checkpoints record their
head count in a `spatial_heads` buffer and a build refuses one that carries more heads than it has.

**MEASURED, AND IT IS A NULL. Do not deploy it.** `nnue_policy_pisp2h6.bin` was trained on
27 September 2026 with `L1C8pi2H6_run.sh` — `SPATIAL_HEADS=6` and otherwise byte-for-byte pisp2's
recipe: same architecture, same 28 cached shards, same two epochs, same batch size, same 68,726-step
schedule ending at `LR_MIN`. 8 h 05 min, no restarts. The engine reproduces the trainer to 1.07e-05
over 56,373 moves with no top-move disagreements.

Trainer validation on the same 19,772 held-out positions:

| | Top-1 | Top-4 | Top-6 | held-out policy loss |
|---|---|---|---|---|
| `pisp2`, one shared head | **39.46** | 74.95 | 84.04 | 2.0695 |
| `pisp2h6`, six heads | 39.40 | **75.04** | **84.15** | **2.0602** |

`bench_blend` on 60,000 test positions, which is the number that matters because it is how the engine
uses the net:

| | policy alone: Top-1 | Top-4 | Top-6 | blended at 0.45: Top-1 | Top-4 | Top-6 |
|---|---|---|---|---|---|---|
| `pisp2` | **39.58** | 74.72 | 83.73 | 38.81 | 76.50 | 85.73 |
| `pisp2h6` | 39.44 | **74.92** | **83.86** | **38.94** | **76.72** | **85.93** |

Every difference is two tenths of a point or less, in both directions, which is well inside the
run-to-run variation the `pisp` → `pisp2` pair already demonstrated (0.4 points of policy Top-1 for
no measurable Elo over 300 games). Against that, the six heads cost **8.3 µs per expanded node**, or
about 13% of the node rate. The trade is not close.

**The term it was aimed at did not move** — the raw piece-minus-move gap went from 8.1 to 8.0 points
overall, queen 14.7 to 14.3, rook 12.4 to 12.5 — but as the correction above explains, that gap was
never a deficit, so its not moving is not informative about the six heads. The term did learn: it grew
to 173% of the mean absolute legal logit and the held-out loss improved by 0.009 nats, so the extra
parameters found something; it simply was not something that reorders the top of the move list. `emb`
is already indexed by (piece, from, to), so the model already had a per-piece per-destination
parameter, and a per-piece *position-dependent* one adds nothing measurable on top.

If it were ever deployed it would need `BlendScale 108`, not 102: at `PolicyBlend 0.45` it puts 0.3934
on its top move at scale 1.00 against pisp2's 0.4025, and 1.05 brings it to 0.4086 against the 0.4087
pisp2 reaches at 1.02.

**Plane layout** records a trainer bug the weights were trained with, because the engine has to
reproduce the planes a net saw, not the planes it should have seen. Under piece indexing the trainer
wrote plane 12's marks at `12*64 + (legal_idx >> 6)`, and `legal_idx >> 6` is `(piece-1)*64 + from`,
so the mark landed in plane 12 + (piece-1): pawn sources in plane 12, but knight sources in plane 13
and bishop, rook, queen and king sources in planes 14–17, the first four own-piece attack planes.
Layout 1 is that; layout 0 puts every mark in plane 12. `SPATIAL_FROM_PLANE_FIX` in the trainer
chooses between them, and since 2026-09-14 it defaults to 1, which builds layout 0.

A wrong choice would be silent. An old layout-1 checkpoint exported by a build with the fix on
gives a net the engine reproduces exactly, so the export check passes, and the net is simply
less accurate than it should be. So checkpoints now carry the layout they were trained with, in a
`plane_layout` buffer, and the trainer refuses to load one that disagrees with its build. A
checkpoint without the buffer predates it and is taken to be layout 1 under piece indexing. Every
piece-indexed spatial checkpoint trained before that date, including the one behind
`nnue_policy_pisp.bin`, needs `-DSPATIAL_FROM_PLANE_FIX=0` to be validated, fine-tuned or exported.
The engine's layout-0 path for a piece-indexed net was checked against the trainer on a short test
run: over 2,000 held-out positions and 56,373 legal moves the largest score difference was 4.4e-6,
and the top move agreed in every position.

**Illegal castling in every policy net trained before 2026-09-13.** `castlingMoves()` used to refuse
castling through pieces or through check with `CastlingPath`, a process-wide table that only
`fen2board()` filled. The trainer builds its boards with `board_from_record()` and never parsed a FEN,
so the table stayed zero and any record with a castling right and its rook generated castling
through pieces and through check. On 2,000 held-out positions 263 had at least one illegal castle
in the legal list, which fed the softmax, the legality term and the spatial planes. The engine parses
FENs and never had them. The first fix filled the table once at trainer startup from the start
position. Since 2026-09-15 libchess has no such table at all: the castling path is computed from each
board's own king and rook squares, so a board built from a record is complete by itself (see *Engine
crashes in local matches* below for why the table had to go).
Measured on the first spatial net, the corrected lists cost it 0.10 Top-1 and 0.03 Top-6, so nets
trained with the defect are fine to keep — but every comparison between nets from before and after
the fix carries that small offset.

**Checking an export.** `EXPORT_CHECK=<tsv>` alongside `EXPORT_WEIGHTS` makes the trainer write its
own logit for every legal move of `EXPORT_CHECK_N` held-out positions (default 2,000), and
`check_policy_export <net.bin> <tsv>` recomputes every one of them through `policy_net.h`, following
the steps `eval_and_expand()` takes. A correct export matches to about 1e-5 with no position where the
top move differs. It is the only test that catches a wrong plane, a flipped square or a misread
tensor, which anywhere else would cost a point of Top-k and look like noise. For the first spatial
net: 56,373 moves, largest difference 9.4e-6, no top-move disagreements.

**The first spatial net**, `nnue_policy_pisp.bin` (version 8: piece-indexed, legality term, one 3x3
convolution with 8 channels, layout 1), trained on all 28 shards for one epoch, validates at
**Top-1 38.84, Top-4 74.52, Top-6 83.62** with correct legal lists, against 34.65 / — / 78.02 for the
piece-indexed net without it. What it costs the engine depends on the mode: in `PolicyMode 2`, where
an expansion does nothing but score moves, the convolution adds about 9 µs to an expansion that
otherwise costs about 46 µs at `Hash 2048`; in the default `PolicyMode 1`, where every child is also
evaluated with NNUE, single-threaded node rate fell 2.6–3.2% against the piece-indexed net.

**In play.** Three matches have been played between `pisp` and `pi`, all at 500 ms a move, `Hash 512`
and 2 threads. Only the third is a clean comparison.

Every one of them also contains games that an engine lost by crashing, not by playing. The cause was
a thread-safety defect in libchess, described in *Engine crashes in local matches* below. A crash
forfeits the game to the other side, and which side it hits is chance, so those games say nothing about
the nets. The figures below give each match as played and then with the crashed games removed.

- The first, 100 games, ran the older `creatica` binary on both sides. That binary cannot read
  version 8, so it refused `pisp` and kept its default net. It did not test `pisp` at all.
- The second, 100 games at default settings: `pisp` scored 49 (17 wins, 64 draws, 19 losses), about
  −7 Elo with a 95% range of −48 to +34. But its `pisp` side ran `creatica_sp` and its `pi` side ran
  the older `creatica`, so it changed the engine binary and the net at the same time. Three games were
  crash forfeits (two lost by `pisp`, one by `pi`); without them `pisp` is 16 wins, 64 draws and 17
  losses, about −4 Elo, range −44 to +37.
- The third, 200 games, with `creatica_sp` on both sides and `pisp` at `BlendScale 105` (see below):
  `pisp` scored 108 (37 wins, 142 draws, 21 losses), about +28 Elo with a 95% range of +2 to +54.
  **Ten of those games were crash forfeits, and seven of the ten went in `pisp`'s favour** (`pi`
  crashed seven times, `pisp` three). Without them `pisp` is 30 wins, 142 draws and 18 losses in 190
  games: about **+22 Elo, with a 95% range of −3 to +47**. So the one match that looked significant
  is not significant once the crashes are removed. It is still a lead for `pisp`, but it could be noise.

No clean match has been played with `pisp` at any other `BlendScale`. So these results do not show
whether 105 is better than the default 115 for `pisp`. They show only that `pisp` at 105 led `pi` at
the default, on the same binary.

**Half of its gain survives the blend.** `bench_blend` on 200,000 positions of the test file, with the
engine's in-check rule and a consistency gate that reproduces the trainer's validation (Top-1 38.75
against 38.84 for `pisp`, 34.59 against 34.65 for `pi`):

| `pisp` − `pi` | Top-1 | Top-4 | Top-6 |
|---|---|---|---|
| policy alone | +4.36 | +6.11 | +5.83 |
| blended at `PolicyBlend 45` | +2.12 | +3.51 | +3.03 |

The other half overlaps with what the 1-ply child evaluations already see — plausibly the attack planes
telling the policy what an evaluation after the move notices anyway. The best blend weight barely moves
(Top-6 peaks at 0.50 for `pisp`, 0.40–0.45 for `pi`). What does move is concentration: at the same
settings the blended prior puts 0.445 on its top move for `pisp` against 0.416 for `pi`, and
**`BlendScale 105` restores 0.416** without changing the ranking. The third match above used it; the
second did not.

`bench_blend` needed three fixes to measure this: spatial-net support; policy scoring only at the root,
because the recursion into a checking move's replies applied their legality term to the root's context;
and the engine's rules that in check the prior is the child evaluations alone and that promotions count
once in the legality term.

**The retrained spatial net, `nnue_policy_pisp2.bin`** (format 8, plane layout 0). Same architecture as
`pisp`, trained from scratch with the three trainer fixes of 14 September: plane 12 marks the true
from-square, the cosine schedule is sized from the exact position count, and the run state makes it
resumable. Two epochs over all 28 cached shards — 277,074,119 positions a pass, 68,726 steps — in 7 h 08 min
under `train_unattended.sh`, with no restarts and the rate ending exactly at `LR_MIN`. Validation on the
first 20,000 test positions: Top-1 39.05 / Top-4 74.40 / Top-6 83.54 after the first epoch, **39.46 / 74.95 /
84.04** after the second. The engine reproduces the trainer to 1.1e-5 over 56,373 moves, with no top-move
disagreements.

`bench_blend` on 200,000 test positions, all three nets in one session with the same tool:

| | policy alone: Top-1 | Top-4 | Top-6 | blended at 0.45: Top-1 | Top-4 | Top-6 |
|---|---|---|---|---|---|---|
| `pi` | 34.60 | 68.36 | 77.74 | 36.48 | 73.04 | 82.65 |
| `pisp` | 39.16 | 74.53 | 83.57 | 38.60 | 76.55 | 85.68 |
| `pisp2` | **39.52** | **74.94** | **83.79** | 38.71 | 76.54 | 85.71 |

As a policy alone `pisp2` is about 0.4 points ahead of `pisp` at Top-1 and Top-4; blended, which is how
the engine uses it, it is 0.1 ahead at Top-1 and level otherwise. The retraining's gain lies mostly in
what the child evaluations already see, so a large difference in strength is not expected. (This run
reads 39.16 for `pisp`'s consistency gate where the paragraph above recorded 38.75; the cause was not
investigated, and the table compares like with like.)

**`BlendScale 102` for `pisp2`.** `bench_blend` now reports the top move's share of the blended prior for
several BlendScale values in one pass, since the scale changes concentration but never the ranking. It
reproduces the earlier calibration — `pi` at 1.15 puts 0.4163 on its top move, `pisp` at 1.05 puts
0.4155 — and `pisp2` is slightly sharper, with 0.4098 at 1.00 and 0.4254 at 1.05, so 1.02 matches.

**`pisp2` against `pisp` in play.** One match, stopped by hand after 152 games: `creatica_frozenreset`
on both sides, 500 ms a move, `Hash 512`, 2 threads, `pisp` at `BlendScale 105` and `pisp2` at 102.
`pisp2` scored 70.5 (21 wins, 99 draws, 32 losses), about −25 Elo with a 95% range of −58 to +7.
Eleven games were crash forfeits, eight of them lost by `pisp2`. Without them `pisp2` is 18 wins, 99
draws and 24 losses in 141 games: **about −15 Elo, with a 95% range of −46 to +16**. That is no
measurable difference, which is what the blended accuracy above predicts.

A second match, 300 games, on `creatica_prefetch` (the engine with the crash fixed) on both sides and
otherwise the same settings, with tablebase adjudication on: `pisp2` scored **153 to 147** (54 wins,
198 draws, 48 losses), **about +7 Elo, with a 95% range of −16 to +30**. No engine crashed. So the two
matches agree: there is no measurable difference in strength between `pisp2` and `pisp`. The retraining
fixed the trainer's defects without gaining or losing strength that a 300-game match can see. (The
missing crashes in this match do not confirm the crash fix, because adjudication kept the engines out
of the tablebase code where the crash started; see *Tablebase adjudication* below.)

### Policy modes

- **`0` off** — the incumbent. Every legal move's child is evaluated with NNUE; the priors,
  the children's initial values and the node's own value all come from those evaluations.
- **`1` prior** — children are still evaluated, but the prior is a blend of the policy head
  and those evaluations. **This is the configuration worth deploying.**
- **`2` full** — children are not evaluated at all; the policy alone drives the search. Much
  cheaper per expansion. **Do not use it** — see the measurements below.

### Retired: the environment-variable binary

`creatica-nnue-policy` has been removed. It was the same search wearing the older
hand-written protocol layer (now `attic/uci-nnue-policy.cpp`), and it read these settings
as environment — `CREATICA_POLICY`, `CREATICA_POLICY_MODE`,
`CREATICA_POLICY_BLEND`, `CREATICA_POLICY_TEMP`, `CREATICA_BLEND_SCALE`, `CREATICA_FPU`,
`CREATICA_SEED_CHILDREN`, `CREATICA_NODE_MINIMAX`. They map one to one onto the options
above, except that a value like `0.45` becomes `45`.

That mechanism is retired. It exists only because adding a real option used to mean editing
a fixed enum in `libchess.h` and rebuilding a 110 MB shared library, which made the correct
mechanism too expensive to use. Nothing new should use it.

`creatica-shared-root` reads no environment variables at all; setting one on it does
nothing, silently.


## Compile-time settings

In `creatica_search.hpp`. Changing any of these needs a rebuild:

```
THREADS 4            MULTI_PV 5           HASH 2048
EXPLORATION_MIN 65   EXPLORATION_MAX 160  EXPLORATION_DEPTH_DECAY 5
VIRTUAL_LOSS 36      EVAL_SCALE 61        TEMPERATURE 58
PV_PLIES 16          MAX_DEPTH 100        SYZYGY_PATH "<your syzygy dir>"
POLICY_WEIGHTS_DEFAULT "nnue_policy_pisp2.bin"  FPU_REDUCTION 0.20
POLICY_BLEND 0.45    POLICY_BLEND_SCALE 1.15
POLICY_MAX_IN 2048   POLICY_MAX_H2 512    (buffer bounds; a larger net is refused at load)
```

The compile line is the first comment line of `creatica_search.cpp`, which is this
repository's convention for files outside CMake.


## What has been measured

Against `creatica-shared-root` at 2 s/move, and head to head between variants. Every
figure below is from an actual match or from held-out validation positions, not from
reasoning.

### Policy prior versus the incumbent

| configuration | result |
|---|---|
| policy **replacing** the eval-derived prior (`blend 1.0`-like) | 6.0/14, 42.9% |
| policy **blended** with it (`blend 0.45`) | 13.0/20, 65.0%, ≈ **+108 Elo** |

Prior quality on held-out positions, agreement with Stockfish's PV1:

| | incumbent | policy alone | blend 0.45 |
|---|---|---|---|
| top-1 | 27.0% | 32.8% | **36.3%** |
| top-4 | 65.8% | 66.2% | **71.6%** |
| top-6 | 77.8% | 75.9% | **81.3%** |

The blend beats both of its components on every measure. The two signals carry different
information: the child evaluation sees the position *after* the move, the policy sees only
the position before it.

### Which uses of the child evaluations are load-bearing

`full` mode removed all three at once and collapsed, which said only that the combination
matters. Testing them individually, each against the unmodified default:

| use of the child evaluations | result |
|---|---|
| in the prior `P` | **load-bearing** — pure policy lost by 3, match stopped |
| seeding the child's `Q` (`N=1, W=tanh(eval)`) | **load-bearing** — removing it lost by 3, match stopped |
| the node's own value (the line commented `//look-ahead update`) | **neutral** — 64 games, 32.5–31.5, ≈ +5 Elo |

**Conclusion: the look-ahead prior is not wrong.** Feeding one evaluation into the prior,
the child's value and the node's value is poor separation of concerns in theory, but two
of the three uses are individually necessary and the third is free either way. Keep the
defaults.

The mechanism behind the two that matter: a move that hangs a piece is seeded with
`W ≈ -1` at expansion and is effectively dead on arrival. Without that, it is
indistinguishable from any other unvisited child until a visit is spent on it, and with
roughly thirty children a plausible-looking blunder can collect the most visits before its
badness ever surfaces. A policy head cannot substitute, because it predicts *what a strong
engine would play* rather than *what this move does to the material*.

### The `creatica` port

`creatica` is the same search on the shared UCI layer, so it should play identically.
Verified three ways: six isolated positions, three with move lists, and seven successive
positions inside one long-lived process without `ucinewgame` — the last being what a match
actually does. In every case it chose the same move, and its evaluations fell inside the old
engine's own run-to-run spread. On the game sequence the old engine disagreed with *itself*
on 2 of 7 positions and `creatica` disagreed on the same 2.

A 20-game match nonetheless finished 3 points down. With the observed draw rate that is
about a one-in-five outcome between equal engines, and the machine was not idle — the match
ran alongside builds and test runs. Treated as a null. It has not been adopted; nothing
depends on it yet.

### Model size

| | 256/128 (819k params) | 512/256 (1.70M params) |
|---|---|---|
| top-1, held out | 33.08% | 34.83% |
| top-6, held out | 75.86% | 77.79% |
| inference cost | 24.7 µs/pos | 36.6 µs/pos |
| head to head, 31 games | 15.0 | 16.0 |

The wider net is better on every offline measure and **not measurably stronger in play**.
Keep the narrow one: identical strength, half the size, 12 µs cheaper per position.

This is the target-ambiguity ceiling. Top-1 went 26.7% → 33.08% → 34.83% while Elo went
+108 → +113 → level. Past roughly 33%, the positions the model newly gets right are ones
where several moves were comparable anyway, so agreeing with Stockfish's PV1 on them means
nothing over the board. **Top-K agreement has stopped predicting playing strength**, and
further optimisation against it is not worth machine time.


### Label smoothing

57% of the training records carry a single PV, so the one-hot target asserts that roughly
twenty-eight legal moves are impossible when the data only says one move was best. Softening
the target is the obvious response.

`LABEL_SMOOTH=0.10`, five epochs from scratch, against the 33.08% model: **-0.13 points of
top-1**. A null result — the model is no better and no worse.

Worth recording because the reasoning behind it was sound and the outcome still says no. It
also does not indicate the loss was ignoring the PV scores; it already trains against them.
The limit is the 57%, and smoothing does not add information that is not in the file.


### Tree reuse

For most of this engine's life the tree was destroyed before every move. `gc()`, the
mark-and-sweep that keeps the subtree reachable from the new root, existed but was reachable
only with `Ponder` on, and the source carried this note:

    //it seems there rarely is some kind of contamination or corruption of the tree
    //so let's try cleanup() instead of gc() if UCI Ponder option is false

That comment is a symptom from an old version, not a diagnosis — it predates the `terminal`
field, and the tree has been rewritten substantially since. Nobody had been able to verify
whether the cause still existed, because the failure was rare, silent, and surfaced many moves
after the damage.

Seven defects were found and fixed before re-enabling it. Three mattered specifically to reuse:
a path-dependent repetition verdict cached on a shared DAG node (in two places, one of which
made a repetition child *born* permanently drawn); a forced-move branch that advanced the board
without advancing the hash, so a node was created under one position's key and expanded with
another position's moves; and `set_root()` never expanding a *reused* root. All three are
invisible under `cleanup()`, because nothing survives long enough to go stale.

**`ValidateTree` is how this stays honest.** It checks, after every collection, that the root is
in the map under its own hash, that every node's key matches its stored hash, that no node
carries a repetition verdict, that `num_children` agrees with the children array, that **every
Edge points at a node still in the map**, that `total_children` matches what is allocated, and
that every surviving node is reachable from the root. It never dereferences a pointer it
suspects — a freed node may be unmapped, so it tests membership against a set of live addresses
collected from the map first.

It was confirmed to work by re-introducing one of the fixed bugs and watching it fire:

    info string validate_tree(after gc): node carries terminal 3 (repetition) which is path-dependent

Silent with the fix, immediate without it. A validator that has never fired proves nothing.

**Measured so far:** 20 successive moves with `ReuseTree` on and `Ponder` off produced zero
violations, and `hashfull` traced 40→44→80→115→142→70→108→139→41→…, which is the tree growing
across moves and being trimmed when the root advances.

**Collection had to be moved off the critical path first.** `gc()` is O(tree): measured at 100 ms
on 410k nodes, rising to 442 ms on 5.1M. Worse, the search clock started *after* it, so the
search then ran its full allocation on top — a 1200 ms move actually took 1642 ms, and the
overshoot grew with the tree. That is a way to lose on time, and it was the real blocker on
enabling reuse. Three changes fixed it:

- The collection is charged to the move's budget rather than added to it.
- It runs only when occupancy reaches `GcThreshold`, since it reclaims memory and nothing else.
- It runs **after `bestmove`**, on the opponent's time, re-rooting onto the move just played
  first — collecting before the root advances frees almost nothing, because everything is still
  reachable from the current root. The garbage is the sibling subtrees of the move not played.

| | before | after |
|---|---|---|
| collection on the critical path | 100–442 ms every move | 0.0–0.4 ms |
| move time against a 1200 ms budget | ~1642 ms | 1187–1189 ms |

The post-move collection is safe against a concurrent command because `searchFlag` is still set
when it runs: `new_game()`, `set_position()` and `stop()` all wait for it to clear.

**That was not enough, and a live session said so.** With `Ponder` on by default the background
collection is disabled (the next `go` arrives immediately and would abort it), so collection went
back onto the critical path. Across one session of two bots: **354 s of collection time per bot
over 384 collections**, 102 of them over a second and the worst **36.6 s**. That worst one freed
about 8.9 million nodes, and freeing is the whole cost — 8.9 million `delete` calls plus a
`delete[]` per Edge array, on a machine already in swap with two bots holding ~1 GB of tree each.
The cost tracks how many nodes **die**, not how big the tree is, so it spikes exactly when the
game leaves the reused subtree.

**The fix is to defer the frees.** The sweep was doing two jobs and only one is urgent. Unlinking
a dead node from `search.tree` is urgent, because `make_child()` finds nodes by hash lookup and
must not hand it back. Freeing it is not urgent at all. So the sweep now only unlinks, and a
background thread frees the corpses *while the search runs*. This is safe because a node reaches
that thread only after `search.tree.erase()`, and a **completed** mark guarantees no surviving
node points at a dead one — an aborted mark still sweeps nothing. `tree_occupancy()` counts nodes
awaiting reclamation, so the engine cannot expand into memory it has not got back.

| two engines, `Hash` 1024, 44 plies | before | with deferred frees |
|---|---|---|
| total time in the collect block | 56.6 s | **18.9 s** |
| worst single collection | 2420 ms | 1258 ms |
| collections over 1 s | 14 | 3 |

The same test at `Hash` 512 with a *single* engine showed nothing (6.68 s vs 7.50 s). That is the
honest result and worth keeping: without memory pressure and large die-offs the frees are not the
bottleneck. Run-to-run spread is around 30%, so differences smaller than about 1.5x from this
harness mean nothing.

**What remained was the map itself.** With the frees deferred, an overnight run still showed a
collection that dropped 8.1M nodes spending **15.8 seconds** in the unlink walk against 229 ms of
marking — roughly 2 microseconds per erase. `std::unordered_map` allocates a hash node per entry,
so every `erase()` is an allocator free of a small block scattered across a gigabyte of live tree.
`search.tree` is now `NodeMap`, an open-addressed table: one contiguous slot array, index from the
low bits of the Zobrist key, linear probing, no per-entry allocation. (It has since stopped
storing pointers and stopped having an `erase()` at all — see *The node arena* below.)

**The first attempt at it was slower, for a reason worth remembering.** Open addressing iterates in
O(*capacity*); `std::unordered_map` iterates in O(*size*), because libc++ threads every element
onto a linked list and never pays for empty space. After a large die-off the flat table still held
~16M slots for 1.6M live entries, and every later sweep rescanned all of them — a collection that
freed *nothing* walked 1.6M live entries in 551 ms, where `unordered_map` walked 4.0M in 390 ms.
Per slot the flat table was about three times quicker; it was simply scanning ten times as many.
`NodeMap::compact()` rehashes down to fit the survivors after each sweep and is cheap precisely
because there is no per-entry allocation. (Tombstones are gone with `erase()`; what `compact()`
drops now are entries whose slot has been retired.)

| `bench_map`, identical keys and erasures | `unordered_map` | `NodeMap` |
|---|---|---|
| 2M entries, 80% killed — sweep | 982 ms | **265 ms** |
| 2M entries, 20% killed — sweep | 859 ms | **239 ms** |
| 4M entries, 85% killed — sweep | 1337 ms | **339 ms** |
| 4M entries — `find` + `emplace` | 2167 ms | **978 ms** |

The fill row matters beyond collection: `make_child()` does millions of lookups per search, and
they become a short probe over adjacent cache lines instead of a chain walk through scattered
allocations.

**A third measurement trap, and the most expensive one.** A game-level A/B *cannot* compare two
collector implementations. Two engines started from the same position diverge into different games,
so they perform different numbers of collections over different tree sizes; the run that first
appeared to show the flat table losing had simply done nearly twice the collections and freed 1.5x
the nodes. Two things fix this. `bench_map.cpp` replays `gc()`'s inner loop against both tables
with identical keys and identical erasures. And in-game, compare **rates** rather than totals,
since those divergence cannot distort:

| in-game, normalised | `unordered_map` | `NodeMap` |
|---|---|---|
| per node scanned | 151 ns | **62 ns** |
| per node freed | 336 ns | **189 ns** |
| mark, per live node | 61 ns | 56 ns |

**Two further measurement traps this exposed.** First, `info string gc took N ms` was timing `set_root()`
and `validate_tree()` as well as the collection — a run in which `gc()` never executed still
reported 700 ms per move under that label, because `ValidateTree` was on. It now reports
`set_root` separately and says whether a collection actually ran; `set_root` turns out to be
negligible. Second, counting those log lines counts *searches*, not collections, because the line
is printed whether or not anything was collected.

**Measured: 9.5/12, about +232 Elo.** Head to head, same binary on both sides, `ReuseTree` the
only difference. At 12 games that is roughly 2.5-3.0 standard errors depending on the
win/draw split, so p is around 0.001-0.005 — significant because the margin is large, not
because the sample is.

Two things inflated it slightly, together worth perhaps 15-25 Elo. **The first has since been
fixed**, so a re-run would be cleaner:

- `cleanup()` cost the no-reuse side 64-89 ms per move, charged to its budget, while the reuse
  side deferred the same class of work to the background — about 8% less search on one side.
  Both configurations now defer to the same idle window after `bestmove`, and the no-reuse
  side's pre-search cost fell to 0.2-1.2 ms.
- In local self-play the reuse side's background collector competes for CPU with the opponent's
  search, which it would not do against a remote opponent on an idle machine.

Even generously discounted the effect is large, and the node measurements say why: the root's
accumulated search averaged 1.88x and reached 4.6x by move 14. For an engine this wide and
shallow (seldepth 12-16), starting from an already-deep tree changes the search's shape rather
than just its size.

**What is still not known** is how much of this survives against a *different* opponent. Both
sides here were creatica, so reuse was measured against an engine that discards its tree every
move; a stronger opponent may not leave the same room.
 The root's visit count averaged 1.88x higher with reuse on,
rising above 4x by move 14 as the tree accumulated and collapsing to 0.61x at move 16 when the
opponent played into an unexplored line. But `nodes` reports `search.root->N`, which with reuse
*includes visits inherited from earlier searches* — so that figure means "this position has had
1.88x more cumulative search behind it", not "the engine searched 1.88x faster". It also makes
the reported `nps` wrong while reuse is on, since it divides inherited visits by this search's
elapsed time. Strength has not been measured at all.


### Why a won endgame drew, and what actually caused it

A rook-and-knight endgame two pawns up was drawn on lichess. The engine's own log showed its
repetition filter discarding its chosen move on six of ten moves, so the filter looked like the
culprit. It was not, and the diagnosis is worth recording because two plausible readings of the
log turned out to be wrong.

**Wrong reading one: the evaluation collapsed.** The `skipping move` lines showed cp falling from
332 to 15 across the sequence. Those are the stale one-ply `cp` fields of the successive *fallback
moves*, not the engine's opinion of the position. The search's own value held at Q ≈ 0.50
throughout — about +3.35 pawns at `EvalScale` 6.1. The engine never thought it was losing its
advantage.

**Wrong reading two: the two repetition tests disagreed about the hash.** They do not. The
descent's `sim_zh.hash` and the node's cached `child->hash` are the same Zobrist key over the same
components, and the tree map is keyed by it.

**What was actually happening.** `N` and `W` are lifetime totals that nothing resets, so under
`ReuseTree` a node keeps them for the whole game. The proof is in the log: one child carries a
bit-identical `W` of 660667.106507 at move 56 and again at move 62, six moves apart. Those
statistics were banked while that node was itself the ponder root, immediately after the engine
played that very move — which is exactly why the inflated nodes are the same set of positions that
are now in the repetition history. `select_best_moves()` ranks by visits, so those frozen nodes
sat at the top of the list every move, the filter deleted the top of a ranking history had already
decided, and with no floor on what it landed on it once took a move holding 2,223 in accumulated
value over one holding 907,491.

**And several of those searches barely ran.** Collections in that game cost 1.5 to 2.25 seconds —
one took 1,810 ms and left 97 nodes — and since the per-move latency change that time comes out of
the move's own clock. The cause was a one-line defect in the eviction budget: `keep_visits` is
`root_visits / 1000`, which is zero whenever a root has under a thousand visits, and
`node->N < keep_visits` on an unsigned `N` is never true against zero. The soft budget switched
itself off and the collector fell back to plain reachability marking — the exact behaviour the
eviction was written to replace — so the tree only ever grew, reaching 14.3 million nodes.

**A fifth cause that has nothing to do with repetitions.** In that endgame every root child sat at
Q ≈ 0.50. The search rated every legal move the same, so it had no opinion for the filter to
override and a one-ply hash lookup was effectively choosing the move. Being two pawns up and
unable to distinguish a winning attempt from a shuffle is a conversion problem, and none of the
repairs below touch it.

**What was repaired:** the eviction floor; a visit floor on the filter; the reported score reading
the search's value instead of the stale one-ply `cp`; the filter's winning guard reading Q instead
of raw `W`; and the repetition history learning to count. **What was not:** the fossil ranking
itself, which is what `EdgeVisits` addresses and which remains unmeasured.

### The reported score, and why it could agree to a draw

`node->cp` is a one-ply value written once at expansion and never refreshed, and it was what the
engine reported as `score cp`. On the same position, old build against new:

| | reported score, top five moves |
|---|---|
| before | 55, 49, 44, 125, 62 |
| after | **325, 325, 324, 324, 316** |

The search's Q sat at about 0.50 throughout, so +3.2 pawns is what the engine actually believed and
the old figures were an estimate frozen millions of simulations earlier. This is not only a display
problem: `lichess_bot.cpp` accepts a draw offer when the reported score is below `DRAW_CP`, 30
centipawns, and in the drawn game the stale figures on some moves read 15, 20 and 26. Mate and
tablebase scores are still reported exactly rather than pushed through `atanh`.

### The online tablebase probe: from blocking to concurrent

Local Syzygy covers five pieces; lichess covers seven. Every six- and seven-piece endgame therefore
went to `tablebase.lichess.ovh` over HTTP — **synchronously, on the move's own clock**, and on
failure the engine then searched anyway, so a timed-out probe cost its timeout *and* a full-length
search. One self-play session logged 26 timeouts in one engine and 8 in the other at three seconds
each, in games with a 120 second base clock, and one was lost on time.

**Gating the probe on having spare clock was tried and rejected.** Probes only happen in endgames,
and endgames are exactly where the clock is already tight, so a rule that skips the probe below
some threshold disables it precisely when it fires most often. The asymmetry decides it: the
tablebase returns perfect play, while the search in a simplified endgame rates every move the same.

**The probe now runs beside the search.** The search starts immediately with its full allocation
and never waits; if an answer arrives first it replaces the search's move and the search is stopped
early, so a successful probe *saves* time. A late answer is discarded by move id. Measured: 396,472
simulations completed during the 883 ms one probe was in flight.

Three things make the answers arrive in time. The curl handle is **reused**, so a warm probe is one
round trip — 379 to 459 ms, against 3,000 ms timeouts before. Answers are **cached per position**
and cleared per game. And after our own move the engine **prefetches one move ahead** on the
opponent's clock, but only when the tablebase leaves them a single best reply: on a tie it stops
rather than spending requests on a guess, because that endpoint rate-limits and two bots on one
machine share an IP.

**The remaining limit.** On a very tight clock the allocation can collapse to 100 ms, and the
search then finishes before any probe can answer. The prefetch closes that only when the opponent's
reply was forced.

**The invariant, and how it was broken.** A probe answer carries the id of the move that asked for
it, and the bestmove site uses it only when that id still matches. The guard was written for a late
reply arriving after its own move had passed — and it missed the easier case, a move that never
asked at all. `want` was set inside the tablebase branch and cleared nowhere, so once a game ended
in a six- or seven-piece ending whose probe had answered, `want` and `have` stayed equal for the
life of the process. The next game skips that branch entirely, thirty pieces on the board, but the
bestmove site saw a matching pair and substituted the **previous game's** tablebase move. In a real
game the engine posted `f3f4` into an opening position with f3 empty, and lichess replied
`Piece on f3 cannot move to f4`.

The reset now lives at the top of `run_go()` rather than in the branch, because the branch is
exactly the code that does not run in the failing case — a reset placed there could never have
fixed it. A `want` of 0 means "no answer applies to this move", which is what every non-tablebase
move must say.

**The regression test is a sequence, not a position**, which is why nothing caught it: every test
stayed inside one regime. Search a six-piece ending until its probe answers, then search an
unrelated thirty-piece position, and check the second move is legal in the second position. It runs
in about sixteen seconds:

```
position fen 8/8/8/1k6/8/8/2PPPP2/4K3 w - - 0 1
go wtime 180000 btime 180000 winc 3000 binc 3000
position fen rnbqkb1r/pp2pppp/5n2/3p4/2PP4/2N5/PP3PPP/R1BQKBNR b KQkq - 2 5
go wtime 180000 btime 180000 winc 3000 binc 3000
```

Broken, both searches answer `e2e4`. Fixed, the second answers a legal Black move.

### Engine crashes in local matches

**What was seen.** In five GUI matches played between 13 and 15 September, an engine exited in the middle
of a search 35 times in 652 games. The GUI records each one as a loss with the termination
`engine error: <engine> exited during search`.

| match | games | crash forfeits |
|---|---|---|
| 13 Sep 12:53, default net against `pi` | 100 | 4 |
| 14 Sep 00:16, first `pisp` against `pi` | 100 | 7 |
| 14 Sep 11:00, second `pisp` against `pi` | 100 | 3 |
| 14 Sep 14:48, third `pisp` against `pi` | 200 | 10 |
| 15 Sep 09:22, `pisp2` against `pisp` | 152 | 11 |

Every crash happened in the first four plies of a game, that is, in each engine's first or second search.

**The cause.** Until 15 September, libchess's `fen2board()` did more than fill the `Board` it was given.
It also rebuilt three process-wide tables from the position it had just parsed: `CastlingPath`, which
says which squares castling needs empty and unattacked, and `CastlingRights[64]` and `CastlingRooks[64]`,
the per-square masks that `do_move()`, `do_move_dp()` and `ff_move()` apply to clear castling rights when
a king or rook moves. So parsing one FEN changed the castling rules for every board in the process.

The engine's online tablebase worker is a separate thread. After our own move in a six- or seven-piece
ending it prefetches the position one move ahead, and to do that it called `fen2board()` on the endgame
position. The GUI starts the next game at once, and the rate-limited tablebase request often answers a
little later. So the worker parsed an endgame, with no castling rights, while the search of the next
game's opening was running. From that moment the search generated castling through pieces and did not
clear castling rights when a king moved. The boards became invalid, and the search crashed reading them.
This also explains why only the start of a game was hit: in the endgame itself there are no castling
rights for the wrong tables to spoil.

**How it was confirmed.** One match log shows 1,645 prefetch attempts. A diagnostic build that calls
`fen2board()` on an endgame FEN from a background thread every 2 ms crashed in 15 of its first 16
searches, with the same crash locations as the match (`kingMoves()` and the NNUE evaluation). The same
build without that thread ran 460 searches without a crash. After the fix below, that same diagnostic
binary, still calling `fen2board()` every 2 ms but now loading the rebuilt library, ran 100 searches
without a crash.

**The fix, in two places.**

1. **libchess has no global castling tables any more.** `castlingMoves()` computes the path from the
   board's own king square and `board.castlingRooks`, and the three move functions clear rights from the
   piece that moved: a king move ends both of its side's rights, and a move from or onto a castling rook's
   square ends that rook's right. `fen2board()` now only fills its `Board`, so it is safe on any thread.
   Checked with every perft suite (the standard suite to depth 5; all three Chess960 suites to depth 4;
   `--check --make all` with `reconcile()` on every suite, including 10,800 runs on the Chess960
   middlegame suite at depth 4) and `test_pos test_fen_strings`: no failures.
   Every engine binary loads `libchess.dylib` when it starts, so **an engine built before the fix gets
   it too**, without being rebuilt.
2. **The worker no longer parses anything.** The thread that asks for the prefetch hands over the
   `Board` it already has, and the worker copies it.

**The prefetch had never worked.** The same code checked the result of `uci2move_idx()` as if it were an
error code. It is the move's index, so the check failed for every real move and the worker gave up
before fetching anything: 1,645 attempts in one match, none fetched. Now the move is checked for
legality instead. Tested on two seven-piece endings where the opponent has one best reply: the log shows
`prefetched the position after e8d7 -- cached`. An engine built before 15 September still has this
defect, so its prefetch does nothing. In 17 lichess games afterwards, all 26 positions where the opponent
had a single best reply were prefetched.

**Refused moves.** An old log contained `play() error: refusing illegal move e5d4` and then `e5d4` and
`f8b4` refused together, on a board where neither move was legal. That engine was built before 14
September, and no driver in this repository sends a move list to an analysis engine, so the sender could
not be identified afterwards. Two changes make the next occurrence both safe and traceable. The engine
now stops at the first refused move in a `position` command, instead of applying the remaining moves to
a board the driver does not have, with the wrong side to move, where one of them could happen to be legal.
And it logs the whole `position` command and its process id, because several engines usually append to
the same `creatica.log`.

**A stale library, found on the way.** `libchess.dylib` had last been built on 10 September at 16:15.
Two minutes later, commit `a4b9804` raised `MAX_UCI_OPTION_SPIN_NUM` from 16 to 32, which changes the
layout of `struct Engine`. Tools that drive an external engine through the library's `engine.cpp`
(`lichess_bot`, `tournament`, `test_pos`) were built after that change, so they and the library disagreed
about where the fields of that struct are. What that did in practice was not investigated. The engine
itself was not affected, because it keeps its own `Engine` and never passes it to the library. The
library, those three tools, `gen_targets` and `self-play-optimization` are now rebuilt. The older
`tournament_*` variants and `eval_saved_kan` were built before the change and now disagree with the
library the other way, so rebuild them before using them.

**Reading older match results.** Any match played before 15 September with engines that probe the
online tablebase can contain these forfeits. Count the `exited during search` terminations in the PGN,
note which side lost each one, and remove those games before computing a score.

### The repetition history learns to count

`position_history` was an `unordered_set`, which can answer "has this position occurred" but never
"how many times", so the engine could not distinguish a first recurrence from a third or reason
about whether a repetition was being forced on it. It is now an `unordered_map<uint64_t,int>`.

**Keyed by the full hash, side to move included.** A turn-neutral key would be wrong twice over. It
overcounts — a board occurring twice with White to move and once with Black reads as three
occurrences when neither position has occurred three times — and it overcounts exactly where it
hurts, because reaching the same board with the *opposite* side to move is **triangulation**, the
standard winning method in these endgames. An engine that scores its own triangulation as a
repetition will refuse the move that wins.

**Two ledgers, one map.** Because the hash carries side to move, the positions each side faces are
disjoint key sets. `rep_count(h)` asks about the opponent's ledger — they are the side who would
complete a cycle on their own move. `rep_count_flipped(h)`, one XOR away via `z.blackMove`, asks the
same question of the other parity; it is a tempo signal rather than a draw signal and is kept as a
diagnostic only.

**One trap that counting introduced.** `set_position()` replays the whole move list on every
`position` command. That was harmless for a set, where re-inserting is a no-op, but with counts it
would add the entire game prefix again every move — after ten moves a position played once would
read as ten occurrences. The history is now cleared and rebuilt unconditionally.

**And the initial position is counted**, which it never was: `play()` only recorded positions after
a move, so a game that manoeuvred back to its own starting position had, by the engine's reckoning,
never been there.

### What a full tree does, and what collection is worth

Two defects that only tree reuse makes reachable, both found in live bot-vs-bot games and both
now fixed.

**A full tree used to end the search.** `hash_full < 1000` was a condition of the workers' main
loop, so when the tree filled every worker exited and the engine played whatever it had — in a
3+2 game it spent 2 s on a move whose neighbours took 4–6. It did not stop *expanding*, it
stopped *thinking*. Before reuse this never fired, because the tree was rebuilt every move and
came nowhere near the ceiling. The ceiling now gates expansion inside `mcts_search()` instead:
a full tree keeps visiting and backpropagating over the nodes it has, which is degraded but
enormously better than abandoning the search.

**Collection is often worthless, and it is not cheap.** Measured over twelve consecutive
collections in a live game:

| tree | freed | cost |
|---|---|---|
| 9.17M nodes | **0.0%** | 867 ms |
| 9.82M nodes | **0.0%** | 760 ms |
| 8.31M nodes | 38.5% | 1161 ms |

Six of the twelve freed nothing at all. That is what reuse does to a mark-and-sweep: the cost
is O(tree) but the yield is O(garbage), and a good prior concentrates the search on the move it
then plays, so the sibling subtrees that become garbage are nearly empty.

**Two guards, and only one of them still exists.** A guard that skipped a collection whenever the
*previous* one reclaimed less than 5% was tried in both places collection runs. It survives only in
the background collection after `bestmove`, which runs only with `Ponder` off. It was **removed**
from the collection before a search, because it predicted this collection's yield from the last
one's: a collection skipped on that basis kept the tree full, a full tree blocks expansion, and in a
live game the engine then gave `f5f3` 98% of 38M visits without ever expanding its refutation.

**The exact skip, which replaces it before a search.** A collection frees only nodes its mark cannot
reach from the root, so it can free something only if what is reachable has changed since the last
collection that completed. The skip applies when **(1)** a collection completed from some root R,
**(2)** no expansion has been abandoned since, and **(3)** the current root is R, or R is reachable from
it within four plies (a capped search of at most 60,000 nodes). Then every live node is reachable from the current root and the collection would free
exactly zero, so skipping it leaves the tree exactly as collecting would have. It is not a
prediction. It rests on edges only ever being *added* to live nodes, which holds today because
eviction is commented out; any code that clears or re-points an edge must invalidate the test.

It exists because of a game lost on time. With `Ponder` on, `go ponder` searches the position after
the engine's own move, and the driver answers with `stop` and a fresh `go` one ply lower — so the
root moves on every move, and in a shuffling endgame it moves to a position from which the old root
is two or three plies away. In that game 57 collections ran on the engine's clock for 68.4 s, and 42
of them freed nothing, 56.8 s in all; late on each took 1.3–2.8 s and left the search 2–3 ms. 

**Why allocations do not break it.** The first version required that not one node had been
allocated since the collection, which covered only the ten collections in that game that followed a
ponder search stopped before it simulated (14.5 s). What actually matters is not allocation but
*orphaning*: a node that is allocated and then attached to nothing. That can only happen when an
expansion is abandoned after children may already exist. `eval_and_expand()` has three exits —
checkmate and stalemate, which return before any child is evaluated, and `expand_node()` — so every
abandon point is inside `expand_node()`: the child table's pool running dry, `make_child()` failing
part way through, and a lost publication race (which should orphan nothing but is counted anyway), plus
`process_check()` failing to get a node. Each increments a counter, and condition (2) requires it
unchanged. With no abandon, every node allocated since the collection hangs under a node that was
reachable from the search's root, edges are never removed, so R still reaches every live node — and if
the current root reaches R, it reaches them all.

Verified with `CREATICA_VERIFY_FUTILE`: 369 futile verdicts across a shuffling endgame and the opening,
125 of them after searches that allocated in between (4.8M simulations over those intervals), every
one freeing exactly 0 nodes, with no validator violations. In the opening the old root is often
reachable too, because a minor piece stepping away and back returns to it in three plies.

**A crash found on the way, and fixed.** `process_check()` dereferenced the result of `make_child()`
without checking it, and `make_child()` returns null when the arena is exhausted. The occupancy gate
keeps that from happening in practice, but it would have been a crash mid-game. It now scores the
check as 0.0 and counts an abandon.

`CREATICA_VERIFY_FUTILE=1` checks it: whenever the test says "free nothing" the engine collects
anyway and logs what was freed, with `THE TEST IS WRONG` if that is more than zero. In a shuffling
endgame test, 42 verdicts all freed 0 nodes, while collections the test did not call futile freed
millions. Skips are logged as `collect SKIPPED-EXACT, …` so they can be told from threshold skips.

**Why a mark gets slower through a game.** In the same lost game the mark cost 17.5 ns per node at
the first collection and 125–148 ns by the end, at similar tree sizes. Two causes, both measured:

- **The endgame tree is a dense graph.** Counted directly with the mark benchmark: opening trees have
  1.07–1.17 edges per node, with 7–15% of edges leading to a node already marked by another path;
  shuffling-endgame trees have 2.0–2.9 edges per node, with 50–66% leading to one already marked. The
  lost game's logged occupancy implies about 3.4 by its end. The mark handles every edge, so an endgame
  node costs two to three times the work of an opening node.
- **Idle pages are compressed, and the mark decompresses them.** On an 8.7M-node, 17.4M-edge endgame
  tree the mark costs about 10.5 ns per node-plus-edge when its pages are in RAM, even with the CPU
  caches flushed before every run. The run right after another process allocated 4 GB took 67.5 ns
  per element, with 463,243 decompressions during that single run; the run after it was back to 11 ns.
  Between collections the search touches only the hot region near the root, so on a memory-pressed
  machine the rest of a large tree goes idle and is compressed, and every collection pays to bring it
  back. The bot's log has no memory counters, so this is shown to produce a slowdown of the lost
  game's size, not proven to be what happened in that game.

CPU-cache prefetching in the mark was tried and does not help: 10–15% slower whenever the pages are
resident.

`CREATICA_MARK_BENCH=<rounds>` (with `CREATICA_MARK_BENCH_EVERY=<n>`) runs the mark alone on the current
tree before a search, flushing CPU caches between runs, and logs per-run cost with the system's
decompression and page-in counts. `CREATICA_GC_LOCALITY=1` logs, per collection, how often the mark
jumps more than 2 MB between consecutive nodes and child arrays. Both are macOS-only diagnostics.

**The child table: what the mark reads now.** The fix for the compression cost is to make the mark
read far less memory. It used to read each reachable node's 64-byte line and every child's 24-byte
`Edge`, about 1.7 GB for an 11.6M-node, 39M-edge tree. It now reads only three arrays of its own:
`kids[slot]` (8 bytes: where a node's children sit in a pool, and how many there are), the pool of
4-byte child slot numbers, and the existing 4-byte mark stamps. A page is decompressed if any byte of it
is read, so the saving only counts because these arrays live in pages of their own rather than beside
the nodes and edges.

To keep the table from *adding* memory, the 8-byte child pointer was removed from `Edge` (24 → 16 bytes),
and every read of a child now goes through `child_at(parent, i)`, using a copy of the pool position kept
in the node's spare bytes so the search pays no extra memory access for it. `NODE_BYTES` (96) and
`EDGE_BYTES` (20) count the table's entries against `Hash`, which leaves capacity about where it was:
roughly 3% fewer nodes for a tree-like middlegame and 3% more for a transposition-dense endgame.
Blocks are recycled by exact size through the reaper, and a new game resets the pool.

**That capacity claim was false in the first builds, and it cost a lichess game.** `pool_off` was
declared after `children`, where there is no padding, so `MCTSNode` grew from 64 to 128 bytes and
`NODE_BYTES` from 96 to 160. At `Hash 2048` a full tree held about 11.7 million nodes instead of about
18.7 million. In game `QHWLWAbv` (14 September, `pisp`, 4 threads) the tree filled at move 26, during
a run of moves with no capture or pawn move, so every node stayed reachable and the collector freed
nothing. From move 28 nearly every simulation was hollow. At move 32 every root move had exactly one
visit, and the engine played the highest-prior move, 32.Qxh6, which lost the queen to ...Qxh6. The
games that day hit a full tree 6 to 37 times each, against 0 to 7 for the build before the child
table. `pool_off` now sits in the four spare bytes after `descents`, and a `static_assert` pins the
node at 64 bytes. Replaying that game with its clocks and pondering on the fixed build reached move 32
at 986 permille with no hollow simulations; in the same replay the unfixed build froze again for several moves.

Measured on a 9.5M-node, 19.3M-edge endgame tree with 6 GB of memory pressure applied before every
run: the old pointer traversal cost 18.9–20.7 ns per node+edge with about 49,000 decompressions per
run; the child table cost 12.6–12.8 ns with about 18,600 — about 38% cheaper and 62% fewer
decompressions. With the pages already in RAM the two cost the same (9.8 against 10.1 ns), which is
expected: both make the same number of random accesses, and the table only shrinks how much memory
those accesses span. The search pays for it too: single-threaded node rate fell 0.5–1.0% on three
positions, close to the noise but consistently in the same direction.

Verified before the pointer was removed, by checking the table against it on every collection (zero
mismatches over 20M nodes and 27M edges), and after, by checking that the collector's and the search's
copies agree and that every published edge leads to the position its move produces (8.5M edges across
the opening, a shuffling endgame and `PolicyMode 2`, zero wrong, zero validator violations).
`CREATICA_VERIFY_KIDS=1` runs those checks; they read the nodes, so they are a diagnostic, not a mode.

**A bug found on the way, not fixed.** `go ponder` on a position with five pieces or fewer takes the
local-tablebase path, which calls `drop_ponder()` and returns without searching and without ever
sending `bestmove`, so a `stop` gets no answer. The lichess driver cannot trigger it, since it ponders
only with more than seven pieces and does not wait for `bestmove` after `stop`, but a GUI that
ponders into a tablebase ending would hang.

**Sizing.** The tree reached 9.8M nodes on a 1 GB `Hash` in a single 3+2 game with pondering,
i.e. the ceiling. Roughly 10M nodes per GB. Size `Hash` for the whole game, not for one move.

### The node arena: why strength ran *inversely* to `Hash`, and what fixed it

By September 2026 the collector had become the engine's largest single cost, and the symptom was
the clearest possible: **playing strength ran opposite to the memory it was given.** Games at
`Hash` 1024, 2048 and 4096 were best at the smallest setting and, in Arkadi's words, "lose rapidly"
at the largest. More memory bought a slower collector, not a deeper tree.

Measured across one day of rated games — 8 games, 519 own-clock searches:

| | |
|---|---|
| search | 722.1 s |
| collection | **317.7 s — 30.6% of all own-clock thinking time** |
| collections over 1 s | 110 |
| worst single collection | 83,828 ms |

After that 84-second sweep the search that followed got **4 ms and 37 simulations**, and the move
was chosen from visit counts inherited from the previous move's tree. Two games were lost to this:
one on time, one to a blundered queen from a winning position.

**The cost was never the frees.** Those already happened on a background thread. It was the
**walk**. Nodes came from `new MCTSNode()`, millions a move, so they were scattered across the heap
in allocation order; the sweep iterates the hash map, so it visited them in *hash* order —
effectively at random across gigabytes — and had to dereference every one to read its generation
stamp. The nodes it wants are by definition the ones the search has not touched, so they are the
coldest pages in the process. Measured at **1.09 µs per entry** over 26.8M entries.

Three changes, each about how much memory the sweep has to move:

- **One flat arena of nodes**, sized from `Hash`, with a free list. Iterating by index is a
  sequential stride instead of a random walk, and a reclaimed node is a returned index rather than
  a call into the allocator.
- **The mark stamp left the node** for its own `uint32` array. Sequential is not the same as cheap:
  reading `generation` out of the node still touches a 64-byte line per slot, 358 MB for a
  5.6M-slot arena. Four bytes a slot is sixteen times less, and measures at **2.9 ns per slot**.
- **The map holds `(index, stamp)` rather than a pointer.** The sweep retires a node by zeroing its
  stamp — one store into the array it is already scanning — and every map entry pointing at that
  slot is stale from that instant. Nothing to erase, no reason to read the corpse. All the cold
  work moved to the reaper.

| Hash 2048, 4 threads, same workload | sweep | per corpse | sims/s |
|---|---|---|---|
| before | 15,926.8 ms | 2,562 ns | 56,117 |
| arena + stamps | 6,407.1 ms | 1,229 ns | 76,045 |
| + indexed map | **313.2 ms** | **54 ns** | **109,130** |

**The search got faster too, and that was not the goal.** The first cut was 27% *slower* before two
fixes: `alloc()` used a compare-exchange loop, so with node creation running at about a million a
second every thread that lost went round again; and `MCTSNode` is exactly 64 bytes but
`new MCTSNode[n]` only guarantees `alignof` (8), so every node straddled two cache lines.
`fetch_add` and `alignas(64)` turned it into 1.9x faster.

**The mark then became the whole remaining cost, and a binary heap was most of it.** The mark walked
the tree through a `std::priority_queue` ordered by visit count — so both settings of the old
`GcBestFirst` paid O(log n) with a sift on every push and pop, tens of millions of times a game.
Nothing read that order: it existed for the retention budget that chose which live subtrees to
truncate, and that eviction is commented out. Replacing it with a plain FIFO over a vector measured
**74 → 14 ns per marked node**, a 5x reduction, on three runs each varying by 1.4%.

Breadth-first, not a stack, and deliberately: depth-first is the other obvious O(1) choice and
measured 672 ns per node. Nodes now live in one arena in creation order, so a level's nodes sit near
each other in memory and a level-by-level walk keeps that locality; a stack abandons it.

**What deliberately did not change is *which* nodes die.** Only nodes unreachable from the root are
reclaimed. That property is load-bearing rather than incidental: search threads hold raw
`MCTSNode *` for a whole simulation — down the tree, through a ~30-evaluation expansion, and back up
writing results into every node on the path — and that is safe *precisely because* a node on a live
thread's path is reachable, so the mark reaches it and the sweep never frees it. A
transposition-table-style arena that overwrote the least valuable entry would let one thread
overwrite a node another is standing on, and the second thread's result would land in an unrelated
position's statistics, silently. Nothing live is ever recycled, so the pointers stay raw.

**A measurement discipline this cost a day to learn.** Under load — a training job running, load
average 9.78, 20% free memory — the same binary measured anywhere between 304 and 672 ns per marked
node, and that spread was read as a real difference between two variants. It was memory pressure.
On an idle machine the same measurement repeats to within 1.4%. **Timings taken while something
else has the machine are not evidence**, and the collector figures in live bot-vs-bot games are
inflated by an order of magnitude if both engines are sized to fill the machine.

**What this means for `GcThreshold`.** Its job has changed. It existed to ration an expensive
operation; the operation is now roughly 100 ms at `Hash` 1024. What is left is keeping occupancy
below the 1000 per-mille ceiling where expansion stops, and 700 is tuned almost exactly to that:
measured over 1,301 searches, occupancy climbs by a median of 53 per-mille during one search, p90
133 and p99 **299** — and 1000 − 299 = 701. Raising it is therefore wrong; roughly one search in ten
would overshoot. Lowering it is now affordable for the first time, and lower is what keeps the tree
away from the ceiling.

**The arena is sized by what the budget can hold, not by the slot size** (16 September 2026). The arena
used to have one 64-byte slot per 64 bytes of `Hash`, as though a node cost only its slot. It costs
`NODE_BYTES` (96) plus at least one edge, so no more than about `Hash`/112 nodes can be alive at once, and
in 17 lichess games at `Hash` 2048 the largest tree ever reached was 18.3 million nodes — `Hash`/117. The
other 45% of the slots could never be used, and all of them are written when the arena is created, which is
why a full tree cost about 1.56 × `Hash`. A slot is now worth `ARENA_BYTES_PER_SLOT` (100) bytes of `Hash`,
which is about 17% more slots than the largest tree measured; the child-table pool, whose ceiling was also
unreachable, went from `Hash`/16 to `Hash`/40 entries. In the same change `Edge`'s prior became a float, so
an edge is 12 bytes and its charge against `Hash` 16 rather than 20 — the same `Hash` therefore holds more
tree.

The margin matters because slots freed by a collection return through the background sweep and the reaper,
so a slot can briefly be neither live nor available. Measured: in the 17 lichess games the reaper was never
behind at a collection (0 nodes in all 384 of them); in self-play games of 60 and 100 moves at `Hash` 2048
and 512, and in 20-second single searches, the arena never ran dry. If it ever does, the expansion is
abandoned and the node stays a leaf, which is how a full tree already behaves.

What it bought, and what it cost:

| | before | after |
|---|---|---|
| memory, full tree at `Hash` 2048 | 3,508 MB | 2,622 MB |
| nodes in a 60-move self-game at `Hash` 2048 | 13.5 million | 15.8 million |
| simulations a second, three alternating 20 s fills | 304,000 | 308,000 |
| mistakes test set, solved of 139 | 57 | 61 |

Validated with `ValidateTree` and `CREATICA_VERIFY_KIDS` over a self-game: no tree violations and no
child-table errors across 19 million published edges.

### A full tree that cannot be collected, and the tree reset

**The failure.** In game `QHWLWAbv` (14 September) the engine lost its queen with 32.Qxh6. The tree
had filled at move 26, and moves 26–31 contained no capture and no pawn move. From the current
position every earlier root could be reached again by moving pieces back, so every node stayed
reachable and the collector freed nothing. With the tree full, every simulation was hollow: it could
expand nothing and taught nothing. Then Black played 31...Ke8, which the ponder search had ranked
18th of 29 and given 16 informed simulations. Those 16 were all the new root inherited, the full tree
let the next search add none in 12.2 million simulations, every root move finished at one visit, and
the engine played the move with the highest prior. (The node-size bug described under *The node
arena* made the tree fill much sooner that day; this section is about what happens once it is full.)

**Freeing what the search cannot reach does not help.** The search stops at any position that has
already occurred in the game, while the mark walks straight through such positions, so it looked as
if the collector was keeping dead subtrees. A probe that repeated the mark but stopped at game
positions found **99–100% of a full tree reachable without passing through any of them**, at
`Hash` 2048 and at 1024, in replays of that game. The full tree is genuinely reachable along paths
the search can take, so only discarding reachable nodes can relieve it. The probe was removed.

**What decides a freeze, measured over 22 games** from the visit dump and the engine log:

- Searches that started with the tree **under 950 per-mille: 0 of 1,539 froze.**
- Searches that started at **950 or more: about half ended at least 90% hollow**, whether the
  opponent had played the predicted move or a surprise.
- What the root inherited decides how much the freeze costs. Across the 189 searches that started
  at 950 or more, the move chosen had a median of over 100,000 informed simulations behind it; only
  7 had fewer than 1,000, and 3 fewer than 100.

**A second defect made every large collection block the search after it.** Once the sweep stopped
reading corpses, nothing took retired nodes off the occupancy figures until the reaper reached them:
`total_nodes` was resynced from `arena.live()`, which still counts retired slots, and
`total_children` fell only as the reaper freed each edge array. In `QHWLWAbv` a collection retired
11.75 million nodes and the next search reported 618,205 nodes at 928 per-mille and ran 94% hollow.
The sweep now counts the retired nodes and reads their child counts from `kids[]` (a separate array,
so it still never touches a corpse), takes both off before the search starts, and a `retired_nodes`
counter carries the nodes until the reaper releases them. In the replay, the search after the capture
now runs at 35 per-mille with no hollow simulations.

**The tree reset.** With `TreeResetBelow` set, the collection before a search checks two things: the
tree is at `TreeResetOccupancy` (950) or more, and the root's evidence — the informed simulations it
inherited — is below `TreeResetBelow`. If both hold, it retires every node, the root included,
through the ordinary sweep and reaper, and `set_root()` builds a fresh root. It is a collection whose
mark reached nothing, so it is exactly as safe as any other collection; the futility snapshot is not
published, so the next collection cannot be skipped against a root that no longer exists. The log
line reads `collect RESET`, preceded by a `tree reset:` line with the numbers.

Replaying `QHWLWAbv` with its clocks and pondering at `Hash` 1024, where even the fixed build fills:
with `TreeResetBelow 1000` the reset fired twice, on roots that had inherited 605 and 932 informed
simulations with the tree at 1002–1003 per-mille. Each retired about 9 million nodes in 228–273 ms,
charged to the search that followed, and each of those searches started at 0 per-mille with no hollow
simulations. It did **not** touch the long freeze in between, where the roots inherited more than
1,000: that is the common case above, and a reset there would throw away a lot to rescue little.

Validated by forcing a reset before every search (`TreeResetOccupancy 0`, `TreeResetBelow` at its
maximum) with `ValidateTree` on and `CREATICA_VERIFY_KIDS=1`: 38 searches, 38 resets, no invariant
violations, and none of 54 million published child entries reached the wrong position.

**Not yet measured:** whether it changes results in games. It is off by default.

**Resetting a frozen tree, measured by position.** From the logs of today's games, 340 searches ran on
a full, frozen tree (at least 90% hollow, 950 per-mille or more). Fifty of them, ten in each
piece-count band, were searched again from an empty tree for the same time, with the bot's settings,
and Stockfish (600 ms a position) scored the move each version chose:

| pieces | fresh search, simulations/s | fresh move had more search behind it | frozen move: mean loss | fresh move: mean loss | fresh better / worse / equal |
|---|---|---|---|---|---|
| ≤ 10 | 460,000 | 7 of 10 | 225 cp | 204 cp | 1 / 0 / 9 |
| 11–16 | 402,000 | 6 of 10 | 45 cp | 113 cp | 3 / 2 / 5 |
| 17–22 | 297,000 | 2 of 10 | 135 cp | 128 cp | 2 / 1 / 7 |
| 23–28 | 274,000 | 8 of 10 | 12 cp | 6 cp | 1 / 0 / 9 |
| 29–32 | 259,000 | 7 of 10 | 7 cp | 9 cp | 0 / 1 / 9 |

A fresh search is indeed faster with fewer pieces, about 1.8 times from a full board to ten pieces or
fewer. In 30 of the 50 positions it put more search behind its chosen move than the frozen search had
inherited; where it did not, the frozen root had inherited a lot (a median of 672,000 in the 17–22
band). By Stockfish the two choices were equivalent in 39 of 50 positions; the fresh move was better in
7 and worse in 4, and it lost a pawn or more 5 times against 7. Fifty positions cannot separate those,
so the measurement says a reset in a frozen position costs nothing measurable, not that it gains.

**`TreeResetHollow`** acts on that: it resets when the previous search was at least that many per-mille
hollow and the tree is at `TreeResetOccupancy`, whatever the root inherited. Replaying `QHWLWAbv` at
`Hash` 1024 with its clocks and pondering, `TreeResetHollow 900` fired 4 times and cut the searches that
were at least 90% hollow from 10 to 4 and those at least 50% hollow from 15 to 6. Each reset follows one
frozen search, since it reacts to the search before. Two of the four discarded roots that had inherited
1.6 and 3.3 million informed simulations — permitted by design, because a frozen tree would have taught
the next search nothing, but the case to watch in games.

### The mark beside the search (`CollectDuringPonder`)

With `Ponder` on, which is how the bot plays, **every search pays its collection inline**. The background
collection after a bestmove was disabled for pondering because the next `go` arrives within milliseconds
and `gc_join()` would have waited for an uninterruptible sweep. The sweep has since become a background
thread that can be stopped, so what was left was the mark — and a mark could not run beside a search.

**Why it could not, and the barrier that fixes it.** The mark walks the tree from the root and stamps what
it reaches; everything else becomes invisible when it publishes its generation. A search thread that finds
an existing node through the map — a transposition — can attach it under a node the mark has already
passed, so the mark would never reach it and the sweep would free a node that is reachable. `mark_barrier()`
stamps such a node with the mark's generation and hands it to the mark as a further starting point; a node
can be handed over at most once per collection, so the mark still terminates. The barrier is one relaxed
load when no mark is running. `mark_active` is cleared and the generation published in the same critical
section, and `make_child()` and `set_root()` re-check visibility afterwards, so a thread either hands its
node over or finds it already invisible and builds a fresh node instead of attaching a doomed one.

`CollectDuringPonder` (off by default) then starts the collection after the bestmove even when pondering,
and the ponder search neither joins nor collects inline. On a ponderhit the root is unchanged, so the
exact futility test skips the collection before the real search — which is the collection that was on our
own clock.

**Measured in a lichess game**, 180+3, `Hash` 2048, 2 threads, `pisp2` at `BlendScale 102`, both tree
resets on, the two accounts differing only in this option:

| | `CollectDuringPonder true` | off |
|---|---|---|
| collections on the engine's own clock | **0** (0 ms) | 13 (5,384 ms, worst 778 ms) |
| collections before ponder searches | 0 | 12 (6,315 ms) |
| collections in the background | 36 (15,075 ms of marking, longest 1,181 ms) | 0 |
| median simulations a search | 479,477 | 442,992 |
| `ValidateTree` violations / child-table errors | 0 / 0 | 0 / 0 |

So a single game kept 5.4 s of its own clock and searched about 8% more per move.

**The barrier is necessary, and now proven so.** In that game it handed over **3,059** nodes — nodes that
a transposition made reachable again after the mark had passed them, and which the sweep would otherwise
have freed while they were in the tree. Earlier runs showed zero handovers and therefore proved nothing:
the driver's games stayed under 1.2 million nodes with 19 ms marks, and a lichess game with the mark
running beside the *ponder* search only still showed none. The hazard appears once the mark runs beside a
real search, which is what the option does after `e86d967`.

**The crash, reproduced and fixed.** A lichess game with the option on crashed when a tree reset fired while
a post-move collection was still marking. Under AddressSanitizer, with a reset forced before every search
(`TreeResetOccupancy 0`, `TreeResetBelow` at maximum), a collection after every bestmove (`GcThreshold 0`)
and `CREATICA_MARK_DELAY_US=2000` to widen the window, the build **with the guard compiled out**
(`-DCREATICA_TEST_NO_RESET_GUARD`) dies at the first opportunity with `thread::join failed: Invalid
argument` — two collectors driving the same collector thread, which is the same collision that corrupted
the tree in the game. The guarded build runs the identical stress to completion: 30 resets beside 21
background collections, no sanitizer error. A realistic ASAN run (`GcThreshold 700`, resets at 950/900,
60 plies) also reports nothing.

Two red herrings from that hunt, recorded so they are not chased again. The ponder driver reports "no
bestmove after go ponder + stop"; the engine's own log shows the bestmove emitted immediately after the
stop, and the driver misses it because its timed wait uses `select()` on a line-buffered stream, where a
line already in Python's buffer reads as "nothing to read". The illegal-move refusals that follow are the
driver's move list desynchronising after that, not an engine fault.

**Not yet measured:** whether the extra search per move is worth Elo, which needs a session of bot-versus-bot
games on lichess, since a GUI match never ponders and the option then does nothing.

### A mate behind a check extension was backed up with the wrong sign

In lichess game `iP6FoytR` the engine had three legal moves at move 41. Two draw; the third, 41...Kf6,
allows 42.g5+ (Black's only reply is 42...hxg5) 43.hxg5#. It played Kf6, with **94% of 733,527 visits**
on it and the position scored at −11 centipawns, on a tree 14% full. It was mated two moves later.

**The cause.** A simulation that ends on a mate node used to back up

```cpp
result = scorecp == -MATE_SCORE ? -1 : 1;     // exact test against -20000
```

A mate found inside a check extension carries its depth in the score (`-MATE_SCORE * 0.01 + iter`, so
−19800 two plies down), which fails that equality and takes the `: 1` branch — **the mated side is
credited with a win**. Dumping the node one move from mate, as the tree held it:

```
h4g5   child N 170    W  +168.000  Q -0.9882  cp -19800  terminal 1   <- the mating move
f4g4   child N 11417  W  +352.191  Q -0.0308  cp     20  terminal 0   <- what it played instead
```

So the mating move scored −0.99 from the mating side's point of view, and the search spent 11,417 of
11,802 visits avoiding it. Mates found at the root's own expansion are stored as exactly −MATE_SCORE and
were always right, which is why this survived: only mates deeper than the root were inverted, and the
deeper the mate the more surely the engine ran toward it.

`terminal == 1` means the side to move is mated, in both places that set it, so the value is simply −1
from that node. After the fix the same node reads `Q +1.0000` with every visit on the mating move, the
mate-in-2 position is solved (`bestmove g4g5`), and the game position gives `bestmove e7f8`, a draw.

**What it is not visible in.** The mistakes test set is unchanged (61 of 139 both ways, 13 gained and 13
lost), and 40 positions from real games where Stockfish sees a mate in five or fewer score identically
(median 1,634 cp against 1,628). Both sets hold mates at or near the root, where the old code was
correct. The cost of the defect is in games, so an engine-versus-engine match is what would price it.

`CREATICA_DUMP_NODE=<fen>` prints a node's children as the tree holds them — move, edge visits, evidence,
W, Q, one-ply cp, terminal flag and prior — which is how this was found; no root-level output shows a
node three plies down.

### Balancing the visits: the share cap and the reply floor

**The search spends almost everything on one move.** Over 15,569 real searches of at least 10,000
simulations, taken from the bot's visit dumps: the most-visited root move holds a **median 93%** of the
root's visits, more than 80% in 68% of searches, and the top two together 98% — so about 27 legal moves
share the remaining 2%. Those visits mostly refine a value that is already decided.

**What that costs, in one position.** In the lichess game `1Gx0Osx2` the engine played 21...Qe4, refuted
by 22.Qxf7+ Kxf7 23.Ng5+ and 24.Nxe4. The priors along that line are 1.0% for the sacrifice, then 81.5%
and 99.9%, so only its first move is badly rated. Searched on its own the position after the sacrifice is
valued correctly within **2,000 simulations**; inside the search it held 17 visits after 50,000
simulations and 61 after 150,000, and took over only after about 900,000. The first visits see a queen
given away, the value drops, and PUCT stops returning. Raising `ExplorationMax` cannot fix that: against a
value gap of about 0.93 with a 1% prior, the exploration term sustains about 6 visits at today's constant
and about 17 at C = 4, where 500 are needed.

**Two rules were built and measured on the mistakes test set** (139 positions, 8 s each from an empty
tree, `Hash` 2048, 4 threads, `pisp2`). Each cell is **how many of the 139 positions were solved by that
time**, so the numbers grow left to right along a row; the last column is the median number of simulations
a position took.

| setting | solved ≤1 s | ≤2 s | ≤4 s | ≤8 s | median sims |
|---|---|---|---|---|---|
| neither rule (the plain engine) | 45 | 48 | 54 | 61 | 2.63 M |
| `VerifyReplies 500` from the start | 43 | 50 | 58 | 66 | 2.44 M |
| `VerifyReplies 2000` from the start | 31 | 41 | 54 | 66 | 2.29 M |
| `VerifyReplies 500`, `VerifyAfter 600` | 49 | 53 | 60 | 66 | 2.44 M |
| **`BalanceShare 900`** | **62** | **69** | **75** | **81** | 1.01 M |

`BalanceShare 900` gained 32 positions and lost 12 against the plain engine, which is the only margin here
beyond churn. At a game-like 2 s a position it solves 68 against 58, gaining 22 and losing 12.

### Reading `VerifyReplies` and `VerifyAfter`

The two numbers answer different questions: **how much** extra looking, and **when**.

**What "evidence" counts.** Not visits. `N` counts every time a simulation arrives at a node, including
arrivals that learned nothing — when the tree is full a simulation cannot expand, so it re-backs-up a value
already counted. `evidence` counts only the simulations that actually reached a fresh evaluation. So
`VerifyReplies 500` means *500 informed simulations*, not 500 arrivals, and a reply cannot satisfy the floor
by being bounced off a full tree five hundred times.

**Where the floor is applied.** Exactly one node per simulation. While descending, the root reports which
child currently has the most visits — the move the engine would play if the search stopped now. One level
below *that child only*, every one of the opponent's replies must reach the floor before PUCT may ignore it.
Replies to moves the engine is not about to play are untouched, and no move anywhere is ever skipped: the
floor only ever *adds* visits.

**Worked example**, from the position that lost a queen in `Gr3fzghd`. The engine is about to play `Kh8`,
which has 49 legal White replies, one of which is the refutation `Qxf8+`. With `VerifyReplies 500` the cost
is at most 49 × 500 ≈ **24,500 simulations** — about 3% of that search's 845,430 — and every one of them is
spent on an answer to the move actually being played.

**Why `VerifyAfter` exists.** Running the floor from the first simulation spends it while the engine is
still working out which moves are worth anything, and that measurably hurts: 43 positions solved within a
second against 45 for the plain engine, and 31 at `VerifyReplies 2000`. Starting at 60% leaves the opening
of the search exactly as it was and spends the tail confirming whichever move won — 49 within a second
against 45, and better at every later time too. That is why the default pairing is `500` with `600`, and it
is the only configuration in the table that beats the plain engine in all four columns.

**What the measured cost turned out to be in real games.** Nothing. Across four lichess games at 120+2 the
median simulations per search was 862,906 with the floor on against 845,346 without — slightly *up*. The
likely reason is tree reuse: the replies to the leading move usually arrive with evidence already inherited
from earlier searches, so the floor is already satisfied and never fires.

**What it has not been shown to do.** It has never been demonstrated to change a specific blunder. From an
empty tree the engine already solves the `Gr3fzghd` position 6 times out of 6 at 2 s with the floor off, so
that position can only detect harm, not benefit; reproducing the real failure needed the game's 12.9-million
node tree, which cannot be reconstructed from a move list.

**What the cap costs.** It converts depth into breadth: each simulation expands more, so fewer run. On a
middlegame position at 12 s, `Hash` 2048:

| | simulations | `seldepth` | tree filled |
|---|---|---|---|
| no cap | 2.85 M | 56 | 310‰ |
| `BalanceShare 900` | 1.24 M | 46 | 366‰ |
| `BalanceShare 700` | 0.57 M | 27 | 400‰ |

In a 60-move self-game at 2 s a move it ran 106,000 simulations a second against 318,000, with the largest
tree slightly smaller (14.1 M nodes against 16.0 M) and no frozen searches or resets either way. So it does
not cost memory, and 700 costs half the depth for no gain the test set can see.

**Measured in games, and it loses.** Two self-play matches, the same binary on both sides, 500 ms a move,
`Hash` 512, `pisp2` at `BlendScale 102`, tablebase adjudication on:

| the capped side | games | result |
|---|---|---|
| `BalanceShare 900` at every depth | 24 | 7.5–16.5, about −150 Elo |
| `BalanceShare 900`, `BalanceDepth 1` | 212 | 25 wins, 141 draws, 46 losses, **about −35 Elo (95% −62 to −8)** |

So the test set and the games disagree about this change, and the games decide. A test-set position is
searched from an empty tree, which rewards breadth; a move in a game stands on a tree the engine has been
deepening for many moves, and the concentration the cap breaks up is partly that accumulated depth being
reused. **Treat the test set as a diagnostic that explains a failure, never as evidence of strength.**

Both rules stay in the code, off by default. What survives from the work is the diagnosis: the refutation
of a losing move is usually one move deep, it needs a couple of thousand visits concentrated in it, PUCT
gives it a few dozen, and no exploration constant can close that gap — which places the problem in the
prior, not in the search's shape.

### Which reset threshold, and how much the frozen tree still costs

Measured over 21 lichess games at `Hash` 2048, 4 threads, `pisp2` at `BlendScale 102`, both resets on
(2,358 searches, median 720,802 simulations each):

- 93 searches ran on a tree at 950 per-mille or more, and **39 of those were at least half hollow** — 1.7%
  of all searches. Runs of consecutive frozen searches were never longer than two.
- The reset fired 27 times: **19 on the hollow trigger, 5 on the starvation trigger, 3 on both**. So
  `TreeResetHollow` does the work in games, while the replay work had only tested `TreeResetBelow`.
- A reset costs nothing measurable: each retired about 18.9 million nodes in **1 ms at worst**, because the
  retirement goes to the background sweep.

**900 against 500, 300 games at 500 ms, `Hash` 512, the same binary and net on both sides:** the 900 side
scored 44 wins, 221 draws and 35 losses — 154.5 to 145.5, about **+10 Elo with a 95% range of −10 to +31**.
No measurable difference, which is what the search counts predicted: lowering the threshold to 500 would
have caught the second search of each two-search frozen run, at most 14 searches of 2,358. Keep 900, and
treat the threshold as settled rather than promising.

The frozen tree itself is still worth attention — those 39 searches are ones the engine largely wasted —
but the reset is not the lever. What costs more, and is already built, is collection on the engine's own
clock: 301 collections over the same 21 games, 79.7 s in total, about 3.8 s a game.

### The sweep off the clock

A collection used to be a mark, then a sweep, both before the search on the move's own clock. At
`Hash` 2048 after the node-size fix, the sweep measured 131–819 ms a collection: 15–125 ms scanning the
node slots and **112–719 ms compacting the node map**. In lichess game `2VnUZWDh` collections before
real searches took 25 seconds of Black's two-minute clock, and one left the search 2 ms and 128
simulations for 105...Ka7.

Only the mark now runs before the search. When it completes, `gc()` publishes its generation as the
arena's *visible generation*, and the map resolves a node only if its generation is at least that
(`NodeMap::valid()`), so every node the mark did not reach is invisible at once. Nothing can hand one
out, and no survivor points at one, so nothing has to be retired before the search starts. A background
thread (`sweep_func`) then retires those slots, drops their map entries cluster by cluster under short
exclusive locks, and only then gives the slots to the reaper — a retired slot must not become a new
node while the walk still treats every entry pointing at it as stale. None of this has to finish: a
collection starting while it runs stops it, and its own sweep takes whatever was left. The occupancy
counters now come from the mark, which is exact, rather than from the sweep.

Measured on replays of `QHWLWAbv` at `Hash` 2048:

| | collection before the search, mean | worst |
|---|---|---|
| sweep on the clock | 568 ms | 912 ms |
| sweep in the background | 67–214 ms | 101–396 ms |

In a controlled benchmark — fill the tree for 30 s, play a pawn move each that leaves most of it
unreachable, search 2 s — the old build spent 266–398 ms collecting and searched 415–527 million
simulations; the new build spent under 1 ms and searched 503–579 million.

**The map is locked by range, not as a whole.** With one lock for the whole map, every step of the
background walk stopped every search thread that wanted to create a node: the search after a collection
fell to about 200,000 simulations a second, against 340,000 otherwise, for as long as the walk lasted.
Moving the walk and the reaper onto macOS's efficiency cores (`QOS_CLASS_UTILITY`) made it worse, not
better: in one of three runs the walk took 1.9 s on a slow core while holding the lock, and the search
behind it fell to 74,000 a second.

So `NodeMap` now locks itself, with 1,024 locks each covering one contiguous stretch of slots. A lookup
or insert locks the stretch its probe starts in, and the next only if the probe runs into it; the walk
locks one stretch at a time; growth and clearing lock all of them. Locks are always taken in increasing
order, so probes no longer wrap from the end of the table to the start — a run that would wrap spills
into a 1,024-slot tail instead, and one that reaches the end forces a rebuild. A rebuild is now sized
from the entries that are still valid, so a table full of stale entries is compacted rather than
doubled.

| 2 s search right after a large collection (controlled test above) | simulations |
|---|---|
| sweep on the clock | 415–527 million |
| background sweep, one map lock | 503–579 million |
| background sweep, range locks | 542–631 million |

In a replay of `QHWLWAbv` the searches that followed a collection ran at 249,000 simulations a second
against 273,000 for the rest, a 9% difference where the single lock had cost 42%.

Validated with `ValidateTree`, `CREATICA_VERIFY_KIDS` and `CREATICA_VERIFY_FUTILE` over a full replay at
`Hash` 1024 and with a reset forced before every search: no invariant violations, no exact-futility
failures, and no child-table errors — for the single-lock version and again for the range-locked one,
which ran 52 searches with every check on and 39 with a reset forced before each. `CREATICA_MAP_CLEAN=0`
skips the map walk, for measurement.

**The map keeps its size between games.** A new game used to drop the map entirely, so every game grew
it again from 1,024 slots by doubling. In 17 lichess games at `Hash` 2048 that was exactly 16 rebuilds
a game, about 600 ms a game, and each rebuild holds all 1,024 locks, so all four search threads stand
still while it runs. A new game now empties the map but keeps its size (`NodeMap::reset()`); only a
change of `Hash` drops it. Tables are allocated with `calloc` instead of `new[]`, so a new or rebuilt table
is not written slot by slot before use: the system's zeroed pages are filled as entries arrive. Measured
over two 9-move games at `Hash` 2048, three runs each:

| | rebuilds, game 1 | rebuilds, game 2 | time stalled in rebuilds, game 2 |
|---|---|---|---|
| before (`creatica_prefetch`) | 16 | 16 | 513–580 ms |
| after (`creatica_mapkeep`) | 16 | 0 | 0 |

Simulations per game did not change beyond run-to-run noise (game 2: 10.0–10.9 million after, 10.1–11.7
million before). Because the table starts a game at its full size, its memory is back in use early in
the game rather than growing with the tree; the full-tree footprint is unchanged. Validated with
`ValidateTree` over three games at `Hash` 256: no violations in 27 searches, and 13 rebuilds in game 1,
none in games 2 and 3.


## Running comparisons

Matches between two configurations are far more informative than each against a third
engine: the shared opponent's variance enters both measurements and the difference you
care about is never contested directly. Use `creatica-shared-root` to check a variant is
in the same league as what is deployed, then compare variants head to head.

The browser GUI (`python3 chess_gui.py`, Tournament tab) sets **UCI options** per side,
built from what each engine advertises, so two configurations of the same binary can be
matched with no code change. There are no environment variables any more; an earlier version
of this section said there were. `tournament.cpp` takes per-side named option lists for the
same purpose, and `self-play-optimization.cpp` sweeps one option at a time.

**Tablebase adjudication** (on by default, a checkbox in the Tournament tab; on because it keeps the
engines' online tablebase requests down, so fast matches do not run into lichess's rate limit). Once a game has 7 pieces
or fewer, the GUI asks `tablebase.lichess.ovh` for the result right after each capture or pawn move and
ends the game with it: a win or loss ends it decisively, and a draw or a win that needs more than fifty
moves (a "cursed" win) ends it drawn, because the GUI enforces the fifty-move rule. Positions with castling
rights, ambiguous answers and network failures are simply played on. The PGN's `Termination` tag says
`tablebase: ...` for these games.

**It saves almost no time.** The estimate made when it was added was that it would cut about a fifth of a
match, because 22–24% of all moves in earlier matches came after the board first reached 7 pieces. That
assumed every move costs the full 500 ms, and those moves do not. Measured over two matches at 500 ms a move:

| | games | moves a game | time a game | time a move |
|---|---|---|---|---|
| 15 Sep 09:22, played out | 152 | 137 | 56.7 s | 414 ms |
| 15 Sep 14:03, adjudicated | 300 | 113 | 56.6 s | 502 ms |

Adjudication removed 24 moves a game, and the games took just as long. The engine log for the played-out
match explains why: 15% of all its moves were answered instantly from the local 5-piece tables, with no
search at all, and many of the 6- and 7-piece moves ended early when the online tablebase answered. The
moves that adjudication removes were nearly free already.

What it gives up: with it on, the engines' own endgame play and their tablebase code are no longer tested
by local matches. In the adjudicated 300-game match the engine log shows no online tablebase answer and no
local tablebase move at all. That matters, because the crashes described in *Engine crashes in local
matches* started in exactly that code and would have stayed hidden. Turn it off for any change that touches
endgames, tablebase probing or time use late in a game; lichess games exercise that code either way.

**The mistakes test set** (`mistakes/`). Positions from the bot's own lichess games where creatica played a
move that lost at least a pawn, used to judge nets, search changes and Hash sizes where games are actually
lost rather than by average top-k accuracy. Built from 124 games of 12–15 September 2026 in three steps:

1. `scan_games.py`: Stockfish at 150 ms scores every creatica move with 8 or more pieces on the board
   (11,684 moves). 407 lost at least 100 cp; 216 of those came when the game was already decided (still
   at least +4 after the move, or at most -4 before it) and are left out.
2. `confirm.py`: Stockfish MultiPV 8 at 2 s on the remaining 191. A move is *acceptable* if it scores
   within 50 cp of the best. 139 positions still show a loss of at least 100 cp and form `testset.tsv`.
   72 of them have exactly one acceptable move.
3. `solve.py`: the engine searches each position from an empty tree for 8 s with the bot's settings
   (Hash 2048, 4 threads, `pisp2` at `BlendScale 102`), and records its preferred move every second. A
   position is *solved at t* if the preferred move is acceptable from t to the end of the search.

Baseline, `creatica` of 16 September (`baseline_creatica_2026-09-16.tsv`), about 2.6 million simulations
in 8 s:

| solved | positions | cumulative |
|---|---|---|
| within 1 s | 43 | 31% |
| within 2 s | 3 | 33% |
| within 4 s | 4 | 36% |
| within 8 s | 7 | 41% |
| not within 8 s | 82 | |

In 59 positions the engine still plays the move it lost with. Positions with a single acceptable move are
solved less often (23 of 72) than those with several (34 of 67). In the unsolved positions the best
acceptable move starts with a median prior of 5.8% (rank 4) against 8.0% for the move actually played; in
the solved ones it is 11.4% (rank 2) against 5.8%. The root prior does not show everything: in 21...Qe4 the
right move had the top prior, and the failure was a 1% prior on the opponent's refutation one move deeper.

**Engine binaries.** Only `creatica` is kept, built from the current source with the command on the
first line of `creatica.cpp`. The step-by-step binaries of September 2026 (`creatica_base`, `creatica_cp`,
`creatica_edge`, `creatica_tb`, `creatica_sp`, `creatica_frozenreset`, `creatica_prefetch`,
`creatica_mapkeep` and the rest) were deleted on 15 September. Measurements in this file that name one of
them describe that build as it was then. To compare against an older state, build that commit's source
under another name.

Two practical notes on sample size. Draw rates run 55–75% between configurations this
similar, so a 20-game match cannot separate a 50-Elo gap from zero — a result that looks
convincing at 20 games has repeatedly washed out at 64. And the opening book matters: the
built-in ten balanced main lines give roughly 36% decisive games, while a book of sharp
gambit and opposite-castling positions gives about 45%, which is worth roughly 25% more
information per game for the same machine time.


## Open questions

Each of these is unsettled, with the experiment that would settle it. Four of the five are
being tested as this is written; the last is waiting on an idle machine.

| question | experiment | why it matters |
|---|---|---|
| Do the exploration constants still want 160/65/5? | running now, in the self-play tuner | they were fitted against the old eval-derived prior, whose top-1 was 27% against the blend's 36%; a prior of a different shape plausibly wants a different exploration balance |
| Is `PolicyBlend 0.45` right *in play*? | running now, sweeping 0.20–0.70 | 0.45 came from offline top-k accuracy, and top-k has stopped predicting Elo; the one match attempt stopped level at 7.5–7.5 after 15 games |
| ~~Is `BlendScale 1.15` right?~~ | **answered** | it is calibrated per net, not a single value: 1.15 for the 256/128 net, 1.05 for `pisp`, **1.02 for `pisp2`**, which is now the default. `bench_blend` reports the top move's share at several scales in one pass |
| Does the ProbabilityMass gate pay? | running now, sweeping 985–1000 | new mechanism: gating before the child evaluations rather than after (see **ProbabilityMass** above). Skips ~35% of evaluations at 990 for ~3% more nodes and a distinctly deeper tree — an untested trade |
| Is `creatica` equal to its predecessor? | 40+ games on an idle machine | gate before renaming it to `creatica` and pointing the bot and harnesses at it |
| ~~What actually caused the 60 s engine stall?~~ **ANSWERED** | reproduced in one move; fixed in `1d26185` | **The engine was not stalling, it was crashing.** Lichess reports castling as KING-TAKES-ROOK (`e8a8`, `e1a1`) in every game created from a position — the UCI_Chess960 convention — and the opening book makes every bot-vs-bot game a `fromPosition` game. libchess understands that notation only behind `board.isChess960` (`board.cpp:869`), which a standard FEN never sets (measured false on all 1370 book FENs). `uci2move_idx()` does no legality check, so `e8a8` was applied literally: the king marched onto its own rook, `ff_move()` left an illegal `Board`, and the next search died in `kingMoves()`. Two crash reports both show `set_root() -> eval_and_expand() -> kingMoves()`. Repro: `position fen <black may castle long> moves e8a8` then `go` is a SIGSEGV; `e8c8` is not. Collection time was **not** the cause — the 11.9 s maximum was real but never reached the 60 s watchdog. |
| ~~Does chess960 work at all?~~ **TESTED — three defects found and fixed** | perft against Stockfish (`UCI_Chess960`) on 1,860 generated positions | It did not. **(1)** `castlingMoves()` tested the king's path against attacks computed with the castling ROOK STILL IN PLACE, so an enemy slider behind it on the back rank was invisible — white king d1, white rook b1, black queen a1: castling vacates b1 and the queen then attacks c1. Standard chess cannot produce this, because the queenside rook starts on a1 with nothing behind it. **(2)** `undo_move()` restored the castling rook to its source before clearing its destination, and in 960 those can be the SAME square — the rook was written and immediately erased from the mailbox. The bitboards survived (two XORs of one bit cancel), so every count-based test passed with the bug present; only `reconcile()` saw it, and it matters because `piecesOnSquares[]` is what NNUE reads. **(3)** `fen2board()` could not read X-FEN at all — plain `KQkq`, where K means the outermost rook, which is exactly what lichess sends (`rbnqbnkr/... w KQkq`, king on g1). 124 of 3840 runs failed on the X-FEN encoding of positions that passed in Shredder form. Suites: `perft_suite_960.txt` (960 starts), `perft_suite_960_mid.txt` (900 midgame positions where castling is legal NOW, two independent seeds), `perft_suite_960_xfen.txt`. **`lichess_bot` now plays 960**, gated on `CREATICA_VARIANT=chess960`, which decides both what is challenged and what is accepted — so a bot configured for standard still declines 960 and cannot be pulled into it by a stranger. A book FEN would have silently overridden the variant to `fromPosition`, so the book is suppressed (and warned about) for non-standard variants; nothing is lost, because lichess randomises the 960 start position every game. Colours are alternated when there is no book. Check a configuration before committing a night to it with `./lichess_bot --print-challenge`, which prints the challenge POST body and exits without a token or network. |

The first four are being swept by `self-play-optimization.cpp` at 40 games and 500 ms per
comparison. See **Tuning parameters by self-play** at the end of this document for how to
read its output and what it can resolve.

Settled and not worth revisiting: `full` mode (tested twice, collapses); the wider 512/256
net (no measurable Elo for twice the parameters); the look-ahead decomposition (two of three
uses load-bearing, the third neutral over 64 games); top-K agreement as a proxy for strength
(stopped predicting above ~33%); label smoothing (null at 0.10); and `Threads` (4 beats 8 on
throughput by 36% and won a 24-game match, direction predicted in advance).

**A caveat that applies to the matches recorded above, but no longer to new ones.** Every
result in this document up to the thread measurement was produced on a contended machine:
two engines at **8 threads each**, 16 threads on 8 cores, frequently with builds, tests or
training running alongside. That inflates variance without biasing either side, which is
much of why twenty- and sixty-four-game matches have repeatedly failed to separate a
fifty-Elo difference from zero. Large effects survived it — a three-point collapse inside
three games is not a scheduling artefact — and small ones did not.

**The default is now 4 threads.** It is `#define THREADS 4` in `creatica_search.hpp`,
which is what the engine advertises; the GUI does not set `Threads` at all and so inherits
it, and `self-play-optimization.cpp` sets 4 explicitly. A match is therefore 8 threads on 8 cores with only one side
searching at any moment, which is a materially quieter measurement than anything above it.
Numbers from before and after the change are not directly comparable, and the older ones
should be read as noisier rather than wrong.

**Sample sizes that actually resolve something**, at the observed 55-75% draw rates:

| games | roughly resolves |
|---|---|
| 20 | a 150+ Elo difference; nothing smaller |
| 40 | 100 Elo |
| 64 | 75 Elo |
| 250 | 30 Elo |

A result that looked convincing at 20 games has washed out at 64 twice in this project.
Plan the sample around the size of the effect being looked for.


## The lichess bot

`./lichess_bot --help` is the authoritative list and prints the compile-time defaults straight
from the `#define`s, so it cannot drift from the code the way a table here would. Almost all of
the configuration is **environment variables**; the command-line flags only cover what changes
between runs of the same setup.

### How big the opponent pool is: `CREATICA_NB`

`GetAndProcessBots()` asks `/api/bot/online?nb=<n>` for the list it challenges from. **That `nb` is a
prefix of a fixed ordering, not a sample of it**, which is the whole reason the number matters.
Measured on 26 September 2026: 396 bots were online, and `?nb=50` returned exactly the first 50 rows
of the `?nb=1000` list, in the same order, on two fetches seconds apart.

The head of that ordering is lichess's own bot family — `maia1` at 1399, `maia5` at 1437, `maia9` at
1599, then the odds-giving Leelas from 2000 up to 3340 — and almost none of it lies inside the
2100–2800 band the bot challenges in. Applying `GetAndProcessBots()`'s own filter to the fetched list:

| `nb` | eligible opponents |
|---|---|
| 50 (the old default) | **12** |
| 1000 | **117** |

So the old value gave the bot 12 candidates out of 117, and *the same 12 every round*, because the
ordering is stable. Bots visibly playing on `lichess.org/games/bot` were simply past the cut rather
than absent or ineligible. The default is now 1000, which is more than the population; the API
returns what it has, the whole list is 336 KB in one request, and it is fetched only between games.

`CREATICA_NB` overrides it, for the same reason `CREATICA_MIN_ELO` and `CREATICA_MAX_ELO` are
overridable: widening the pool changes *who* creatica has been playing, so its rating will move for
reasons that have nothing to do with the engine. A run whose purpose is to compare two engine
versions against a known field should pin `CREATICA_NB=50` to keep the field the earlier games used.

Of the 396 online, 202 were at or below 2100 and 77 at or above 2800, so the rating band itself
excludes 279 by design — that is not a defect, and the real pool is the remaining 117. All 396 carried
a blitz rating and none were flagged `tosViolation` or `disabled`, so neither of those filters trims
anything in practice.

### Saying something when a draw offer is declined: `CREATICA_CHAT_DRAW_DECLINED`

The bot offers a draw **at most once a game, and only when the tablebase says the position is a dead
draw** — `tablebaseVerdict()` requires the search to have returned depth 1, one node and one
tablebase hit, so the verdict is a proof rather than an opinion. Opponents essentially never accept.
Measured over the 297 games in `creatica_creaticachessbot.log`, **58 reached a tablebase-drawn
position**, so roughly one game in five plays out a draw that both sides could already have agreed.

When the offer is turned down, the bot now posts one line into the game's *player* chat (the two
players' room, not the public spectator one). The default is
`solar energy is free in Queensland - burn, baby, burn`; `CREATICA_CHAT_DRAW_DECLINED` replaces it,
and setting it to the empty string switches it off.

Three things this deliberately gets right:

- **There is no "declined" event in the game stream**, so a decline is read off the bot's own
  `wdraw`/`bdraw` flag going from set to clear while the game is still running. Had they *accepted*,
  the status would be terminal and the handler returns before reaching the check. An opponent's move
  implicitly declines and clears the flag too, which is the same answer for this purpose.
- **The text is form-encoded** with `curl_easy_escape` rather than pasted into the body. The default
  contains spaces and commas; unencoded, the message would be truncated or the `room` field corrupted.
- **One line per game**, tracked by `draw_decline_chatted` and reset alongside `tb_draw_offered` when
  a game starts. A failed chat POST is logged and dropped with no retry — a chat line is decoration,
  and a bot that spent clock retrying it would be trading a game for a joke.

It is also read with `env_str_or_empty()` rather than `env_str()`, because `env_str()` treats an empty
value as *unset* and falls back to the default. That is right for a path, where `""` can only be a
mistake, and wrong for a setting whose empty value is how you turn it off.

Check a configuration before committing a night to it:

```sh
./lichess_bot --print-challenge          # prints the challenge POST body, then exits
```

No token and no network. This exists because the failure it catches is invisible until the games
have been played — a book FEN is sent as `variant=fromPosition`, which silently overrides
`CREATICA_VARIANT=chess960`, and the logs of the resulting standard match look completely normal.

**Two bots playing each other, standard, with an opening book:**

```sh
LICHESS_USERNAME=creaticachessbot  CREATICA_VISITS=bot1_visits.tsv CREATICA_THREADS=2 \
  CREATICA_HASH=1024 CREATICA_PONDER=1 CREATICA_CLOCK=120 CREATICA_INC=2 \
  CREATICA_BOOK=~/libchess/book.tsv \
  ./lichess_bot --challenge=creaticachessbot2 --casual --accept-only=creaticachessbot2

LICHESS_USERNAME=creaticachessbot2 CREATICA_VISITS=bot2_visits.tsv CREATICA_THREADS=2 \
  CREATICA_HASH=1024 CREATICA_PONDER=1 \
  ./lichess_bot --no-challenge --accept-only=creaticachessbot
```

**The same pair playing Chess960** — drop the book and set the variant on *both* sides, since it
decides what is accepted as well as what is challenged:

```sh
LICHESS_USERNAME=creaticachessbot  CREATICA_VARIANT=chess960 CREATICA_VISITS=bot1_visits.tsv \
  CREATICA_THREADS=2 CREATICA_HASH=1024 CREATICA_PONDER=1 CREATICA_CLOCK=120 CREATICA_INC=2 \
  ./lichess_bot --challenge=creaticachessbot2 --casual --accept-only=creaticachessbot2

LICHESS_USERNAME=creaticachessbot2 CREATICA_VARIANT=chess960 CREATICA_VISITS=bot2_visits.tsv \
  CREATICA_THREADS=2 CREATICA_HASH=1024 CREATICA_PONDER=1 \
  ./lichess_bot --no-challenge --accept-only=creaticachessbot
```

**Rated games only.** `--rated-only` declines casual challenges, with lichess's own `casual` reason so the
challenger is told why. Without it any challenge is accepted: the incoming challenge's `rated` flag was
read and never used. It matters because the bot plays one game at a time, so a casual game keeps it off
the ladder for its whole length. `--casual` is the opposite setting and applies only to challenges the bot
SENDS.

Things that have actually cost time here:

- **Give each bot its own `CREATICA_VISITS` and let it have its own `CREATICA_LOG`.** Two engines
  appending to one log is not merely untidy: creatica ponders the position where the OPPONENT is
  to move, which is the same position the opponent is searching for real, so a shared log shows
  what looks like one engine emitting two `bestmove` lines. That artefact was read as a protocol
  violation and a driver change was made on the strength of it. The bot now sets `CREATICA_LOG`
  per instance by default.
- **`CREATICA_HASH=1024` on both bots swaps an 8 GB machine.** Each engine takes about 1.8 GB once its
  tree is full (see the next point), so two of them take 3.7 GB. 512 is the safer pairing.
- **What `Hash` really costs in RAM.** `Hash` sets the tree's budget, not the process size. Measured with
  `footprint`, 4 threads, `pisp2`, 16 September 2026, once the tree is full:

  | `Hash` | before 16 September | after the arena and edge changes |
  |---|---|---|
  | 1024 | 1,842 MB | 1,396 MB |
  | 2048 | 3,508 MB | 2,622 MB |
  | 3072 | (about 5.2 GB, not measured) | 4,113 MB |

  About 170 MB is fixed (nets, threads, tables). The rest is roughly 1.2–1.4 MB per MB of `Hash`; it is not
  quite linear because the node map's size doubles in steps, and at `Hash` 3072 that table alone is about
  1 GB. `Hash 4096` should now cost about 5.3 GB rather than 6.6 GB, so it fits an 8 GB Mac only with the
  apps closed. Two thirds of the saving is the arena sizing described in *The node arena*, the rest is the
  12-byte `Edge`.
- **Only the challenging side's clock settings are used.** Setting `CREATICA_CLOCK` on the
  `--no-challenge` instance does nothing.
- **`CREATICA_SPEEDS` decides what the ACCEPTING side will take**, and it is easy to trip over,
  because lichess derives the speed from `initial + 40 * increment` rather than from the clock you
  asked for. `60+1` estimates at 100 s and is **bullet**, so a 1+1 match is declined with
  `reason=timeControl` even though `CREATICA_CLOCK`/`CREATICA_INC` are set correctly — the two
  settings control different sides and nothing connects them. `60+3` is 180 s and is blitz, the
  fastest control the default accepts.

      CREATICA_SPEEDS="bullet,blitz,rapid,classical"   # to allow 1+1

  The default excludes bullet deliberately: a lichess round trip from this machine measures about
  **350 ms typical and 580 ms at worst** on a reused connection, so at a one-second increment a
  third of every move is gone to the network before the engine thinks. Widen it for local
  experiments, not for rated play.
- **`CREATICA_OPTIONS` passes any UCI option through**, as `Name=Value` pairs. It tries check, spin
  and string in turn and lets the engine's own advertised option list decide which the name is —
  it used to dispatch on what the *value* looked like, so `PolicyWeights=some.bin` went to the spin
  path, was dropped, and a match ran the old net on both sides while appearing to test a new one.
- **`--accept-only=` on both sides** keeps a passing bot out of a self-play experiment.
- **Measuring a policy, not an engine?** Prefer the local GUI (`python3 chess_gui.py`, then
  <http://127.0.0.1:8080>) over lichess. Same binary both sides differing only by
  `PolicyWeights`, fixed movetime so time management drops out, paired openings so each position is
  played once with each engine on each colour, and no 350 ms round trip per move. Paste FENs into
  its opening box; `book.tsv` column 1 is the source, filtered by its `cp` column for balance.

## Collecting a self-play dataset

`VisitDumpFile` appends one tab-separated line per completed search:

    tag \t fen \t simulations \t rootQ \t rootCP \t ponder \t "move:visits:prior ..."

Every legal move appears, including those with zero visits — a move the search refused to visit
is as much a part of the target as the one it chose, and dropping those would bias the
distribution toward flatness. The line is written on the **raw** search result, before
`select_best_moves()` applies its repetition-avoidance edits: those are a playing decision
rather than something the search concluded, and training on them would teach the policy a
heuristic instead of an evaluation.

**Why this target rather than the current one.** The policy head is trained today on whether it
picks Stockfish's PV1, and that has saturated — top-1 went 26.7% → 33.08% → 34.83% while Elo
went +108 → +113 → level. Past roughly 33% the positions the model newly gets right are ones
where several moves were comparable anyway, so agreeing about them means nothing over the
board. A visit distribution instead says *how much better*, across every move, and it comes
from a search that looked far deeper than the prior it would be teaching.

**Each move's prior is recorded alongside its visits**, which lets the file answer the question
that decides whether any of this is worth doing, before a single epoch is run: how often, and
by how much, does the search actually disagree with the prior it started from? If it rarely
does, the target is nearly the model's own output and training on it is an expensive no-op.

    python3 visit_dump_stats.py visits.tsv

reports exactly that, along with record counts, mean legal moves, and how concentrated the
distributions are. Its verdict line is a go/no-go, not a summary.

**Cost.** None to generate — these are searches the engine performs regardless. Roughly 600
bytes a record, so a night of blitz is a few tens of MB. `lichess_bot` sets `GameTag` to the
lichess game id at the start of each game, so a row joins to the result recorded in
`results_<bot>.csv` — which is what a *value* target would need, as opposed to a policy one.

**What is not built yet:** the training side. `nnue_policy_train.cpp` still trains against PV1
identity; consuming a distribution means a cross-entropy against the visit shares rather than a
one-hot target. That is the next piece, and it should not be written until the statistics above
say the data carries signal.


### Generating targets from stored positions

A training record needs a **position** and a **search**. Playing games is a slow way to get
positions — you pay real time for both sides, the clock, the network and the opponent's
thinking, and you get whatever positions the games happen to visit.

    ./bin2fen <file.bin> [max] [min_abs_cp] [max_abs_cp] \
      | VISIT_DUMP=targets.tsv MOVETIME=1000 STRIDE=13 ./gen_targets

| source | records/night (10 h) |
|---|---|
| playing bot games at 1+1 | ~5,000 |
| `gen_targets` at 3 s/position | ~11,000 |
| `gen_targets` at 1 s/position | ~35,000 |

Measured at 58 searches/min at 1000 ms, four threads.

**`bin2fen` reads the `pgn_parser.cpp` format, which is NOT the training format.** It writes
position and eval then aligns, with no PV block, while `nnue_policy_train.cpp` expects a PV
count and move list next — so pointing the trainer at `lichess_db_broadcast_*.bin` segfaults.
It is also LSB-first, and reading it MSB-first yields plausible-looking garbage rather than an
error, which is how a format mismatch hides.

**`STRIDE` matters more than it looks.** `bin2fen` emits every position of every game, so
consecutive inputs differ by one move and their searches largely repeat each other. A stride
spreads the same number of searches over far more distinct material.

**`ReuseTree` is forced off** in `gen_targets`. Positions from a list are unrelated, so there is
no subtree to inherit — but the tree would still accumulate across all of them, grow
monotonically and hit the `Hash` ceiling, degrading every later search. Reuse is a win when
positions follow each other in a game and a liability when they do not.

**The Stockfish eval is kept but is not a target.** creatica already blends policy with eval at
search time, and that blend beats both components (top-1 27% / 32.8% / **36.3%**) precisely
because they carry different information; training the policy toward eval-derived targets makes
it more eval-like and shrinks the diversity the ensemble depends on. The eval earns its place
twice over elsewhere: **curation**, via `bin2fen`'s `|eval|` band, so expensive searches are not
spent on decided positions; and **drift detection**, since self-distillation has no external
corrective and falling agreement with Stockfish is how you would notice the policy drifting into
its own blind spots.


### Reading the dataset report

    python3 visit_dump_stats.py targets.tsv

A real report from the first 276 records of a broadcast run at 1000 ms:

    records                    276   (0 from ponder searches)
    distinct games (tags)      1
    repeated positions         2
    legal moves, mean          32.8
    simulations, mean          305721
    seldepth, mean             23.5
    top move's visit share     0.831
    distribution entropy       0.586 nats  (uniform over 32.8 = 3.492)
    search agrees with prior   36.6%
    mean prior->visits shift   0.665

**`search agrees with prior` is the number that decides.** Distillation can only teach the
policy head what the search knows and the prior does not. At 36.6% the search picks a different
move from the policy head on **63% of positions** — a great deal to learn. If this figure ever
climbs above about 90%, the target has become the model's own output and training on it would be
an expensive no-op. `prior->visits shift` corroborates it across the whole distribution rather
than just the top move.

**`legal moves, mean` at 32.8 is a good sign.** Opening positions run 25–27; this is middlegame
material, where there is genuinely something to decide. A position with three legal moves teaches
almost nothing.

**`seldepth` is the quality dial, and 23.5 is modest** — the same positions reach ~33 at 3000 ms
and ~36 at 5000 ms. Because it is recorded per record, a mixed-depth dataset can be filtered or
weighted at training time rather than committed to now. That is the reason it is in the file.

**`top move's visit share` of 0.831 says the target is nearly one-hot.** creatica concentrates
hard, so distillation will mostly teach *which* move rather than *how much* better — less rich
than AlphaZero-style targets, which stay soft because of Dirichlet noise added at the root during
self-play. That noise deliberately weakens play, which is a fair trade for dedicated generation
and a bad one for games you also want to win.

**`repeated positions` is waste**: each duplicate FEN is a search spent twice for one sample. Two
out of 276 is nothing; a large share would mean `STRIDE` is too small for the source.

**What the report does not tell you.** A low agreement figure says the target *differs* from the
model's current belief — not that it is *better*. A shallow or broken search also disagrees, just
wrongly. What justifies trusting it is that the target comes from ~300k NNUE-backed simulations
to depth ~24, against a single forward pass of the policy head. This project has already been
burned once by a proxy that stopped predicting strength — top-1 agreement with Stockfish went
26.7% → 33.08% → 34.83% while Elo went +108 → +113 → level — so treat every figure here as a
necessary condition and never a sufficient one. The decisive test remains a match.


### Training on a visit dump

    TRAIN_DATA=targets.tsv TEST_DATA=targets_val.tsv SELDEPTH_MIN=20 ./nnue_policy_train

This is a **data source**, not a second trainer. The loss already does listwise cross-entropy
over the legal moves against a sparse (index, weight) target, so a visit distribution slots
straight in and the model, cosine schedule, feature cache, checkpointing and export are
untouched.

`load_shard()` detects a dump from its own `tag\t` header rather than from the file extension,
because these two formats share no framing and reading one as the other yields plausible garbage
rather than an error — which is exactly how the `pgn_parser` mismatch stayed hidden until it
segfaulted.

| variable | meaning |
|---|---|
| `TRAIN_DATA` | shard or dump; default `../lichess_db_pvs_eval.bin` |
| `TEST_DATA` | validation set; must not overlap `TRAIN_DATA` |
| `SELDEPTH_MIN` | drop records from searches shallower than this |

**`SELDEPTH_MIN` is the quality dial.** On 799 broadcast records, a floor of 20 kept 523 of them
and moved the training loss from 3.16 to 3.00 — deeper searches make more learnable targets.
That is the reason seldepth is recorded per record instead of being fixed by the time control at
generation time.

Two things about the record semantics:

**`share[]` carries the target directly.** When it is set, the weights are the search's visit
shares and are used as-is rather than softmaxed out of centipawn scores — a share is not a
centipawn score, so pushing it through that path would be meaningless. The bin reader sets
`share[0] = -1` explicitly, because an uninitialised float that happened to be >= 0 would
silently switch the target semantics on some fraction of samples with no error.

**The value head is not trained.** The loss is `loss_policy` alone, so the `Value Loss: 0` the
validation prints is an untouched accumulator, not a measurement. A dump does carry `value_q` —
the search's own root value, already in tanh units and already side-to-move relative, so unlike a
white-relative `eval_cp` it must not be sign-flipped for Black — and wiring it in would be
`loss_policy + w * mse(...)`. Deliberately not done: the value side has never been measured, and
adding an untested term to a loss that works is the wrong order.


## Training options

Not engine settings, but the policy net the engine loads comes from `nnue_policy_train.cpp`,
and its knobs are environment variables there (a training script, not a protocol server, so
there is no reason for them to be UCI options).

| variable | default | meaning |
|---|---|---|
| `FEATURE_CACHE` | — | Directory of pre-extracted NNUE features. Removes 73% of the per-sample CPU cost; without it training runs at roughly half speed. |
| `BUILD_CACHE` | — | Build that cache and exit. Resumable; a complete shard is skipped. ~48 min for all 28 shards. |
| `BATCH_SIZE` | **8192** | Measured fastest on MPS once the cache removes the CPU bottleneck: CPU under 70%, GPU over 90%, ~80k positions/s. |
| `CHUNK_SIZE` | **250000** | Positions held in RAM per chunk. 1000000 made the OS memory compressor thrash and halved throughput. |
| `LR_MAX` | **2e-3** | Peak learning rate. Measured better at batch 8192; the previous 2e-4 was roughly 5-10x too low, and the two belong together. |
| `EPOCHS` | 1 | Drives both the loop and `TOTAL_STEPS`, so the cosine is sized to the whole run. |
| `TOTAL_STEPS` | exact | Steps the cosine anneals over. With `FEATURE_CACHE` set it is counted exactly from each shard's cache header, following the same chunking and batching as the loop, so the rate reaches `LR_MIN` on the last batch. Without a cache it falls back to a guess of 300M positions per epoch, and the trainer prints a warning. The guess was what every run used before 2026-09-14; the first full-corpus spatial run ended with the rate still at about 3e-5 instead of 1e-5. |
| `RESUME` | 0 | 1 continues the run recorded in `<CKPT_PREFIX>run_state.txt` after a crash. See below. |
| `SEED` | random | Fixes each epoch's shard order, the batch order within each shard and a fresh model's initial weights. A fresh run without it draws one and records it in the run state. |
| `TRAIN_SHARDS` / `MAX_BATCHES_PER_SHARD` | all / all | Train on the first n shards, and stop each shard after n batches. For testing a change in minutes. Both are included in the exact step count. |
| `LABEL_SMOOTH` | 0 | Share of target mass spread uniformly over the legal moves. 0 is the original loss. |
| `VALIDATE_EVERY` / `VALIDATE_N` | 1 / 1000000 | Validation runs after every *file*, not every epoch — 28 times an epoch. Both were needed to stop validation costing more than training. |
| `NUM_WORKERS` | 1 | DataLoader workers. **Leave at 1.** Two workers measured *half* the throughput of one: each holds its own accumulator cache, so they get colder caches and contend for memory bandwidth on the 1024-wide accumulator rows. The same shape as the engine's thread finding. |
| `WEIGHTS` / `CKPT_PREFIX` | `nnue_policy.pt` / … | Lets a differently-shaped model train without colliding with the current one. |
| `EXPORT_WEIGHTS` | — | Write the trained net as the flat `.bin` the engine loads, and exit. |

**Resuming a run.** After every shard the trainer writes `<CKPT_PREFIX>run_state.txt`. It holds the
seed, the epoch in progress, the shards already trained in that epoch, the step counter, and the
names of the checkpoint and of the Adam optimizer state saved after that shard. Both files are
complete on disk before the state names them, and the state itself is replaced atomically.
`RESUME=1` reads it back. The seed redraws the same shard order, the shards already done are
skipped, and the weights, Adam's moment estimates and the step counter continue from the last
completed shard, so a crash loses at most one shard. A test run killed partway through its second
shard and resumed reproduced the uninterrupted run's validation numbers to within the GPU's run-to-run
noise. A fresh run refuses to start while a run state exists under its `CKPT_PREFIX`, and
`RESUME=1` on a finished run does nothing. Only the latest optimizer file is kept.

Before this, a `.step` file beside the weights was the only thing saved. It kept the cosine from
jumping back to `LR_MAX`, but a restarted epoch drew a new random shard order, so it could not tell
which shards it had already trained on, and Adam restarted with empty moments. The `.step` file is
still written, and is still honoured by a run that is not resuming, but only when the weights it
belongs to exist.

`train_unattended.sh <trainer>` supervises a long run. It waits until the feature cache is readable
before every start, logs the SSD's USB link speed, restarts the trainer with `RESUME=1` after a
crash, stops and restarts a trainer whose log has been silent for 20 minutes, keeps the machine
awake with `caffeinate`, and gives up after three failures in a row at the same step.


### CREATICA_EDGE_VISITS

Sets the `EdgeVisits` UCI option on that bot's engine, so a head-to-head can run both sides from one
binary rather than two builds that might differ in something else. Off unless set. See the option's
own section for what it changes and what it costs.

## Setting options from a driver

`tournament.cpp`, `self-play-optimization.cpp` and `lichess_bot.cpp` drive a spawned engine
through `engine.cpp`. They set its options **by name**:

```cpp
setEngineSpin(engine, "PolicyBlend", 45);      // 0.45 — see the scaling note above
setEngineCheck(engine, "PerformanceCores", true);
setEngineStringOption(engine, "SyzygyPath", "~/syzygy");
setOptions(engine);                            // sends everything that differs from default
```

Each returns `false` and prints a warning naming the engine and the option if that engine
does not advertise it. `engineSpinRange()` reports the advertised `min`/`max`/`default`, so a
driver sweeping a parameter can stay inside the bounds and skip what the engine lacks.

**Do not index `engine.optionSpin[]` with the `EngineSpinOptions` enum.** `getOptions()`
fills that array in the order the engine happens to advertise its options, which is not the
enum's order. Against `creatica` the enum's `ProbabilityMass` (index 9) is `PolicyMode`,
`EvalScale` (10) is `PolicyBlend`, `EvalDepth` (11) is `PolicyTemp`, `MaxNodes` (12) is
`BlendScale` and `NegamaxDepth` (13) is `FpuReduction`; on the check side `FinalInfoLines`
(0) is `PerformanceCores`, so the obvious line for quietening the info output would instead
have turned off performance-core pinning. An index past `numberOfSpinOptions` is not an error
either — `setOptions()` stops at that count, so the write is simply dropped. Neither failure
produced any diagnostic. The enum is kept only because `engine.cpp` and older unbuilt files
still refer to it.

Engines that use the `Engine` struct as their **own** settings store are unaffected: they
bind each option to a slot explicitly (`o.spin("Hash", &chessEngine.optionSpin[Hash].value,
…)`), so declaration order does not matter there.


## Tuning parameters by self-play

`self-play-optimization.cpp` is a greedy coordinate climb: for each parameter it plays a
match at one step up, and if that does not win, one step down.

Two things about it are worth knowing before reading its output.

**Sample size decides what it can see.** With draw rates between 55% and 75%, a 10-game match
cannot resolve anything smaller than roughly 150 Elo — one game swings the score by ten
percentage points, more than any of these parameters is worth. It now plays 40 games at
500 ms rather than 10 at 2000 ms. A shorter time control moves a parameter's optimum
slightly, but the *ordering* of candidate values, which is all a hill-climb consumes, almost
always survives.

**A candidate must clear a standard-error bar, not just a percentage.** Acceptance requires
both a score above `WIN_THRESHOLD` and a margin of at least `MIN_SIGMA` standard errors above
50%. Comparing a rate against a fixed 55% with no reference to how many games produced it is
what let single-game noise be recorded as an improvement — and a climb making dozens of
comparisons will always turn up a few that clear 55% by luck. Each result now prints its
win/draw/loss split, its Elo, and its sigma.

At startup the tuner prints every parameter with the range the engine actually advertises,
and skips any option the engine does not have.

## The value is compressed, and the tanh is not why

**Related but distinct, 8 October 2026:** the `tanh` is not the cause of the *compression* measured here,
and that finding stands. It is however the cause of a separate problem -- **saturation**. Because it maps
an unbounded centipawn score into the interval from -1 to 1, above roughly 1200 cp it stops carrying
information: measured in a live search, a forced mate and a position worth +1188 cp differ by 0.04 in `Q`,
and a mate in 2 from a mate in 3 by 0.0005. In that region the prior decides the move. See the last
section of this document.

A node's value is `Q = W / evidence`, and `W` is a running sum of `tanh(pawns / eval_scale)` — a **mean
over every leaf the subtree has ever visited**. Measured against Stockfish over 682 searches from fourteen
lichess games, the engine reports about **60–70% of the true magnitude** once a position is unbalanced:

| Stockfish says | n | median Stockfish | median creatica | ratio |
|---|---|---|---|---|
| 0–50 cp | 217 | 24 | 20 | 0.83 |
| 50–150 cp | 164 | 85 | 61 | 0.72 |
| 150–300 cp | 71 | 201 | 134 | 0.67 |
| 300–600 cp | 143 | 425 | 297 | 0.70 |
| 600–1500 cp | 38 | 763 | 580 | 0.76 |

They agree on **who** is better in 99% of positions. It is only the magnitude that is lost.

**The tanh was the obvious suspect and it is nearly innocent.** A diagnostic accumulator carries the same
backup in pawns without ever squashing it (`ThreadParams::root_pawns`), and one `info string compression:`
line per search reports both means. Over 24 positions:

| Stockfish says | median Stockfish | squashed | linear, no tanh | tanh's share | total |
|---|---|---|---|---|---|
| 0–100 cp | 47 | 30 | 35 | 0.86 | 0.64 |
| 100–300 cp | 197 | 118 | 123 | **0.96** | 0.60 |
| 300–1500 cp | 374 | 226 | 240 | **0.94** | 0.60 |

Removing the tanh entirely would recover four to six per cent of a forty per cent problem. Jensen's
inequality only bites when a subtree's leaves disagree *wildly* — half of them four or more pawns apart —
and real subtrees are not that spread out. So **averaging in eval space is not worth doing**, and
`EvalScale` is irrelevant to this (it is also the 600 cp constant the policy was trained against).

What remains is **mixture dilution**: a mean counts lines nobody will play, where a minimax takes the best.
The sharpest case is the position after 24...Kh8 in lichess `Gr3fzghd`, which Stockfish scores **+680 for
White**: `squashed 14 cp, linear 10 cp over 960471 backups`. Both agree with each other and both are wrong
by seven pawns, because the refutation is one queen sacrifice among a million ordinary backups.

**Why this is not a display bug.** The compression is proportional to a subtree's *variance*, so it is not a
scaling you could divide out. A move whose subtree hides one refutation among forty quiet lines is penalised;
a move that is honestly a little worse is not. That is the same arithmetic that lost a queen in `Gr3fzghd`
(the blunder held 753,670 of 845,346 visits at `cp -13`, with its own one-ply value at `stale -514`) and the
same arithmetic behind the silent slides below.

### Silent slides

A scan of fourteen games for stretches where Stockfish falls by 250 cp or more over at most twelve of
creatica's moves while creatica's own value moves less than 100 cp found **six of fourteen games affected**,
in the opening, middlegame and endgame alike:

| game | moves | Stockfish | creatica |
|---|---|---|---|
| `Gr3fzghd` | 24→36 | −32 → −1000 | −14 → −6 (**+8**) |
| `gPdH6u8p` | 84→90 | 1000 → 557 | 996 → 1000 |
| `XSu1CIQ7` | 60→66 | −64 → −473 | −53 → −142 |
| `YcP9DAbG` | 28→38 | 450 → 63 | 127 → 58 |
| `Ecq2KhAg` | 32→37 | 426 → 139 | 183 → 90 |
| `7ZApAVTR` | 4→14 | −7 → −288 | 5 → −54 |

`XSu1CIQ7` was lost this way from a dead draw: 0.00 through move 58, mated at move 72, with **no single move
losing 100 cp from a level position**. A per-move blunder threshold cannot see this, which is worth
remembering when reading any blunder-rate figure here.

### `MinimaxBackup`, and why it measured nothing

The obvious repair is to back up something less lossy than a mean. `MinimaxBackup` blends a root child's
mean with the value the opponent gets by choosing best at it — a **min** over the grandchildren, since they
are the opponent's replies. It is root-only and cached (one thread refreshes a 256-entry table every 1024
simulations) because neither `MCTSNode` nor `Edge` has a spare byte: they are exactly 64 and 12, and
widening either shrinks the tree a given `Hash` can hold. Cost measured at −2.8% and +0.7% simulations.

**In a 126-game self-play match it scored 48.8%, about −8 Elo with a 95% interval of −51 to +34.** That is a
coin flip. A match that size is a disaster detector — `BalanceShare` announced itself at 31% within 24
games — so the result means "not harmful", not "helpful". Resolving ±10 Elo would need ~2,300 games.

The null result is what the diagnosis predicts. The compression is a whole-tree averaging problem, and this
corrects one level, at one node, on the root's children only. It is the version that fits in the current
node layout, not the version the measurement calls for. Left in at default 0 as scaffolding.

## Tablebase routing when the local set is incomplete

`TB_LARGEST` is the size of the **largest table present**, not a promise that everything at or below that
size is covered. With the complete 3-4-5 piece set the distinction never mattered. It appears the moment a
**partial 6-piece set** is installed, and all three routing decisions were deciding by piece count:

- **In the search**, a leaf inside `TB_LARGEST` whose table is absent took a path that wrote a full error
  line through `log_file()`, which takes a global mutex. At a few hundred thousand leaves a second across
  four threads that would not merely fill the log, it would **serialise the search**. Now counted in
  `tb_probe_misses`, logged once per search, and reported in the `info string search:` line. The value
  fallback was already correct and is unchanged: a miss returns the ordinary NNUE leaf value and the
  simulation continues. **The search never falls back to the network** — at 10⁵–10⁶ leaves a second that is
  not slow, it is impossible.
- **At the root**, `numberOfPieces > TB_LARGEST` sent a position to lichess. With a partial set, a 6-piece
  configuration that had not been downloaded took the local branch instead, failed, and searched — *strictly
  worse than before the download began*. Now the local probe runs first and the lichess path is the
  fall-through whenever it did not answer. `drop_ponder()` moved after the probe, so a ponder is no longer
  abandoned for a table we do not have.
- **The prefetch** used the same piece-count test and now probes WDL locally to decide.

Positions with castling rights go to lichess: Syzygy cannot express castling.

This also makes a **WDL-only download** work, which is what you want since only `.rtbw` is needed in the
search. `tb_probe_root` needs DTZ, so it fails for every 6-piece position even where the WDL table is
present — unless the best move captures into 5-piece territory, where the existing DTZ answers. Verified
with 26 of 59 configurations installed, WDL only:

| case | result |
|---|---|
| quiet `KRPvKRP` (downloaded) | root probe fails → lichess answers in 774 ms; search gets **594,627 tbhits** |
| `KRRvKRP` (absent entirely) | **140,009 misses, one log line**, 696,051 simulations/s, lichess answers |
| best move captures into 5 pieces | instant root move in 0.1 s, as before |

The miss count is the better guide to what to download next than any offline frequency list: it counts what
the *search* actually wanted, not what the game happened to reach.

### Is the one-ply value better than the search's average?

`node->cp` is the one-ply minimax plus quiescence written when a node is expanded; `Q` is the mean of every
leaf the subtree has visited. Two blunders showed `cp` far closer to the truth than `Q` — `Gr3fzghd` 24...Kh8
(`Q -13`, `cp -514`, truth −652) and `UU5b1QSz` 13.Nxd5 (`Q +23`, `cp -175`, truth −232) — which raised the
question of whether the backup should shrink toward `cp`.

**It should not.** Over 802 played moves from 140 games, with Stockfish as judge:

| \|Q − cp\| | n | median \|Q − truth\| | median \|cp − truth\| | Q wrong by ≥150 cp |
|---|---|---|---|---|
| 0–50 | 343 | 16 | 23 | 6% |
| 50–150 | 243 | 29 | 106 | 9% |
| 150–300 | 128 | 31 | 215 | 9% |
| 300–500 | 53 | 46 | 397 | 11% |
| **500+** | **35** | **612** | 463 | **63%** |

`Q` beats `cp` in every band but the last, and over a separate 300-position sample taken at the root its
median error was 20.5 cp against 67.5. Averaging earns its keep; blending `cp` into the backup would lose
more than it gains, almost everywhere.

**But the disagreement is a detector.** Past 500 centipawns of disagreement, `Q`'s median error jumps from
about 25 cp to 612 and it is badly wrong 63% of the time, against a 6–9% base rate — roughly sevenfold
enrichment, for one comparison between two numbers already stored on the node. It is a cliff, not a
gradient: the 300–500 band is still fine.

Two limits worth stating. In the 500+ band `cp` is bad too (median error 463), so when the two disagree
violently **neither number knows anything** and the right response is more search in that subtree, not
substituting one value for the other — which argues for using the signal to allocate search, as
`VerifyReplies` does, rather than to correct a value. And it catches only the catastrophic cases: of the two
known blunders, `Kh8` sits at 501 and would fire, while `Nxd5` sits at 198 and would not.

I think a better alternative to more search strategy is better priors (the policy). At least if the policy can eliminate
catastrophically bad priors, Q will stay bounded and not explode to blunders.

Untested as code. The obvious experiment is to gate `VerifyReplies` on the disagreement, which would let the
floor be raised a long way while being paid for only where the engine is far more likely to be wrong.


## Why `VerifyReplies` backfires

Measured on lichess `A5rTtcRP`, move 19: Black played `exd4` into `20.Qe8+ Rxe8 21.Rxe8#`, a forced mate in
two. Replayed from an empty tree, 3 s a move, `Hash` 2048, 4 threads, twelve runs per setting:

| setting | walked into the mate |
|---|---|
| `VerifyReplies 0` | **0 / 12** |
| `VerifyReplies 500`, `VerifyAfter 600` | **4 / 12** |
| `VerifyReplies 2000`, `VerifyAfter 200` | **12 / 12** |

A clean dose-response, in the direction opposite to the option's purpose. It does not merely fail to find
the refutation; **it causes the blunder.**

**Why, and it follows directly from the compression result.** `Q` is a *mean*. The floor forces evidence onto
*every* reply to the move the engine is about to play — here about forty answers to `exd4`, of which one
(`Qe8+`) refutes it and the rest are mediocre for White. Averaging thirty-nine mediocre replies into the
move's value pulls it **up**. The single refutation the floor exists to find is outvoted by the siblings it
was found among, and the harder the floor is driven the more thoroughly the losing move is whitewashed.

So the option's premise is sound — spend evidence where a refutation would be — and its arithmetic is wrong
for this search. It would need a **min over the verified replies** rather than their mean: exactly what
`MinimaxBackup` computes. Verifying and then averaging is worse than not verifying at all.

**This also invalidates an earlier claim in this document.** A blunder-rate scan that reported 1 blunder in
187 balanced moves with `VerifyReplies` on, against 6 in 101 with it off, was confounded — different games,
different opponents, a denominator that grows when the engine does not blunder, and a threshold metric that
cannot see slow drift. It should not have been read as evidence for the option. The twelve-run controlled
replay above is what a test of it actually looks like.


## The inherited tree in the endgame

Four blunders were examined from lichess games, and replaying each from an **empty tree** at the game's own
clock splits them into two different failures:

| game | move | loses | from an empty tree | in the game |
|---|---|---|---|---|
| `Gr3fzghd` | 24…Kh8 | 670 cp | **0/6** — always correct | blundered |
| `hKCOETfO` | 56…g5 | 618 cp | **0/7** — correct even at 20 s | blundered |
| `UU5b1QSz` | 13.Nxd5 | 218 cp | 4/6 — usually blunders | blundered |
| `A5rTtcRP` | 18…Rd5 | 254 cp | 6/6 — always blunders | blundered |

The bottom two are evaluation failures: the position is misjudged with a clean tree and plenty of time. The
top two are not — the engine finds the right move every time from scratch and only fails with a large
inherited tree.

`hKCOETfO` 56…g5 shows the mechanism. A dead-drawn rook endgame (Stockfish gives its top five moves 0.00);
`g5` is −618. The engine's record:

    visits 2,255,709   engine cp -10   stale cp -259
    1,537,455 simulations, tree 10,019,621 nodes, 594 permille

**More visits on that one move than the whole search performed**, so most were inherited from earlier
searches where `g5` was a sideline. The node's own one-ply value was −259, roughly right; its accumulated
mean was −10. A mean with two million samples cannot be outvoted by one search, so the correct assessment
the node was born with is averaged away permanently. Note the occupancy: 594 per mille, nowhere near the
950 the other reset triggers require, which is why this one ignores occupancy.

It also explains why every visit-allocation option has failed to help here. `BalanceShare`, `MinimaxBackup`
and `VerifyReplies` all act on *this* search's visits; none can touch a value that arrived with two million
inherited ones already in it. The `RepetitionGuard` filter's own source comment describes the same root
cause reaching it from a different direction — ranking by a lifetime `N` that nothing resets.

**Ruled out:** repetition avoidance. Across all four games the filter fired four times in total, every one
of them on a *ponder* search with the opponent to move, and never on a move actually played.

**Measured support, directional only.** Over 60 real game positions where a substantial tree had been
inherited (`hashfull` > 300), each position's played move compared with the same position searched fresh,
Stockfish scoring both:

| bucket | n | same move | mean loss with tree | mean loss fresh |
|---|---|---|---|---|
| endgame ≤12 pieces | 30 | 12/30 | **57 cp** | **50 cp** |
| middlegame ≥17 pieces | 30 | 21/30 | **9 cp** | **14 cp** |

The sign matches the hypothesis in both buckets — reuse slightly worse in endgames, slightly better in
middlegames — and **neither difference is significant at n = 30**. Two things in that table are clear
regardless: the engine is about six times worse in endgames than middlegames whatever the tree does, and
reuse changes the endgame move 60% of the time against 30% in the middlegame. A mechanism with that much
leverage, in the phase where the engine is six times weaker, is worth testing properly.

**So this is an option to be matched, not a default.** A position scan cannot settle it — the effect is
cumulative over a game, and such scans have already mis-sold `BalanceShare` and `VerifyReplies` in this
document. The test is a head-to-head at the real time control, `ReuseTreeMinPieces=0` against a non-zero value, same
binary both sides. A trigger that only fires in the endgame cannot touch the opening or middlegame, so the
downside is bounded.


### Measured: resetting the tree in the endgame loses about 40 Elo

`ReuseTreeMinPieces=15` against `=0`, same binary both sides, 300 games:

| | score | Elo | 95% interval |
|---|---|---|---|
| reset below 15 pieces | **132.5 / 300 = 44.2%** | **−41** | −69 to −13 |

A 35-point margin against a standard error of about 12, so this is not noise: **discarding the tree in the
endgame costs real strength**, and the hypothesis it was built to test is refuted.

**What it means.** The accumulated tree's *depth* is worth more than the occasional entrenched wrong value.
Endgames are where depth matters most and where a 2–3 second search from nothing goes least far, so throwing
the tree away each move loses more than the stale averages cost. That is consistent with the original
`ReuseTree` measurement of about +232 Elo.

**The blunders are still real.** `Gr3fzghd` 24…Kh8 and `hKCOETfO` 56…g5 are both correct 0/6 and 0/7 times
from an empty tree and wrong with an inherited one, and in the second the losing move carried 2,255,709
visits against a search of 1,537,455 simulations. Both facts hold. They are simply rarer than the benefit.

**So the correct target is narrower than the option was.** Not *discard the tree* — keep the structure, the
expansion work and the priors, and refresh only the **values**, so a new search can outvote an old belief.
Decaying `N` and `W` by a factor at each root change would do that, and would have fixed both blunders
without touching what this match says is worth 41 Elo.

**The match was played at 0.5 s a move, which is the condition most favourable to reuse.** The lichess bot
searches for roughly 2.4–5.5 s a move, and a fresh search is far stronger there: on one endgame position
from an empty tree, 500 ms gives about 152,000 simulations at seldepth 12 while 2,760 ms gives about
1,380,000 at seldepth 21 — nine times the simulations and nearly double the depth. Discarding the tree
therefore costs most at 0.5 s and less at game speed, so **−41 Elo is an upper bound on the cost under
lichess conditions, not a transferable figure.** The sign is unlikely to flip, but the magnitude is not
comparable across controls, and the same caveat attaches to every other Elo number in this document measured
at 0.5 s a move — `BalanceShare` among them.

**The threshold that was actually tested was 15, not the 12 suggested here.** That matters for how far the
result generalises. Measured over 13,297 real searches, a root has fewer than 15 pieces in **38.3%** of
searches and fewer than 12 in **25.8%**, so `=15` resets about **1.5 times as often** as `=12` would — it
reaches well into late middlegames rather than only endgames. The −41 Elo is therefore the cost of the more
aggressive setting. A smaller threshold would fire less often and so cost less, but there is no reason in
this data to expect it to turn *positive*: the mechanism is the same and the sign is against it at every
piece count the scan looked at. Treat 12 as untested and unpromising rather than as refuted.

**And a third strike for position scans.** The scan that motivated this — 60 positions, mean loss 57 cp with
the tree against 50 fresh at ≤12 pieces — pointed the *wrong way*, as `BalanceShare` and `VerifyReplies` did
before it. Its n=30 buckets were explicitly reported as not significant, and they were not merely
inconclusive but actively misleading. A position scan can explain a mechanism; it cannot rank an option.


## Decaying the weight, not the value

**The measurement this is built on.** A node's value is `Q = W / evidence`, a mean, and under `ReuseTree`
`evidence` is a lifetime total that nothing resets. Against Stockfish over 420 real search roots, the
engine's reported magnitude falls as that total grows:

| root simulations | n | median \|Stockfish\| | median \|engine\| | ratio |
|---|---|---|---|---|
| 50k–500k | 13 | 534 | 453 | 0.85 |
| 500k–2M | 101 | 447 | 373 | 0.83 |
| 2M–8M | 80 | 454 | 337 | **0.74** |

and by occupancy, 0.82 / 0.87 / 0.81 / **0.69** across 0–200, 200–500, 500–800 and 800–1000 per mille.

**CORRECTION — that trend is confounded, and the controlled test points the other way.** The table above is
*observational*: positions that accumulate huge trees are not the same positions as those with small ones,
they are later, simpler and more often endgames. A controlled comparison on identical positions says the
opposite. Replaying two games at 2 s a move with `ReuseTree` on and off:

| | median simulations | ratio |
|---|---|---|
| `ReuseTree` on | 577,128 | **0.74** |
| `ReuseTree` off | 310,956 | **0.69** |

The fresh search does 46% fewer simulations and is *more* compressed, not less — which is what UCT theory
predicts, since the average converges toward minimax as samples accumulate. So **more search reduces
compression**, and the earlier correlation with tree size should be read as position selection rather than
as an effect of the tree. Treat "a bigger tree is a more compressed one" as withdrawn.

That is the opposite of what UCT promises. The convergence proof assumes a stationary search from **one**
root; with reuse, the samples in a node's mean come from searches of *different* root positions — from when
it was a deep sideline, from when it was the ponder root, from three moves ago. More samples is then a
broader and staler mixture, not a converging estimate. In `hKCOETfO` the losing move held 2,255,709 visits
at `cp -10` against a true −618, with its own one-ply value at −259; a search of 1,537,455 simulations
cannot outvote that.

**What the option does.** When the root moves, multiply `W` and `evidence` by the same factor on the top few
plies. Because both are scaled together, `Q` is arithmetically unchanged — the engine believes exactly what
it believed a moment ago, and the ranking among the root's children is untouched because they are all
scaled alike. The only thing that changes is **how much new evidence it takes to move that belief**. The
value is preserved; its inertia is not.

**Why only the top plies.** A whole-tree pass is unaffordable: the note on `NodeArena::gen` records 183 ns
per slot once something touches each node's cache line, which is over a second on a ten-million-node tree,
every move. It is also unnecessary — the move is chosen among the root's children, and fresh leaf
evaluations reach them through backup whatever the deep tree holds.

**What is deliberately not scaled.** `N` and the edge counts, which drive the exploration term. Scaling them
would change how widely the search looks as well as how fast it learns; only the second is what the
measurement points at, and one change at a time.

**Why this rather than `ReuseTreeMinPieces`.** That option discarded the tree and measured −41 Elo, because
the accumulated *depth* is worth a great deal. Decay keeps every node, every edge, every prior and all the
expansion work — it gives up none of the depth — and targets only the quantity the table above indicts.

Measured cost: none detectable. Median simulations per search 504,022 with `keep=250 depth=2` against
467,217 with it off, and 41–1,281 nodes scaled per move at depth 2.

### Measured, and it does the opposite of what it was built to do

A self-play match at **2 s a move** — the control this option most deserved, since the compression it targets
grows with tree size — reached **22 : 21 after 43 games**, about +8 Elo with a 95% interval of −65 to +82.
Noise, and no disaster.

The mechanism test is the decisive one, and it fails. Two match games were replayed move by move through a
single engine so the tree accumulated exactly as in play, at the same 2 s, once with decay off and once with
`keep=250 depth=2`, capturing the root value at every move. Identical positions both runs, 47 of them with
|Stockfish| ≥ 100 cp:

| | median ratio, engine magnitude ÷ Stockfish |
|---|---|
| decay **off** | **0.74** |
| decay **on** | **0.50** |

Simulations were essentially unchanged (−7.7%), so this is not a throughput effect. **The option makes the
very quantity it was built to improve substantially worse.**

**Why the reasoning behind it was wrong.** Scaling `W` and `evidence` together preserves `Q` exactly *at the
instant of scaling*, and I treated that as if it remained true afterwards. It does not. Decaying only the top
two plies leaves the deep tree at full weight, so a root child ends up holding far less evidence than its own
subtree does; its `Q` is then dominated by the handful of new samples that arrive, while the exploration term
still uses the undecayed `N` and so does not widen to compensate. The inconsistency between a decayed top and
an undecayed bottom is itself a dilution mechanism. That is a hypothesis for the direction, not a proven
account of the size.

**Caveat on the number.** 45 sharp positions from two games, all in one simulation bucket — the replay's
trees never reached the 2M–8M range where the original compression was worst. The magnitude is imprecise;
the direction is not.

**The standing finding is unaffected.** Bigger trees are still more compressed — 0.85, 0.83, 0.74 by
simulations and 0.69 above 800 per mille. What this shows is that the compression cannot be fixed by
rescaling the counters, at least not in the top few plies while the rest of the tree keeps its weight.


## Is this an MCTS problem?

**Answered, 5 October 2026 — yes, and it is not repairable by reallocating visits.** See *Why no search
change survived: the allocation law and the test-set artefact* at the end of this document: a
non-leading move's share of the search is `C*P/(gap*sqrt(D))` and decays as `1/sqrt(D)`, the
concentration that causes it IS what buys depth on the principal line, and forcing a constant share
instead made the deep-refutation bucket 3.5x worse. The section below predates that measurement.

**Answered more fundamentally, 6-8 October 2026 — the algorithm is matched to the wrong domain.** See *Why
MCTS is the wrong fit: a probabilistic search over a deterministic estimator* at the end of this document.
MCTS averages because it was built for domains where a position returns a different number each visit; a
deterministic evaluator returns the same number, and the game defines a maximum rather than a mean, so the
search spends its compute converting an average into a maximum that alpha-beta has at the first node. The
measured cost is two to three times. Eight search changes have now been measured and none survived.

Essentially yes, and it is worth being exact about which part.

**The cause is the backup rule, not tree reuse.** `Q = W/evidence` is a mean over every leaf a subtree has
visited, and most of those leaves lie on lines nobody will play. That is true of a one-move-old tree as much
as a ten-million-node one, which is why turning `ReuseTree` off does not remove the compression (0.69 against
0.74) and why every fix aimed at the *tree* failed — discarding it in the endgame cost 41 Elo, decaying its
counters took the ratio from 0.74 to 0.50.

**What reuse does is make a wrong average immovable.** It grows the sample count, so in `hKCOETfO` the losing
move held 2,255,709 visits at `cp −10` against a truth of −618 and a search of 1,537,455 simulations could
not outvote it. Reuse converts a soft error into an entrenched one; it does not create the error.

**But it is not a permanent property of MCTS.** UCT's estimate converges toward the minimax value as samples
accumulate, and the controlled comparison above shows that happening. The compression is a *finite-sample*
effect, and how severe it is at a practical budget depends on how the visits are distributed.

**Which makes it a prior problem wearing a search problem's clothes.** A sharp policy concentrates visits on
a narrow set of lines, so the mean is dominated by the principal variation and sits close to the minimax
value. A weak policy spreads visits over many mediocre lines, so the mean is a mixture and compresses. That
is the same bottleneck recorded throughout this document: creatica's priors are weak, its tree is wide, and
the value it reports is the average of that width. Compression is a symptom of the policy, not an independent
defect of the search — which is also why five separate search-level interventions all failed to move it.


## Removed from the engine

Nine options were taken out after measurement, with all their code. They are documented above and in git
history so the results are not lost, but they are no longer tunable and no longer clutter the option list —
26 options remain where there were 35.

| removed | measured |
|---|---|
| `BalanceShare`, `BalanceDepth` | −150 Elo |
| `MinimaxBackup`, `MinimaxMinEv` | ±0 — 48.8% over 126 games |
| `VerifyReplies`, `VerifyAfter` | caused a forced mate to be walked into: 0/12, 4/12, 12/12 blunders as the floor rose |
| `RootDecayKeep`, `RootDecayDepth` | compression 0.74 → 0.50, worse than doing nothing |
| `ReuseTreeMinPieces` | −41 Elo |

The compression diagnostic went with them — the second accumulator carrying each backup unsquashed, and its
per-search log line. It had answered its question: the `tanh` accounts for only 4–6% of the compression.

**What the whole exercise established, and what survives it.** Creatica's value is compressed to roughly
60–70% of the truth; that is mixture dilution in the mean backup, not the squashing; it is not caused by tree
reuse (turning reuse off gives 0.69 against 0.74); and more search reduces it, as UCT predicts. Its severity
is set by how widely the visits spread, which is set by the priors. Five separate search-level interventions
failed to move it, which is the strongest evidence in this document that **the policy is the bottleneck and
the search is not where the fix lives.**


## The defaults changed, September 2026

A bare `./creatica` used to run the 256/128 net at a scale calibrated for it, with both tree resets off and
the collection inline. That was not the engine being developed — and it is what `test_smp`, `tournament` and
any unconfigured side of a GUI match actually ran, so every comparison against "default creatica" was
against a handicapped baseline.

| option | was | now | evidence |
|---|---|---|---|
| `PolicyWeights` | `nnue_policy.bin` | `nnue_policy_pisp2.bin` | best net measured: 39.52 Top-1, 74.94 Top-4 |
| `BlendScale` | 115 | **102** | calibrated for `pisp2`; must move with the net |
| `CollectDuringPonder` | false | **true** | 0 collections on the move's own clock against 13 costing 5,384 ms; ~8% more simulations |
| `TreeResetBelow` | 0 | **1000** | mechanism plus live use; the match is inside the noise |
| `TreeResetHollow` | 0 | **900** | same |

**`BlendScale` and `PolicyWeights` are a pair.** The blend constants are calibrated per net, so changing one
without the other leaves a sharp net flattened by a scale meant for a blunt one — worse than either old
default alone.

**Honesty about the two reset values.** 154.5 : 145.5 over 300 games is 51.5%, about +10 Elo against a
standard error near 12. They are defaults because the mechanism is sound — a frozen tree learns nothing, so
discarding it costs little, and the rule only fires at `TreeResetOccupancy` or above — and because they have
run for weeks in live play without trouble. Not because a match showed they help.

**This moves every baseline.** Any future A/B where one side is given "no options" is now a different and
stronger engine than it was before this change.

## Where the limit actually is: MCTS, alpha-beta and Stockfish on the same positions, 3–4 October 2026

This is the longest measurement run in this document and it answers the question *Is this an MCTS
problem?* above with numbers rather than argument. Everything here uses the **same 171-position
mistakes test set** (`mistakes/testset.tsv`, drawn from the bot's own games) and, where an evaluator is
involved, **the same two nets creatica embeds** — verified identical: `nn-c288c895ea92.nnue` and
`nn-37f18f62d772.nnue` are the defaults of both creatica and the installed Stockfish 18.

Scripts and binaries for all of it live in `$EXPERIMENTS/` -- a scratch directory kept OUTSIDE the repo,
both so a reboot does not clear it the way `/private/tmp` does and so that multi-gigabyte intermediates
never risk being committed. Set it to wherever you keep yours; every path below is relative to it.

### How the set is scored, and why not as a rate

Four repeats per position, and each position is classed **always solved**, **never solved** or
**sometimes solved**. The headline number is how many positions move between *never* and *always*,
because that is the only movement that says something about the mechanism. A change that shuffles
positions inside the *sometimes* band has not done the same kind of thing at all.

The noise floor for class movement is known, because two independent runs of the *same* unmodified
search are available: they disagree on **one** position in one direction and **zero** in the other. So
a change that converts fifteen positions is doing something real; a two-point difference in the
aggregate rate is not.

### 1. Exploration: the curve never turns, so no constant is the answer

`ExplorationRoot` applies its own constant at depth 0 only. Three repeats per setting, in-batch
control:

| `ExplorationRoot` | solved |
|---|---|
| 0 (default schedule) | 35.7% |
| 2 | 32.5% |
| 10 | 40.4% |
| 20 | 44.6% |
| 50 | **49.1%** |

It never turns inside the range the option allows, and the cost is real: the top move's visit share
falls from 89.9% to 55.5%, and simulations from 676,288 to 578,895. **A monotonically rising sweep is a
diagnostic, not a setting** — it says no single constant is sufficient, because the quantity a
refutation has to overturn grows with the tree.

Why it cannot be fixed by making the constant adaptive: seven candidate "this node is tactical" signals
read off the root's own children — best-minus-mean child value, best-minus-second, standard deviation,
how often the leader changed, how far its value drifted, and two more — were tested for their ability to
separate the never-solved positions from the always-solved ones. **Every AUC landed between 0.47 and
0.62, against 0.5 for no information at all.** The search cannot tell which positions it is about to get
wrong. One number from that run is worth keeping: the best-minus-mean child value has a median of
0.197, while the best-minus-**visit-weighted**-mean is 0.0026, seventy-five times smaller. By the end of
a search the moves still being visited all agree with each other, and the move that would disagree has
stopped being visited.

### 2. `ProveTerminals`: propagating exact verdicts converts nothing

Off by default. A node is *proven* when its outcome is exact — mate, stalemate, a tablebase hit — and
exactness composes by backward induction: a node with one child lost for the child's mover is won, and a
node all of whose children are won for their mover is lost. Proofs override PUCT in selection, back up
as ±1 rather than as evaluations, and outrank every visit count at the root.

It fires at scale: across 684 searches it stamped **2,957,835** tablebase verdicts and proved **296,872**
nodes won and **93,362** lost. The result:

| | solved | always | never | never → always |
|---|---|---|---|---|
| off | 36.1% | 39 | 86 | — |
| on | 34.4% | 34 | 83 | **0** |

**Zero conversions in either direction.** Two implementation notes that cost real time and are worth
not repeating. The verdict rides in the unused high bits of `terminal` because `MCTSNode` is pinned at
64 bytes; an in-check node is stored as the plain `int` −1, which has *every* bit set, so a bare mask
read both proof bits out of it and declared every check a forced mate — the principal variation
collapsed from nineteen plies to two. And the first version stamped proofs only in `make_move_policy()`,
which the blended prior never calls, so the counter read **exactly zero** across 684 searches; the site
that matters is the tablebase branch in `position_eval()`.

### 3. `MinimaxWeight`: the backup operator, and the first thing that moves a deterministic failure

Off by default (0). Every node carries `mm`, the best value its subtree has been shown to reach, in
centipawns from its own side to move, recomputed on backup as the maximum over its children of the
negation of theirs. Selection scores a child on `(1 − w)·mean + w·minimax`.

It cost no memory: `expanding` is one byte at offset 36 and `descents` needs four-byte alignment at 40,
so bytes 37–39 were padding and a two-byte `int16` fits there. The `static_assert` on 64 bytes still
passes. The value is recomputed in full rather than kept as a running maximum, because a maximum that
only rises is a high-water mark — when a child's value falls the parent's must fall with it.

| weight | solved | always | never | median simulations | never → always | always → never |
|---|---|---|---|---|---|---|
| 0 (control) | 34.6% | 40 | 90 | 599,283 | — | — |
| 0.50 | 46.2% | 63 | 74 | 375,669 | **15** | 2 |
| 0.60 | 47.4% | 66 | 77 | 373,987 | **15** | 3 |
| **0.75** | **48.7%** | 65 | 71 | 351,387 | **15** | 2 |
| 1.00 | 44.3% | 61 | 77 | 61,956 | 14 | 4 |

A genuine interior optimum, unlike the exploration sweep — and pure minimax being worse is what the
reasoning predicted, since the maximum of many noisy estimates is wrong upward.

**The smaller tree is not the explanation.** The blend costs 41% of the simulations, so a
simulation-matched control was run: weight 0 at 1,170 ms, which reaches 325,270 simulations — *fewer*
than the blend's 351,387. It scores 33.6% and converts **zero** positions. Halving creatica's search
changes nothing about which positions it fails on.

The affected positions are mostly weight-specific: 24 distinct positions convert for at least one
weight and only **four** convert for all of them, so no single global constant collects much more than
0.75 already does.

### 4. Why that result matters more than its size

The two results above, taken together, say something the engine's design has been assuming the opposite
of. Propagating **exact** verdicts converted zero positions. Propagating **maxima over ordinary
estimates** converted fifteen, while changing no evaluation anywhere — same net, same policy, same child
evaluations, same seeds. So what MCTS lacks here is not access to certainties. It is the **backup
operator applied to guesses**: `Q` is a mean where alpha-beta's root value is a minimax, and that is
true of estimates and not only of proofs.

### 5. How good the birth value is, and how deep it would have to be

Full-width negamax over every legal root move, no pruning, no ordering, nothing skipped, same NNUE.
Depth 1 is exactly what the engine seeds a child with today.

| | top move acceptable | leaf evaluations |
|---|---|---|
| depth 1 — today's seed | **21.6%** | ~34 |
| depth 2 | 30.4% | ~1,000 |
| depth 3 | 37.4% | ~37,000 |

About eight points a ply, straight. For comparison, creatica's MCTS at 600,000 simulations scores
34.6% — so **two seconds of MCTS is worth about three plies of brute force**, and an exhaustive depth-3
search costing a sixth of a second scores higher.

Of the 90 positions MCTS never solves, a depth-3 birth value gets **27** right, against 19 for the
minimax backup, for a twentieth of the cost. Of the 57 that neither MCTS arm ever solves it gets 12.

### 6. Stockfish on the same positions: it is not the evaluator

This is the control that should have been run first. Same nets, same clock, one thread.

| engine, 2 s a move | solved | mean depth | mean nodes | of the 90 | of the 57 |
|---|---|---|---|---|---|
| creatica MCTS, 4 threads | 34.6% | ~3 effective | 600,000 sims | 0 | 0 |
| a plain alpha-beta probe, 1 thread | 53.8% | 5.6 | 1,379,969 | 38 | 23 |
| **Stockfish 18, 1 thread** | **94.2%** | **23.9** | **1,739,264** | **82** | **52** |
| Stockfish 18, 4 threads | 95.9% | 22.2 | 4,532,535 | 85 | 55 |

**The information is in the net.** Any claim that creatica's evaluation is the ceiling on a set of
positions is unfounded until the reference engine has been run on the identical positions, because it
shares the net. A plateau measured with a weak searcher is a fact about the searcher.

The one number that explains the table: effective branching factor — nodes to the power of one over
depth — is **1.83 for Stockfish and 12.54 for the probe**. Per ply Stockfish pays 1.83 times the nodes
and the probe pays 12.5, and compounded that is the whole difference between depth 6 and depth 24 at the
same cost.

Stockfish's ten misses are not failures of the set: in every one its two-second move appears in that
position's own `sf_lines` at rank 2 to 6, and the `acceptable` column is a strict subset of that list.
Seven are near-misses of 71 to 122 cp. Two are real: at `e9o2RVij` ply 95 it misses a forced mate, and
at `j3zkChdp` ply 111, at depth 27, it plays a move its own deeper analysis scores at −19960. The
labels came from a MultiPV 8 run, which orders the top moves far more carefully than a single-PV search
of the same length, so **94.2% is not a ceiling and not the right yardstick** — the set's own labelling
conditions are.

### 7. What depth the set actually needs, and what it costs

Asked directly, rather than inferred from Stockfish's reported depth — which is not the same unit as a
near-full-width depth, because reductions make it selective.

| Stockfish at fixed depth | solved | mean nodes |
|---|---|---|
| 4 | 40.9% | 620 |
| 6 | 40.9% | 1,544 |
| 8 | 55.6% | 4,158 |
| 12 | **73.1%** | **25,546** |
| 14 | 82.5% | 73,351 |
| 16 | **89.5%** | **200,553** |

**Depth 12 costs 25,546 nodes and depth 16 costs 200,553. Creatica spends 600,000 simulations in the
same two seconds.** The compute for the depth these positions need is already in the engine three times
over. It is not the evaluator, not the time control, and not an unreachable depth — it is the number of
nodes each ply costs.

(Depth 4 and depth 6 scoring identically at 40.9% despite 2.5 times the nodes is the only
non-monotonic point in the curve and is unexplained.)

### 8. What the ordering and pruning machinery is worth, and where it stops

Measured on the probe so the contribution of each mechanism is separable. The mean position here has
**32.3 legal moves**, so alpha-beta that searches every move has a floor of √32.3 = **5.68** on its
branching factor, reachable only with perfect ordering. Stockfish runs at 2.1 — *below* that floor,
which is only possible by not searching everything to full depth.

| rung | solved | mean depth | branching factor |
|---|---|---|---|
| captures-first by victim, no pruning | 49.7% | 5.3 | 11.33 |
| + killers and history | 51.5% | 5.5 | 10.46 |
| + null-move pruning | 53.8% | 5.8 | 9.04 |
| + late move reductions | 54.4% | 7.1 | 5.92 |
| + both | 53.8% | 7.5 | **5.45** |

Ordering buys about two points and a tenth of the branching factor; **pruning buys five points and
nearly halves it**. Killers and history are worth noting separately because they are *not* hand-crafted
chess knowledge — a killer is "this move caused a cutoff here a moment ago" and a history score is "this
from-to pair has been causing cutoffs", both statistics the search accumulates about itself at runtime,
costing a table lookup. The learned policy head, by contrast, costs 13.76 µs a node and was worth 1.7
points when used on the top three plies only.

**This line of work is closed.** Arkadi's ruling: "we are here not to reconstruct Stockfish." The probe
was built to diagnose and it has; its mechanisms are not a plan for creatica.

### 9. Four defects in the probe, and why they are recorded here

Absolute solve rates from a young search are far less trustworthy than differences between its arms, and
every number in sections 5 to 8 was produced by code that had at least one of these live at some point.

1. **Proof bits colliding with the in-check encoding** — described in section 2.
2. **`board.isCheck` read before the position's own moves were generated.** It looks like a field of the
   position and is not: it is a cache that `king_moves()` writes as a side effect, valid only for the
   position `kingMoves()` was last called on. Reading it at node entry returns the *parent's* answer.
   That let null-move pruning fire while the side to move was in check, the opponent captured the king,
   and the next generator call dereferenced a null pointer. Compute it instead:
   `getCheckers(board, lsBit(side[stm] & pieceTypes[King-1]))`, verified against the library's own
   answer over 567,436 nodes with zero disagreements.
3. **An unbounded check extension.** `if (in_check) ++depth` with no limit; in a forcing sequence the
   depth never decreases. Worth **+5.8 points** to turn off entirely, and the curve is monotonic in the
   limit — 0 beats 2 beats 4 beats unbounded.
4. **In-check leaves scored as a dead draw.** `evaluate_nnue()` returns its `NNUE_CHECK` sentinel in
   check, and `eval_cp()` mapped the sentinel to 0. Worth **+13.5 points** against the thing that
   replaced it, because "evade, then evaluate statically" is a horizon effect in the middle of a
   tactical sequence. Both are wrong; the engine's own `process_check()` has the right answer, a one-ply
   minimax over the evasions.

Defects 2 and 4 **masked** each other, which is the general warning: a search has no oracle, and one
component's error routinely hides another's. The first "correct" version measured 22 points *worse* than
the buggy one. The only trustworthy signals are the ones that cannot be rationalised — a crash, a
corrupt board, a counter reading exactly zero.

### 10. What a per-move value model would have to achieve, and where the data is

Taking Stockfish's own per-move cp from `sf_lines` as truth and adding independent Gaussian noise to
every move, then taking the argmax:

| noise σ | top-8 only | plus 24 unlabelled moves |
|---|---|---|
| 30 cp | 98.4% | 98.3% |
| **50 cp** | 93.1% | **91.6%** |
| **100 cp** | 76.5% | **69.8%** |
| 200 cp | 57.6% | 41.1% |

**σ ≈ 50 cp buys about 90%, σ ≈ 100 cp about 70%.** NNUE's measured error is a mean absolute 163 cp with
a 90th percentile of 485, which lands near the 200-cp row — and the one-ply static argmax measures
21.6%, so reality is at or below that. The requirement is a factor of two to four, not fifty.

Two consequences. The **tail** is what matters, not the average: the argmax over 32 moves is decided by
the worst-behaved move, so NNUE's median of 64 cp is misleading and its 90th percentile of 485 is the
number that hurts. And the quantity to optimise is not absolute accuracy at all but the error on **value
differences within one position**, because a model that is uniformly 80 cp optimistic in a position ranks
that position perfectly. The noise model above assumes independent errors, which is the pessimistic case;
real model errors are correlated across the children of one position, so the true requirement is likely
looser.

Nothing currently trained on this corpus optimises that. NNUE is fitted to absolute evaluations and the
policy head classifies which move Stockfish would play.

**Where the labels are.** The Stockfish eval shards carry a cp for *every* PV — the format is
`[16 bits] PV1 cp`, `[4 bits] num_pvs-1`, then per PV `[6] from [6] to [2] promo [16] cp`, with the cp
omitted only for PV1 — and the writer's own measurement over 200,000 records gives 1 PV 61.7%, 2 PVs
8.1%, 3 PVs 15.0%, 4 PVs 2.7%, 5 PVs 12.5%. So 38.3% of records carry two or more moves with values.
But every move they list is one Stockfish rates highly, so they teach how good moves compare and nothing
about what a bad move looks like.

The **broadcast PGNs** supply exactly that. `lichess_db_broadcast_2020-2025.pgn` is 3.7 GB and 22.76
million lines, and nine monthly `lichess_db_broadcast_2026-MM.pgn.zst` archives sit beside it. Every
move carries `[%eval]`, so each one yields the value of the move a human actually played — and in human
games that is sometimes a mistake.

**Extracted in full on 4 October 2026: 81,909,315 rows from about 1.29 million games, 1.5 GB gzipped**,
one shard per source file in `$EXPERIMENTS/movevals/`. Validated over every row: zero
malformed rows, zero malformed moves.

| source | games | annotated moves | rows |
|---|---|---|---|
| `lichess_db_broadcast_2020-2025.pgn` | 992,278 | 61,388,019 | 61,330,230 |
| the nine 2026 monthly archives | ~297,000 | 20,601,933 | 20,579,085 |

| | |
|---|---|
| median loss | 10 cp |
| 90th percentile | 80 cp |
| 99th percentile | 490 cp |

| loss at least | rows | share |
|---|---|---|
| 25 cp | 21,587,955 | 26.4% |
| 50 cp | 12,871,955 | 15.7% |
| **100 cp** | **6,633,901** | **8.1%** |
| **200 cp** | **2,850,745** | **3.5%** |
| 400 cp | 1,094,481 | 1.3% |
| 800 cp | 517,915 | 0.6% |

So **6.6 million labelled bad moves with magnitudes**, which is the supervision the tail requires and
the Stockfish shards cannot supply.

Three things a consumer of this file has to know.

**The mean is meaningless; use the percentiles.** 1,263,960 rows (1.54%) have a mate score on one side
of the pair and an ordinary evaluation on the other, and since a mate enters the file as ±(20000 − n)
those rows carry a mean absolute loss of 11,867 cp. They are perfectly valid — a move that throws away
a forced mate really did give away everything — but they drag the mean over the whole file to 203 cp
against a median of 10, and a regression on raw `loss` would spend most of its gradient on them. Over
the 80,645,355 rows with ordinary evaluations on both sides the mean loss is **31.8 cp** and the mean
absolute **43.3**, with 6,324,321 (7.84%) losing 100 cp or more. Either squash the target the way the
engine already does with `tanh(cp/610)`, or bucket the mate rows separately.

**The label noise is low but not zero.** 265,274 rows (0.32%) show the move *gaining* more than 100 cp,
which cannot really happen — a move cannot improve on the best move — so each one is either evaluation
noise between consecutive positions, since lichess's broadcast analysis runs at variable depth, or a gap
in the annotations that the before/after pairing did not catch. That 0.32% is a floor on how much of the
`loss` column is noise rather than signal.

**`src` is a quality filter, not just provenance.** The 2026 archives are annotated at 94% of moves
against 69% for the five-year file, and they produced **zero** SAN failures in eight of nine months
against 3,977 games in the older one. So 20.6 million of the rows are from the materially cleaner half.

Extraction cost about twenty minutes for the whole corpus. Three things had to be fixed to get there,
all recorded in `pgn_movevals.cpp`: the lifted `extractEval` constructed its `std::regex` as a local,
so it was recompiled once per move — a hundred million times — and making it `static` is what brought
the big file under ten minutes; the first version buffered all of stdin and then copied it, about 7.4 GB
of peak memory for a 3.7 GB file on a machine with 8 GiB, so it now streams `std::cin` a line at a time;
and the output goes to a named file, with a `.gz` path streamed through `gzip`, because `san2move()`
prints an error line *and an ASCII board* to stdout when a game desyncs and that landed in the middle of
the data. Dedup is available (`DEDUP=1`, worth about 8% of rows, almost all repeated openings) but was
deliberately left off for the full run: its hash set needs 3–4 GB, and `MIN_PLY` achieves the same thing
at no memory cost.

### What the incumbent evaluator scores on that metric

`movevals_nnue_err.cpp` measures NNUE on the quantity itself rather than on a proxy. Each row of the
dataset gives the true per-move loss, and NNUE can estimate the same thing from two evaluations:
`est_loss = evaluate_nnue(position) + evaluate_nnue(child)`, the second added rather than subtracted
because the child's side to move is the opponent. 400,000 rows from the clean 2026 shards, mate rows
excluded, error reported as estimate minus truth:

| true loss | rows | mean error | mean absolute | sigma | median |
|---|---|---|---|---|---|
| all | 400,000 | −56.7 | 81.5 | **279.2** | −15 |
| under 25 cp | 291,884 | −25.6 | 56.6 | 168.0 | −7 |
| 25–99 | 72,601 | −51.0 | 62.5 | 85.1 | −37 |
| **100–199** | 19,925 | **−131.2** | 133.9 | 89.5 | −120 |
| **200–399** | 10,296 | **−266.3** | 266.5 | 105.3 | −251 |
| **400+** | 5,294 | **−1161.4** | 1161.4 | 1673.9 | −570 |

The method validates itself on the quiet rows: for moves that lose nothing the estimated loss is about
−5 cp against a true value near 2, so the two evaluations are properly antisymmetric across a good move.

Then the tail. For moves truly losing 200–399 cp the mean error is **−266**, so the estimated loss is
about **+14 cp**; for moves losing 400 or more the error is again roughly equal and opposite to the loss
itself. **NNUE does not mis-size the big mistakes, it does not see them** — its estimate of the loss is
approximately the negation of the loss, which is what "reports a blunder as a fine move" looks like
numerically.

That closes the loop on this whole section. The static one-ply argmax scores 21.6% because the one-ply
value rates the losing move as fine; the lock-out then fixes that judgement inside the first 3% of the
search; and no exploration constant, backup operator or proof propagation can recover a move whose birth
value says nothing is wrong. Against the requirement — sigma about 100 cp for 70% of the test set and
about 50 for 90% — the incumbent sits at **sigma 279 overall, with a conditional error on the tail equal
to the tail**.

The open question, stated honestly: the losses in that tail are TACTICAL, visible only after a search,
and how much of that is learnable statically is not known. Stockfish's net is already trained on searched
evaluations and still shows this blindness at one ply. So a trained comparator is a well-posed experiment
with a measured baseline, not a solved problem.

### A narrow learned search: the containment curve and the one number that gates it

Arkadi's proposal, on 5 October 2026, after everything above: "MCTS is incapable of amplifying the best
move out of top-6, which covers 80% and would reduce branching from 12 to 6! alpha-beta with amazing
branching 2 requires hand-crafted move ordering and pruning. And I'm sure there ways to improve the
model as well. The knoweledge that is presented into stockfish euristics, can be transferred to a
policy head."

This section is the arithmetic and the measurements for that. **It also reopens move narrowing**, which
had been a standing no — `ProbabilityMass` was tested twice and lost both times — so the ban no longer
applies, but the reason it failed does: a gate DELETES a move from a node permanently, and under tree
reuse that outlives the move.

#### The prize, and why the sharp column is the only one that matters

Searching k moves a node instead of about 32 takes alpha-beta's perfect-ordering floor from
sqrt(32) = 5.68 to sqrt(k). At creatica's existing budget of roughly 1.7 million nodes that converts
straight into depth, and depth converts into accuracy by Stockfish's measured fixed-depth curve.

Measured on 40,000 of the labelled positions, mean 28.2 legal moves: how often is the move a depth-12
search prefers inside `nnue_policy_pisp2.bin`'s top k?

| k | all positions | **sharp positions** | floor sqrt(k) | depth at 1.7M nodes | expected rate |
|---|---|---|---|---|---|
| 1 | 34.8% | 65.1% | 1.00 | — | — |
| 2 | 51.6% | 79.6% | 1.41 | 41 | 94.2% |
| 4 | 69.5% | **89.8%** | 2.00 | 20.7 | 92.3% |
| **6** | 79.3% | **93.8%** | 2.45 | **16.0** | **89.5%** |
| **8** | 85.7% | **95.8%** | 2.83 | 13.8 | 81.6% |
| 10 | 90.0% | 97.2% | 3.16 | 12.5 | 75.3% |
| 16 | 96.5% | 98.9% | 4.00 | 10.3 | 63.4% |

**Only 5,703 of the 40,000 positions (14%) are sharp** -- the best move worth a pawn or more over the
second. In the other 86% several moves are near-equal and losing "the best" one costs under 100 cp,
which a search absorbs. Dropping a move only matters in the sharp 14%, and there containment is far
better than the headline: 93.8% at k=6 against 79.3% overall, because the policy head's misses
concentrate exactly where the choice does not matter.

The per-node failure rate that follows is P(sharp) x P(miss | sharp) = 0.14 x 0.062 = **0.9% at k=6**,
so about 87% of 16-ply lines keep every decisive move. At k=8 it is 0.6% a node over 14 plies, about
92%. **So the design point is k = 6 to 8: depth 14 to 16 at the budget the engine already spends, which
the measured curve values at 82-89% against the 34.6% it gets now.**

Two caveats. The depth and rate columns assume PERFECT ordering inside the retained set, so they are
upper bounds -- though Stockfish runs at 1.83 with full width where the full-width floor is 5.68,
because pruning takes it below the floor, so the floor binds in neither direction. And the compounding
risk is real: 79.5% containment per node is 0.795^d if the dropped moves are gone for good. The remedy
is to REDUCE rather than EXCLUDE -- search the first few at full depth and the rest shallower,
re-searching anything that beats alpha -- which measured 11.33 to 5.92 on the branching factor without
excluding anything.

#### The spatial term cannot be dropped

`nnue_policy_pi.bin` is format 5 -- piece-indexed with the legality term and no spatial block -- so it
is the fair comparison rather than an ablation, and the library refuses an ablation anyway:
`policy_score()` fatals when a spatial net is scored without its buffer, because that "would score
moves by a different function".

| | sharp containment k=6 | k=8 |
|---|---|---|
| `pisp2`, with the spatial term | **93.8%** | 95.8% |
| `pi`, without it | 87.3% | 91.1% |

Worth 6.5 points where it matters, which doubles the per-node failure rate from 0.9% to 1.8% and over
16 plies is the difference between 13% and 25% of lines losing a decisive move.

#### The one number that gates the whole design

`polcost2.cpp`. Components on a hot accumulator, 20 legal moves, then the marginal cost measured by
walking a depth-3 tree with `do_move_dp` and the accumulator pushed and popped as a search does:

| | |
|---|---|
| `nnue_features` (read the accumulator) | **0.15 us** |
| `policy_context` (1024 -> 256 -> 128) | **5.69 us** |
| `policy_build_planes` (26 x 64) | 0.47 us |
| `policy_spatial_forward` (conv + projection) | **7.38 us** |
| `policy_score` over 20 moves | 1.41 us |
| summed | 15.10 us |
| bare walk (movegen, do_move, undo) | 0.35 us/node |
| walk + `evaluate_nnue` every node | 2.66 us/node |
| walk + the full policy forward every node | 19.91 us/node |
| **MARGINAL cost of the policy per node** | **19.56 us** |
| marginal cost of an evaluation, for scale | 2.31 us |

Feature extraction is already free at 0.15 us, so the incremental accumulator is doing its job -- the
hope that a search would amortise part of the policy cost is dead. The marginal cost in a search is
**higher** than the 13.76 us standalone figure, because between nodes the caches are cold rather than
hot on one repeated board. Against a budget of about 1.2 us a node, **the policy is 16 times too
expensive**, and it costs 8.5 times an evaluation. Two dense matmuls are 87% of it: the trunk at 5.69 us
and the spatial convolution plus projection at 7.38 us.

**So the blocker is now a single precisely-sized engineering problem: get the policy forward from
19.6 us to about 1.2 us without losing the spatial term.** Three levers, and they multiply:
int8 quantization of both matmuls -- NNUE itself runs a 1024-wide transform plus its layers in 1.42 us
with int8 SIMD, so 4-8x here is what the surrounding code already achieves; a narrower trunk, since
1024 -> 256 -> 128 is 295k MACs and halving both widths is 4x on that matmul at a cost the containment
curve can price; and caching the context vector, which depends only on the position's features and not
on its move list, across the revisits that iterative deepening produces.

That is a different kind of problem from everything else in this section. It is an implementation target
with a known number rather than a research question.

#### Making the policy forward affordable: what worked, and five things that did not

The design above needs the policy at EVERY node, so the whole question is whether its 19.56 us can
become about 1.2 us. The answer splits cleanly in two: the trunk is solved by batching and the spatial
term resisted everything.

**The trunk: solved, by batching, and the margin is large.**

| batch | libtorch addmm | Accelerate `cblas_sgemm` |
|---|---|---|
| 1 | 11.95 us | 8.24 us |
| 8 | 1.55 | 1.11 |
| **64** | 0.64 | **0.60** |
| 256 | 0.55 | 0.56 |

Against 5.63 us for the hand-written float version one position at a time. The two libraries converge,
which says this is the hardware rather than either library being clever: 18.9 million MACs in 38 us is
about 455 GMAC/s, roughly nine times what hand-written NEON reaches, so Apple's matrix coprocessor is
doing it. A 1024x256 matmul is exactly the shape it wants.

**libtorch cannot be used per call, and a newer version would not change that.** A single EMPTY op costs
**1.28 us** of dispatch overhead -- the entire per-node budget before any arithmetic -- and the trunk
through libtorch is 11.95 us against the hand-written 5.63. The installed 2.12.0 is current; the floor
is inherent to dynamic dispatch. Sparse tensors are worse again: the plane input is only 8% dense, and
every sparse formulation tried here lost to dense SIMD (below).

**The spatial term: not solved.** 7.39 us hand-written, 7.10 us a position batched at 64, and the five
attempts below all lost. Batching cannot help it because every GEMM inside it has K=26 and N=8, far too
small to amortise anything, so it runs at memory speed however many positions are stacked.

| attempt | result |
|---|---|
| scatter over set input bits, float | 6.57 us, **1.12x** -- bit-exact |
| gather per output square, float | 16.00 us, **0.46x** -- bit-exact (1.9e-06) |
| gather per output square, int8 | 8.27 us, **0.89x** -- 4.6% of peak magnitude |
| int8 trunk, one scale per tensor | 6.09 us, **0.92x** -- and 73% error, scale far too coarse |
| batched GEMM, nine taps through Accelerate | 9.11 us a position at batch 64, worse than `conv2d` |

**Why all of them lost, which is the transferable lesson.** The planes are strictly binary -- the
maximum byte value is 1 -- and only 135.6 of 1664 cells are set, 8.1%, so the dense convolution performs
119,808 multiply-accumulates of which 92% are multiplications by zero. Cutting 92% of the multiplies
made it SLOWER. The library's loop is eight independent float adds streaming over contiguous memory with
no branch in the innermost statement: near-perfect instruction-level parallelism. Every sparse
formulation replaces that with a `ctz`-driven loop carrying a serial dependency through the same eight
accumulators, an unpredictable branch per iteration and a border test per tap, and pays back more than
it saves. **At this size the arithmetic was never the bottleneck**, and reasoning from operation counts
predicted the opposite of what happened three times in a row. The existing implementation has clearly
already been through this: its comments record measured penalties for exactly these alternatives.

**So the only lever that works on the spatial term is doing less arithmetic.** Priced with the library's
own kernel, one conv layer, mean over the 171 positions:

| planes | channels | MACs | us/position | vs 26x8 |
|---|---|---|---|---|
| 10 | 4 | 23,040 | 4.12 | 1.9x |
| 10 | 8 | 46,080 | 4.88 | 1.6x |
| **14** | **4** | 32,256 | **3.18** | **2.5x** |
| 14 | 8 | 64,512 | 5.54 | 1.4x |
| 20 | 4 | 46,080 | 3.78 | 2.1x |
| **26** | **8** | 119,808 | **7.80** | current |
| 26 | 16 | 239,616 | 14.60 | 0.53x |

Sub-linear in the MAC count -- 3.7x less arithmetic buys 2.5x less time -- because the plane unpacking,
the padded-buffer memset and the projection do not shrink with the channels. **Channels are the stronger
lever than planes.** And the floor is about 3 us, which is still 2.5x over the whole per-node budget.
Any of these shapes needs a retrain, and the containment curve above is how to price what it costs.

#### Which settles the algorithm question, in MCTS's favour

The 1.2 us budget belongs to a NARROW ALPHA-BETA at depth 16, where each node's bound depends on the
last, so the policy is needed 1.7 million times a move, sequentially, and cannot be batched. On that
design the spatial term alone is 2.5x over budget at its floor.

In MCTS the policy is needed once per EXPANSION, about 75,000 times a move rather than 1.7 million --
which is why creatica can already afford it at 13 us, some 13% of expansion cost. With the trunk batched
to 0.60 us the whole forward would be roughly 8 us an expansion, about 0.6 seconds of thread time out of
eight. And MCTS is the only one of the two algorithms that CAN batch, because virtual loss exists
precisely so that several threads hold different leaves in flight.

**So the expensive learned model is affordable in a search that batches and amortises it over
expansions, and unaffordable in one that needs it at every node.** That is an argument for the narrowing
to top-k happening inside MCTS rather than inside alpha-beta -- which keeps the algorithm Arkadi wanted
to keep for its genericity, and uses the one structural advantage it has over the alternative.

#### `TopKMin` / `TopKWiden`: progressive widening, and what it measured instead

Off by default, and with `TopKMin` 0 the search is bit-for-bit the incumbent's. `k(N) = TopKMin +
floor(TopKWiden * log2(1 + N))` over the node's own DESCENTS, applied to the children in the order
`expand_node()` publishes them, which is descending prior order -- so the narrowing is a bound on the
selection loop and nothing else.

**It is not `ProbabilityMass`.** That option deleted the tail at expansion, before the child
evaluations, so a dropped move contributed nothing and under tree reuse the truncation outlived the
move. Here every legal move is still expanded and still carries an edge. Three places that matters, and
tree reuse is the least of them: the node's own one-ply minimax value is a `max` over EVERY child, so
the tail still sets the parent's `cp` immediately; a node widens as it accumulates descents, which
happens exactly where the search is concentrating; and only then does the width persist across moves.
The cost is that the tail is still evaluated and still materialised, so the arena fills about five times
faster than a genuinely narrow search needs -- roughly a ply given away to nodes never visited again.

Calibration, mean width achieved at the root position of the test set: `TopKMin 6 TopKWiden 0` gives
5.72 of 19.69 available, `6/0.10` gives 6.22, `6/0.25` gives 8.43, `6/0.50` gives 11.48. `TopKWiden 1`
reaches a mean width of 16.94 and barely narrows at all, because `log2(1+N)` is 20 at the root.

Two repeats an arm, control in the same batch:

| arm | solved | both right | both wrong | median sims | wrong->right | right->wrong |
|---|---|---|---|---|---|---|
| control (off) | 120/342 = 35.1% | 44 | 95 | 562,944 | — | — |
| k6 widen 0 | 122/342 = 35.7% | 50 | 99 | 550,067 | 11 | 10 |
| k6 widen 0.10 | 115/342 = 33.6% | 50 | 106 | 582,488 | 9 | 9 |
| k4 widen 0.25 | 127/342 = 37.1% | 53 | 97 | 603,954 | 8 | 7 |
| k8 widen 0.25 | 119/342 = 34.8% | 49 | 101 | 570,076 | 4 | 4 |
| k6 widen 0.25 | 118/342 = 34.5% | 52 | 105 | 586,511 | 2 | 7 |

**A symmetrical null** -- 11 against 10, 9 against 9, 8 against 7 -- which is reshuffling, not
improvement, and quite unlike `MinimaxWeight`'s 15 against 2. The simulation counts barely move either,
so the narrowing did not buy the depth the branching arithmetic promised.

#### Why it failed: the prior has a trustworthy SET and almost no ordering

Three measurements, all from the engine's own output, explain it.

**The search spends a quarter of its root visits below rank 6, and plays from rank 9 or worse 19% of the
time.** Root visits by prior rank over 342 searches, reproduced on an independent 684: ranks 1 to 10 take
25.0, 18.5, 8.3, 9.1, 8.6, 4.3, 2.1, 5.3, 3.0, 3.1 per cent, with 12.7% below rank 10. Not even
monotone -- rank 8 takes more than ranks 6 and 7 together. Narrowing to six removes a fifth of the moves
the engine would have chosen.

**The prior's first choice is the LEAST reliable of its leading moves.** Over 1,026 searches, by the
prior rank of the move actually played:

| rank played | how often | and right |
|---|---|---|
| 1 | 25.0% | **28.0%** |
| 2 | 19.3% | 40.4% |
| 3 | 9.0% | **58.7%** |
| 4 | 8.9% | 41.8% |
| 5 | 8.5% | 20.7% |
| 11 or worse | 12.1% | 32.3% |

The prior's first choice is played a quarter of the time and is right 28.0% of those; **anything else is
played 75.0% of the time and is right 37.1%**. So the search is more accurate precisely when it
overrides the prior, and the deviations a narrow search would remove are where its lift over the prior
alone -- 34.6% against 26.9% -- comes from. One caveat that is load-bearing: this set is SELECTED for
positions the engine got wrong, so rank 1 is over-represented among failures by construction. The
unbiased version of this table would need the broadcast positions, where every move has a depth-12
value.

**And the prior is nearly flat past rank 2.** From `VisitDumpFile` over the 171 positions, the prior's
own mass and the visits it actually received:

| prior rank | prior mass | visit share | visits / prior |
|---|---|---|---|
| 1 | **40.2%** | 25.5% | **0.63x** |
| 2 | 13.3% | 18.8% | 1.41x |
| 3 | 7.9% | 7.8% | 0.98x |
| 5 | 4.8% | 8.3% | 1.73x |
| 8 | 2.8% | 5.7% | **2.05x** |
| 11+ | 13.4% | 12.7% | — |

Consecutive prior-mass ratios: **3.02, 1.68, 1.36, 1.22, 1.25, 1.18, 1.16**. Forty per cent of the mass
on one move, and after rank 2 each move carries only 16-36% more than the next -- barely an ordering.
PUCT then spends 0.63x the prior's request on rank 1 and up to 2.05x on rank 8, which the accuracy table
says is the right direction.

**So the defect is calibration, not width.** Six moves is enough: 93.8% sharp containment at k=6. What
is wrong is that 40% of the prior's mass sits on a move that is right 28% of the time. `P` multiplies
the exploration term directly, so a prior whose mass matched the probability of being best would change
the allocation at every node with no change to the search at all. That is a training objective, and the
245,000 positions with a depth-12 value for every legal move are exactly the data to fit or measure it
against.

### The comparator, trained — and why it failed in a way worth knowing

Trained on 4 October 2026 and it is a **negative result with a diagnosed cause**, which is why it is
written up at length rather than dropped.

The model deliberately mirrored the policy head so that only the objective differed: trunk
1024 → 256 → 128, the same 24,576 × 128 move embedding indexed `(pt-1)*4096 + from*64 + to` with both
squares mirrored for Black, 3,441,026 parameters, `score = dot(ctx, emb[move]) * scale + bias`. Target:
the move's loss in pawns under a Huber loss — not a `tanh` squash, which saturates exactly where the
signal is. 1,849,055 records from 2026-01…07, tail-weighted so 64% of them lose 100 cp or more;
validation on 400,000 **uniform** rows from 2026-08…09, a temporal split so no game appears on both
sides. Three epochs, about fifteen minutes.

On the per-move error it looked like a clear win over NNUE, measured on identical rows:

| true loss | NNUE mean error | comparator | NNUE sigma | comparator sigma |
|---|---|---|---|---|
| all | −56.7 | +63.2 | 279.2 | **140.7** |
| 100–199 | −131.2 | **+0.5** | 89.5 | 80.4 |
| 200–399 | −266.3 | **−115.3** | 105.3 | 107.1 |
| 400+ | −1161.4 | **−542.3** | 1673.9 | 443.2 |

Calibrated at 100–199 where NNUE is 131 cp blind, and it flags a 100+ cp mistake as one 76.7% of the
time at 100–199, 86.9% at 200–399 and 92.4% at 400+.

**And it ranks worse than the static one-ply value it was meant to replace: 8.8% against 21.6%** on the
171 mistakes positions, with an acceptable move in its top three only 24.6% of the time. Random ranking
over about thirty moves with one to five acceptable is roughly 5–15%, so this is close to arbitrary.

The diagnostic that explains it, the spread of the predicted loss in centipawns:

| | |
|---|---|
| WITHIN a position, averaged over positions | **21.9** |
| BETWEEN positions, of the per-position mean | **44.5** |
| mean predicted loss over all moves | 166.3 |

**The model learned the position, not the move.** Its output varies by 22 cp across all the legal moves
of one position, while the true losses within a position span hundreds — the best move loses nothing and
a blunder loses 400. Twice as much of its variance is between positions as within them. It learned "how
dangerous is it here", because that is the only thing the training data varies over.

**The cause, stated so it is not repeated.** With **one labelled move per position there is no
within-position contrast in the training signal at all.** Sparse labels are trainable for an *absolute*
value and not for *discrimination*: the model never saw a move nobody would play, so nothing taught it
that such a move is bad, and fitting the position is the only gradient available. Human games give
magnitudes on bad moves but no contrast; the Stockfish shards give contrast — 38.3% of records carry two
or more moves each with a cp — but only among moves Stockfish already rates highly. Neither supplies
what is needed, which is **contrast that includes bad moves**.

**And a lesson about the metric.** The per-move error table above is measuring the wrong thing. A model
can score well on it by knowing the position, because the row it is asked about is always a move a human
actually played. Judge a per-move value model on **ranking all legal moves**, never on regression error
over played moves — the two pointed in opposite directions here, by a wide margin.

### Fine-tuning the real policy net on the per-move targets: no

The comparator above was a reduced model; this is the deployed net itself, fine-tuned on the depth-12
per-move labels through its own trainer, so the spatial and legality terms are intact and only the
target changed. `labels_to_visits.py` converts the labels into the trainer's VISITS_TSV format -- which
already accepts a per-move DISTRIBUTION, the format creatica's own `VisitDumpFile` writes -- with the
target weight a softmax over the negated loss at 58 cp, the engine's own prior temperature.
`pol_train_L1C8pi3` is the build with pisp2's exact architecture (3,459,544 parameters) and
`L1C8pi2_h1256_h2128_c0_p1_l1_e2_f28.pt` is pisp2's final checkpoint, so the fine-tune starts from the
deployed weights.

First the BASELINE, which should have been measured before any of this (`policy_rank.cpp`): the policy
head alone, no search and no blend with the child evaluations, on the 171 mistakes positions --
**top-1 26.9%, top-3 59.1%, top-6 79.5%.** Better than the static one-ply value's 21.6%, and the right
move is in its top six four times in five.

| | top-1 | top-3 | top-6 |
|---|---|---|---|
| pisp2, deployed | **26.9%** | **59.1%** | **79.5%** |
| one epoch at the default 2e-3 | 27.5% | 56.7% | 72.5% |
| 20 epochs at a constant 1e-5, epoch 1 | 28.7% | 54.4% | 73.1% |
| ... epoch 5 | 26.9% | 58.5% | 75.4% |
| ... epoch 10 | 28.1% | 59.1% | 75.4% |
| ... epoch 20 | 28.1% | 58.5% | 76.0% |

On its own target it learned properly -- top-1 against the depth-12 labels rose 33.4% to 34.2% with the
loss easing 2.675 to 2.596. On the positions that matter it buys a point or two of top-1 and costs
three to four points of top-6, at every checkpoint, and the shape is the same at 2e-3 for one epoch as
at 1e-5 for twenty. **So it is the objective, not the schedule.** Sharpening the first choice at the
expense of the top six is the wrong trade here, because the search needs the right move to be PRESENT.

A note on the schedule, because it is not obvious: the trainer has no learning-rate environment knob,
but `progress = min(1.0, global_step / TOTAL_STEPS)` is clamped, so `TOTAL_STEPS=1` puts the cosine at
`LR_MIN` = 1e-5 from the first step and holds it there -- a low constant rate with no recompile. And
`EXPORT_WEIGHTS` exports the LOADED weights and exits without training, which is worth knowing before
combining it with a training run.

**What this does and does not test.** The trainer's objective is cross-entropy against a distribution
and it keeps only the top MAX_PVS = 16 moves, and a softmax over the negated loss gives a bad move a
weight near zero -- so the magnitude information, the whole reason for building the dataset, is
discarded by the loss. What was tested is whether depth-12 distributions over all legal moves beat
eight-PV softmax distributions as a POLICY target, at 245,000 positions against the 300 million the net
was trained on. Slightly no. Testing the value objective properly needs a loss that uses the
magnitudes, which means a new head inside that trainer.

One trap worth recording: `uci2move_idx()` returns the PACKED MOVE INDEX — `dst | src<<6`, so 731 for
d2d4 — and not a status code. Reading a non-zero return as failure, the way a status code would be read,
rejected all 5.8 million rows on the first run. The engine's own callers ignore the return entirely.

`pgn_movevals.cpp` extracts them. Its parser is lifted verbatim from `pgn_parser.cpp`, the tool that
built the broadcast bins, so the SAN handling and the eval regex are the proven ones; what changes is
that it records the position *before* each move, the move itself, and the evaluations on both sides of
it, where `pgn_parser` records the position *after* the move and its evaluation alone. Every cp is
flipped into the mover's frame. Rows go to a named file rather than stdout, because `san2move()` prints
an error line and an ASCII board to stdout when a game desyncs, and in the first test that put 19
malformed rows in the middle of the data.

## Why no search change survived: the allocation law and the test-set artefact, 5 October 2026

**Two numbers in this section are unsafe and were corrected on 8 October. Read *Two corrections to how
this project measures things* in the last section first.** Every uncaptured centipawn mean quoted from the
deep set here is dominated by mate-labelling artefacts -- the depth-12 labels charge about 19,000 cp for
playing a slower mate than the labeller found -- and the harness's own run-to-run noise is 32 of 425
positions, which puts `MinimaxWeight`'s result inside the noise rather than measured. The allocation law
itself, the depth gradient and the test-set selection bias all stand.

### The whole thing in one paragraph

MCTS decides how much to look at a move from two numbers it does not know well enough: the move's prior
and how far behind it currently looks. Solve the selection formula and a non-leading move gets about
`C * P * sqrt(D) / gap` visits and no more -- twenty or thirty in practice, against a million for the
leader -- while a refutation that lives five plies down needs a subtree of thousands before it shows up at
all. So the move is dismissed on the strength of a thirty-sample estimate, and the dismissal is
self-confirming, because the exploration term shrinks as the move accumulates the very visits that would
have redeemed it. The part that makes this a limit rather than a bug is that its share of the search
decays as `1/sqrt(D)`: thinking twice as long gives the neglected move 1.41 times more visits out of twice
the total, so its share FALLS, and more time makes the problem worse rather than better. And the visits
cannot simply be spread out, because that concentration is exactly what buys depth on the main line --
when every leading candidate was forced a guaranteed share, the positions needing deep refutations got
three and a half times worse, since searching everything shallowly is worse than searching the wrong move
deeply. The inequality in the sampling and the depth of the search are the same property, so MCTS cannot
allocate depth selectively: it would have to know in advance which moves deserve it, which is the one
thing it is trying to find out.

### What the symbols mean

Every formula in this section is about **one node and one of its children**, and all of it is read
straight out of `select_best_child()` in `creatica_search.cpp`. Selection scores each child as `Q + U`
and takes the largest.

| symbol | is | where it lives |
|---|---|---|
| `W` | the sum of all values backed up into this child, from the point of view of the side to move AT the child | `MCTSNode::W` |
| `evidence` | visits that actually produced information, as distinct from `N`, which counts every arrival — they differ only when a node cannot be expanded | `MCTSNode::evidence` |
| `Q` | `W / evidence`, the child's mean value. The parent sees its **negation**, because a child states its value from its own side to move | computed |
| `P` | the move's prior: a float in [0,1], summing to about 1 over the node's children. **Not the policy head's output.** In the default `PolicyMode 1` it is a softmax (`get_prob()`) over a BLEND of the policy head's logit and the child's own NNUE evaluation — `policy_blend * policy_logit / policy_temperature + (1 - policy_blend) * res / temperature`, all scaled by `policy_blend_scale`, so at `PolicyBlend` 0.45 it is 45% head and **55% evaluation**. `res` is the child's evaluation in pawns, already in the parent's frame (the sibling line stores `cp = -res*100`). The head alone is used only in `POLICY_FULL`, which is not the default; `policy_blend = 1.0` is the switch that gets there. The starved moves measured here ran from 0.000074 to 0.133, against a flat `1/num_children` of about 0.03 | `Edge::P`, written at `creatica_search.cpp:1812`, built at 2734 |
| `n` | how many times **this edge** has been taken. Not the child's visit count: the two differ for any transposition, and enormously under `ReuseTree` | `Edge::n` |
| `D` | **descents** through the parent — how many times the search has passed through it, which is the sum of its edges' `n`. On the same scale as `n`, which is the point of using it | `MCTSNode::descents` |
| `C` | the exploration constant, `ExplorationMax` 1.6 at the root and decaying by `ExplorationDepthDecay` 0.05 a ply down to `ExplorationMin` 0.65 | option |
| `U` | the exploration term itself, `C * P * sqrt(D) / (1 + n)` | computed |
| `gap` | `Q(leader) − Q(candidate)`, both negated into the parent's frame: how much better the current best child looks. Dimensionless, since `W` accumulates `tanh(eval_pawns / EvalScale)` with `EvalScale` 6.1 — a gap of 0.123 was about 162 cp at the operating points measured here, and the ten cases ran from 17 to 421 cp | computed |
| `n*` | the equilibrium: the edge count at which `U` has decayed to equal `gap`, after which the move is abandoned | derived |
| `k`, `share`, `d` | `MinVisitK` (how many leading children get a floor), `MinVisitShare` (the floor, as a fraction of `D`), and plies of depth the floor is applied over | options |

So `n* = C * P * sqrt(D) / gap` comes from setting `C * P * sqrt(D) / (1 + n) = gap` and dropping the 1,
and the share `n*/D = C * P / (gap * sqrt(D))` is that divided by the parent's total descents. The
leader's own `U` is dropped throughout: the exact selection condition is
`U_candidate > (Q_leader + U_leader) − Q_candidate`, and the leader's `U` measured 0.0008 at the median
against gaps of 0.09 to 0.37, so under 1%, and it changes no verdict in the tables below.

### How this was measured

The day before, a long run of measurements had narrowed the question to *where* creatica fails. This
section is what happened when five candidate fixes were measured against samples that were **not** built
from the engine's own mistakes. All five are null or negative, two of them after looking like clear wins,
and the reason the two disagreed with themselves is the most useful result here.

Everything runs from `$EXPERIMENTS/` (see *How the set is scored* above). Three new harnesses were built and are kept:

- `hard_pool.tsv` — about 7,500 positions that are **sharp** (a depth-12 search prefers the best move by
  at least 100 cp over the second) **and** where the policy head's own top choice gives away at least
  100 cp. It conditions on the PRIOR, which is an input to the search rather than its output, so it is
  not circular for judging a search change.
- `deepset.txt` / `deep_pool.tsv` — 425 positions whose refutation **creatica's own evaluator cannot see
  within four plies**, measured by full-width negamax with the engine's NNUE. The sharpest harness of the
  three, because the selection criterion is a property of the evaluator rather than of the search.
- `pool_run.py` — scores either against the depth-12 per-move labels in centipawns, graded, with the
  prior's own error reported beside the engine's.

### 1. The test sets, and why two of them lie

A representative sample of positions is useless for judging a search change. Over 61,753 positions with
a depth-12 value for every legal move, only **13% are sharp**; in the other 87% the median gap between
the best two moves is **15 cp** and a mean of 6.1 moves out of 30.8 are within 50 cp of best, so the
choice does not matter and the evaluator's 165 cp of absolute error largely cancels — a bias shared
across the moves of one position disappears in the comparison. Any aggregate over unfiltered positions
is dominated by that 87%.

But `mistakes/testset.tsv` is not the fix, and the contrast is stark. Measured on it, the prior's first
choice is played 25.0% of the time and is right 28.0% of those, while everything else is played 75.0% and
is right 37.1% — the prior looks anti-correlated with the truth. Measured on 200 broadcast positions the
engine never saw, rank 1 is played 41.5% of the time and is right **72.3%** against 52.1% for everything
else; restricted to the sharp ones, it is played two thirds of the time and is right **100%** while its
deviations give away 76 cp. The mistakes set is not a sample of sharp positions — it is a sample of sharp
positions **where the prior was wrong**, because those are the ones that cost games and therefore got
collected.

One objection and its answer, because the arithmetic matters. "Within 50 cp" looks saturated on neutral
positions, and NNUE's mean absolute error is about 165 cp, so the engine cannot be accurate to 50 cp.
But a **random** legal move scores within 50 cp only **23.2%** of the time (mean 6.1 of 30.8 moves
qualify), and only **8.3%** in sharp positions, against the engine's ~100%. The metric is saturated
because the engine is good at it, not because the positions are permissive. Mean absolute error against a
searched evaluation does not bound how well an evaluator RANKS moves inside one position.

### 2. Where the failures are: the depth gradient

On 300 positions from `hard_pool.tsv` the engine gives away more than 100 cp in **11.0%** of them, with a
**median loss of zero** — it finds the exact best move more than half the time — while the prior's own
top choice on those same positions is off by a mean of **2,152 cp**. The search converts a catastrophic
prior into a 153 cp mean loss, a fourteenfold reduction, and **the failure rate does not depend on how
wrong the prior was**: 11.6%, 8.4% and 13.6% across the 100–199, 200–399 and 400+ buckets. That retires
the prior as the cause of the losses.

What it does depend on is depth. Full-width negamax with creatica's own NNUE, and the smallest depth at
which it prefers a move within 50 cp of the depth-12 best:

| needs depth | solved | failed | failure rate |
|---|---|---|---|
| 1 | 83 | 3 | 3.5% |
| 2 | 41 | 2 | 4.7% |
| 3 | 46 | 4 | 8.0% |
| 4 | 25 | 3 | 10.7% |
| more than 4 | 72 | 21 | **22.6%** |

Monotone, a 6.5-fold rise, and two thirds of all failures in the deepest bucket while it holds 31% of
the positions. One ply is free because every legal child is evaluated at expansion; from three plies the
move must EARN visits while its Q still looks bad.

### 3. The allocation law

Diagnosing those failures by root trace: the right move gets tens of visits against the leader's million.
Setting the PUCT exploration term equal to the Q gap and solving for the edge count gives the visits a
non-leading move ever receives:

    n* = C * P * sqrt(D) / gap

and it predicts the traces across three decades of P. With C = 1.6 and D about 600,000: P = 0.0015 and a
gap of 0.09 predicts 20.7 visits against 30 observed; P = 0.127 and a gap of 0.123 predicts 1,280 against
2,244. The *gap* here is the leader's backed-up value minus the candidate's, both negated into the root's
frame because W and evidence live on the child — a median of about **130 cp** in these cases, ranging
from 17 to 421. The leader's own exploration term, dropped from the comparison, is 0.0008 at the median
against gaps of 0.09 to 0.37, so under 1% and it changes no verdict.

A refutation five plies down needs a subtree of thousands. The two numbers never meet, and **no constant
closes them**:

- **Raise C.** Buying 10,000 visits for a P = 0.0015 move needs C = 774 against the incumbent 1.6. That
  is uniform search, and raising C measures as a loss here (see *Exploration: the curve never turns*).
- **Raise P** — a floor, flattening, a better policy head, or reweighting the blend it sits in (see the
  symbol table: `P` is 45% head and 55% child evaluation by default, not the head alone). Does nothing,
  because the starved moves are
  not the low-prior ones: over ten measured cases the right move carried priors from 0.000074 to 0.133,
  with three at **two to four times** the flat 1/num_children, and was starved identically.
- **Shrink Q toward the parent at low n**, the textbook Bayesian correction. Solving
  `n^2 - An - Am = 0` gives `n ~ A + m`: it adds m visits, not m-fold. With m = 50 a 30-visit move
  reaches 44.
- **Switch to UCB1.** Its allocation is `~ln(D)/gap^2` against PUCT's `sqrt(D)/gap`. Logarithmic beats
  square-root downward, so UCB1 explores **less**. The textbook alternative is worse.
- **Use `evidence` instead of N in the denominator**, as Q does. A no-op here: N and evidence are equal
  in all twelve starved cases, because they only diverge on hollow nodes and these are simply unvisited.
  It would also reintroduce the live-lock the note on `MCTSNode::evidence` warns about.

The structural statement is about the SHARE. It is `C * P / (gap * sqrt(D))`, which decays as
`1/sqrt(D)`: doubling the search multiplies a non-leader's visits by 1.41 while the total grows by 2, so
its share falls by 29%. **More time makes the inertia worse**, which is why every attempt in this
document to buy the answer with a bigger tree returned a null.

### 4. MinVisitK / MinVisitShare / MinVisitDepth, and why equalising fails

The one remedy the arithmetic endorses is a share that does not decay: guarantee each of the first k
children (descending prior order) a floor fraction of the parent's descents, and let PUCT allocate the
rest. Implemented, default off, see the note in `creatica_search.hpp`. k = **8** because that is where
the right move's prior rank actually sits — over ten starved cases the ranks were 2, 2, 2, 5, 6, 6, 7, 8,
18, 27, so top-4 covers 30% and top-8 covers 80%, and it plateaus (top-16 covers no more). Note this is
NOT the k = 4 the mistakes set's played-rank table suggests; which rank the engine ends up PLAYING is a
different quantity from where the right move sits.

It works mechanically. On the first deep-failure position the floor gave the prior-rank-2 move its 13,888
visits, the move discovered it was better, and **PUCT itself then poured 349,586 into it** — the floor
pays only for the discovery, and the gain is unbounded because a floored move can become the leader. It
costs about 2.4x the simulations, because searching eight moves broadly expands more nodes.

And it loses, on 300 pool positions, worse the deeper it is applied:

| arm | failures | mean loss | deep-bucket failures | deep-bucket mean loss |
|---|---|---|---|---|
| control | 35 (11.7%) | 217 cp | 20/93 | 249 cp |
| root only | 36 (12.0%) | 218 | 18/93 | 244 |
| depth <= 1 | 38 (12.7%) | 347 | 22/93 | 660 |
| depth <= 2 | 48 (16.0%) | 416 | 22/93 | 662 |
| depth <= 3 | 44 (14.7%) | 415 | 25/93 | **875** |
| k = 4, depth <= 3 | 40 (13.3%) | 221 | 23/93 | 255 |

It fails hardest in the bucket it was built for: 249 cp to 875, a 3.5x degradation, with fixed-against-
broken asymmetric the wrong way (7 against 12 at depth 3). Narrowing k from 8 to 4 recovers most of the
damage, which confirms cost as the cause: the leader's own line keeps `(1 - k*share)^d` of the budget,
0.76 at the root and 0.44 three plies down.

**The conclusion, and it is the point of the whole section.** The unequal sampling is not a defect
sitting beside the depth mechanism — it IS the depth mechanism. Concentrating visits on one line is how
MCTS buys depth there, and the `1/sqrt(D)` share decay is the same fact from the other side. Equalise the
samples and everything is searched shallowly, and shallow-everywhere loses to deep-on-the-wrong-move.
So **no reallocation of visits can work**: floors, guaranteed shares, round-robin budgets, a larger C,
prior floors, flattening. That family is closed.

### 5. The two false positives

Both changes that looked like wins were scored on the mistakes set. Both reverse on unselected positions.

**FlattenTopK** — equal mass across the top k, tail untouched, total preserved. On the mistakes set at
two repeats: control 119/342 = 34.8%, k4 **141/342 = 41.2%** with 18 never-to-always against 6, k6 40.9%,
k10 41.8%, at *fewer* simulations. At four repeats, control 36.0% and k4 43.0% with 8 against 2. It was
predicted in advance from three independent measurements of the prior's miscalibration. And the prior is
not miscalibrated: on unselected sharp positions its first choice is right 100% of the time. The six
points were the test set's construction, not a defect being fixed.

**MinimaxWeight 0.75** — `Q_blended = (1-w)*mean + w*minimax`, the one mechanism none of the allocation
arithmetic touches, since it changes how values COMBINE rather than how visits are spent and costs no
simulations. On the mistakes set: 48.7% against 34.6%, **19 never-to-always against 2**, with a
simulation-matched control at 33.6% and zero conversions, and at four repeats 47.5% against 36.0% with
7 against 1. On the 425-position deep set it **reverses**: failures 23.3% against 26.6%, mean loss
**628 cp against 767**, paired movement **24 fixed against 38 broken** (two-sided exact p = 0.098 — no
evidence of benefit, a suggestion of harm).

It also composed almost disjointly with flattening on the mistakes set — each fixing 8 and 7 with an
overlap of 1, and 17 together — which read as two independent mechanisms reinforcing. Both were the same
artefact.

### 6. What this means for measurement

Five search interventions — `ProveTerminals` (2.96M tablebase verdicts, 390k nodes proved, **zero**
conversions), `TopKMin` (symmetrical null, 11 against 10), `FlattenTopK`, `MinVisitK`, `MinimaxWeight` —
and every one is null or negative once scored on positions not chosen by the engine's own failures.

`mistakes/testset.tsv` manufactures false positives, because a change is being scored on positions
selected precisely because the baseline fails them: any change that perturbs the search can only move
them one way, and the positions where it does harm are not in the sample. Two of five changes looked
like clear wins there. **Measure on unselected positions or do not measure.** The harnesses for that now
exist, and the deep set is the one to prefer: it is selected on the evaluator's horizon, which the search
does not control, and it holds about 90 failures in 425 positions rather than 20 in 300, so a third-sized
effect is resolvable rather than inside the noise.

No search change should be played. By elimination the remaining direction is the value and policy models,
which was the starting position: *make better value and policy models, which absorb chess knowledge
indirectly, and leave MCTS alone.* The difference is that it is now a conclusion with five negatives and
a mechanism behind it. Selective depth — spending the extra visits only where they are deserved — would
need to know which moves deserve them, and seven root signals for that scored AUC 0.47 to 0.62.

### Diagnostic added

`root_trace_snapshot()` now records the **prior** as a sixth field per child
(`move:edgeN:N:W:evidence:P`). Without it the exploration term cannot be reconstructed offline at all:
substituting a flat 1/num_children for P overstates U by orders of magnitude for exactly the starved
moves the trace exists to study, and that substitution produced a table claiming terms of 1.3 to 1.5
against gaps of 0.1 to 0.35 — which would mean those moves should be selected constantly, when they had
thirty visits in six hundred thousand.

## Why MCTS is the wrong fit: a probabilistic search over a deterministic estimator, 6-8 October 2026

### The whole thing in one paragraph

Monte Carlo tree search averages. It was built for domains where visiting a position twice gives you
two different numbers, so that a mean over many visits converges to something real -- an expected
outcome. Creatica's leaf evaluation is a deterministic neural network returning a centipawn estimate of
a minimax value. There is no distribution to average, so what `Q` estimates is not a quantity the game
defines; the game defines a maximum over the subtree, and a mean is a biased estimator of a maximum.
UCT does converge to the minimax value in the limit (Kocsis and Szepesvari, 2006) but only by letting
the visit distribution become degenerate, so the search spends its compute turning an average into a
maximum -- which alpha-beta has at the first node, for free, and which is where the measured factor of
two to three in compute comes from. Everything else in this section is that sentence with numbers
attached: a bounded alpha-beta placed at every leaf costs about 127,000 nodes per leaf and makes the
engine monotonically worse; the same search used *as the whole engine* matches creatica at roughly a
third of the CPU; the engine's own root traces show it finding the right move, assigning it the better
`Q`, and then playing something else because the final choice is made by a cumulative visit counter;
and two of this project's own measurement instruments turned out to be unsafe for the questions they
were being asked.

### What the symbols mean

| symbol | what it is | where it lives |
|---|---|---|
| `Q` | a node's value as a **mean**: `W / evidence`, negated into the parent's frame | `select_best_child()` |
| `W` | running sum of `tanh(eval_pawns / EvalScale)` over every simulation through the node | `MCTSNode::W` |
| `evidence` | how many of those simulations taught the node something (differs from `N` only on hollow nodes) | `MCTSNode::evidence` |
| `N` | visit count, including hollow visits; drives selection so the search cannot live-lock | `MCTSNode::N` |
| `edgeN` | the parent's own count of descents down one edge -- **this is what picks the move** | `Edge::n` |
| `U` | exploration term, `C * P * sqrt(D) / (1 + n)` | `select_best_child()` |
| `P` | the prior on an edge; at `PolicyMode 1` a softmax over `PolicyBlend * head_logit / PolicyTemp + (1 - PolicyBlend) * child_eval / Temperature`, so 45% policy head and 55% child NNUE eval at the default | `Edge::P` |
| `C` | exploration constant, decaying with depth between `ExplorationMin` and `ExplorationMax` | `creatica_search.hpp` |
| `D` | the parent's visit count, the `sqrt` of which scales `U` | -- |
| `gap` | difference in `Q` between the leading child and another, in the same frame | -- |
| `mm` | a node's 1-ply minimax value in centipawns, `MM_NONE` when unset | `MCTSNode::mm` |
| `EvalScale` | 6.1, the pawn scale inside the `tanh`; `cp = 100 * 6.1 * atanh(Q)` | `creatica_search.hpp` |
| `LeafDepth` | new UCI option: plies of alpha-beta run at every expanded leaf. 0 is off and is the default | `creatica.cpp` |

### How this was measured

The deep set: 425 positions, unselected, chosen because creatica's own NNUE needs more than four plies
of full-width negamax before it prefers the move a depth-12 search prefers. Scored in centipawns against
per-move depth-12 labels. Harness `pool_run.py`, data `deep_pool.tsv` and `labels_0.tsv`, all in the
experiments directory outside this repository. Engine built to a separate binary; `./creatica` in the
repository root was never rebuilt, because that is the binary the lichess bot launches.

**Read section 5 before trusting any centipawn mean quoted from that harness.** Two of its properties
were discovered during this work and both invalidate results this project had already drawn.

### 1. The mismatch, stated exactly

In stochastic MCTS -- Go with random rollouts, or a bandit problem -- sampling an action twice gives two
independent draws, and the sample mean converges to the expected value. The averaging is *estimating a
random variable that exists*.

Creatica's leaf is `evaluate_nnue()`: deterministic, and returning a centipawn number that is itself a
point estimate of a minimax value. Two observations follow.

**What is averaged is distinct leaves, not repeated draws.** Each simulation descends to a different
leaf, so `Q` at an interior node is a visit-weighted mean over the distinct leaf evaluations in its
subtree. That is a well-defined operation. It is simply not the operation the game calls for: the
game-theoretic value of that subtree is a minimax over it, and a mean is a biased and inconsistent
estimator of a maximum.

**`tanh` is the adapter that hides the category error.** `W` accumulates `tanh(eval / EvalScale)`, which
turns an unbounded centipawn score into something shaped like a probability so that averaging looks
sensible. `EvalScale` 6.1 is therefore not an arbitrary tuning constant -- it is the conversion that lets
a minimax estimate enter a machine built for outcome probabilities. And because the adapter must compress
an unbounded score into the interval from -1 to 1, it saturates: above roughly 1200 centipawns it stops
carrying information. Measured in a live search, a forced mate and a position worth +1188 cp differ by
0.04 in `Q`, and a mate in 2 and a mate in 3 by 0.0005. In that region `Q` has nothing left to say and
the prior decides, which is section 2.

This also explains why the same algorithm works elsewhere. Leela's value head emits a **win
probability**. Averaging win probabilities over a subtree estimates the expected outcome of the game from
that point, which genuinely is a random variable over future play, and the mean is its right summary.
Creatica feeds a minimax-style evaluator into the same machinery. The architecture is not wrong in
general; it is paired with the wrong kind of number.

The honest counterweight, because it is the one real argument the other way: averaging reduces variance,
and minimax over noisy evaluations amplifies error -- the minimax pathology of Beal (1980) and Nau (1982).
So MCTS is genuinely more robust to a bad evaluator. That robustness costs the two-to-three-times factor,
and it is only worth buying if the evaluator is bad enough to need it. The KAN and CNN experiments
answered that from the other side: those evaluators are measurably **more** accurate than NNUE against
depth-30 references and still lose in play, because what the search needed was throughput.

### 2. What the traces show: the search is right and the counter is wrong

Position `4Q3/p7/1p1B1k2/2p2p2/6r1/2P3K1/PP2R3/5R2 w - - 0 44`. White is in check, so there are exactly
four legal moves. Creatica, two seconds, four threads, 4.29 million simulations. Final root trace:

| move | edge visits | `Q` | cp | `P` |
|---|---|---|---|---|
| `e6g6` | **2,631,715** | 0.999517 | 2544 | 0.576 |
| `d6f4` | 1,525,972 | **0.9999982** | 4247 | 0.112 |
| `f1f5` | 123,115 | 0.99545 | 1659 | 0.175 |

`d6f4` is mate in 2 and its principal variation in creatica's own output is the mating sequence, move for
move. So the line is in the tree, and `Q` is right about it -- it is the *highest* `Q` at the root. The
engine played `e6g6` because the move is chosen by **edge visits**, and `e6g6` has more of them.

Why it has more: once both moves saturate, `Q` separates them by 0.0005, at which point the PUCT score is
dominated by `U = C * P * sqrt(D) / (1 + n)`, and `e6g6`'s prior is five times larger. The first snapshot
of the same search, at 16,793 simulations, has `d6f4` holding 13,855 of them -- 82%. The search found the
mate almost immediately and then four million further simulations *migrated the visits away from it*.

This is not specific to that position. Across 2,936 searches in six independent incumbent-configuration
arms (`c4_ctrl`, `mm0`, `mm0_short`, `prove_off`, `fl_ctrl`, `tk_ctrl`, `neutral`):

| condition | share of searches |
|---|---|
| most-visited root child is not the highest-`Q` child | 11.2% |
| ... and the higher-`Q` child holds at least 10% of the leader's visits | 8.9% |
| ... at least 50% | 4.2% |

Those are not low-visit flukes. Examples: `b4b3` played with 381,648 visits at +140 cp while `d7f6` sat at
236,472 visits and +495 cp; `c3e4` with 456,274 visits at +140 cp against `e3h6` at 174,531 and +308 cp.

**And the search knows.** Comparing the last two trace snapshots of each such search, the higher-`Q` move
is gaining visits faster in 83.4% of cases -- and in **all 15** of the disagreements worth more than 100 cp.
In the clearest cases the move the engine goes on to play receives *zero* new simulations while the better
move takes seventeen thousand. Visit counts are cumulative and never decay, so the move selection reports
where the search *used to be looking*. Resolving the material cases would take a median of about 84% more
search time, and they are 1.4% of all searches.

Selecting by `Q` instead is not the fix and was not adopted: a high `Q` on few visits is usually noise,
which is why every strong MCTS engine selects by visits. The point is narrower and it is about stopping,
not selecting -- the search has changed its mind and run out of clock, and `search_budget` in
`creatica_search.cpp` is computed once up front with no stability check anywhere.

### 3. Alpha-beta at the leaf: the exchange rate, measured

`LeafDepth` replaces an expanded leaf's static value with a bounded alpha-beta search from that leaf:
MVV-LVA plus best-move and killer ordering, iterative deepening, a capture quiescence with check
extensions, no transposition table and no unsound pruning. It skips nodes in check, whose value already
comes from a 1-ply minimax, and nodes in tablebase range, where the probe is exact.

Cost, on one deep position:

| `LeafDepth` | simulations in 2 s | nodes per leaf |
|---|---|---|
| 0 | 452,559 | -- (one net evaluation) |
| 1 | 159,346 | 192 |
| 2 | 32,818 | 1,607 |
| 3 | 2,784 | 13,650 |
| 4 | 294 | 127,025 |

That is a branching factor near seven per ply rather than the 1.83 `abprobe.cpp` reaches with the full set
of orderings and reductions, and adding iterative deepening plus killers did not improve it -- the cost is
dominated by the capture quiescence with its check extensions, not by the main search.

Result on the deep set, 425 positions, 2000 ms:

| `LeafDepth` | gives away >100 cp | >400 cp |
|---|---|---|
| 0 | **20.5%** | **3.1%** |
| 1 | 23.1% | 3.8% |
| 2 | 30.4% | 5.2% |
| 3 | 45.4% | 9.6% |

Monotonically worse at every depth. The plies bought at the leaf are worth less than the simulations they
displace. This is the sixth search option proposed for creatica and measured as null or negative; with
`MinVisitK` from 5 October it is the eighth change in this family.

A correctness note on the leaf search, because the obvious test for it does not run. The right check is
that the same search with every cutoff removed must return the identical value -- alpha-beta only avoids
visiting nodes whose value cannot matter. That cutoff-free twin does not finish a single position at depth
1 in two minutes: the cutoffs are what make the quiescence survivable at all, so there is no cheap
exhaustive reference. What stands in its place is that accuracy rises monotonically with depth (section 4),
which rules out gross errors but not subtle ones. One real bug was found and fixed along the way: when the
quiescence ran out of check extensions it returned the window's `alpha`, which made a leaf's value depend
on the window it happened to be searched with. It now returns the 1-ply static minimax over the evasions,
which depends on nothing but the position.

### 4. Alpha-beta alone against the whole engine

The same leaf search, used as a complete move chooser with no tree, no time management and one thread,
over the same 425 positions (tool generated by `genpick.py`, extracting the search verbatim from
`creatica_search.cpp`):

| depth | exact | >100 cp | mean loss | nodes per position | cutoff share |
|---|---|---|---|---|---|
| 1 | 34.1% | 65.9% | 2698 cp | 677 | 65% |
| 2 | 46.6% | 53.4% | 1519 cp | 4,139 | 55% |
| 3 | 60.2% | 39.8% | 803 cp | 31,001 | 59% |
| 4 | 73.6% | 26.1% | 366 cp | 201,617 | 55% |
| 5 | **80.5%** | **19.3%** | 307 cp | 1,372,112 | 58% |

Against the engine, which used four threads and two seconds -- 8.0 core-seconds -- depth 5 used 2.3
core-seconds:

| | >100 cp | mean loss capped at 200 cp |
|---|---|---|
| MCTS 2 s, 4 threads (two separate runs) | 23.3% / 20.5% | 39 / 35 |
| alpha-beta depth 5, 1 thread | 19.3% | 33 |

**What this does and does not establish.** It is level, at roughly a third of the CPU, deterministically.
It does *not* establish that alpha-beta is more accurate: paired against one control it fixes 59 and breaks
42 (p = 0.111), against the other it fixes 51 and breaks 46 (p = 0.685), and on the capped mean the three
numbers are within six centipawns of each other. The compute ratio is the robust part; the accuracy
difference is inside the harness's noise.

Two caveats on reading it as a strength claim. Alpha-beta parallelises badly where MCTS parallelises
almost linearly -- four threads of Lazy SMP typically yield well under 2.5x in time-to-depth -- so the
core-second ratio overstates the wall-clock advantage at four threads. And the ordering arms in
`abprobe.cpp` show where alpha-beta's depth actually comes from: mean depth 5.9 with neither, 6.1 adding
history, 6.9 adding null-move, 8.0 adding late-move reductions, 8.7 with both, all at about 1.8 million
nodes. Three plies from search engineering, none of it from a model.

### 5. Two corrections to how this project measures things

**The deep set's labels mis-score mates, and the error is enormous.** `labels_0.tsv` holds depth-12
per-move values. In a tactically rich position depth 12 cannot resolve which moves mate: one move returns
`mate 7` and its siblings return `cp 1188`, and by depth 14 all of them are mate. The label then charges
the engine about 19,000 centipawns for choosing a sibling. Of the 11 positions where the control gave away
more than 5000 cp, re-scored with Stockfish 18 at depth 20:

- **6** -- creatica played a forced mate itself; the label's "best" was a faster mate. No error at all.
- **2** -- lost by force whichever move is played; creatica was mated a few plies sooner.
- **2** -- creatica played a winning move (+664, +287 cp) rather than a mate in 2 or 3. Still winning.
- **1** -- a genuine blunder, into mate in 2 from a position already at -1227.

The worst case is the four-legal-move position of section 2: Stockfish at depth 22 says **all four moves
mate** -- mate 5, mate 5, mate 5, mate 7. At depth 12 only one showed mate. Creatica played a mate in 6 and
was charged 18,805 cp. And the set was *selected* for positions whose answer is deeper than four plies, so
the selection and the scoring share a blind spot.

**Consequence: never quote the uncapped mean loss from this harness.** Use the mean with losses capped at
200 or 500 cp. A count of "catastrophes" over 5000 cp measures the labeller, not the engine.

**The harness cannot resolve small differences.** Two runs of the *identical* configuration over the same
425 positions: 23.3% and 20.5% failures, 628 and 533 cp mean, and they disagree about failure on **32 of
the 425 positions**. The engine is nondeterministic -- four threads on a wall clock -- so anything smaller
than about 30 positions in a single run is unresolvable, and a comparison against one control arm inherits
all of it. The same alpha-beta numbers gave p = 0.111 against one control and p = 0.685 against the other.

For anything expected to move this set by less than ~30 positions: run the control at least twice and
quote both, or draw more positions (`hard_pool.tsv` holds about 7,500). Note also that comparing two
*move-selection rules read from the same tree* has a noise floor of exactly zero, which makes it a far
better instrument than two engine runs.

### 6. What was withdrawn

Recorded because the pattern is consistent and worth recognising next time.

| claim | why it fell |
|---|---|
| `FlattenTopK` gains six points | artefact of `mistakes/testset.tsv`, which samples sharp positions *where the prior was wrong* |
| `MinimaxWeight 0.75` is the surviving direction | reversed on unselected positions; and at 26.6% against controls of 23.3% and 20.5% it is about one noise unit worse -- inconclusive, not measured |
| alpha-beta halves the catastrophic failures, 6 against 11 | it halves a labelling artefact; alpha-beta maximises centipawns and a mate score is the maximum, so agreeing with the labeller here is mechanical |
| the refutations are not in the tree | drawn from one position using a known-incomplete `MinimaxWeight` probe; the root traces show the mating line present, with the correct `Q` |
| MCTS cannot support bounds, so measure the fully-expanded core | `d_core = 1 + min over children`, and MCTS leaves most children unexpanded by construction. The answer follows from the selection rule; there was nothing to measure |

The common shape: a number was taken at face value before checking what produced it. In four of the five
the contradicting evidence was already on screen -- the capped means disagreed with the uncapped ones
twice before the labels were checked.

### 7. The evaluator question, closed from the other side

The natural next move after section 4 is to replace the evaluator, and the architecture argument for it is
tidy: an incremental evaluator cannot be batched and does not need to be, which suits alpha-beta's
depth-first sequential dependency; an expensive evaluator must be batched, which only MCTS can do by
collecting leaves across parallel simulations. By that logic creatica today is the mismatched corner --
a cheap incremental evaluator inside MCTS, getting neither alpha-beta's cutoffs nor any benefit from
batching -- and the coherent pairing would be a dual-head KAN or CNN giving value and policy from one
amortised forward pass.

That experiment has already been run in this repository. `creatica-kan-gr.cpp`, `creatica-kan.cpp`,
`creatica-cnn.cpp` and the CoreML variants all batch their evaluations, at the right granularity: all legal
children of a node in one forward pass. The recorded outcome is that **NNUE wins in play, and wins because
it searches deeper in the same budget**, while the KAN and CNN evaluators are measurably more accurate
against depth-30 references (CNN validation loss 0.05, KAN under 0.07, NNUE considerably higher, at about
ten epochs each).

So the accurate-but-slower evaluator loses inside MCTS as well as inside alpha-beta, and every axis points
the same way:

- MCTS + NNUE against alpha-beta + NNUE: alpha-beta, by two to three times in compute for the same quality.
- MCTS + NNUE against MCTS + KAN/CNN: NNUE, because throughput beat accuracy.
- Eight search changes inside MCTS: all null or negative.

One note for anyone reading the 165 cp figure for NNUE's mean absolute error: it was measured against a
*searched* value, and a static evaluator cannot match a search. Much of that gap is the horizon rather than
error, and no static function of the pieces can recover it. Measuring a static evaluator against a deep
search overstates its error, which is the same mistake class as the mate labels in section 5.

### 8. Where this leaves creatica

Creatica embeds `nn-c288c895ea92.nnue` and `nn-37f18f62d772.nnue`. Those are Stockfish 18's own two nets --
the Stockfish 18 binary on this machine reports loading exactly them. So the matched-evaluator,
equal-hardware comparison of the two search architectures has already been run at scale by other people:
same weights, same machine, roughly 1100 Elo in alpha-beta's favour. That number conflates architecture
with fifteen years of search tuning, so it bounds the architectural gap rather than isolating it; the two-
to-three-times compute ratio in section 4 is the isolated version, measured here.

On an M1 with 8 GB the configuration that exploits an NNUE properly is alpha-beta, and building it well
produces something strictly weaker than the engine whose evaluator it borrows. The configuration that
would justify MCTS needs an evaluator emitting outcome probabilities at a rate this hardware cannot
supply -- which is why Leela needs a GPU, and why the local KAN and CNN attempts lost on throughput.

So the conclusion is not that creatica has a search bug, and not that one more option might work. It is
that the algorithm is matched to the wrong domain:

> MCTS averages, because it was built for domains where a position returns a different number each visit.
> Chess with a deterministic evaluator returns the same number, and what the game defines is a maximum
> rather than a mean. MCTS therefore spends its compute converting an average into a maximum -- which
> alpha-beta has at the first node, and which costs two to three times. That cost cannot be recovered by
> changing the backup, the exploration term, the visit allocation, the stopping rule or the leaf
> evaluation; eight such changes have now been measured. And the evaluator that would justify the
> averaging is one this hardware cannot afford to run at the rate the search needs.

Diagnostic added, off by default: `LeafDepth` (0 to 8), with its cost reported at the end of each search as
`info string leaf alpha-beta: depth D, L leaves searched, N nodes (X per leaf), C cutoffs, B refused in
check`. `ProveTerminals` is also worth a note while the machinery is being described: `PROVEN_WIN` and
`PROVEN_LOSS` are stamped only from tablebase probes, so a checkmate the search finds in its own tree is
recorded as a terminal but never seeds a proof. That is why the option converted nothing -- it was switched
on with almost nothing feeding it. Left as it is, since by this section's argument the thing it would buy
is mate distance.
