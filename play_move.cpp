//compile with c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -I /Users/ap/libchess  -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess play_move.cpp -o play_move
#include <string>
#include <cstdio>
#include "libchess.h"

int main(int argc, char ** argv) {
  std::string fenString;
  std::string uci_move;
  if (argc == 3) {
    fenString = std::string(argv[1]);
    uci_move = std::string(argv[2]);
  }
  else {
    fprintf(stderr, "Usage: play_move <fenString> <uci_move>\n");
    exit(1);
  }
  init_magic_bitboards();
  Board board;
  Move move;
  if (fenString == "startpos") fenString = startPos;
	if (fen2board(board, fenString.c_str())) {
		fprintf(stderr, "error: fen2board() failed; FEN %s\n", fenString.c_str());
		return 1;
	}
	uci2move_idx(uci_move.c_str(), move);
	ff_move(board, move);
	fprintf(stderr, "fen after move %s: %s\n", uci_move.c_str(), fenString.c_str());
	writeDebug(board);
  cleanup_magic_bitboards();
}