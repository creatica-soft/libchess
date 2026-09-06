// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -O3 -I /Users/ap/libchess -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess -o bench_encode bench_encode.cpp
// Board -> 576-float model input. This is per-position work the search must pay on top
// of the forward pass, and it is not in the libtorch timing.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <vector>
#include "nnue/bitboard.h"
#include "libchess.h"
Board board; Zobrist z;
static inline void encode(const Board& b, float * out) {
  memset(out, 0, 576 * sizeof(float));
  // 12 piece-type planes x 64 squares = 768 in a full one-hot; the 576 layout packs
  // 8 planes + state, so approximate the same work: one pass over the mailbox plus
  // the bitboard-derived state features.
  for (int sq = 0; sq < 64; ++sq) {
    int pc = b.piecesOnSquares[sq];
    if (pc != PieceNone) out[(pc & 7) * 64 + sq] = 1.0f;
  }
  out[512] = (float)b.sideToMove;
  out[513] = (float)b.castlingRights;
  out[514] = (float)b.enPassant;
  out[515] = (float)b.halfmoveClock;
  for (int i = 0; i < 6; ++i) out[516 + i] = (float)bitCount(b.pieceTypes[i]);
}
int main(int argc, char ** argv) {
  zobristHash(z); Stockfish::Bitboards::init();
  std::vector<std::string> fens; std::ifstream f(argc > 1 ? argv[1] : "test_fen_strings");
  for (std::string l; std::getline(f, l); ) if (!l.empty()) fens.push_back(l);
  std::vector<Board> boards;
  for (auto& s : fens) if (!fen2board(board, s.c_str())) boards.push_back(board);
  alignas(64) float buf[576];
  const int REPS = 20000;
  auto t0 = std::chrono::steady_clock::now();
  volatile float sink = 0;
  for (int r = 0; r < REPS; ++r) for (auto& b : boards) { encode(b, buf); sink += buf[0]; }
  double us = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()
              * 1e6 / (REPS * (double)boards.size());
  printf("board -> 576-float encode: %.3f us per position\n", us);
  printf("  at batch 32 that is %.1f us of encoding per forward\n", us * 32);
  return 0;
}
