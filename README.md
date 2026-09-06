# libchess

A C++20 chess library — board representation, legal move generation, FEN/SAN/PGN, Zobrist
hashing, Syzygy probing and NNUE evaluation — together with **creatica**, a UCI engine built
on it that searches with MCTS and evaluates leaves with NNUE.

The library descends from a C# chess-game analyser
(https://chessgame-analyzer.creatica.org) by way of a C implementation, and was reworked to
C++20. `nnue/` is a heavily stripped fork of Stockfish, ported to evaluate libchess's own
`Board` rather than Stockfish's `Position`.

Top-level code is MIT (see `LICENSE`); `nnue/` retains Stockfish's GPL-3 headers.

> **Historical code lives in [`attic/`](attic/README.md)** — the libtorch CNN and
> transformer era, the original C implementation, the cffi Python bindings, and superseded
> engines. It is stale against the current API and nothing builds it. The root directory is
> still a working scratchpad rather than a curated tree, so `CMakeLists.txt` and the compile
> lines described below remain the authority on what is live.


## Build

In-source CMake build; the generated `Makefile`, `CMakeCache.txt` and `CMakeFiles/` live in
the repository root and are untracked.

```sh
cmake .
make -j            # everything
make chess         # just the shared library
make test_pos      # a single target
```

CMake targets: `chess` (the shared library), `test_pos`, `perft`, `test_nnue`, `test_tb`,
`test_smp`, `tournament`, `lichess_bot`.

**Everything else carries its exact compile command in a comment at the top of its own
source file** — the first line, or the second where the first reads `//For MacOS using
clang`. That is the convention here; use it rather than inventing flags:

| binary | source |
|---|---|
| `creatica` | `creatica.cpp` (options and UCI adapter) + `creatica_search.cpp` (the MCTS search) |
| `creatica-shared-root` | `creatica-shared-root.cpp` — the engine before the policy head, kept as a baseline |
| `creatica-shared-root` | `creatica-shared-root.cpp` — the engine without the policy head |
| `nnue_policy_train` | `nnue_policy_train.cpp` — trains the policy head (needs libtorch) |
| `gui_helper` | `gui_helper.cpp` — move legality and PGN parsing for the browser GUI |
| `bench_policy_net`, `bench_prior_acc`, `bench_blend` | prior-quality measurement |

Notes:

- The library is around 110 MB because both NNUE nets are embedded at compile time with
  `incbin`. The filenames are `#define`s in `nnue/evaluate.h`; switching nets means editing
  that header and rebuilding `chess`.
- `CMakeLists.txt` hardcodes `-rpath /Users/ap/libchess` and `-O3 -march=native`. Adjust the
  rpath when building elsewhere.
- Build **without** extra SIMD macros. `-march=native` alone is measurably faster here than
  forcing `USE_NEON`.


## Required initialisation

Call once at program start, in this order:

```cpp
Stockfish::Bitboards::init();   // sliding-piece attack tables — REQUIRED, segfaults without it
zobristHash(z);                 // Zobrist tables from noise.h
init_nnue();                    // loads the embedded nets
init_nnue_context(ctx);         // one NNUEContext per search thread
```

`init_magic_bitboards()` / `cleanup_magic_bitboards()` are **obsolete**. `board.cpp` uses
`Stockfish::attacks_bb<>` from `nnue/bitboard.h`, and `magic_bitboards.cpp` is dead code kept
only for the unbuilt experiment files. Older comments in the tree still mention them.


## The engine

`creatica` is a UCI engine: per-thread MCTS trees over a shared root, NNUE leaf evaluation,
Syzygy probing at five pieces or fewer, and a learned policy head blended into the move
priors. It plays roughly 100 Elo above the same engine without the policy head.

Every tunable is a UCI option — there are no environment variables — and the option block is
generated from the engine's own declarations, so adding a knob is one line in the engine.
See **[ENGINE_SETTINGS.md](ENGINE_SETTINGS.md)** for what each one does and what has actually
been measured about them.

```sh
./creatica                      # speaks UCI on stdin/stdout
./creatica bench                # one fixed search, for a smoke test after a build
```

Two files carry the shared protocol layer and are used by any engine here:
`uci_options.h` (self-describing options), `uci_engine.h` (the `SearchEngine` interface) and
`uci_frontend.cpp` (the one protocol loop).


## Browser GUI

A local web interface for playing, analysing and running engine matches:

```sh
python3 chess_gui.py            # then open the printed URL
```

Standard library only — nothing to install. It binds to `127.0.0.1`. Three tabs:

- **Play** — human against any engine, with legal-move highlighting and undo
- **Analyse** — step through a game or position, with evaluation, best move and PV; engine
  settings can be changed per analysis, so two configurations can be compared on one position
- **Tournament** — engine-vs-engine matches with a configurable opening book, a live board,
  and PGN saved as each game finishes

Move legality and PGN parsing come from `gui_helper`, which links `libchess` — deliberately,
so the GUI can never disagree with the engine about what is legal. Settings editors are built
from what each engine advertises rather than a hand-maintained list.


## Tests

There is no test framework. Each `test_*` is a standalone binary taking a **FEN as six
separate argv words** (no argument means the start position):

```sh
./test_pos "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8" w - - 0 1
./test_pos test_fen_strings        # one FEN per line; the tracked regression set
./test_tb  <fen…>                  # Syzygy root probe
./test_nnue <fen…>                 # NNUE eval and move ordering for one position
./test_smp [fen…] [ucimove] [hash] # multithreaded MCTS search
./tournament                       # engine-vs-engine self-play match
```

`test_pos` is the correctness harness worth running after any change to move generation,
`do_move`/`undo_move`, hashing or the NNUE accumulator. For every legal move in a position it
checks the FEN round-trip, `reconcile()` (bitboards against the mailbox), incremental
`updateHash()` against a full `getHash()`, the incremental NNUE accumulator against a fresh
context, and the result against an external Stockfish's own evaluation. **It reports failures
on stdout and returns early — read the output; the exit code is not a verdict.**

External paths (`STOCKFISH`, `SYZYGY_PATH`, movetime, hash size, thread count) are `#define`s
at the top of each test file, not command-line flags.


## Training the policy head

`nnue_policy_train.cpp` trains a policy head over the NNUE feature transformer's own output,
from lichess evaluation shards. It needs libtorch; the compile line is in the file.

```sh
BUILD_CACHE=<dir> ./nnue_policy_train          # extract features once (~48 min)
FEATURE_CACHE=<dir> BATCH_SIZE=8192 LR_MAX=2e-3 EPOCHS=5 ./nnue_policy_train
EXPORT_WEIGHTS=nnue_policy.bin ./nnue_policy_train   # write the flat net the engine loads
```

Caching the extracted features removes about 73% of the per-sample CPU cost and is worth
doing before any long run. The training knobs are environment variables — this is a script,
not a protocol server — and are documented in
[ENGINE_SETTINGS.md](ENGINE_SETTINGS.md#training-options).


## Historical

`README` previously documented the C codebase (`board.c`, `game_omp.c`, OpenMP, SQLite) and a
cffi Python-bindings flow (`gcc -E libchess.h > libchess.ph`, `tasks.py`, `eval.py`). Those
bindings no longer regenerate: `libchess.h` now uses templates, references and
`std::pair`/`std::tuple` returns that cffi's `cdef` cannot parse. The checked-in `libchess.ph`
predates the C to C++20 rework. The libtorch CNN and transformer models from that era are
still in the tree but are not built.


## Licence

Top-level code is MIT — see `LICENSE`. `nnue/` is a fork of Stockfish and retains its GPL-3
headers.

Fathom Syzygy tablebase probing code is included under its own terms:

> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
> INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
> PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
> FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
> OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
> DEALINGS IN THE SOFTWARE.
