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
	//hash->hash2 = STARTPOS_HASH2;
	hash->prevCastlingRights = STARTPOS_CASTLING_RIGHTS;
	//hash->prevCastlingRights2 = STARTPOS_CASTLING_RIGHTS2;
	hash->prevEnPassant = 0;
	//hash->prevEnPassant2 = 0;
	int k = 0;
	//int k2 = 0;
	
	//empty square hashes
	for (int s = SquareA1; s <= SquareH8; s++) {
		hash->piecesAtSquares[0][s] = bitStrings[k++];
		//hash->piecesAtSquares2[0][s] = bitStrings2[k2++];
	}
	//occupied square hashes
	for (int c = 0; c <= 1; c++) {
		for (int i = Pawn; i <= King; i++) {
			if (i == Pawn)
				for (int j = SquareA2; j <= SquareH7; j++) { 
					hash->piecesAtSquares[c * 6 + i][j] = bitStrings[k++];
					//hash->piecesAtSquares2[c * 6 +i][j] = bitStrings2[k2++];
				}
			else
				for (int j = SquareA1; j <= SquareH8; j++) {
					hash->piecesAtSquares[c * 6 + i][j] = bitStrings[k++];
					//hash->piecesAtSquares2[c * 6 + i][j] = bitStrings2[k2++];
				}
		}
	}

	//black move hash
	hash->blackMove = bitStrings[k++];
	//hash->blackMove2 = bitStrings2[k2++];
	//castling hashes
	for (int i = CastlingRightsWhiteNoneBlackNone; i <= CastlingRightsWhiteBothBlackBoth; i++) {
		hash->castling[i] = bitStrings[k++];
		//hash->castling2[i] = bitStrings2[k2++];
	}
	//en passant hashes
	for (int i = FileA; i <= FileH; i++) {
		hash->enPassant[i] = bitStrings[k++];
		//hash->enPassant2[i] = bitStrings2[k2++];
	}
	//printf("k %d k2 %d\n", k, k2);
}

void getHash(struct ZobristHash * hash, struct Board * board) {	
  assert(hash && board);
	if (strncmp(board->fen->fenString, startPos, strlen(startPos)) == 0) {
		resetHash(hash);
	  board->zh = hash;
	  return;
	}
	hash->hash = 0; hash->prevEnPassant = 0;
	//hash->hash2 = 0; hash->prevEnPassant2 = 0;
	//xor in empty squares
  int sn, pt;
	unsigned long long bitboard = board->occupations[PieceNameNone];
	while (bitboard) {
		sn = lsBit(bitboard);
		pt = PC_COLOR(board->piecesOnSquares[sn]) * 6 + PC_TYPE(board->piecesOnSquares[sn]);
		hash->hash ^= hash->piecesAtSquares[pt][sn];
		//hash->hash2 ^= hash->piecesAtSquares2[pt][sn];
		bitboard &= bitboard - 1;
	}
	//xor in pieces
	bitboard = board->occupations[PieceNameAny];
	while (bitboard) {
		sn = lsBit(bitboard);
		pt = PC_COLOR(board->piecesOnSquares[sn]) * 6 + PC_TYPE(board->piecesOnSquares[sn]);
		hash->hash ^= hash->piecesAtSquares[pt][sn];
		//hash->hash2 ^= hash->piecesAtSquares2[pt][sn];
		bitboard &= bitboard - 1;
	}
	//xor in black's move
	if (board->fen->sideToMove == ColorBlack) {
		hash->hash ^= hash->blackMove;
		//hash->hash2 ^= hash->blackMove2;
	}
	//xor in castling rights
	hash->hash ^= hash->castling[board->fen->castlingRights];
	//hash->hash2 ^= hash->castling2[board->fen->castlingRights];
	//save castling rights for an update
	hash->prevCastlingRights = hash->castling[board->fen->castlingRights];
	//hash->prevCastlingRights2 = hash->castling2[board->fen->castlingRights];
	//xor in en passant if any and save it for an update
	if (board->fen->enPassant != FileNone) {
		hash->hash ^= hash->enPassant[board->fen->enPassant];
		//hash->hash2 ^= hash->enPassant2[board->fen->enPassant];
		hash->prevEnPassant = hash->enPassant[board->fen->enPassant];
		//hash->prevEnPassant2 = hash->enPassant2[board->fen->enPassant];
	}
	board->zh = hash;
}

void resetHash(struct ZobristHash * hash) {
	hash->hash = STARTPOS_HASH;
	//hash->hash2 = STARTPOS_HASH2;
	hash->prevCastlingRights = STARTPOS_CASTLING_RIGHTS;
	//hash->prevCastlingRights2 = STARTPOS_CASTLING_RIGHTS2;
	hash->prevEnPassant = 0;
	//hash->prevEnPassant2 = 0;
}

void updateHash(struct Board * board, struct Move * move) {
  assert(board && board->zh);
	if (board->zh->prevEnPassant /*&& board->zh->prevEnPassant2*/) {
		board->zh->hash ^= board->zh->prevEnPassant;
		//board->zh->hash2 ^= board->zh->prevEnPassant2;
		board->zh->prevEnPassant = 0;
		//board->zh->prevEnPassant2 = 0;
	}
	const int pcColor = PC_COLOR(move->movingPiece);
	const int oppColor = OPP_COLOR(pcColor);
	const int srcPieceType = pcColor * 6 + PC_TYPE(move->movingPiece);
	if ((move->type & (MoveTypeEnPassant | MoveTypeCapture)) == (MoveTypeEnPassant | MoveTypeCapture)) {
		const int s = pcColor == ColorWhite ? move->dst - 8 : move->dst + 8;
		//xor out the capturing piece in its src square
		board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][move->src];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][move->src];
		//xor in empty square in the src square
		board->zh->hash ^= board->zh->piecesAtSquares[0][move->src];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[0][move->src];
		//xor out empty square in the dst square
		board->zh->hash ^= board->zh->piecesAtSquares[0][move->dst];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[0][move->dst];
		//xor in empty square in the captured square
		board->zh->hash ^= board->zh->piecesAtSquares[0][s];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[0][s];
		//xor out captured piece in the dst square
		const int pt = oppColor * 6 + PC_TYPE(move->capturedPiece);
		board->zh->hash ^= board->zh->piecesAtSquares[pt][s];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[pt][s];
		//xor in the capturing piece in its dst square
		board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][move->dst];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][move->dst];
	}
	else if ((move->type & MoveTypeCapture) == MoveTypeCapture) {
		//xor out the capturing piece in its src square
		board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][move->src];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][move->src];
		//xor in empty square in the src square
		board->zh->hash ^= board->zh->piecesAtSquares[0][move->src];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[0][move->src];
		//xor out captured piece in the dst square
		const int pt = oppColor * 6 + PC_TYPE(move->capturedPiece);
		board->zh->hash ^= board->zh->piecesAtSquares[pt][move->dst];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[pt][move->dst];
		//xor in the promotion piece in its dst square
		if ((move->type & MoveTypePromotion) == MoveTypePromotion) {
			const int pt = pcColor * 6 + PC_TYPE(move->promoPiece);
			board->zh->hash ^= board->zh->piecesAtSquares[pt][move->dst];
			//board->zh->hash2 ^= board->zh->piecesAtSquares2[pt][move->dst];
		}
		//xor in the capturing piece in its dst square
		else {
			board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][move->dst];
			//board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][move->dst];
		}
	}
	else if (((move->type & MoveTypeCastlingKingside) == MoveTypeCastlingKingside) || ((move->type & MoveTypeCastlingQueenside) == MoveTypeCastlingQueenside)) {
		const int castlingKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
		const int castlingRookSquare[2][2] = { { SquareF1, SquareF8 }, { SquareD1, SquareD8 } };
		const unsigned char side = ((move->type & (MoveTypeCastlingKingside | MoveTypeCastlingQueenside)) - 1) >> 2;
		int rookSquare = SquareNone;
		const unsigned char whiteBlack[2] = { 0, 56 };
		//xor out the king in its src square
		board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][move->src];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][move->src];
		//xor in empty square in the src square
		board->zh->hash ^= board->zh->piecesAtSquares[0][move->src];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[0][move->src];
		//xor out empty square in the king dst square
		board->zh->hash ^= board->zh->piecesAtSquares[0][castlingKingSquare[side][pcColor]];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[0][castlingKingSquare[side][pcColor]];
		//xor in the king in its dst square
		board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][castlingKingSquare[side][pcColor]];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][castlingKingSquare[side][pcColor]];

		//xor out the rook in its src square
		//need to find out the src square of the rook for chess960
		assert(move->castlingRook != FileNone);
		rookSquare = move->castlingRook + whiteBlack[pcColor];
		board->zh->hash ^= board->zh->piecesAtSquares[(pcColor + 1) * Rook][rookSquare];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[(pcColor + 1) * Rook][rookSquare];
		//xor in empty square in rook src square
		board->zh->hash ^= board->zh->piecesAtSquares[0][rookSquare];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[0][rookSquare];
		//xor out empty square in rook dst square
		board->zh->hash ^= board->zh->piecesAtSquares[0][castlingRookSquare[side][pcColor]];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[0][castlingRookSquare[side][pcColor]];
		//xor in the rook in its dst square
		int pt = pcColor * 6 + Rook;
		board->zh->hash ^= board->zh->piecesAtSquares[pt][castlingRookSquare[side][pcColor]];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[pt][castlingRookSquare[side][pcColor]];
	}
	else { //normal move
		//xor out the moving piece in its src square
		board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][move->src];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][move->src];
		//xor in empty square in the src square
		board->zh->hash ^= board->zh->piecesAtSquares[0][move->src];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[0][move->src];
		//xor out empty square in the dst square
		board->zh->hash ^= board->zh->piecesAtSquares[0][move->dst];
		//board->zh->hash2 ^= board->zh->piecesAtSquares2[0][move->dst];
		//xor in the promotion piece in its dst square
		if ((move->type & MoveTypePromotion) == MoveTypePromotion) {
			int pt = pcColor * 6 + PC_TYPE(move->promoPiece);
			board->zh->hash ^= board->zh->piecesAtSquares[pt][move->dst];
			//board->zh->hash2 ^= board->zh->piecesAtSquares2[pt][move->dst];
		}
		//xor in the moving piece in its dst square
		else {
			board->zh->hash ^= board->zh->piecesAtSquares[srcPieceType][move->dst];
			//board->zh->hash2 ^= board->zh->piecesAtSquares2[srcPieceType][move->dst];
			if ((move->type & MoveTypeEnPassant) == MoveTypeEnPassant) {
				board->zh->hash ^= board->zh->enPassant[board->fen->enPassant];
				//board->zh->hash2 ^= board->zh->enPassant2[board->fen->enPassant];
				board->zh->prevEnPassant = board->zh->enPassant[board->fen->enPassant];
				//board->zh->prevEnPassant2 = board->zh->enPassant2[board->fen->enPassant];
			}
		}
	}
	//xor out prev castling right
	board->zh->hash ^= board->zh->prevCastlingRights;
	//board->zh->hash2 ^= board->zh->prevCastlingRights2;
	//xor in new castling rights (may or may not change)
	board->zh->hash ^= board->zh->castling[board->fen->castlingRights];
	//board->zh->hash2 ^= board->zh->castling2[board->fen->castlingRights];
	board->zh->prevCastlingRights = board->zh->castling[board->fen->castlingRights];
	//board->zh->prevCastlingRights2 = board->zh->castling2[board->fen->castlingRights];
	//xor in black's move if it's black turn or xor out black's move if it's white turn
	board->zh->hash ^= board->zh->blackMove;
	//board->zh->hash2 ^= board->zh->blackMove2;
}
#ifdef __cplusplus
}
#endif
