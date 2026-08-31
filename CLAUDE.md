# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`libchess` is a C++20 chess library (board representation, legal move generation, FEN/SAN/PGN, Zobrist hashing, Syzygy probing, NNUE evaluation) plus `creatica`, a UCI engine built on it that uses MCTS with NNUE leaf evaluation. `nnue/` is a heavily stripped and modified fork of Stockfish, ported to evaluate libchess's own `Board` instead of Stockfish's `Position`.

Top-level code is MIT (see `LICENSE`); `nnue/` retains Stockfish's GPL-3 headers.

## Build

In-source CMake build (generated `Makefile`, `CMakeCache.txt`, `CMakeFiles/` all live in the repo root and are untracked):

```sh
cmake .
make -j          # everything
make chess       # just libchess.dylib / .so / .dll
make test_pos    # a single target
```

CMake targets: `chess` (the shared library), `test_pos`, `test_nnue`, `test_tb`, `test_smp`, `tournament`, `lichess_bot`. **`CMakeLists.txt` is the only authority on what is live code** — most `.cpp` files in the repo root are not part of any target.

Notes:
- The library is ~110 MB because both NNUE nets are embedded at compile time via `incbin`. Net filenames are `#define`s in `nnue/evaluate.h` (`EvalFileDefaultNameBig` / `EvalFileDefaultNameSmall`); switching nets means editing that header and rebuilding `chess`.
- `CMakeLists.txt` hardcodes `-rpath /Users/ap/libchess` and `-O3 -march=native`. Adjust the rpath when building elsewhere.
- Files outside CMake carry their exact compile command in the **first comment line of the file** — that is the convention here. E.g. `creatica.cpp` starts with the `c++ -std=c++20 ... creatica.cpp uci.cpp tbcore.c tbprobe.c -o creatica` line. Use it rather than inventing flags.

## Tests

There is no test framework. Each `test_*` is a standalone binary that takes a **FEN as six separate argv words** (no argument = start position):

```sh
./test_pos "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8" w - - 0 1
./test_pos test_fen_strings        # one FEN per line; the tracked regression set
./test_tb  <fen…>                  # Syzygy root probe
./test_nnue <fen…>                 # NNUE eval + move ordering for one position
./test_smp [fen…] [ucimove] [hash] # multithreaded MCTS search
./tournament                       # engine-vs-engine self-play match
```

`test_pos` is the correctness harness worth running after any change to move generation, `do_move`/`undo_move`, hashing, or the NNUE accumulator. For every legal move in a position it checks: FEN round-trip, `reconcile()` (bitboards vs. mailbox agree), incremental `updateHash()` vs. full `getHash()`, incremental NNUE accumulator vs. a fresh context, and the result against an external Stockfish's own eval. It reports failures on stdout and returns early — read the output; the exit code is not a verdict.

External paths (`STOCKFISH`, `SYZYGY_PATH`, movetime, hash size, thread count) are `#define`s at the top of each test file, not CLI flags. Change the `#define` and rebuild. Current values assume `/Users/ap/stockfish-macos-m1-apple-silicon` and `/Users/ap/syzygy` (5-piece tables).

## Required initialization

Call once at program start, in this order:

```cpp
Stockfish::Bitboards::init();   // sliding-piece attack tables — REQUIRED, segfaults without it
zobristHash(z);                 // Zobrist tables from noise.h
init_nnue();                    // loads the embedded nets
init_nnue_context(ctx);         // one NNUEContext per search thread
```

`README.md`, the comment at the top of `libchess.h`, and several older root-level `.cpp` files still say to call `init_magic_bitboards()` / `cleanup_magic_bitboards()`. That is obsolete: `board.cpp` now uses `Stockfish::attacks_bb<>` from `nnue/bitboard.h`, and `magic_bitboards.cpp` is dead code kept only for the unbuilt experiment files.

## Architecture

**`struct Board`** (`chess_types.h:76`) — bitboards `side[2]` and `pieceTypes[6]`, plus a redundant `piecesOnSquares[64]` mailbox, castling rights/rooks, en passant file, side to move, and cached `isCheck`/`isMate`/`isStaleMate`. About 152 bytes and **carries no move history**; the caller owns the `StateInfo` stack.

`Board` has exactly **one** definition, in `chess_types.h`, together with the five enums it needs (`Color`, `File`, `Square`, `PieceType`, `Piece`) and the `static_assert`s pinning its layout. `libchess.h` includes it; `nnue/board.h` is a one-line forwarder that keeps its name so the vendored tree's ten `#include "../board.h"` lines stay byte-identical and re-syncable from upstream. Do not re-introduce a second definition — it was duplicated until 2026-08-31, both copies mangled as `4Board`, so the linker could not tell them apart and a one-sided field change would have silently produced wrong evaluations with no diagnostic.

**Move generation** (`board.cpp`) is staged, pin-aware and legal-only; there is no pseudo-legal filtering pass. The canonical loop, as written in `test_pos.cpp` and `test_smp.cpp`:

```cpp
auto [kingMoveBB, pinned, pinning, checkers, kingSq] = kingMoves(board);
// … iterate kingMoveBB first; if bitCount(checkers) > 1 those are the only legal moves
auto [check_mask, ep_mask] = checkers ? checkMask(board, kingSq, checkers)
                                      : std::make_pair(~0ULL, 0ULL);
for (PieceType pt = Queen; pt >= Pawn; --pt)
  for (each src of board.side[stm] & board.pieceTypes[pt-1])
    uint64_t moves = piece_moves(board, pt, src, kingSq, pinned, pinning, check_mask, ep_mask);
```

Move bitboards are consumed with `lsBit(moves)` / `moves &= moves - 1`. Promotions are expanded by the caller (`promoMove()` then loop `Knight..Queen`).

**Make / unmake**: `do_move(board, move, state)` for plain search; `do_move_dp(board, move, state, dp, dts)` additionally fills Stockfish's `DirtyPiece` and `DirtyThreats` so the NNUE accumulator can update incrementally. `undo_move(board, move, state)` restores from the caller's `StateInfo`. Both return the captured `PieceType`, which `updateHash()` needs.

**NNUE** (`nnue/`) — everything that isn't evaluation has been deleted from the Stockfish fork (`search.h`, `tt.h`, `movegen.h`, `movepick.h`, `thread.h`, `position.cpp`, …). `nnue/nnue.cpp` is the thin wrapper the rest of the project uses: `init_nnue`, `init_nnue_context`, `evaluate_nnue` (returns pawns, from the side-to-move's perspective), `accumulator_stack_push/pop/reset`, `nnue_eval` (trace string). Accumulator discipline is push → `do_move_dp` → … → `undo_move` → pop, one `NNUEContext` per thread. `evaluate_nnue` returns the sentinel `NNUE_CHECK` (0.00001) when the side to move is in check.

**External-engine driver** (`engine.cpp`) — spawns and drives a *separate* UCI engine over a pair of named pipes (`mkfifo` + `fork`/`execlp` on POSIX, `CreateNamedPipe` + `CreateProcess` on Win32): `initChessEngine`, `isReady`, `newGame`, `position`, `go`, `eval`, `getPV`, `stop`, `quit`, filling `struct Evaluation`. This is how `test_pos`, `tournament`, and `lichess_bot` obtain a reference eval or an opponent. It is unrelated to creatica's own search.

**Creatica engine** — `creatica.cpp` (MCTS) + `uci.cpp` (UCI protocol), sharing `creatica.hpp`. Per-thread trees held as flat `std::vector<MCTSNode>` with index-based `Edge`s, shared root statistics in atomic `root_N`/`root_W`, virtual loss, softmax move priors over NNUE evals (temperature), node value `W = tanh(eval / eval_scale)`, an exploration constant decaying linearly with depth, quiescence at leaves, Syzygy probing at ≤5 pieces, and libcurl lookups against online eval/tablebase endpoints. Every tunable is exposed as a UCI spin option (`EngineSpinOptions` in `libchess.h`) with a `#define` default in `creatica.hpp`.

> **`creatica.cpp` and `uci.cpp` do not currently compile against the reworked library.** They call the pre-rework signatures: `do_move_dp` without `DirtyThreats`, `checkMask` as returning a single mask rather than a `pair`, and `piece_moves` without `ep_mask`. That is why the `creatica` target is commented out in `CMakeLists.txt`. `test_smp.cpp` holds the up-to-date version of the same MCTS search — port from it rather than guessing.

**PGN / game layer** (`pgn.cpp`, `tag.cpp`, `move.cpp`) — `countGames`, `initGame`, `playGame`, `ecoClassify` over fixed-size `Game`/`Tag`/`EcoLine` structures sized by the `MAX_*` macros at the top of `libchess.h`; SAN ↔ `Move` ↔ UCI conversion lives in `move.cpp`.

## Repo shape

The root directory is a working scratchpad, not a curated source tree. Numbered and suffixed variants — `chess_cnn2..9`, `train_chess_cnn*`, `boards_legal_moves*.c`, `uci-*.cpp`, `*copy.cpp`, `*.old`, `creatica-*` — are historical experiments, largely from an earlier libtorch CNN/MCTS line of work and from the original C version of the library. They are stale with respect to the current API. Check `CMakeLists.txt` before assuming a file matters, and do not "fix" or refactor the variants as a side effect of other work.

`README.md` documents the pre-rework C codebase (`board.c`, `game_omp.c`, `piece.c`, `square.c`, OpenMP, SQLite, libtorch CNN training) and the cffi Python bindings flow (`gcc -E libchess.h > libchess.ph`; `python3 tasks.py`; `eval.py`). The checked-in `libchess.ph` predates the C→C++20 rework and the bindings no longer regenerate — `libchess.h` now uses templates, references, and `std::pair`/`std::tuple` returns that cffi's `cdef` cannot parse. Treat the README's build lines as historical.

## Conventions

- Configuration is `#define`s at the top of the relevant `.cpp`, not flags or config files.
- Boards are passed by reference; move generators return `uint64_t` bitboards; multi-value returns use `std::pair`/`std::tuple` with structured bindings.
- Logging is C-style `printf`/`fprintf`; the engine writes `uci.log` and `print()`/`log_file()` in `uci.cpp` are the mutex-guarded wrappers.
- Public library symbols are marked `CHESS_API` in `libchess.h` (a no-op outside Win32).
