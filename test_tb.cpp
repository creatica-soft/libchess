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

#include "libchess.h"

#define SYZYGY_PATH "/Users/ap/syzygy"

  int main(int argc, char ** argv) {
    struct Board board;
    struct Fen fen;
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
    
    tb_init(SYZYGY_PATH);
    if (TB_LARGEST == 0) {
        printf("error unable to initialize tablebase; no tablebase files found in %s\n", SYZYGY_PATH);
    } else {
      printf("info string successfully initialized tablebases in %s. Max number of pieces %d\n", SYZYGY_PATH, TB_LARGEST);
    }
    
    const unsigned int ep = lsBit(enPassantLegalBit(&board));
    unsigned int res = tb_probe_root(board.occupations[PieceNameWhite], board.occupations[PieceNameBlack], 
    board.occupations[WhiteKing] | board.occupations[BlackKing],
        board.occupations[WhiteQueen] | board.occupations[BlackQueen], board.occupations[WhiteRook] | board.occupations[BlackRook], board.occupations[WhiteBishop] | board.occupations[BlackBishop], board.occupations[WhiteKnight] | board.occupations[BlackKnight], board.occupations[WhitePawn] | board.occupations[BlackPawn],
        board.fen->halfmoveClock, 0, ep == 64 ? 0 : ep, board.fen->sideToMove == ColorWhite ? 1 : 0, NULL);
    //if (res == TB_RESULT_FAILED) {
      fprintf(stderr, "info: res %d, TB_LARGEST %d, occupations %u, fen %s, ep %u, halfmoveClock %u, whiteToMove %u, whites %llu, blacks %llu, kings %llu, queens %llu, rooks %llu, bishops %llu, knights %llu, pawns %llu\n", res, TB_LARGEST, __builtin_popcountl(board.occupations[PieceNameAny]), board.fen->fenString, ep, board.fen->halfmoveClock, board.fen->sideToMove == ColorWhite ? 1 : 0, board.occupations[PieceNameWhite], board.occupations[PieceNameBlack], board.occupations[WhiteKing] | board.occupations[BlackKing],
        board.occupations[WhiteQueen] | board.occupations[BlackQueen], board.occupations[WhiteRook] | board.occupations[BlackRook], board.occupations[WhiteBishop] | board.occupations[BlackBishop], board.occupations[WhiteKnight] | board.occupations[BlackKnight], board.occupations[WhitePawn] | board.occupations[BlackPawn]);
      //exit(-1);
    //}
    double result;
    unsigned int wdl = TB_GET_WDL(res); //0 - loss, 4 - win, 1..3 - draw
    if (wdl == 4) result = 1.0;
    else if (wdl == 0) result = -1.0;
    else result = 0.0;
    unsigned int src = TB_GET_FROM(res);
    unsigned int dst = TB_GET_TO(res);
    unsigned int promotes = TB_GET_PROMOTES(res);
    printf("result %.0f, uci_move %s%s%c\n", result, squareName[src], squareName[dst], uciPromoLetter[6 - promotes]);
    cleanup_magic_bitboards();
  }
