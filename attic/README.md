# attic

Historical code, kept for reference and not built by anything.

These files are **stale against the current API** and most will not compile. They are here
rather than deleted because they record how the project got to where it is, and git history
alone makes them easy to forget. Nothing in the live tree includes or links any of them.

- **libtorch CNN and transformer era** — `chess_cnn*`, `chess_trans*`, `train_chess_*`,
  `resnet_cnn_train.cpp`, `simple_cnn*`, `dual_head_kan_train.cpp`. An earlier line of work
  that trained board-plane models and searched with them. Superseded by the NNUE policy head
  (`nnue_policy_train.cpp`), which reads the NNUE feature transformer's own output instead of
  hand-designed planes.
- **The original C implementation** — `boards_legal_moves*.c`, `game_omp.c`, `sqlite.c`,
  `dbsearch.c`, `my_md5.cpp`, `libchess.ph`. From before the C to C++20 rework, with OpenMP
  and SQLite dependencies the library no longer has.
- **Python bindings and scripts** — `tasks.py`, `eval.py`, `genEndGames.py`,
  `openGamesFromPGNfile*.py`. The cffi flow no longer regenerates: `libchess.h` now uses
  templates, references and `std::pair`/`std::tuple` returns that cffi's `cdef` cannot parse.
- **Superseded engines** — `creatica.cpp`/`uci.cpp` (the original, which calls pre-rework
  signatures), `creatica-kan.*`/`uci-kan.cpp` (the KAN-model engine), `lichess_bot2.cpp`,
  `chess_mcts_smp.cpp`, `chess_cnn_mcts.cpp`.
- **Superseded search experiments** — `minimax.cpp`, `negamax.cpp`, `test_quiescence.cpp`,
  `bench_priors.cpp`, `bench_encode.cpp`, `nnue_eval.cpp`, `play_move.cpp`.

If you need one of these, expect to port it: move generation, `do_move`/`undo_move` and the
NNUE accumulator have all changed signatures. `test_smp.cpp` in the root holds the current
version of the MCTS search and is the right thing to copy from.
