#pragma warning(disable:4334)

#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "libchess.h"

//#ifdef __cplusplus
//extern "C" {
//#endif

void zobristHash(Zobrist& z) {
	int k = 0;
	//empty square hashes
	for (int s = SquareA1; s <= SquareH8; s++)
		z.emptySquares[s] = bitStrings[k++];
	//occupied square hashes
	for (int c  = ColorWhite; c <= ColorBlack; c++) {
		for (int i = 0; i < 5; i++) {
			for (int s = SquareA1; s <= SquareH8; s++) {
  			if (i == 0 && s < 48) z.pawns[c][s] = bitStrings[k++];
				z.nonPawns[c][i][s] = bitStrings[k++];
			}
		}
	}
	//black move hash
	z.blackMove = bitStrings[k++];
	//castling hashes
	for (int i = 0; i <= 15; i++)
		z.castling[i] = bitStrings[k++];
	//en passant hashes
	for (int i = FileA; i <= FileH; i++)
		z.enPassant[i] = bitStrings[k++];
}

void getHash(ZobristHash& hash, const Board& board, const Zobrist& z) {	
	hash.hash = 0; hash.prevEnPassant = 0;
	//xor in empty squares and pieces
	for (int sn = SquareA1; sn <= SquareH8; sn++) {
		const int pt = PC_TYPE(board.piecesOnSquares[sn]);
		const int color = PC_COLOR(board.piecesOnSquares[sn]);
		if (pt == PieceTypeNone) {
			hash.hash ^= z.emptySquares[sn];
			//printf("getHash() debug (empty square %s): %llx => %llx\n", squareName[sn], z.emptySquares[sn], hash.hash);
		} else if (pt == Pawn) {
			hash.hash ^= z.pawns[color][sn - 8];
			//printf("getHash() debug (pawns %s): %llx => %llx\n", squareName[sn], z.pawns[color][sn - 8], hash.hash);
		}
		else {
			hash.hash ^= z.nonPawns[color][pt - 2][sn];
			//printf("getHash() debug (%s at square %s): %llx => %llx\n", pieceName[board.piecesOnSquares[sn]], squareName[sn], z.nonPawns[PC_COLOR(board.piecesOnSquares[sn])][PC_TYPE(board.piecesOnSquares[sn]) - 2][sn], hash.hash);
		}
	}
	//printf("getHash() debug: board hash after empty squares and pieces %llx\n", hash.hash);
	//xor in castling rights
	const int castlingRights = (board.castlingRook[0][0] != FileNone) | ((board.castlingRook[0][1] != FileNone) << 1) | ((board.castlingRook[1][0] != FileNone) << 2) | ((board.castlingRook[1][1] != FileNone) << 3);
	hash.hash ^= z.castling[castlingRights];
	//printf("getHash() debug: board hash after castling rights %x: %llx => %llx\n", castlingRights, z.castling[castlingRights], hash.hash);
	//save castling rights for an update
	hash.prevCastlingRights = z.castling[castlingRights];
	//xor in en passant if any and save it for an update
	if (board.enPassant != FileNone) {
		hash.hash ^= z.enPassant[board.enPassant];
  	//printf("getHash() debug: board hash after en passant %c: %llx => %llx\n", enumFiles[board.enPassant], z.enPassant[board.enPassant], hash.hash);
		hash.prevEnPassant = z.enPassant[board.enPassant];
	}
	//xor in black's move
	if (board.sideToMove == ColorBlack) {
		hash.hash ^= z.blackMove;
  	//printf("getHash() debug: board hash after black to move %llx => %llx\n", z.blackMove, hash.hash);
	}
}

//things to remember:
//the move has been made! so the board.sideToMove has changed and the moving piece is no longer in its src square!
void updateHash(ZobristHash& zh, const Board& board, const Move& move, const int capturedType, const Zobrist& z) {
	//printf("updateHash() debug: board hash %llx\n", zh.hash);
	if (zh.prevEnPassant) {
		zh.hash ^= zh.prevEnPassant;
  	//printf("updateHash() debug: board hash after previous en passant %llx => %llx\n", zh.prevEnPassant, zh.hash);
		zh.prevEnPassant = 0;
	}
	const int oppColor = board.sideToMove;
	const int sideToMove = OPP_COLOR(oppColor);
	//assert(board.piecesOnSquares[move.dst] != PieceNone); //move has been made, the piece is now in its dst square with some exceptions
	int mpType = PC_TYPE(board.piecesOnSquares[move.dst]) - 2;
	//en passant capture
	if (move.type == MoveTypeEnPassantCapture) { //mpType = Pawn - 2 = -1
		//xor out the capturing pawn in its src square
		zh.hash ^= z.pawns[sideToMove][move.src - 8];
  	//printf("updateHash() debug: xor out the capturing pawn in its src square %s: %llx => %llx\n", squareName[move.src], z.pawns[sideToMove][move.src - 8], zh.hash);
		//xor in empty square in the src square
		zh.hash ^= z.emptySquares[move.src];
  	//printf("updateHash() debug: xor in empty square in the src square %s: %llx => %llx\n", squareName[move.src], z.emptySquares[move.src], zh.hash);
		//xor out empty square in the dst square
		zh.hash ^= z.emptySquares[move.dst];
  	//printf("updateHash() debug: xor out empty square in the dst square %s: %llx => %llx\n", squareName[move.dst], z.emptySquares[move.dst], zh.hash);
		//xor in empty square in the opponent pawn square
		const int s = sideToMove == ColorWhite ? move.dst - 8 : move.dst + 8;
		zh.hash ^= z.emptySquares[s];
  	//printf("updateHash() debug: xor in empty square in the captured pawn square %s: %llx => %llx\n", squareName[s], z.emptySquares[s], zh.hash);
		//xor out captured pawn in its square
		zh.hash ^= z.pawns[oppColor][s - 8];
  	//printf("updateHash() debug: xor out captured pawn in its square %s: %llx => %llx\n", squareName[s], z.pawns[oppColor][s - 8], zh.hash);
		//xor in the capturing pawn in its dst square
		zh.hash ^= z.pawns[sideToMove][move.dst - 8];
  	//printf("updateHash() debug: xor in the capturing pawn in its dst square %s: %llx => %llx\n", squareName[move.dst], z.pawns[sideToMove][move.dst - 8], zh.hash);
	} 
	//normal or promo capture
	else if (move.type == MoveTypeCapture) {
		//xor in the promotion piece in its dst square
		if (move.promoType != PieceTypeNone) {
			mpType = -1; //Pawn - 2
			zh.hash ^= z.nonPawns[sideToMove][move.promoType - 2][move.dst];
  	  //printf("updateHash() debug: xor in the promotion piece %s %s in its dst square %s: %llx => %llx\n", color[sideToMove], pieceType[move.promoType], squareName[move.dst], z.nonPawns[sideToMove][move.promoType - 2][move.dst], zh.hash);			
		}		
		//xor in the capturing piece in its dst square
		else {
			zh.hash ^= (mpType == -1) ? z.pawns[sideToMove][move.dst - 8] : z.nonPawns[sideToMove][mpType][move.dst];
	    //printf("updateHash() debug: xor in %s in its dst square %s %llx => %llx\n", pieceName[PC(sideToMove, mpType + 2)], squareName[move.dst], (mpType == -1) ? z.pawns[sideToMove][move.dst - 8] : z.nonPawns[sideToMove][mpType][move.dst], zh.hash);			
	  }
		//xor out the capturing piece in its src square
		zh.hash ^= (mpType == -1) ? z.pawns[sideToMove][move.src - 8] : z.nonPawns[sideToMove][mpType][move.src];
	  //printf("updateHash() debug: xor out %s in its src square %s %llx => %llx\n", pieceName[PC(sideToMove, mpType + 2)], squareName[move.src], (mpType == -1) ? z.pawns[sideToMove][move.src - 8] : z.nonPawns[sideToMove][mpType][move.src], zh.hash);			
		//xor in empty square in the src square
		zh.hash ^= z.emptySquares[move.src];
	  //printf("updateHash() debug: xor in empty square in the src square %s %llx => %llx\n", squareName[move.src], z.emptySquares[move.src], zh.hash);			
		//xor out captured piece in the dst square
		zh.hash ^= (capturedType == Pawn) ? z.pawns[oppColor][move.dst - 8] : z.nonPawns[oppColor][capturedType - 2][move.dst]; 
  	//printf("updateHash() debug: xor out %s in the dst square %s: %llx => %llx\n", pieceName[PC(oppColor, capturedType)], squareName[move.dst], (capturedType == Pawn) ? z.pawns[oppColor][move.dst - 8] : z.nonPawns[oppColor][capturedType - 2][move.dst], zh.hash);
	}
	//castling
	else if (move.type == MoveTypeCastlingKingside | move.type == MoveTypeCastlingQueenside) {
		const unsigned char castlingKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
		const unsigned char castlingRookSquare[2][2] = { { SquareF1, SquareF8 }, { SquareD1, SquareD8 } };
		const int side = (move.type == MoveTypeCastlingKingside) ? 0 : 1;
		const int rookRank = sideToMove == ColorWhite ? Rank1 : Rank8;
		//xor out the king in its src square, King - 2 = Rook
		zh.hash ^= z.nonPawns[sideToMove][Rook][move.src];
  	//printf("updateHash() debug: xor out the king in its src square %s: %llx => %llx\n", squareName[move.src], z.nonPawns[sideToMove][Rook][move.src], zh.hash);
		//xor in empty square in the src square
		zh.hash ^= z.emptySquares[move.src];
  	//printf("updateHash() debug: xor in empty square in the src square %s: %llx => %llx\n", squareName[move.src], z.emptySquares[move.src], zh.hash);
		//xor out empty square in the king dst square
		zh.hash ^= z.emptySquares[castlingKingSquare[side][sideToMove]];
  	//printf("updateHash() debug: xor out empty square in the king dst square %s: %llx => %llx\n", squareName[move.dst], z.emptySquares[castlingKingSquare[side][sideToMove]], zh.hash);
		//xor in the king in its dst square, King - 2 = Rook
		zh.hash ^= z.nonPawns[sideToMove][Rook][castlingKingSquare[side][sideToMove]];
  	//printf("updateHash() debug: xor in the king in its dst square %s: %llx => %llx\n", squareName[move.dst], z.nonPawns[sideToMove][Rook][castlingKingSquare[side][sideToMove]], zh.hash);

		//xor out the rook in its src square, Rook - 2 = Knight
		//need to find out the src square of the rook for chess960
		const int rookSquare = board.isChess960 ? move.dst : move.type == MoveTypeCastlingKingside ? SQ(rookRank, FileH) : SQ(rookRank, FileA);
		zh.hash ^= z.nonPawns[sideToMove][Knight][rookSquare];
  	//printf("updateHash() debug: xor out the rook in its src square %s: %llx => %llx\n", squareName[move.src], z.nonPawns[sideToMove][Knight][rookSquare], zh.hash);
		//xor in empty square in rook src square
		zh.hash ^= z.emptySquares[rookSquare];
  	//printf("updateHash() debug: xor in empty square in rook src square %s: %llx => %llx\n", squareName[move.src], z.emptySquares[rookSquare], zh.hash);
		//xor out empty square in rook dst square
		zh.hash ^= z.emptySquares[castlingRookSquare[side][sideToMove]];
  	//printf("updateHash() debug: xor out empty square in rook dst square %s: %llx => %llx\n", squareName[move.dst], z.emptySquares[castlingRookSquare[side][sideToMove]], zh.hash);
		//xor in the rook in its dst square, Rook - 2 = Knight
		zh.hash ^= z.nonPawns[sideToMove][Knight][castlingRookSquare[side][sideToMove]];
    //printf("updateHash() debug: xor in the rook in its dst square %s: %llx => %llx\n", squareName[move.dst], z.nonPawns[sideToMove][Knight][castlingRookSquare[side][sideToMove]], zh.hash);
	}
	//normal or promo move without no capture
	else { 
		//xor in the promotion piece in its dst square
		if (move.promoType != PieceTypeNone) {
			mpType = -1; //Pawn - 2
			zh.hash ^= z.nonPawns[sideToMove][move.promoType - 2][move.dst];
    	//printf("updateHash() debug: xor in the promotion piece in its dst square %s: %llx => %llx\n", squareName[move.dst], z.nonPawns[sideToMove][move.promoType - 2][move.dst], zh.hash);
		}
		//xor in the moving piece in its dst square
		else {
			zh.hash ^= (mpType == -1) ? z.pawns[sideToMove][move.dst - 8] : z.nonPawns[sideToMove][mpType][move.dst];
    	//printf("updateHash() debug: xor in %s %s in its dst square %s: %llx => %llx\n", color[sideToMove], pieceType[mpType + 2], squareName[move.dst], (mpType == -1) ? z.pawns[sideToMove][move.dst - 8] : z.nonPawns[sideToMove][mpType][move.dst], zh.hash);
			if (move.type == MoveTypeEnPassant) {
				zh.prevEnPassant = z.enPassant[board.enPassant];
				zh.hash ^= zh.prevEnPassant;
		  	//printf("updateHash() debug: xor in en passant file %c: %llx => %llx\n", enumFiles[board.enPassant], zh.prevEnPassant, zh.hash);
			}
		}
		//xor out the moving piece in its src square
		zh.hash ^= (mpType == -1) ? z.pawns[sideToMove][move.src - 8] : z.nonPawns[sideToMove][mpType][move.src];
  	//printf("updateHash() debug: xor out %s %s in its src square %s: %llx => %llx\n", color[sideToMove], pieceType[mpType + 2], squareName[move.src], (mpType == -1) ? z.pawns[sideToMove][move.src - 8] : z.nonPawns[sideToMove][mpType][move.src], zh.hash);
		//xor in empty square in the src square
		zh.hash ^= z.emptySquares[move.src];
  	//printf("updateHash() debug: xor in empty square in the src square %s: %llx => %llx\n", squareName[move.src], z.emptySquares[move.src], zh.hash);
		//xor out empty square in the dst square
		zh.hash ^= z.emptySquares[move.dst];
  	//printf("updateHash() debug: xor out empty square in the dst square %s: %llx => %llx\n", squareName[move.dst], z.emptySquares[move.dst], zh.hash);
	} //end of normal or promo move without capture
	//xor out prev castling right
	zh.hash ^= zh.prevCastlingRights;
	//printf("updateHash() debug: xor out prev castling right %llx => %llx\n", zh.prevCastlingRights, zh.hash);
	//xor in new castling rights (may or may not change)
	const int castlingRights = (board.castlingRook[0][0] != FileNone) | ((board.castlingRook[0][1] != FileNone) << 1) | ((board.castlingRook[1][0] != FileNone) << 2) | ((board.castlingRook[1][1] != FileNone) << 3);	
	zh.hash ^= z.castling[castlingRights];
	zh.prevCastlingRights = z.castling[castlingRights];
	//printf("updateHash() debug: xor in new castling rights %x: %llx => %llx\n", castlingRights, z.castling[castlingRights], zh.hash);
	//xor in black's move if it's black turn or xor out black's move if it's white turn
	zh.hash ^= z.blackMove;
	//printf("updateHash() debug: xor in/out black/white move %llx => %llx\n", z.blackMove, zh.hash);
}
//#ifdef __cplusplus
//}
//#endif
