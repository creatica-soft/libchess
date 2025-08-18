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

void piece(struct Square * sq, struct ChessPiece * pc, int pcName) {
	pc->name = pcName;
	pc->type = pcName & 7;
	pc->color = pcName >> 3;
	if (sq != &(pc->square))
	  memcpy(&(pc->square), sq, sizeof (struct Square));
}

#ifdef __cplusplus
}
#endif
