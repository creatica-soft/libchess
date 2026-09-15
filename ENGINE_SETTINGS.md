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
| `TreeResetBelow` | spin | 0 | 0–1000000000 | **Off at 0.** Before a search, if the tree is at `TreeResetOccupancy` or more and the root inherited fewer than this many informed simulations (its evidence), discard the whole tree instead of collecting it. Aimed at the one case a full tree turns into a blunder: a root the last search barely looked at, on a tree nothing can be freed from. 1000 is the value tested in replays; it has not been measured in games. See *A full tree that cannot be collected*. |
| `TreeResetOccupancy` | spin | 950 | 0–1000 | Per-mille of `Hash` at or above which `TreeResetBelow` applies. Below 950 no search in 22 games froze, so resetting there would only throw knowledge away. |
| `TreeResetHollow` | spin | 0 | 0–1000 | **Off at 0.** A second trigger for the same reset: discard the tree when the *previous* search was at least this many per-mille hollow — the tree is frozen, whatever the root inherited. Applies at `TreeResetOccupancy` or more. 900 is the value tested in replays; not yet measured in games. |
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
| `PolicyWeights` | string | `nnue_policy.bin` | — | The exported policy net. Changing it reloads the net; if the load fails the previous net is kept and an `info string` says why, so the engine never runs on a half-loaded net. A reload is refused while a search is running, because the worker threads read the net without a lock. The `info string` names the row count, which is how you check which net is actually in use: **4096 = the narrow table, 24576 = piece-indexed**, and then lists `legality term` and `spatial term: …` when the net has them. See *Policy file formats*. |
| `PolicyMode` | spin | 1 | 0–2 | 0 = off, 1 = prior, 2 = full. See below. |
| `PolicyBlend` | spin | 45 | 0–100 | Share of the prior taken from the policy head, in hundredths; the rest comes from the 1-ply child evaluations. 0 reproduces the incumbent prior exactly, 100 is policy-only. Ignored when `PolicyMode` is 2. |
| `PolicyTemp` | spin | 75 | 0–300 | Temperature the policy logits are read at, in hundredths. Chosen so the resulting prior has the concentration the exploration constants were tuned for. |
| `BlendScale` | spin | 115 | 0–300 | Overall sharpness of the blended prior, in hundredths. A mixture of two disagreeing signals is flatter than either alone; this restores it. |
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
PolicyTemp 0.75, BlendScale 1.15, FpuReduction 0.2, SeedChildren true, NodeMinimax true
```

There is also a `settings` command, outside the UCI protocol, that prints the same values
in a readable column with both the engine-side number and the UCI integer.

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

The engine implements 1, 2, 4, 5, 7 and 8. Versions 3 and 6 use the untied `Cl` table and are
refused. An engine older than versions 7 and 8 refuses them too, which is deliberate: read as a 2 or
a 5, a spatial net would run without its spatial term.

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

**The spatial term** (versions 7 and 8) adds `dot(u[from], v[to])` to each move's score, where `u`
and `v` are per-square vectors computed from 8x8 planes by 3x3 convolutions. It is what lets the
policy see the board as a board: which squares are defended, and by what. The planes, all oriented
from the side to move's point of view, are: own pieces by type (0–5), enemy pieces (6–11), squares a
legal move starts from (12), squares a legal move lands on (13), and squares attacked by each piece
type of each side, empty squares included (14–25) — the part the NNUE threat features cannot supply,
because they only index attacks on occupied squares.

The spatial block after `Wl` is an 8-integer header — magic `SPAT`, planes, channels, dim, layers,
scale, plane layout, attack planes — followed by `Wc1`, `bc1`, `Wc2`/`bc2` if there are two layers,
`Wu` and `Wv`. The engine reads only shapes it implements (14 or 26 planes, up to 64 channels, dim up
to 32, one or two layers) and refuses the rest.

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
measurable difference, which is what the blended accuracy above predicts. A match on an engine
without the crash (`creatica_prefetch` or later) is needed before reading anything more into it.

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
PV_PLIES 16          MAX_DEPTH 100        SYZYGY_PATH "/Users/ap/syzygy"
POLICY_WEIGHTS_DEFAULT "nnue_policy.bin"  FPU_REDUCTION 0.20
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
   Every engine binary loads `libchess.dylib` when it starts, so **the older `creatica_*` binaries get
   this fix too**, without being rebuilt.
2. **The worker no longer parses anything.** The thread that asks for the prefetch hands over the
   `Board` it already has, and the worker copies it.

**The prefetch had never worked.** The same code checked the result of `uci2move_idx()` as if it were an
error code. It is the move's index, so the check failed for every real move and the worker gave up
before fetching anything: 1,645 attempts in one match, none fetched. Now the move is checked for
legality instead. Tested on two seven-piece endings where the opponent has one best reply: the log shows
`prefetched the position after e8d7 -- cached`. Old binaries still have this defect, so their prefetch
still does nothing. `creatica_prefetch` (built 15 September) is the first binary with a working one.

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

**The binaries left by the 2026-09-10 work**, oldest first, each adding to the one before, so a
result can be attributed to a step rather than to the whole stack:

| binary | adds |
|---|---|
| `creatica_base` | nothing — the engine as it was when the endgame drew |
| `creatica_cp` | eviction floor; repetition-filter visit floor; honest reported score |
| `creatica_edge` | search-summary logging; edge-local *N(s,a)* |
| `creatica_tb` | async tablebase probe, warm connection, cache, one-move prefetch |
| `creatica` | the above plus repetition counting, the Q guard, and both new options |

`creatica` against `creatica_base` compares everything from that day *except* the two experiments,
since `EdgeVisits` defaults off and `RepetitionGuard` defaults to the old threshold. One caveat when
reading such a result: `creatica_base` carries the old blocking tablebase probe, and in a self-play
match the two bots share an IP and rate-limit each other at that endpoint, which penalises the old
engine harder than a real opponent would.

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
| Is `BlendScale 1.15` right? | running now, sweeping 0.85–1.45 | it was set analytically to match the old prior's concentration and has never been tested at all |
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
  CREATICA_BOOK=/Users/ap/libchess/book.tsv \
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

Things that have actually cost time here:

- **Give each bot its own `CREATICA_VISITS` and let it have its own `CREATICA_LOG`.** Two engines
  appending to one log is not merely untidy: creatica ponders the position where the OPPONENT is
  to move, which is the same position the opponent is searching for real, so a shared log shows
  what looks like one engine emitting two `bestmove` lines. That artefact was read as a protocol
  violation and a driver change was made on the strength of it. The bot now sets `CREATICA_LOG`
  per instance by default.
- **`CREATICA_HASH=1024` on both bots swaps an 8 GB machine.** Trees reach 10M nodes, which is
  about a gigabyte each. 512 is the safer pairing.
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
setEngineStringOption(engine, "SyzygyPath", "/Users/ap/syzygy");
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
