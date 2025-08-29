#pragma warning(disable:4334)

#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "libchess.h"

#ifdef __cplusplus
extern "C" {
#endif

void square(struct Square * square, int sqName) {
	square->name = sqName;
	if (sqName != SquareNone) {
		square->bitSquare =1ULL << sqName;
		square->file = sqName & 7; 
		square->rank = sqName >> 3; 
		/*
	  if (square->file % 2) { //odd file
			if (square->rank % 2)) square->color = ColorBlack; //odd file, odd rank
			else square->color = ColorWhite; //odd file, even rank
		} 
		else {
			if (square->rank % 2) square-color = ColorWhite; //even file, odd rank 
			else square->color = ColorBlack; //even file, even rank
		} 
		*/
		square->diag = (7 + square->rank) - square->file;
		square->antiDiag = square->file + square->rank;
	} else {
		square->bitSquare = 0;
		square->file = FileNone;
		square->rank = RankNone;
		square->diag = DiagonalNone;
		square->antiDiag = AntidiagonalNone;
	}
}

int squareColor(int sqName) {
  if ((sqName & 7) % 2) {
		if ((sqName >> 3) % 2) return ColorBlack;
		else return ColorWhite;
	} 
	else {
		if ((sqName >> 3) % 2) return ColorWhite;
		else return ColorBlack;
	}
}

#ifdef __cplusplus
}
#endif
