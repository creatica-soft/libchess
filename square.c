#pragma warning(disable:4334)

#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "libchess.h"

void square(struct Square * square, enum SquareName sqName) {
	square->name = sqName;
	if (sqName != SquareNone) {
		square->bitSquare = 1UL << sqName;
		square->file = sqName & 7;
		square->rank = sqName >> 3;
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
