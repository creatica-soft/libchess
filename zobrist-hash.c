#pragma warning(disable:4334)

#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "libchess.h"

void zobristHash(struct ZobristHash * hash) {
  //printf("starting zobristHash...\n");
	hash->hash = 0;
	hash->prevEnPassant = 0;
	hash->prevCastlingRights = 0;
	int k = 0;
	//empty square hashes (64) - perhaps it is not needed to hash empty squares for performance and simplicity reasons
  //but to minimize the chance of a collision, it's probably beneficient!
  /*
	for (enum SquareName s = SquareA1; s <= SquareH8; s++) 
		hash->piecesAtSquares[0][s] = bitStrings[k++];*/
	//occupied square hashes (1 pawn x 2 color x 48 + 5 pieces x 2 color x 64 = 752)
  //printf("zobristHash: starting main loops...\n");
	for (int c = ColorWhite; c <= ColorBlack; c++) {
    //printf("zobristHash: color loop %d\n", c);
		for (int i = Pawn; i <= King; i++) {
      //printf("zobristHash: piece type loop %d\n", i);
			if (i == Pawn)
				for (int j = SquareA2; j <= SquareH7; j++) {
          //printf("zobristHash: pawn squares loop %d\n", j);
					hash->piecesAtSquares[(c << 3) | i][j] = bitStrings[k++];
        }
			else
				for (int j = SquareA1; j <= SquareH8; j++) {
          //printf("zobristHash: other pieces squares loop %d\n", j);
					hash->piecesAtSquares[(c << 3) | i][j] = bitStrings[k++];
        }
		}
	}
  //printf("zobristHash: finished main loops...\n");
	//en passant hashes (8)
	for (int i = FileA; i <= FileH; i++) hash->enPassant[i] = bitStrings[k++];
  //printf("zobristHash: finished en passant loop...\n");
	//castling hashes (16)
	for (int i = CastlingRightsWhiteNoneBlackNone; i <= CastlingRightsWhiteBothBlackBoth; i++) hash->castling[i] = bitStrings[k++];
  //printf("zobristHash: finished castling loop...\n");
	//black move hash (1)
	hash->blackMove = bitStrings[k++];
  //total 761 random 64-bit numbers are needed
  //printf("zobrishHash: total bit strings k = %d, it should be 761\n", k);
}

void getHash(struct ZobristHash * hash, struct Board * board) {
  
  if (strncmp(board->fen->fenString, startPos, strlen(startPos)) == 0) {
		//printf("getHash: board FEN %s equals startPos FEN %s of length %ld - resetting hash...\n", board->fen->fenString, startPos, strlen(startPos));
    resetHash(hash);
		return;
	}
  
	hash->hash = 0; 
  //hash->prevEnPassant = 0;
	//xor in empty squares - not needed
  /*
	for (enum SquareName s = SquareA1; s <= SquareH8; s++)
		if (board->piecesOnSquares[s] == PieceNameNone) 
			hash->hash ^= hash->piecesAtSquares[0][s];
  */
	//xor in pieces
	unsigned long any = board->occupations[PieceNameAny];
	struct Square sq;
	square(&sq, lsBit(any));
	while (sq.name < SquareNone) {
		//hash->hash ^= hash->piecesAtSquares[(((board->piecesOnSquares[sq.name]) >> 3) + 1) * ((board->piecesOnSquares[sq.name]) & 7)][sq.name];
    hash->hash ^= hash->piecesAtSquares[board->piecesOnSquares[sq.name]][sq.name];
    //printf("getHash: xored in piece %d on %d (%lx), hash %lx\n", board->piecesOnSquares[sq.name], sq.name, hash->piecesAtSquares[board->piecesOnSquares[sq.name]][sq.name], hash->hash);
		any ^= (1UL << sq.name);
		square(&sq, lsBit(any));
	}
  //printf("getHash: all pieces hash %lx\n", hash->hash);
  //xor out previous en passant if any
  /*
	if (hash->prevEnPassant > 0) {
		hash->hash ^= hash->prevEnPassant;
    //printf("getHash: xored out prevEnPassant %lx, hash %lx\n", hash->prevEnPassant, hash->hash);
  	hash->prevEnPassant = 0;
  }*/
  //xor in en passant if any and save it
	if (board->fen->enPassant < FileNone) {
		hash->hash ^= hash->enPassant[board->fen->enPassant];
		hash->prevEnPassant = hash->enPassant[board->fen->enPassant];
    //printf("getHash: xored in enPassant %lx, hash %lx\n", hash->enPassant[board->fen->enPassant], hash->hash);
	}
	//xor out previous castling rights
	//hash->hash ^= hash->prevCastlingRights;
  //printf("getHash: xored out prevCastlingRights %lx, hash %lx\n", hash->prevCastlingRights, hash->hash);
	//xor in castling rights, they may be the same as prevCastlingRights, so in this case hash does not change
	hash->hash ^= hash->castling[board->fen->castlingRights];
  //printf("getHash: xored in castlingRights %lx, hash %lx\n", hash->castling[board->fen->castlingRights], hash->hash);
	//save castling rights
	hash->prevCastlingRights = hash->castling[board->fen->castlingRights];
	//xor in black's move
  if (board->fen->sideToMove == ColorBlack) {
    hash->hash ^= hash->blackMove;
    board->hash = hash->hash;
    //printf("getHash: xored in black move %lx, hash %lx\n", hash->blackMove, hash->hash);
  }
}

void resetHash(struct ZobristHash * hash) {
	hash->hash = STARTPOS_HASH;
	hash->prevCastlingRights = STARTPOS_CASTLING_RIGHTS;
	hash->prevEnPassant = 0;
}
/*
int updateHash(struct ZobristHash * hash, struct Board * board, struct Move * move) {
	if (hash->prevEnPassant > 0) {
		hash->hash ^= hash->prevEnPassant;
		hash->prevEnPassant = 0;
	}
	int srcPieceType = (board->movingPiece.color + 1) * board->movingPiece.type;
	//if (move->type & (MoveTypeEnPassant | MoveTypeCapture)) {
  if ((move->type & MoveTypeEnPassant) && (move->type & MoveTypeCapture)) {
		enum SquareName s;
		if (move->chessBoard->fen->sideToMove == ColorWhite) 
			s = move->destinationSquare.name - 8;
		else s = move->destinationSquare.name + 8;
		//xor out the capturing piece in its src square
		hash->hash ^= hash->piecesAtSquares[srcPieceType][move->sourceSquare.name];
		//xor in empty square in the src square
		hash->hash ^= hash->piecesAtSquares[0][move->sourceSquare.name];
		//xor out empty square in the dst square
		hash->hash ^= hash->piecesAtSquares[0][move->destinationSquare.name];
		//xor in empty square in the captured square
		hash->hash ^= hash->piecesAtSquares[0][s];
		//xor out captured piece in the dst square
		hash->hash ^= hash->piecesAtSquares[(board->capturedPiece & 7) * ((board->capturedPiece >> 3) + 1)][s];
		//xor in the capturing piece in its dst square
		hash->hash ^= hash->piecesAtSquares[srcPieceType][move->destinationSquare.name];
	}
	else if (move->type & MoveTypeCapture) {
		//xor out the capturing piece in its src square
		hash->hash ^= hash->piecesAtSquares[srcPieceType][move->sourceSquare.name];
		//xor in empty square in the src square
		hash->hash ^= hash->piecesAtSquares[0][move->sourceSquare.name];
		//xor out captured piece in the dst square
		hash->hash ^= hash->piecesAtSquares[(board->capturedPiece & 7) * ((board->capturedPiece >> 3) + 1)][move->destinationSquare.name];
		//xor in the promotion piece in its dst square
		if (move->type & MoveTypePromotion) hash->hash ^= hash->piecesAtSquares[(board->promoPiece & 7) * ((board->promoPiece >> 3) + 1)][move->destinationSquare.name];
		//xor in the capturing piece in its dst square
		else hash->hash ^= hash->piecesAtSquares[srcPieceType][move->destinationSquare.name];
	}
	else if ((move->type & MoveTypeCastlingKingside) || (move->type & MoveTypeCastlingQueenside)) {
		enum SquareName castlingKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
		enum SquareName castlingRookSquare[2][2] = { { SquareF1, SquareF8 }, { SquareD1, SquareD8 } };
		unsigned char side = ((move->type & (MoveTypeCastlingKingside | MoveTypeCastlingQueenside)) - 1) >> 2;

		enum SquareName rookSquare = SquareNone;
		unsigned char whiteBlack[2] = { 0, 56 };
		//xor out the king in its src square
		hash->hash ^= hash->piecesAtSquares[srcPieceType][move->sourceSquare.name];
		//xor in empty square in the src square
		hash->hash ^= hash->piecesAtSquares[0][move->sourceSquare.name];
		//xor out empty square in the king dst square
		hash->hash ^= hash->piecesAtSquares[0][castlingKingSquare[side][move->chessBoard->fen->sideToMove]];
		//xor in the king in its dst square
		hash->hash ^= hash->piecesAtSquares[srcPieceType][castlingKingSquare[side][move->chessBoard->fen->sideToMove]];

		//xor out the rook in its src square
		//need to find out the src square of the rook for chess960
		if (move->chessBoard->fen->castlingRook[side][move->chessBoard->fen->sideToMove] != FileNone)
			rookSquare = move->chessBoard->fen->castlingRook[side][move->chessBoard->fen->sideToMove] + whiteBlack[move->chessBoard->fen->sideToMove];
		else {
			printf("updateHash() error: %s %s castling rook is on %c; FEN %s\n", move->chessBoard->fen->sideToMove == ColorWhite ? "White" : "Black", ((side + 1) << 2) == MoveTypeCastlingKingside ? "Kingside" : "Queenside", move->chessBoard->fen->castlingRook[side][move->chessBoard->fen->sideToMove] + 'a', move->chessBoard->fen->fenString);
			return 1;
		}
		hash->hash ^= hash->piecesAtSquares[(move->chessBoard->fen->sideToMove + 1) * Rook][rookSquare];
		//xor in empty square in rook src square
		hash->hash ^= hash->piecesAtSquares[0][rookSquare];
		//xor out empty square in rook dst square
		hash->hash ^= hash->piecesAtSquares[0][castlingRookSquare[side][move->chessBoard->fen->sideToMove]];
		//xor in the rook in its dst square
		hash->hash ^= hash->piecesAtSquares[(move->chessBoard->fen->sideToMove + 1) * Rook][castlingRookSquare[side][move->chessBoard->fen->sideToMove]];
	}
	else { //normal move
		//xor out the moving piece in its src square
		hash->hash ^= hash->piecesAtSquares[srcPieceType][move->sourceSquare.name];
		//xor in empty square in the src square
		hash->hash ^= hash->piecesAtSquares[0][move->sourceSquare.name];
		//xor out empty square in the dst square
		hash->hash ^= hash->piecesAtSquares[0][move->destinationSquare.name];
		//xor in the promotion piece in its dst square
		if (move->type & MoveTypePromotion) 
      hash->hash ^= hash->piecesAtSquares[(board->promoPiece & 7) * ((board->promoPiece >> 3) + 1)][move->destinationSquare.name];
		//xor in the moving piece in its dst square
		else {
			hash->hash ^= hash->piecesAtSquares[srcPieceType][move->destinationSquare.name];
			if (move->type & MoveTypeEnPassant) {
				hash->hash ^= hash->enPassant[board->fen->enPassant];
				hash->prevEnPassant = hash->enPassant[board->fen->enPassant];
			}
		}
	}
	//xor out prev castling right
	hash->hash ^= hash->prevCastlingRights;
	//xor in new castling rights (may or may not change)
	hash->hash ^= hash->castling[board->fen->castlingRights];
	hash->prevCastlingRights = hash->castling[board->fen->castlingRights];
	//xor in black's move
	hash->hash ^= hash->blackMove;
	return 0;
}
*/
