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

//uci move can be encoded as 15 bit integer because src and dst sq can be encoded with 6 bits each and 3 bits are for promo
//move_idx = (src << 9) | (dst << 3) | promotion_type
//0 and 1 (pawn) - no promo, 2 - knight, 3 - bishop, 4 - rook, 5 - queen
//decoding:
//promotion_type = move_idx & 7 (lower 3 bits)
//dst = (move_idx >> 3) & 63 (bits from 4 to 9)
//src = move_idx >> 9 (bits from 10 to 15)
int move_to_idx(const char * uci_move, int * src, int * dst, int * promo) {
  assert(uci_move);
  assert(strlen(uci_move) >= 4);
  *src = ((uci_move[1] - '1') << 3) | (uci_move[0] - 'a');
  *dst = ((uci_move[3] - '1') << 3) | (uci_move[2] - 'a');
  int idx = (*src << 9) | (*dst << 3);
  if (strlen(uci_move) == 5) { //this will not include null-terminated char '\0'
  	const char promos[6] = "nbrq";
    const char * p = strchr(promos, uci_move[4]);
    if (p) {
      *promo = p - promos + 2;
      idx |= *promo;
    } 
  	/*for (*promo = 0; *promo < 5; (*promo)++) {
  		if (promos[*promo] == uci_move[4]) {
  			break;
  		}
  	}*/
  }
  return idx;
}

char * idx_to_move(const int move_idx, char * uci_move) {
  assert(uci_move);
  assert(sizeof(uci_move) >= 6);
  uci_move[0] = 0;
  const int src = move_idx >> 9;
  const int dst = (move_idx >> 3) & 63;
  const int promo = move_idx & 7;
  strcat(uci_move, squareName[src]);
  strcat(uci_move, squareName[dst]);
  uci_move[4] = uciPromoLetter[promo]; //index 0 and 1 are '/0'
  return uci_move;
}


void getMoveType(char * mvType, unsigned int type) {
	mvType[0] = '\0';
	unsigned int i;
	strcat(mvType, moveType[0]);
	while (type) {
		i = lsBit(type);
		strcat(mvType, " | ");
		strcat(mvType, moveType[i + 1]);
		type ^= (1 << i);
	}
}

unsigned char moveCandidateScore(const int sq, const int srcFile, const int srcRank)
{
  const int file = SQ_FILE(sq);
  const int rank = SQ_RANK(sq);
	unsigned char score = 1;
	if (file == srcFile && rank == srcRank)
		score = 4;
	else if (rank == srcRank)
		score = 3;
	else if (file == srcFile)
		score = 2;
	return score;
}

bool parseUciMove(const char * move) {
	const char promo[] = "bnqr";
	size_t len = strlen(move);
	if (move[0] >= 'a' && move[0] <= 'h') {
		if (move[1] >= '1' && move[1] <= '8') {
			if (move[2] >= 'a' && move[2] <= 'h') {
				if (move[3] >= '1' && move[3] <= '8') {
					if (len == 4) return true;
					else if (len == 5) {
						if (strchr(promo, move[4])) return true;
					} else return false;
				}
			}
		}
	}
	return false;
}

int validateSanMove(struct Move * move) {
	if (strcmp(move->sanMove, "--") == 0) {
		move->type = MoveTypeNull | MoveTypeValid;
		return 0;
	}
	char sanMove[12] = "";
	char mvType[94] = "";
	size_t len = strlen(move->sanMove);
	if (len > 11) {
		getMoveType(mvType, move->type);
		printf("validateSanMove() error: len(move->sanMove) is greater than 11 (%zu). %u %s %s, move type %s\n", len, move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", sanMove, mvType);
		writeDebug(move->chessBoard, false);
		return 1;
	}
	strncpy(sanMove, move->sanMove, len + 1);
	if (sanMove[len - 1] == '+' || sanMove[len - 1] == '#')
		sanMove[len - 1] = '\0';
	int castlingSide = CastlingSideNone;
	if (strcmp(sanMove, "O-O-O") == 0)
		castlingSide = CastlingSideQueenside;
	else if (strcmp(sanMove, "O-O") == 0)
		castlingSide = CastlingSideKingside;
	const char pieces[] = "NBRQ";
	if (sanMove[0] >= 'a' && sanMove[0] <= 'h') {
		if (sanMove[1] == '1' || sanMove[1] == '8') {
			if (sanMove[2] == '=') {
				const char * idx = strchr(pieces, sanMove[3]);
				if (idx) {
					move->promoPiece = PC(move->chessBoard->fen->sideToMove, idx - pieces + 2);
					move->type |= MoveTypePromotion;
					sanMove[2] = '\0';
				}
				else {
					getMoveType(mvType, move->type);
					printf("validateSanMove() error: unknown promotion piece %u %s %s, move type %s\n", move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", sanMove, mvType);
					writeDebug(move->chessBoard, false);
					return 1;
				}
			}
		} else {
			if (sanMove[1] == 'x') {
				if (sanMove[2] >= 'a' && sanMove[2] <= 'h') {
					if (sanMove[3] == '1' || sanMove[3] == '8') {
						if (sanMove[4] == '=') {
							const char * idx = strchr(pieces, sanMove[5]);
							if (idx) {
								move->promoPiece = PC(move->chessBoard->fen->sideToMove, idx - pieces + 2);
								move->type |= MoveTypePromotion | MoveTypeCapture;
								sanMove[4] = '\0';
							}
							else {
								getMoveType(mvType, move->type);
								printf("validateSanMove() error: unknown promotion piece %u %s %s, move type %s\n", move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", sanMove, mvType);
								writeDebug(move->chessBoard, false);
								return 1;
							}
						}
					}
				}
		  }	
		}
	}

	//castling
	if (castlingSide != CastlingSideNone) {
		move->type |= (castlingSide << 2);
		const int kingCastlingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
		move->movingPiece = PC(move->chessBoard->fen->sideToMove, King);
		move->src = lsBit(move->chessBoard->occupations[move->movingPiece]);

		if (!move->chessBoard->fen->isChess960) {
			if ((move->chessBoard->movesFromSquares[move->src] & SQ_BIT(kingCastlingSquare[castlingSide - 1][move->chessBoard->fen->sideToMove])) > 0) {
				move->dst = kingCastlingSquare[castlingSide - 1][move->chessBoard->fen->sideToMove];
				move->type |= MoveTypeValid;
			} else {
				getMoveType(mvType, move->type);
				printf("validateSanMove() error: %s king move from %s to %s is not possible, move type %s\n", move->chessBoard->fen->sideToMove == ColorWhite ? "White" : "Black", squareName[move->src], squareName[kingCastlingSquare[castlingSide - 1][move->chessBoard->fen->sideToMove]], mvType);
				writeDebug(move->chessBoard, false);
				return 1;
			}
		} else {
			const unsigned char whiteBlack[2] = { 0, 56 };
			if ((move->chessBoard->movesFromSquares[move->src] & SQ_BIT(move->chessBoard->fen->castlingRook[castlingSide - 1][move->chessBoard->fen->sideToMove] + whiteBlack[move->chessBoard->fen->sideToMove])) > 0) {
				move->dst = move->chessBoard->fen->castlingRook[castlingSide - 1][move->chessBoard->fen->sideToMove] + whiteBlack[move->chessBoard->fen->sideToMove];
				move->type |= MoveTypeValid;
			}
		}
		return 0;
	}

	int dstFile = FileNone;
	int srcFile = FileNone;
	int dstRank = RankNone;
	int srcRank = RankNone;

	for (signed char i = (signed char)strlen(sanMove) - 1; i >= 0; i--) {
		if (sanMove[i] >= '1' && sanMove[i] <= '8') {
			const int x = sanMove[i] - '1';
			if (dstRank == RankNone) dstRank = x;
			else srcRank = x;
		} else if (sanMove[i] >= 'a' && sanMove[i] <= 'h') {
			const int x = sanMove[i] - 'a';
			if (dstFile == FileNone) dstFile = x;
			else srcFile = x;
		} else {
			const char pieces[] = ".xNBRQK";
			const char * idx = strchr(pieces, sanMove[i]);
			if (idx) {
				const long long pieceIndex = idx - pieces;
				if (pieceIndex > 1)
					move->movingPiece = PC(move->chessBoard->fen->sideToMove, pieceIndex);
				else if (pieceIndex == 1) {
					move->type |= MoveTypeCapture;
				}
			} else {
				printf("validateSanMove() error: unknown char ('%c') in move %u%s%s\n", sanMove[i], move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? "." : "...", sanMove);
				writeDebug(move->chessBoard, false);
				return 1;
			}
		}
	}
	move->dst = SQ(dstRank, dstFile);
	if (move->dst >= SquareNone) {
		printf("validateSanMove() error: dstRank or dstFile are not set in move %u%s%s\n", move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? "." : "...", sanMove);
		writeDebug(move->chessBoard, false);
		return 1;
	}
	if (move->movingPiece == PieceNameNone) {
		move->movingPiece = move->chessBoard->fen->sideToMove == ColorWhite ? WhitePawn : BlackPawn;
		if ((move->type & MoveTypeCapture) != MoveTypeCapture) {
			srcFile = SQ_FILE(move->dst);
			//detect EnPassant move
			const int dstRanks[] = { Rank4, Rank5 };
			const int srcRanks[] = { Rank2, Rank7 };
			const int srcSquare = SQ(srcRanks[move->chessBoard->fen->sideToMove], srcFile);
			if (SQ_RANK(move->dst) == dstRanks[move->chessBoard->fen->sideToMove]) {
				if ((move->chessBoard->movesFromSquares[srcSquare] & SQ_BIT(move->dst)) > 0) {
					unsigned long long opponentPawns = move->chessBoard->occupations[PC(OPP_COLOR(move->chessBoard->fen->sideToMove), Pawn)];
					while (opponentPawns) {
  					int opponentPawnSquare = lsBit(opponentPawns);
						if (SQ_RANK(opponentPawnSquare) == dstRanks[move->chessBoard->fen->sideToMove] && ((SQ_FILE(opponentPawnSquare) == srcFile + 1 && srcFile < FileH) || (SQ_FILE(opponentPawnSquare) == srcFile - 1 && srcFile > FileA))) {
							move->type |= (MoveTypeValid | MoveTypeEnPassant);
							move->src = srcSquare;
							break;
						}
						//oppositePawns ^= oppositePawnSquare.bitSquare;
						//square(&oppositePawnSquare, lsBit(oppositePawns));
						opponentPawns &= opponentPawns - 1;
					}
				}
			}
		} else if ((move->chessBoard->fen->enPassant < FileNone) && move->chessBoard->fen->enPassant + (move->chessBoard->fen->sideToMove == ColorWhite ? Rank6 << 3 : Rank3 << 3) == move->dst) {
			move->type |= MoveTypeEnPassant; // capture flag should have been set up already from "x" pattern in SAN move
			move->src = move->chessBoard->fen->sideToMove == ColorWhite ? SQ_FILE(move->dst) > srcFile ? move->dst - 9 : move->dst - 7 : SQ_FILE(move->dst) > srcFile ? move->dst + 7 : move->dst + 9;
		}
	}
	if (move->src == SquareNone && srcFile != FileNone && srcRank != RankNone)
		move->src = SQ(srcRank, srcFile);
	else {
   	int n = 0; //number of candidates
  	int moveCandidates[10];
		//find move candidates
		//get a bitboard of squares where moving piece names (for example, white knights) are
		unsigned long long cp = move->chessBoard->occupations[move->movingPiece];
		//iterate over all squares where movingPiece.name are located
		while (cp) {
  		int s = lsBit(cp);
			if ((move->chessBoard->movesFromSquares[s] & SQ_BIT(move->dst)) > 0) {
  			//if moving piece is a pawn (its source file is known and equals to the square s->file) or a king
				if ((PC_TYPE(move->movingPiece) == Pawn && SQ_FILE(s) == srcFile) || PC_TYPE(move->movingPiece) == King) {
					//we are sure that square s is the source square
					move->src = s;
					break;
				} else { //otherwise add this square to moveCandidates squares
					if (n >= 10) {
				    printf("validateSanMove() error: too many move candidates for %s\n", sanMove);
				    writeDebug(move->chessBoard, false);
				    return 1;
					}				
					moveCandidates[n++] = s;
        }
			}
			cp &= cp - 1;
		} //end of while() loop over squares
		//if moving piece source square is still unknown (i.e. its not a pawn or a king)
		if (move->src == SquareNone) {
			int t, maxT = 0;
			//score move candidates such as 
			//if both src rank and src file are the same as square s ones, rate them as the most probable (4)
			//if only src rank is the same, rate them below (3)
			//if only file is the same, rate them as even below (2)
			//if nothing is known, rate them as the least probable (1)
			//find the most probable candidate
			int maxI;
			//array of candidates where index is the rating, index 0 correspond to rating 0, etc 
			//for example, maxTcandidates[4] = 1 means that there is one candidate with rating 4
			//it should be only one candidate with the highest rating, otherwise, the move is ambigious
			int maxTcandidates[5] = {0, 0, 0, 0, 0}; 
			for (int i = 0; i < n; i++) {
				t = moveCandidateScore(moveCandidates[i], srcFile, srcRank);
				maxTcandidates[t]++;
				if (t > maxT) {
					maxT = t;
					maxI = i;
				}				
			}
			if (maxTcandidates[maxT] == 1) {
  			move->src = moveCandidates[maxI];
  		}
			else {
				getMoveType(mvType, move->type);
				printf("validateSanMove() error: ambiguous move %s, moveType %s, srcRank %c, srcFile %c, max candidate rating %d, max candidates %d\n", move->sanMove, mvType, enumRanks[srcRank], enumFiles[srcFile], maxT, n);
				writeDebug(move->chessBoard, true);
				return 1;
			}
		}
	}
	if (move->src == SquareNone) {
		getMoveType(mvType, move->type);
		printf("validateSanMove() error: source square not defined for the moving piece %s, move type %s\n", pieceName[move->movingPiece], mvType);
		writeDebug(move->chessBoard, false);
		return 1;
	}
	if ((move->type & MoveTypeValid) != MoveTypeValid)
	{
		if ((move->chessBoard->movesFromSquares[move->src] & SQ_BIT(move->dst)) > 0) {
			move->type |= MoveTypeValid;
		}
		else {
			printf("validateSanMove() error: %s move from %s to %s is illegal in %u%s%s; FEN %s\n", pieceName[move->movingPiece], squareName[move->src], squareName[move->dst], move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? "." : "...", sanMove, move->chessBoard->fen->fenString);
			writeDebug(move->chessBoard, false);
			//for (int i = 0; i < n; i++) printf("moveCandidate %s\n", squareName[moveCandidates[i]]);
			return 1;
		}
	}
	return 0;
}

int validateUciMove(struct Move * move) {
	//3 lower bits for letter and 3 higher bits for number in uci move src and dst: 0..63 range
	move->src = SQ(move->uciMove[1] - '1', move->uciMove[0] - 'a');
	move->dst = SQ(move->uciMove[3] - '1', move->uciMove[2] - 'a');
	move->movingPiece = move->chessBoard->piecesOnSquares[move->src];
	if (move->movingPiece == PieceNameNone) {
		fprintf(stderr, "validateUciMove() error: the source square %s is empty in move from %s to %s in %u%s%s; FEN %s\n", squareName[move->src], squareName[move->src], squareName[move->dst], move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? "." : "...", move->sanMove, move->chessBoard->fen->fenString);
		return 1;
	}
	if ((move->chessBoard->movesFromSquares[move->src] & SQ_BIT(move->dst)) > 0)
		move->type |= MoveTypeValid;
	else {
		fprintf(stderr, "validateUciMove() error: %s move from %s to %s is illegal in %u%s%s; legal moves from %s are %llu\n", pieceName[move->movingPiece], squareName[move->src], squareName[move->dst], move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? "." : "...", move->sanMove, squareName[move->src], move->chessBoard->movesFromSquares[move->src]);
		writeDebug(move->chessBoard, true);
		reconcile(move->chessBoard);
		return 1;
	}
	return 0;
}

void san2uci(struct Move * move) {
	int castling = ((move->type & (MoveTypeCastlingKingside | MoveTypeCastlingQueenside)) - 1) >> 3;
	//struct Square s;
	int s = SquareNone;
	if (castling != CastlingSideNone && move->chessBoard->fen->isChess960)
	{
		//need to move the king on the kingside rook
		const int r[2] = { Rank1, Rank8 };
		unsigned long long rooks = move->chessBoard->occupations[PC(PC_COLOR(move->movingPiece), Rook)];
		const int kingFile = SQ_FILE(lsBit(move->chessBoard->occupations[PC(PC_COLOR(move->movingPiece), King)]));
		while (rooks) {
  		s = lsBit(rooks);
			if (PC_COLOR(move->movingPiece) == ColorWhite) {
				if (SQ_RANK(s) == r[PC_COLOR(move->movingPiece)] && SQ_FILE(s) > kingFile) {
					move->uciMove[0] = SQ_FILE(move->src) + 'a';
					move->uciMove[1] = SQ_RANK(move->src) + '1';
					move->uciMove[2] = SQ_FILE(s) + 'a';
					move->uciMove[3] = SQ_RANK(s) + '1';
					break;
				}
			} else {
				if (SQ_RANK(s) == r[PC_COLOR(move->movingPiece)] && SQ_FILE(s) < kingFile) {
					move->uciMove[0] = SQ_FILE(move->src) + 'a';
					move->uciMove[1] = SQ_RANK(move->src) + '1';
					move->uciMove[2] = SQ_FILE(s) + 'a';
					move->uciMove[3] = SQ_RANK(s) + '1';
					break;
				}
			}
			rooks &= rooks - 1;
		}
	} else {
		move->uciMove[0] = SQ_FILE(move->src) + 'a';
		move->uciMove[1] = SQ_RANK(move->src) + '1';
		move->uciMove[2] = SQ_FILE(move->dst) + 'a';
		move->uciMove[3] = SQ_RANK(move->dst) + '1';
		if ((move->type & MoveTypePromotion) == MoveTypePromotion) {
			move->uciMove[4] = uciPromoLetter[PC_TYPE(move->promoPiece)];
			move->uciMove[5] = '\0';
			return;
		}
		move->uciMove[4] = '\0';
	}
}

void uci2san(struct Move * move) {
	int castling = ((move->type & (MoveTypeCastlingKingside | MoveTypeCastlingQueenside))) >> 2;
	if (castling == CastlingSideQueenside) {
		strcpy(move->sanMove, "O-O-O");
		return;
	} else if (castling == CastlingSideKingside) {
		strcpy(move->sanMove, "O-O");
		return;
	}
	int i = 0;
	const int mpType = PC_TYPE(move->movingPiece);
	if (mpType != Pawn)
		move->sanMove[i++] = pieceLetter[mpType];
	else if ((move->type & MoveTypeCapture) == MoveTypeCapture)
		move->sanMove[i++] = SQ_FILE(move->src) + 'a';

	if (mpType != Pawn && mpType != King) {
		//find move candidates
		int moveCandidates[10];
		unsigned long long cp = move->chessBoard->occupations[move->movingPiece];
		int idx = 0;
		while (cp) {
  		int s = lsBit(cp);
			if ((move->chessBoard->movesFromSquares[s] & SQ_BIT(move->dst)) > 0) {
				moveCandidates[idx++] = s;
			}
			cp &= cp - 1;
		}
		for (int j = 0; j < idx; j++) {
			if (move->src == moveCandidates[j]) continue;
			if (SQ_RANK(move->src) == SQ_RANK(moveCandidates[j]))
				move->sanMove[i++] = SQ_FILE(move->src) + 'a';
			else if (SQ_FILE(move->src) == SQ_FILE(moveCandidates[j]))
				move->sanMove[i++] = SQ_RANK(move->src) + '1';
			else move->sanMove[i++] = SQ_FILE(move->src) + 'a';
		}
	}
	if ((move->type & MoveTypeCapture) == MoveTypeCapture) move->sanMove[i++] = 'x';
	move->sanMove[i++] = SQ_FILE(move->dst) + 'a';
	move->sanMove[i++] = SQ_RANK(move->dst) + '1';

	if ((move->type & MoveTypePromotion) == MoveTypePromotion) {
		move->sanMove[i++] = '=';
		move->sanMove[i++] = promoLetter[PC_TYPE(move->promoPiece)];
	}
	move->sanMove[i] = '\0';
	//Move has to be made in order to figure out if it checks, mates or leads to stale mate
}

void setUciMoveType(struct Move * move) {
	//Is move a capture? destination square is occupied by a piece of the opposite color
	//need to take care of chess 960 castling move where king captures its own rook!
	if (move->chessBoard->piecesOnSquares[move->dst] != PieceNameNone && PC_COLOR(move->chessBoard->piecesOnSquares[move->dst]) != move->chessBoard->fen->sideToMove) move->type |= MoveTypeCapture;

	//Is move a promotion?
	if (strlen(move->uciMove) == 5) {
		move->type |= MoveTypePromotion;
		const char promo[7] = "..nbrq";
		const char * idx = strchr(promo, move->uciMove[4]);
		if (idx)
			move->promoPiece = PC(move->chessBoard->fen->sideToMove, idx - promo);
		else {
			printf("setUciMoveType() error: unknown promotion piece %u%s%s\n", move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move->uciMove);
			exit(-1);
		}
	} else if (PC_TYPE(move->movingPiece) == Pawn) {
		//Is move en passant capture?
		if ((move->chessBoard->fen->enPassant < FileNone) && move->chessBoard->fen->enPassant + (move->chessBoard->fen->sideToMove == ColorWhite ? Rank6 << 3 : Rank3 << 3) == move->dst)
			move->type |= (MoveTypeCapture | MoveTypeEnPassant);
		//Is move en passant?
		else if (abs(move->src - move->dst) == 16) {
			//all opponent pawns
			unsigned long long pawns = move->chessBoard->occupations[PC(OPP_COLOR(move->chessBoard->fen->sideToMove), Pawn)];
			//opponent pawns on Rank 4 or 5 depending on the side to move
			if (move->chessBoard->fen->sideToMove == ColorWhite)
			  pawns &= RANK4;
			else pawns &= RANK5;
			//opponent pawns on adjacent files (adjacent to the moving pawn from its initial rank to rank 4 or 5)
			const int srcFile = SQ_FILE(move->src);
			if (srcFile == FileA) pawns &= files_bb[srcFile + 1];
			else if (srcFile == FileH) pawns &= files_bb[srcFile - 1];
			else pawns &= (files_bb[srcFile + 1] | files_bb[srcFile - 1]);
			//if there are such opponent pawns, then the move is en passant
			if (pawns) move->type |= MoveTypeEnPassant;
		}
	}
  //Is move castling?
	else if (PC_TYPE(move->movingPiece) == King) {
		//the king moves to the square occupied by its own rook in Chess960
		if (move->chessBoard->fen->isChess960 && move->chessBoard->piecesOnSquares[move->dst] == PC(move->chessBoard->fen->sideToMove, Rook)) {
			if (SQ_FILE(move->dst) == move->chessBoard->fen->castlingRook[1][move->chessBoard->fen->sideToMove])
				move->type |= MoveTypeCastlingQueenside;
			else if (SQ_FILE(move->dst) == move->chessBoard->fen->castlingRook[0][move->chessBoard->fen->sideToMove])
				move->type |= MoveTypeCastlingKingside;
			else {
				printf("setUciMoveType() error: illegal castling move in chess 960: the %s rook on %s is not a castling one\n", move->chessBoard->fen->sideToMove == ColorWhite ? "white" : "black", squareName[move->dst]);
				exit(-1);
			}
		} else {
			const int diff = move->src - move->dst;
			if (abs(diff) == 2) {
				const int dstKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
				if (move->dst == dstKingSquare[1][move->chessBoard->fen->sideToMove])
					move->type |= MoveTypeCastlingQueenside;
				else if (move->dst == dstKingSquare[0][move->chessBoard->fen->sideToMove])
					move->type |= MoveTypeCastlingKingside;
				else {
					printf("setUciMoveType() error: illegal %s king move from %s to %s; FEN %s\n", move->chessBoard->fen->sideToMove == ColorWhite ? "white" : "black", squareName[move->src], squareName[move->dst], move->chessBoard->fen->fenString);
					exit(-1);
				}
			}
		}
	}
}

void initMove(struct Move * move, struct Board * board, const char * moveString) {
	assert(move && board && moveString);
	move->type = MoveTypeNormal;
	move->src = SquareNone;
	move->dst = SquareNone;
  move->movingPiece = PieceNameNone;
	move->capturedPiece = PieceNameNone;
	move->promoPiece = PieceNameNone;
	move->castlingRook = FileNone;
  move->sanMove[0] = '\0';
  move->uciMove[0] = '\0';
  move->otherCastlingRook = FileNone;  //used in undoMove()
  move->prevCastlingRights = CastlingRightsWhiteNoneBlackNone;
  move->prevEnPassant = FileNone;
  move->prevHalfmoveClock = 0;
  move->prevCastlingRook = FileNone;
	move->chessBoard = board;
  int res = 0;
	if (!parseUciMove(moveString)) {
		strncpy(move->sanMove, moveString, sizeof move->sanMove);
		res = validateSanMove(move);
		assert(!res);
		if ((move->type & MoveTypeNull) != MoveTypeNull) {
			san2uci(move);
		}
	} else {
		strncpy(move->uciMove, moveString, sizeof move->uciMove);
		res = validateUciMove(move);
		assert(!res);
		setUciMoveType(move);
		uci2san(move);
	}
}

bool promoMove(struct Board * board, const int src, const int dst) {
  if (PC_TYPE(board->piecesOnSquares[src]) == Pawn) {
    int pre_promo_rank = board->fen->sideToMove == ColorWhite ? Rank7 : Rank2;
    int promo_rank = board->fen->sideToMove == ColorWhite ? Rank8 : Rank1;
    if (SQ_RANK(src) == pre_promo_rank && SQ_RANK(dst) == promo_rank) return true;
  }
  return false;
}

//because normally src and dst squares are taken from board's occupation and legal moves,
//no validation is necessary here
void init_move(struct Move * move, struct Board * board, int src, int dst, int promo) { //promo is 2 - knight, 3 - bishop, 4 - rook, 5 - queen
	move->type = MoveTypeValid;
	move->src = src;
	move->dst = dst;
	move->capturedPiece = PieceNameNone;
	move->castlingRook = FileNone;
  move->sanMove[0] = '\0';
  move->uciMove[0] = '\0';
  move->otherCastlingRook = FileNone;  //used in undoMove()
  move->prevCastlingRights = CastlingRightsWhiteNoneBlackNone;
  move->prevEnPassant = FileNone;
  move->prevHalfmoveClock = 0;
  move->prevCastlingRook = FileNone;
	if (board->piecesOnSquares[dst] && PC_COLOR(board->piecesOnSquares[dst]) == OPP_COLOR(board->fen->sideToMove)) 
	  move->type |= MoveTypeCapture;
	move->promoPiece = promoMove(board, src, dst) ? (board->fen->sideToMove == ColorWhite ? promo : promo + 8) : PieceNameNone;
	if (move->promoPiece) move->type |= MoveTypePromotion;
	move->movingPiece = board->piecesOnSquares[src];
	move->chessBoard = board;
	strcat(move->uciMove, squareName[src]);
	strcat(move->uciMove, squareName[dst]);
	move->uciMove[4] = uciPromoLetter[promo];
	if (PC_TYPE(move->movingPiece) == Pawn) {
		//Is move en passant capture?
		if ((board->fen->enPassant < FileNone) && board->fen->enPassant + (board->fen->sideToMove == ColorWhite ? Rank6 << 3 : Rank3 << 3) == dst)
			move->type |= (MoveTypeCapture | MoveTypeEnPassant);
		//Is move en passant?
		else if (abs(move->src - move->dst) == 16) {
			//all opponent pawns
			unsigned long long pawns = move->chessBoard->occupations[PC(OPP_COLOR(move->chessBoard->fen->sideToMove), Pawn)];
			//opponent pawns on Rank 4 or 5 depending on the side to move
			if (move->chessBoard->fen->sideToMove == ColorWhite)
			  pawns &= RANK4;
			else pawns &= RANK5;
			//opponent pawns on adjacent files (adjacent to the moving pawn from its initial rank to rank 4 or 5)
			const int srcFile = SQ_FILE(move->src);
			if (srcFile == FileA) pawns &= files_bb[srcFile + 1];
			else if (srcFile == FileH) pawns &= files_bb[srcFile - 1];
			else pawns &= (files_bb[srcFile + 1] | files_bb[srcFile - 1]);
			//if there are such opponent pawns, then the move is en passant
			if (pawns) move->type |= MoveTypeEnPassant;
		}
  }
  //Is move castling?
	else if (PC_TYPE(move->movingPiece) == King) {
		//the king moves to the square occupied by its own rook in Chess960
		if (board->fen->isChess960 && board->piecesOnSquares[dst] == PC(board->fen->sideToMove, Rook)) {
			if (SQ_FILE(move->dst) == board->fen->castlingRook[1][board->fen->sideToMove])
				move->type |= MoveTypeCastlingQueenside;
			else if (SQ_FILE(move->dst) == board->fen->castlingRook[0][board->fen->sideToMove])
				move->type |= MoveTypeCastlingKingside;
			else {
				updateFen(board);
				printf("init_move() error: illegal castling move in chess 960: the %s rook on %s is not a castling one, fen %s\n", board->fen->sideToMove == ColorWhite ? "white" : "black", squareName[dst], board->fen->fenString);
				exit(-1);
			}
		} else {
			if (abs(src - dst) == 2) {
				const int dstKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
				if (dst == dstKingSquare[1][board->fen->sideToMove])
					move->type |= MoveTypeCastlingQueenside;
				else if (dst == dstKingSquare[0][board->fen->sideToMove])
					move->type |= MoveTypeCastlingKingside;
				else {
					updateFen(board);
					printf("init_move() error: illegal %s king move from %s to %s; FEN %s\n", board->fen->sideToMove == ColorWhite ? "white" : "black", squareName[src], squareName[dst], board->fen->fenString);
					writeDebug(board, true);
  				exit(-1);
				}
			}
		}
	}
}

#ifdef __cplusplus
}
#endif
