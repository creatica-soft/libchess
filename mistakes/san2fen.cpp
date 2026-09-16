// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -O2 -I /Users/ap/libchess -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess -o mistakes/san2fen mistakes/san2fen.cpp
// SAN moves on stdin (optionally preceded by a line "FEN <fen>"): prints the final FEN and the UCI move list.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "nnue/nnue/nnue_accumulator.h"
#include "nnue/bitboard.h"
#include "libchess.h"
int main(int argc, char ** argv) {
  Stockfish::Bitboards::init();
  Zobrist z; zobristHash(z);
  Board board;
  fen2board(board, argc > 1 ? argv[1] : "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
  std::vector<StateInfo> st(2048);
  char san[32]; int ply = 0; std::string uci;
  while (std::scanf("%31s", san) == 1) {
    Move m;
    if (san2move(board, san, m) != 0) { std::fprintf(stderr, "bad SAN %s at ply %d\n", san, ply); return 1; }
    char u[8]; std::snprintf(u, sizeof u, "%s%s", square[m.src], square[m.dst]);
    uci += u; if (m.promoType != PieceTypeNone) uci += uciPromoLetter[m.promoType]; uci += ' ';
    do_move(board, m, st[ply++]);
    if (getenv("EVERY_PLY")) { char f[128]; std::printf("%d\t%s\t%s\n", ply, board2fen(board, f), uci.c_str()); }
  }
  char fen[128];
  std::printf("%d\t%s\t%s\n", ply, board2fen(board, fen), uci.c_str());
  return 0;
}
