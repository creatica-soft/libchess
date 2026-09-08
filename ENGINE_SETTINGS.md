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
| `GcThreshold` | spin | 700 | 0–1000 | Per-mille of `Hash` at which the tree is collected. Collection is O(tree) and reclaims only memory — an unreachable node is never traversed — so collecting every move paid a growing cost for nothing. `creatica` only. |
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
| `PolicyWeights` | string | `nnue_policy.bin` | — | The exported policy net. Changing it reloads the net; if the load fails the previous net is kept and an `info string` says why, so the engine never runs on a half-loaded net. A reload is refused while a search is running, because the worker threads read the net without a lock. |
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
low bits of the Zobrist key, linear probing, erase as a tombstone write, no per-entry allocation.

**The first attempt at it was slower, for a reason worth remembering.** Open addressing iterates in
O(*capacity*); `std::unordered_map` iterates in O(*size*), because libc++ threads every element
onto a linked list and never pays for empty space. After a large die-off the flat table still held
~16M slots for 1.6M live entries, and every later sweep rescanned all of them — a collection that
freed *nothing* walked 1.6M live entries in 551 ms, where `unordered_map` walked 4.0M in 390 ms.
Per slot the flat table was about three times quicker; it was simply scanning ten times as many.
`NodeMap::compact()` rehashes down to fit the survivors after each sweep, which also sweeps out the
tombstones, and is cheap precisely because there is no per-entry allocation.

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
then plays, so the sibling subtrees that become garbage are nearly empty. A collection is now
skipped when the previous one reclaimed less than 5%, unless occupancy has reached
`GcThreshold`, which still forces one. In a 20-move test that cut collections from 20 to 2
while keeping the tree bounded.

Declining a futile collection is not the deferred-bulk-work pattern that `GcThreshold` exists
to avoid: there is no work being deferred, because there is nothing there to reclaim.

**Sizing.** The tree reached 9.8M nodes on a 1 GB `Hash` in a single 3+2 game with pondering,
i.e. the ceiling. Roughly 10M nodes per GB. Size `Hash` for the whole game, not for one move.


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
| ~~Does chess960 work at all?~~ **TESTED — three defects found and fixed** | perft against Stockfish (`UCI_Chess960`) on 1,860 generated positions | It did not. **(1)** `castlingMoves()` tested the king's path against attacks computed with the castling ROOK STILL IN PLACE, so an enemy slider behind it on the back rank was invisible — white king d1, white rook b1, black queen a1: castling vacates b1 and the queen then attacks c1. Standard chess cannot produce this, because the queenside rook starts on a1 with nothing behind it. **(2)** `undo_move()` restored the castling rook to its source before clearing its destination, and in 960 those can be the SAME square — the rook was written and immediately erased from the mailbox. The bitboards survived (two XORs of one bit cancel), so every count-based test passed with the bug present; only `reconcile()` saw it, and it matters because `piecesOnSquares[]` is what NNUE reads. **(3)** `fen2board()` could not read X-FEN at all — plain `KQkq`, where K means the outermost rook, which is exactly what lichess sends (`rbnqbnkr/... w KQkq`, king on g1). 124 of 3840 runs failed on the X-FEN encoding of positions that passed in Shredder form. Suites: `perft_suite_960.txt` (960 starts), `perft_suite_960_mid.txt` (900 midgame positions where castling is legal NOW, two independent seeds), `perft_suite_960_xfen.txt`. **`lichess_bot` still declines 960** — the library is tested, the bot path is not. |

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
| `TOTAL_STEPS` | 300M/batch | Steps the cosine anneals over. Must match the real run length or the schedule bottoms out early or never reaches its floor. |
| `LABEL_SMOOTH` | 0 | Share of target mass spread uniformly over the legal moves. 0 is the original loss. |
| `VALIDATE_EVERY` / `VALIDATE_N` | 1 / 1000000 | Validation runs after every *file*, not every epoch — 28 times an epoch. Both were needed to stop validation costing more than training. |
| `NUM_WORKERS` | 1 | DataLoader workers. **Leave at 1.** Two workers measured *half* the throughput of one: each holds its own accumulator cache, so they get colder caches and contend for memory bandwidth on the 1024-wide accumulator rows. The same shape as the engine's thread finding. |
| `WEIGHTS` / `CKPT_PREFIX` | `nnue_policy.pt` / … | Lets a differently-shaped model train without colliding with the current one. |
| `EXPORT_WEIGHTS` | — | Write the trained net as the flat `.bin` the engine loads, and exit. |

The schedule resumes across restarts: a `.step` file beside the weights records the
optimizer step, so a killed run continues its cosine instead of jumping back to `LR_MAX`.
Delete it to start a schedule over. Note that changing `BATCH_SIZE` invalidates it, since a
step no longer means the same number of positions.


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
