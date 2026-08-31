//c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -I /Users/ap/libchess  -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess test_tb.cpp tbcore.c tbprobe.c -o test_tb

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <iostream>
#include <fstream>
#include <string>
#include "tbprobe.h"

#ifdef __GNUC__
#include <cstring>
#endif
#include "nnue/bitboard.h"
#include "libchess.h"

#define SYZYGY_PATH "/Users/ap/syzygy"

  int main(int argc, char ** argv) {
    struct Board board;
    char fenString[MAX_FEN_STRING_LEN] = "";
    char uciMove[6] = "";
  	if (argc == 1) strncpy(fenString, startPos, MAX_FEN_STRING_LEN);
  	else if (argc >= 7) {
  	  for (int i = 1; i < 7; i++) {
  	    strcat(fenString, argv[i]);
  	    strcat(fenString, " ");
  	   }
  	} 
  	Stockfish::Bitboards::init();
  	if (fen2board(board, fenString)) {
  		printf("test_nnue error: fen2board() failed; FEN %s\n", fenString);
  		return 1;
  	}
    
    tb_init(SYZYGY_PATH);
    if (TB_LARGEST == 0) {
        printf("error unable to initialize tablebase; no tablebase files found in %s\n", SYZYGY_PATH);
    } else {
      printf("info string successfully initialized tablebases in %s. Max number of pieces %d\n", SYZYGY_PATH, TB_LARGEST);
    }
    
    const unsigned int ep = legalEnPassantMove(board);
    unsigned int res = tb_probe_root(board.side[ColorWhite], board.side[ColorBlack], board.pieceTypes[King - 1], board.pieceTypes[Queen - 1], board.pieceTypes[Rook - 1], board.pieceTypes[Bishop - 1], board.pieceTypes[Knight - 1], board.pieceTypes[Pawn - 1],
        board.halfmoveClock, 0, ep == SquareNone ? 0 : ep, board.sideToMove == ColorWhite ? 1 : 0, NULL);
        char fen[MAX_FEN_STRING_LEN];
        fprintf(stderr, "info: res %u, TB_LARGEST %d, occupations %u, fen %s, ep %u, halfmoveClock %u, whiteToMove %u, whites %llu, blacks %llu, kings %llu, queens %llu, rooks %llu, bishops %llu, knights %llu, pawns %llu\n", res, TB_LARGEST, __builtin_popcountl(board.side[ColorWhite] | board.side[ColorBlack]), board2fen(board, fen), ep, board.halfmoveClock, board.sideToMove == ColorWhite ? 1 : 0, board.side[ColorWhite], board.side[ColorBlack], board.pieceTypes[King - 1],
        board.pieceTypes[Queen - 1], board.pieceTypes[Rook - 1], board.pieceTypes[Bishop - 1], board.pieceTypes[Knight - 1], board.pieceTypes[Pawn - 1]);
    double result;
    unsigned int wdl = TB_GET_WDL(res); //0 - loss, 4 - win, 1..3 - draw
    if (wdl == 4) result = 1.0;
    else if (wdl == 0) result = -1.0;
    else result = 0.0;
    unsigned int src = TB_GET_FROM(res);
    unsigned int dst = TB_GET_TO(res);
    unsigned int promotes = TB_GET_PROMOTES(res);
    printf("result %.0f, uci_move %s%s%c\n", result, square[src], square[dst], uciPromoLetter[6 - promotes]);
  }
