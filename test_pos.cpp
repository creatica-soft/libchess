// compile with c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -Wl,-lchess,-rpath,/Users/ap/libchess -L /Users/ap/libchess -o test_pos test_pos.cpp
#include <algorithm>
#include "tbprobe.h"
#include "libchess.h"


int main(int argc, char ** argv) {
  struct Board board;
  struct Fen fen;
	struct Move move;
  char fenString[MAX_FEN_STRING_LEN] = "";
  char uciMove[6] = "";
	if (argc == 1) strncpy(fenString, startPos, MAX_FEN_STRING_LEN);
	else if (argc >= 7) {
	  for (int i = 1; i < 7; i++) {
	    strcat(fenString, argv[i]);
	    strcat(fenString, " ");
	   }
	} 
	init_magic_bitboards();
	if (strtofen(&fen, fenString)) {
		printf("test_nnue error: strtofen() failed; FEN %s\n", fenString);
		return 1;
	}
	if (fentoboard(&fen, &board)) {
		printf("test_nnue error: fentoboard() failed; FEN %s\n", fen.fenString);
		return 1;
	}

  generateMoves(&board); //needed for checks such as isMate or isStaleMate as well as for sim_board->movesFromSquares
  if (board.isMate) printf("%s is mated\n", color[board.fen->sideToMove]);
  else if (board.isStaleMate) printf("stalemate, %s has no moves\n", color[board.fen->sideToMove]);
  else if (board.isCheck) printf("%s is checked\n", color[board.fen->sideToMove]);
  updateFen(&board);
  reconcile(&board);
  writeDebug(&board, true);
  cleanup_magic_bitboards();
  return 0;
}
