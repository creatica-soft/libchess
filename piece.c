#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "libchess.h"

void piece(struct Square * sq, struct ChessPiece * pc, enum PieceName pcName) {
	pc->name = pcName;
	pc->type = pcName & 7;
	pc->color = pcName >> 3;
	memcpy(&(pc->square), sq, sizeof (struct Square));
}
