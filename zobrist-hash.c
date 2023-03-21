#pragma warning(disable:4334)

#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "libchess.h"

void zobristHash(struct ZobristHash * hash) {
	hash->hash = STARTPOS_HASH;
	hash->prevCastlingRights = STARTPOS_CASTLING_RIGHTS;
	hash->prevEnPassant = 0;
	int k = 0;
	//empty square hashes
	for (enum SquareName s = SquareA1; s <= SquareH8; s++) 
		hash->piecesAtSquares[0][s] = bitStrings[k++];
	//occupied square hashes
	for (unsigned char c = 1; c <= 2; c++) {
		for (unsigned char i = Pawn; i <= King; i++) {
			if (i == Pawn)
				for (unsigned char j = SquareA2; j <= SquareH7; j++) 
					hash->piecesAtSquares[c * i][j] = bitStrings[k++];
			else
				for (unsigned char j = SquareA1; j <= SquareH8; j++) 
					hash->piecesAtSquares[c * i][j] = bitStrings[k++];
		}
	}
	//black move hash
	hash->blackMove = bitStrings[k++];
	//castling hashes
	for (int i = 0; i < 16; i++) hash->castling[i] = bitStrings[k++];
	//en passant hashes
	for (int i = 0; i < 8; i++) hash->enPassant[i] = bitStrings[k++];
}

void getHash(struct ZobristHash * hash, struct Board * board) {
	if (strncmp(board->fen->fenString, startPos, sizeof startPos)) {
		resetHash(hash);
		return;
	}
	hash->hash = 0; hash->prevEnPassant = 0;
	//xor in empty squares
	for (enum SquareName s = SquareA1; s <= SquareH8; s++)
		if (board->piecesOnSquares[s] == PieceNameNone) 
			hash->hash ^= hash->piecesAtSquares[0][s];
	//xor in pieces
	unsigned long any = board->occupations[PieceNameAny];
	struct Square sq;
	square(&sq, lsBit(any));
	while (sq.name < SquareNone) {
		hash->hash ^= hash->piecesAtSquares[(((board->piecesOnSquares[sq.name]) >> 3) + 1) * ((board->piecesOnSquares[sq.name]) & 7)][sq.name];
		any ^= (1UL << sq.name);
		square(&sq, lsBit(any));
	}
	//xor in black's move
	if (board->fen->sideToMove == ColorBlack) hash->hash ^= hash->blackMove;
	//xor in castling rights
	hash->hash ^= hash->castling[board->fen->castlingRights];
	//save castling rights for an update
	hash->prevCastlingRights = hash->castling[board->fen->castlingRights];
	//xor in en passant if any
	if (board->fen->enPassant != FileNone) {
		hash->hash ^= hash->enPassant[board->fen->enPassant];
		hash->prevEnPassant = hash->enPassant[board->fen->enPassant];
	}
}

void resetHash(struct ZobristHash * hash) {
	hash->hash = STARTPOS_HASH;
	hash->prevCastlingRights = STARTPOS_CASTLING_RIGHTS;
	hash->prevEnPassant = 0;
}

int updateHash(struct ZobristHash * hash, struct Board * board, struct Move * move) {
	if (hash->prevEnPassant > 0) {
		hash->hash ^= hash->prevEnPassant;
		hash->prevEnPassant = 0;
	}
	int srcPieceType = (board->movingPiece.color + 1) * board->movingPiece.type;
	if (move->type & (MoveTypeEnPassant | MoveTypeCapture)) {
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
			printf("updateHash() error: %s %s castling rook is on %c; FEN %s\n", move->chessBoard->fen->sideToMove == ColorWhite ? "White" : "Black", (side + 1) == MoveTypeCastlingKingside ? "Kingside" : "Queenside", move->chessBoard->fen->castlingRook[side][move->chessBoard->fen->sideToMove] + 'a', move->chessBoard->fen->fenString);
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
		if (move->type & MoveTypePromotion) hash->hash ^= hash->piecesAtSquares[(board->promoPiece & 7) * ((board->promoPiece >> 3) + 1)][move->destinationSquare.name];
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
