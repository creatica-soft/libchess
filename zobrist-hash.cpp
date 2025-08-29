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

void zobristHash(struct ZobristHash * hash) {
	hash->hash = STARTPOS_HASH;
	hash->hash2 = STARTPOS_HASH2;
	hash->prevCastlingRights = STARTPOS_CASTLING_RIGHTS;
	hash->prevCastlingRights2 = STARTPOS_CASTLING_RIGHTS2;
	hash->prevEnPassant = 0;
	hash->prevEnPassant2 = 0;
	int k = 0;
	int k2 = 0;
	
	//empty square hashes
	for (int s = SquareA1; s <= SquareH8; s++) {
		hash->piecesAtSquares[0][s] = bitStrings[k++];
		hash->piecesAtSquares2[0][s] = bitStrings2[k2++];
	}
	//occupied square hashes
	for (int c = 0; c <= 1; c++) {
		for (int i = Pawn; i <= King; i++) {
			if (i == Pawn)
				for (int j = SquareA2; j <= SquareH7; j++) { 
					hash->piecesAtSquares[c * 6 + i][j] = bitStrings[k++];
					hash->piecesAtSquares2[c * 6 +i][j] = bitStrings2[k2++];
				}
			else
				for (int j = SquareA1; j <= SquareH8; j++) {
					hash->piecesAtSquares[c * 6 + i][j] = bitStrings[k++];
					hash->piecesAtSquares2[c * 6 + i][j] = bitStrings2[k2++];
				}
		}
	}

	//black move hash
	hash->blackMove = bitStrings[k++];
	hash->blackMove2 = bitStrings2[k2++];
	//castling hashes
	for (int i = CastlingRightsWhiteNoneBlackNone; i <= CastlingRightsWhiteBothBlackBoth; i++) {
		hash->castling[i] = bitStrings[k++];
		hash->castling2[i] = bitStrings2[k2++];
	}
	//en passant hashes
	for (int i = FileA; i <= FileH; i++) {
		hash->enPassant[i] = bitStrings[k++];
		hash->enPassant2[i] = bitStrings2[k2++];
	}
	//printf("k %d k2 %d\n", k, k2);
}

void getHash(struct ZobristHash * hash, struct Board * board) {	
  if (!hash || !board) {
  	fprintf(stderr, "getHash() arg error: either hash or board or both are NULL\n");
  	return;
  }
	if (strncmp(board->fen->fenString, startPos, strlen(startPos)) == 0) {
		resetHash(hash);
	  board->zh = hash;
		return;
	}
	hash->hash = 0; hash->prevEnPassant = 0;
	hash->hash2 = 0; hash->prevEnPassant2 = 0;
	//xor in empty squares
  int sn, pt;
	unsigned long long bitboard = board->occupations[PieceNameNone];
	while (bitboard) {
		sn = lsBit(bitboard);
		pt = (((board->piecesOnSquares[sn]) >> 3) * 6) + ((board->piecesOnSquares[sn]) & 7);
		hash->hash ^= hash->piecesAtSquares[pt][sn];
		hash->hash2 ^= hash->piecesAtSquares2[pt][sn];
		bitboard &= bitboard - 1;
	}
	//xor in pieces
	bitboard = board->occupations[PieceNameAny];
	while (bitboard) {
		sn = lsBit(bitboard);
		pt = (((board->piecesOnSquares[sn]) >> 3) * 6) + ((board->piecesOnSquares[sn]) & 7);
		hash->hash ^= hash->piecesAtSquares[pt][sn];
		hash->hash2 ^= hash->piecesAtSquares2[pt][sn];
		bitboard &= bitboard - 1;
	}
	//xor in black's move
	if (board->fen->sideToMove == ColorBlack) {
		hash->hash ^= hash->blackMove;
		hash->hash2 ^= hash->blackMove2;
	}
	//xor in castling rights
	hash->hash ^= hash->castling[board->fen->castlingRights];
	hash->hash2 ^= hash->castling2[board->fen->castlingRights];
	//save castling rights for an update
	hash->prevCastlingRights = hash->castling[board->fen->castlingRights];
	hash->prevCastlingRights2 = hash->castling2[board->fen->castlingRights];
	//xor in en passant if any
	if (board->fen->enPassant != FileNone) {
		hash->hash ^= hash->enPassant[board->fen->enPassant];
		hash->hash2 ^= hash->enPassant2[board->fen->enPassant];
		hash->prevEnPassant = hash->enPassant[board->fen->enPassant];
		hash->prevEnPassant2 = hash->enPassant2[board->fen->enPassant];
	}
	board->zh = hash;
}

void resetHash(struct ZobristHash * hash) {
	hash->hash = STARTPOS_HASH;
	hash->hash2 = STARTPOS_HASH2;
	hash->prevCastlingRights = STARTPOS_CASTLING_RIGHTS;
	hash->prevCastlingRights2 = STARTPOS_CASTLING_RIGHTS2;
	hash->prevEnPassant = 0;
	hash->prevEnPassant2 = 0;
}

int updateHash(struct Board * board, struct Move * move) {
  if (!board || !(board->zh)) {
  	fprintf(stderr, "updateHash() arg error: either board, board->zh or both are NULL\n");
  	return 1;
  }
	if (board->zh->prevEnPassant && board->zh->prevEnPassant2) {
		board->zh->hash ^= board->zh->prevEnPassant;
		board->zh->hash2 ^= board->zh->prevEnPassant2;
		board->zh->prevEnPassant = 0;
		board->zh->prevEnPassant2 = 0;
	}
	int srcPieceType = (board->movingPiece.color * 6) + board->movingPiece.type;
	if ((move->type & (MoveTypeEnPassant | MoveTypeCapture)) == (MoveTypeEnPassant | MoveTypeCapture)) {
		int s;
		if (board->movingPiece.color == ColorWhite) 
			s = move->destinationSquare.name - 8;
		else s = move->destinationSquare.name + 8;
		//xor out the capturing piece in its src square
		board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][move->sourceSquare.name];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][move->sourceSquare.name];
		//xor in empty square in the src square
		board->zh->hash ^= board->zh->piecesAtSquares[0][move->sourceSquare.name];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[0][move->sourceSquare.name];
		//xor out empty square in the dst square
		board->zh->hash ^= board->zh->piecesAtSquares[0][move->destinationSquare.name];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[0][move->destinationSquare.name];
		//xor in empty square in the captured square
		board->zh->hash ^= board->zh->piecesAtSquares[0][s];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[0][s];
		//xor out captured piece in the dst square
		int pt = ((board->capturedPiece >> 3) * 6) + (board->capturedPiece & 7);
		board->zh->hash ^= board->zh->piecesAtSquares[pt][s];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[pt][s];
		//xor in the capturing piece in its dst square
		board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][move->destinationSquare.name];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][move->destinationSquare.name];
	}
	else if ((move->type & MoveTypeCapture) == MoveTypeCapture) {
		//xor out the capturing piece in its src square
		board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][move->sourceSquare.name];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][move->sourceSquare.name];
		//xor in empty square in the src square
		board->zh->hash ^= board->zh->piecesAtSquares[0][move->sourceSquare.name];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[0][move->sourceSquare.name];
		//xor out captured piece in the dst square
		int pt = ((board->capturedPiece >> 3) * 6) + (board->capturedPiece & 7);
		board->zh->hash ^= board->zh->piecesAtSquares[pt][move->destinationSquare.name];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[pt][move->destinationSquare.name];
		//xor in the promotion piece in its dst square
		if ((move->type & MoveTypePromotion) == MoveTypePromotion) {
			int pt = ((board->promoPiece >> 3) * 6) + (board->promoPiece & 7);
			board->zh->hash ^= board->zh->piecesAtSquares[pt][move->destinationSquare.name];
			board->zh->hash2 ^= board->zh->piecesAtSquares2[pt][move->destinationSquare.name];
		}
		//xor in the capturing piece in its dst square
		else {
			board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][move->destinationSquare.name];
			board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][move->destinationSquare.name];
		}
	}
	else if (((move->type & MoveTypeCastlingKingside) == MoveTypeCastlingKingside) || ((move->type & MoveTypeCastlingQueenside) == MoveTypeCastlingQueenside)) {
		int castlingKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
		int castlingRookSquare[2][2] = { { SquareF1, SquareF8 }, { SquareD1, SquareD8 } };
		unsigned char side = ((move->type & (MoveTypeCastlingKingside | MoveTypeCastlingQueenside)) - 1) >> 2;
		int rookSquare = SquareNone;
		unsigned char whiteBlack[2] = { 0, 56 };
		//xor out the king in its src square
		board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][move->sourceSquare.name];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][move->sourceSquare.name];
		//xor in empty square in the src square
		board->zh->hash ^= board->zh->piecesAtSquares[0][move->sourceSquare.name];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[0][move->sourceSquare.name];
		//xor out empty square in the king dst square
		board->zh->hash ^= board->zh->piecesAtSquares[0][castlingKingSquare[side][board->movingPiece.color]];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[0][castlingKingSquare[side][board->movingPiece.color]];
		//xor in the king in its dst square
		board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][castlingKingSquare[side][board->movingPiece.color]];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][castlingKingSquare[side][board->movingPiece.color]];

		//xor out the rook in its src square
		//need to find out the src square of the rook for chess960
		if (move->castlingRook != FileNone) {
			rookSquare = move->castlingRook + whiteBlack[board->movingPiece.color];
 		}
		else {
			printf("updateHash() error: %s %s castling rook is on FileNone; FEN %s\n", board->movingPiece.color == ColorWhite ? "White" : "Black", (side + 1) == MoveTypeCastlingKingside ? "Kingside" : "Queenside", move->chessBoard->fen->fenString);
			return 1;
		}
		board->zh->hash ^= board->zh->piecesAtSquares[(board->movingPiece.color + 1) * Rook][rookSquare];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[(board->movingPiece.color + 1) * Rook][rookSquare];
		//xor in empty square in rook src square
		board->zh->hash ^= board->zh->piecesAtSquares[0][rookSquare];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[0][rookSquare];
		//xor out empty square in rook dst square
		board->zh->hash ^= board->zh->piecesAtSquares[0][castlingRookSquare[side][board->movingPiece.color]];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[0][castlingRookSquare[side][board->movingPiece.color]];
		//xor in the rook in its dst square
		int pt = (board->movingPiece.color * 6) + Rook;
		board->zh->hash ^= board->zh->piecesAtSquares[pt][castlingRookSquare[side][board->movingPiece.color]];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[pt][castlingRookSquare[side][board->movingPiece.color]];
	}
	else { //normal move
		//xor out the moving piece in its src square
		board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][move->sourceSquare.name];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][move->sourceSquare.name];
		//xor in empty square in the src square
		board->zh->hash ^= board->zh->piecesAtSquares[0][move->sourceSquare.name];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[0][move->sourceSquare.name];
		//xor out empty square in the dst square
		board->zh->hash ^= board->zh->piecesAtSquares[0][move->destinationSquare.name];
		board->zh->hash2 ^= board->zh->piecesAtSquares2[0][move->destinationSquare.name];
		//xor in the promotion piece in its dst square
		if ((move->type & MoveTypePromotion) == MoveTypePromotion) {
			int pt = ((board->promoPiece >> 3) * 6) + (board->promoPiece & 7);
			board->zh->hash ^= board->zh->piecesAtSquares[pt][move->destinationSquare.name];
			board->zh->hash2 ^= board->zh->piecesAtSquares2[pt][move->destinationSquare.name];
		}
		//xor in the moving piece in its dst square
		else {
			board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][move->destinationSquare.name];
			board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][move->destinationSquare.name];
			if ((move->type & MoveTypeEnPassant) == MoveTypeEnPassant) {
				board->zh->hash ^= board->zh->enPassant[board->fen->enPassant];
				board->zh->hash2 ^= board->zh->enPassant2[board->fen->enPassant];
				board->zh->prevEnPassant = board->zh->enPassant[board->fen->enPassant];
				board->zh->prevEnPassant2 = board->zh->enPassant2[board->fen->enPassant];
			}
		}
	}
	//xor out prev castling right
	board->zh->hash ^= board->zh->prevCastlingRights;
	board->zh->hash2 ^= board->zh->prevCastlingRights2;
	//xor in new castling rights (may or may not change)
	board->zh->hash ^= board->zh->castling[board->fen->castlingRights];
	board->zh->hash2 ^= board->zh->castling2[board->fen->castlingRights];
	board->zh->prevCastlingRights = board->zh->castling[board->fen->castlingRights];
	board->zh->prevCastlingRights2 = board->zh->castling2[board->fen->castlingRights];
	//xor in black's move
	board->zh->hash ^= board->zh->blackMove;
	board->zh->hash2 ^= board->zh->blackMove2;
	return 0;
}
#ifdef __cplusplus
}
#endif
