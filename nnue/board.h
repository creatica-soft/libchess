// nnue/board.h -- FORWARDER ONLY.
//
// struct Board has exactly one definition and it lives in ../chess_types.h.
// This file keeps its name and location so that the ten
//     #include "../board.h" / #include "../../board.h"
// lines inside the vendored Stockfish tree stay byte-identical, which is what
// makes nnue/ re-syncable from upstream: this is a libchess file, there is no
// upstream counterpart, and it must never grow a second definition again.
//
// It also fixes a latent bug in the file it replaces: that one used uint8_t and
// uint64_t with no #include <cstdint> and compiled only because every include
// site happened to pull types.h first.

#include "../chess_types.h"
