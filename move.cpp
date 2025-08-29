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

///<summary>
/// returns string representation in the first argument
/// of a bit field move type given in a secondary argument
///</summary>
void getMoveType(char * mvType, unsigned int type) {
	//unsigned char N = bitCount((unsigned long long)type);
	mvType[0] = '\0';
	unsigned int i;
	strcat(mvType, moveType[0]);
	while (type) {
		i = lsBit(type);
		strcat(mvType, " | ");
		strcat(mvType, moveType[i + 1]);
		type ^= 1 << i;
	}
}

unsigned char moveCandidateScore(struct Square * sq, int srcFile, int srcRank)
{
	unsigned char score = 1;
	if (sq->file == srcFile && sq->rank == srcRank)
		score = 4;
	else if (sq->rank == srcRank)
		score = 3;
	else if (sq->file == srcFile)
		score = 2;
	return score;
}

bool parseUciMove(const char * move) {
	// regex "^[a-h][1-8][a-h][1-8][bnqr]?$"
	const char promo[] = "bnqr";
	size_t len = strlen(move);
	if (move[0] >= 'a' && move[0] <= 'h')
		if (move[1] >= '1' && move[1] <= '8')
			if (move[2] >= 'a' && move[2] <= 'h')
				if (move[3] >= '1' && move[3] <= '8')
					switch (len) {
					case 4:
						return true;
					case 5:
						if (strchr(promo, move[4]))
							return true;
					default:
						return false;
					}
	return false;
}

//__attribute__((no_sanitize("address")))
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
					move->chessBoard->promoPiece = (int)((move->chessBoard->fen->sideToMove << 3) | (idx - pieces + 2));
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
								move->chessBoard->promoPiece = (int)((move->chessBoard->fen->sideToMove << 3) | (idx - pieces + 2));
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
		int kingCastlingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
		int kingPc = move->chessBoard->fen->sideToMove << 3 | King;
		struct Square kingSrcSq, kingDstSq;
		square(&kingSrcSq, lsBit(move->chessBoard->occupations[kingPc]));
		memcpy(&(move->sourceSquare), &kingSrcSq, sizeof(struct Square));
		struct ChessPiece king;
		piece(&(move->sourceSquare), &king, kingPc);
		memcpy(&(move->chessBoard->movingPiece), &king, sizeof(struct ChessPiece));

		if (!move->chessBoard->fen->isChess960) {
			if ((move->chessBoard->movesFromSquares[king.square.name] & (1UL << kingCastlingSquare[castlingSide - 1][move->chessBoard->fen->sideToMove])) > 0) {
				square(&kingDstSq, kingCastlingSquare[castlingSide - 1][move->chessBoard->fen->sideToMove]);
				move->type |= MoveTypeValid;
			} else {
				getMoveType(mvType, move->type);
				printf("validateSanMove() error: %s king move from %s to %s is not possible, move type %s\n", move->chessBoard->fen->sideToMove == ColorWhite ? "White" : "Black", squareName[king.square.name], squareName[kingCastlingSquare[castlingSide - 1][move->chessBoard->fen->sideToMove]], mvType);
				writeDebug(move->chessBoard, false);
				return 1;
			}
		} else {
			unsigned char whiteBlack[2] = { 0, 56 };
			if ((move->chessBoard->movesFromSquares[king.square.name] & (1UL << (move->chessBoard->fen->castlingRook[castlingSide - 1][move->chessBoard->fen->sideToMove] + whiteBlack[move->chessBoard->fen->sideToMove]))) > 0) {
				square(&kingDstSq, move->chessBoard->fen->castlingRook[castlingSide - 1][move->chessBoard->fen->sideToMove] + whiteBlack[move->chessBoard->fen->sideToMove]);
				move->type |= MoveTypeValid;
			}
		}
		memcpy(&(move->destinationSquare), &kingDstSq, sizeof(struct Square));
		return 0;
	}

	int dstFile = FileNone;
	int srcFile = FileNone;
	int dstRank = RankNone;
	int srcRank = RankNone;

	for (signed char i = (signed char)strlen(sanMove) - 1; i >= 0; i--) {
		if (sanMove[i] >= '1' && sanMove[i] <= '8') {
			int x = sanMove[i] - '1';
			if (dstRank == RankNone) dstRank = x;
			else srcRank = x;
		} else if (sanMove[i] >= 'a' && sanMove[i] <= 'h') {
			int x = sanMove[i] - 'a';
			if (dstFile == FileNone) dstFile = x;
			else srcFile = x;
		} else {
			const char pieces[] = ".xNBRQK";
			const char * idx = strchr(pieces, sanMove[i]);
			if (idx) {
				long long pieceIndex = idx - pieces;
				if (pieceIndex > 1)
					piece(&(move->chessBoard->movingPiece.square), &(move->chessBoard->movingPiece), (int)((move->chessBoard->fen->sideToMove << 3) | pieceIndex));
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
	square(&(move->destinationSquare), (dstRank << 3) | dstFile);
	if (move->destinationSquare.name >= SquareNone) {
		printf("validateSanMove() error: dstRank or dstFile are not set in move %u%s%s\n", move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? "." : "...", sanMove);
		writeDebug(move->chessBoard, false);
		return 1;
	}
	if (move->chessBoard->movingPiece.name == PieceNameNone) {
		piece(&(move->chessBoard->movingPiece.square), &(move->chessBoard->movingPiece), move->chessBoard->fen->sideToMove == ColorWhite ? WhitePawn : BlackPawn);
		if ((move->type & MoveTypeCapture) != MoveTypeCapture) {
			srcFile = move->destinationSquare.file;
			//detect EnPassant move
			const int dstRanks[] = { Rank4, Rank5 };
			const int srcRanks[] = { Rank2, Rank7 };
			int srcSquare = (srcRanks[move->chessBoard->fen->sideToMove] << 3) | srcFile;
			struct Square srcSq;
			square(&srcSq, srcSquare);
			if (move->destinationSquare.rank == dstRanks[move->chessBoard->fen->sideToMove]) {
				if ((move->chessBoard->movesFromSquares[srcSquare] & move->destinationSquare.bitSquare) > 0) {
					struct Square oppositePawnSquare;
					unsigned long long oppositePawns = move->chessBoard->occupations[move->chessBoard->opponentColor << 3 | Pawn];
					//oppositePawns &= 0xFFFFFFFFFFFFFFFFUL;
					assert(oppositePawns == (oppositePawns & 0xFFFFFFFFFFFFFFFFUL));
					square(&oppositePawnSquare, lsBit(oppositePawns));
					while (oppositePawnSquare.name < SquareNone) {
						if (oppositePawnSquare.rank == dstRanks[move->chessBoard->fen->sideToMove] && ((oppositePawnSquare.file == srcFile + 1 && srcFile < FileH) || (oppositePawnSquare.file == srcFile - 1 && srcFile > FileA))) {
							move->type |= (MoveTypeValid | MoveTypeEnPassant);
							memcpy(&(move->sourceSquare), &srcSq, sizeof(struct Square));
							square(&(move->chessBoard->movingPiece.square), move->sourceSquare.name);
							break;
						}
						oppositePawns ^= oppositePawnSquare.bitSquare;
						square(&oppositePawnSquare, lsBit(oppositePawns));
					}
				}
			}
		} else if ((move->chessBoard->fen->enPassant < FileNone) && move->chessBoard->fen->enPassant + (move->chessBoard->fen->sideToMove == ColorWhite ? Rank6 << 3 : Rank3 << 3) == move->destinationSquare.name) {
			move->type |= MoveTypeEnPassant; // capture flag should have been set up already from "x" pattern in SAN move
			square(&(move->chessBoard->movingPiece.square), move->chessBoard->fen->sideToMove == ColorWhite ? move->destinationSquare.file > srcFile ? move->destinationSquare.name - 9 : move->destinationSquare.name - 7 : move->destinationSquare.file > srcFile ? move->destinationSquare.name + 7 : move->destinationSquare.name + 9);
		}
	}
	if (move->chessBoard->movingPiece.square.name == SquareNone && srcFile != FileNone && srcRank != RankNone)
		square(&(move->chessBoard->movingPiece.square), (srcRank << 3) | srcFile);
	else {
		//find move candidates
		struct Square * moveCandidates[10];
		//get a bitboard of squares where moving piece names (for example, white knights) are
		unsigned long long cp = move->chessBoard->occupations[move->chessBoard->movingPiece.name];
		struct Square * s;
		int n = 0; //number of candidates
		s = (struct Square *)malloc(sizeof(struct Square));
		//iterate over all squares where movingPiece.name are located
		square(s, lsBit(cp));
		while (s->name < SquareNone) {
			if ((move->chessBoard->movesFromSquares[s->name] & move->destinationSquare.bitSquare) > 0) {
  			//if moving piece is a pawn (its source file is known and equals to the square s->file) or a king
				if ((move->chessBoard->movingPiece.type == Pawn && s->file == srcFile) || move->chessBoard->movingPiece.type == King) {
					//we are sure that square s is the source square
					square(&(move->chessBoard->movingPiece.square), s->name);
					free(s);
					s = NULL;
					break;
				} else { //otherwise add this square to moveCandidates squares
					if (n >= 10) {
				    printf("validateSanMove() error: too many move candidates for %s\n", sanMove);
				    for (int i = 0; i < n; i++) free(moveCandidates[i]);
				    writeDebug(move->chessBoard, false);
				    return 1;
					}				
					moveCandidates[n++] = s;
        }
			}
			//continue with other pieces (or squares where these pieces are)
			cp ^= (1UL << s->name);
			//if no more pieces left
			if (!cp) {
				//if there are candidates
				if (n)
				  //if the last candidate is not on square s, free the square to prevent memory leak
					if (moveCandidates[n - 1] != s) {
						free(s);
						s = NULL;
					}
				break; //exit the loop 
			}
			//if pieces (for example, white knights) are still left and there are candidates for the move
			if (n)
			  //if last candidate is on square s
				if (moveCandidates[n - 1] == s)
				  //allocate a new square s
					s = (struct Square *)malloc(sizeof(struct Square));
			square(s, lsBit(cp));
		} //end of while() loop over squares
		if (n == 0) free(s); //free square s if there are no candidates
		//if moving piece source square is still unknown (i.e. its not a pawn or a king)
		if (move->chessBoard->movingPiece.square.name == SquareNone) {
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
  			square(&(move->chessBoard->movingPiece.square), moveCandidates[maxI]->name);
  			for (int i = 0; i < n; i++) free(moveCandidates[i]);
  		}
			else {
				getMoveType(mvType, move->type);
				printf("validateSanMove() error: ambiguous move %s, moveType %s, srcRank %c, srcFile %c, max candidate rating %d, max candidates %d\n", move->sanMove, mvType, enumRanks[srcRank], enumFiles[srcFile], maxT, n);
				writeDebug(move->chessBoard, true);
  			for (int i = 0; i < n; i++) free(moveCandidates[i]);
				return 1;
			}
		}
	}
	if (move->chessBoard->movingPiece.square.name == SquareNone) {
		getMoveType(mvType, move->type);
		printf("validateSanMove() error: source square not defined for the moving piece %s, move type %s\n", pieceName[move->chessBoard->movingPiece.name], mvType);
		writeDebug(move->chessBoard, false);
		return 1;
	}
	if ((move->type & MoveTypeValid) != MoveTypeValid)
	{
		if ((move->chessBoard->movesFromSquares[move->chessBoard->movingPiece.square.name] & move->destinationSquare.bitSquare) > 0) {
			square(&(move->sourceSquare), move->chessBoard->movingPiece.square.name);
			move->type |= MoveTypeValid;
		}
		else {
			printf("validateSanMove() error: %s move from %s to %s is illegal in %u%s%s; FEN %s\n", pieceName[move->chessBoard->movingPiece.name], squareName[move->chessBoard->movingPiece.square.name], squareName[move->destinationSquare.name], move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? "." : "...", sanMove, move->chessBoard->fen->fenString);
			writeDebug(move->chessBoard, false);
			return 1;
		}
	}
	return 0;
}

int validateUciMove(struct Move * move) {
	//char src[3] = { move->uciMove[0], move->uciMove[1], '\0' };
	//char dst[3] = { move->uciMove[2], move->uciMove[3], '\0' };
	struct Square srcSq, dstSq;
	//square(&srcSq, string2square(src));
	square(&srcSq, (move->uciMove[1] - '1') << 3 | move->uciMove[0] - 'a');
	//square(&dstSq, string2square(dst));
	square(&dstSq, (move->uciMove[3] - '1') << 3 | move->uciMove[2] - 'a');
	memcpy(&(move->sourceSquare), &srcSq, sizeof(struct Square));
	memcpy(&(move->destinationSquare), &dstSq, sizeof(struct Square));
	struct ChessPiece cp;
	piece(&(move->sourceSquare), &cp, move->chessBoard->piecesOnSquares[move->sourceSquare.name]);
	memcpy(&(move->chessBoard->movingPiece), &cp, sizeof(struct ChessPiece));
	if (move->chessBoard->movingPiece.name == PieceNameNone) {
		printf("validateUciMove() error: the source square %s is empty in move from %s to %s in %u%s%s; FEN %s\n", squareName[move->sourceSquare.name], squareName[move->sourceSquare.name], squareName[move->destinationSquare.name], move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? "." : "...", move->sanMove, move->chessBoard->fen->fenString);
		return 1;
	}
	if ((move->chessBoard->movesFromSquares[move->sourceSquare.name] & move->destinationSquare.bitSquare) > 0)
		move->type |= MoveTypeValid;
	else {
		printf("validateUciMove() error: %s move from %s to %s is illegal in %u%s%s; legal moves %llu\n", pieceName[move->chessBoard->movingPiece.name], squareName[move->sourceSquare.name], squareName[move->destinationSquare.name], move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? "." : "...", move->sanMove, move->chessBoard->movesFromSquares[move->sourceSquare.name]);
		writeDebug(move->chessBoard, true);
		reconcile(move->chessBoard);
		return 1;
	}
	return 0;
}

void san2uci(struct Move * move) {
	int castling = ((move->type & (MoveTypeCastlingKingside | MoveTypeCastlingQueenside)) - 1) >> 3;
	struct Square s;
	if (castling != CastlingSideNone && move->chessBoard->fen->isChess960)
	{
		//need to move the king on the kingside rook
		int r[2] = { Rank1, Rank8 };
		unsigned long long rooks = move->chessBoard->occupations[move->chessBoard->movingPiece.color << 3 | Rook];
		struct Square king;
		square(&king, lsBit(move->chessBoard->occupations[move->chessBoard->movingPiece.color << 3 | King]));
		square(&s, lsBit(rooks));
		while (s.name < SquareNone) {
			if (move->chessBoard->movingPiece.color == ColorWhite) {
				if (s.rank == r[move->chessBoard->movingPiece.color] && s.file > king.file) {
					move->uciMove[0] = move->chessBoard->movingPiece.square.file + 'a';
					move->uciMove[1] = move->chessBoard->movingPiece.square.rank + '1';
					move->uciMove[2] = s.file + 'a';
					move->uciMove[3] = s.rank + '1';
					break;
				}
			} else {
				if (s.rank == r[move->chessBoard->movingPiece.color] && s.file < king.file) {
					move->uciMove[0] = move->chessBoard->movingPiece.square.file + 'a';
					move->uciMove[1] = move->chessBoard->movingPiece.square.rank + '1';
					move->uciMove[2] = s.file + 'a';
					move->uciMove[3] = s.rank + '1';
					break;
				}
			}
			rooks ^= (1UL << s.name);
			square(&s, lsBit(rooks));
		}
	} else {
		move->uciMove[0] = move->chessBoard->movingPiece.square.file + 'a';
		move->uciMove[1] = move->chessBoard->movingPiece.square.rank + '1';
		move->uciMove[2] = move->destinationSquare.file + 'a';
		move->uciMove[3] = move->destinationSquare.rank + '1';
		if ((move->type & MoveTypePromotion) == MoveTypePromotion) {
			move->uciMove[4] = promoLetter[move->chessBoard->promoPiece & 7];
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
	if (move->chessBoard->movingPiece.type != Pawn)
		move->sanMove[i++] = pieceLetter[move->chessBoard->movingPiece.type];
	else if ((move->type & MoveTypeCapture) == MoveTypeCapture)
		move->sanMove[i++] = move->chessBoard->movingPiece.square.file + 'a';

	if (move->chessBoard->movingPiece.type != Pawn && move->chessBoard->movingPiece.type != King) {
		//find move candidates
		struct Square * moveCandidates[10];
		unsigned long long cp = move->chessBoard->occupations[move->chessBoard->movingPiece.name];
		struct Square * s;
		s = (struct Square *)malloc(sizeof(struct Square));
		square(s, lsBit(cp));
		int idx = 0;
		while (s->name < SquareNone) {
			if ((move->chessBoard->movesFromSquares[s->name] & move->destinationSquare.bitSquare) > 0) {
				moveCandidates[idx++] = s;
			}
			cp ^= s->bitSquare;
			if (!cp) {
				if (idx)
					if (moveCandidates[idx - 1] != s) {
						free(s);
						s = NULL;
					}
				break;
			}
			if (idx)
				if (moveCandidates[idx - 1] == s)
					s = (struct Square *)malloc(sizeof(struct Square));
			square(s, lsBit(cp));
		}
		if (idx == 0) free(s);
		for (int j = 0; j < idx; j++) {
			if (move->chessBoard->movingPiece.square.name == moveCandidates[j]->name) continue;
			if (move->chessBoard->movingPiece.square.rank == moveCandidates[j]->rank)
				move->sanMove[i++] = move->chessBoard->movingPiece.square.file + 'a';
			else if (move->chessBoard->movingPiece.square.file == moveCandidates[j]->file)
				move->sanMove[i++] = move->chessBoard->movingPiece.square.rank + '1';
			else move->sanMove[i++] = move->chessBoard->movingPiece.square.file + 'a';
			free(moveCandidates[j]);
		}
	}
	if ((move->type & MoveTypeCapture) == MoveTypeCapture) move->sanMove[i++] = 'x';
	move->sanMove[i++] = move->destinationSquare.file + 'a';
	move->sanMove[i++] = move->destinationSquare.rank + '1';

	if ((move->type & MoveTypePromotion) == MoveTypePromotion) {
		move->sanMove[i++] = '=';
		move->sanMove[i++] = promoLetter[move->chessBoard->promoPiece & 7];
	}
	move->sanMove[i] = '\0';
	//Move has to be made in order to figure out if it checks, mates or leads to stale mate
}

void setUciMoveType(struct Move * move) {
	//Is move a capture? destination square is occupied by a piece of the opposite color
	//need to take care of chess 960 castling move where king captures its own rook!
	if (move->chessBoard->piecesOnSquares[move->destinationSquare.name] != PieceNameNone && (move->chessBoard->piecesOnSquares[move->destinationSquare.name] >> 3) != move->chessBoard->fen->sideToMove)
		move->type |= MoveTypeCapture;

	//Is move a promotion?
	if (strlen(move->uciMove) == 5) {
		move->type |= MoveTypePromotion;
		const char promo[7] = "..nbrq";
		//strncpy(promo, "..nbrq", 6);
		const char * idx = strchr(promo, move->uciMove[4]);
		if (idx)
			move->chessBoard->promoPiece = (int)((move->chessBoard->fen->sideToMove << 3) | (idx - promo));
		else {
			printf("setUciMoveType() error: unknown promotion piece %u%s%s\n", move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move->uciMove);
			exit(-1);
		}
	} else if (move->chessBoard->movingPiece.type == Pawn) {
		//Is move en passant capture?
		if ((move->chessBoard->fen->enPassant < FileNone) && move->chessBoard->fen->enPassant + (move->chessBoard->fen->sideToMove == ColorWhite ? Rank6 << 3 : Rank3 << 3) == move->destinationSquare.name)
			move->type |= (MoveTypeCapture | MoveTypeEnPassant);
		//Is move en passant?
		else if (abs(move->sourceSquare.name - move->destinationSquare.name) == 16) {
			//all opponent pawns
			unsigned long long pawns = move->chessBoard->occupations[(move->chessBoard->opponentColor << 3) | Pawn];
			//opponent pawns on Rank 4 or 5 depending on the side to move
			if (move->chessBoard->fen->sideToMove == ColorWhite)
			  pawns &= RANK4;
			else pawns &= RANK5;
			//opponent pawns on adjacent files (adjacent to the moving pawn from its initial rank to rank 4 or 5)
			if (move->sourceSquare.file < FileH) pawns &= files_bb[move->sourceSquare.file + 1]; 
			if (move->sourceSquare.file > FileA) pawns &= files_bb[move->sourceSquare.file - 1];
			//if there are such opponent pawns, then the move is en passant
			if (pawns) move->type |= MoveTypeEnPassant;
			/*
			//this is a deprecated algorithm for finding if a move is en passant or not
			//it is slower because it iterates over all opponent pawns and breaks when it finds the first one
			struct Square s;
			int ranks[2] = { Rank4, Rank5 };
			square(&s, lsBit(pawns));
			while (s.name < SquareNone) {
				if (s.rank == ranks[move->chessBoard->fen->sideToMove] && ((move->sourceSquare.file < FileH && s.file == move->sourceSquare.file + 1) || (move->sourceSquare.file > FileA && s.file == move->sourceSquare.file - 1))) {
					move->type |= MoveTypeEnPassant;
					break;
				}
				pawns ^= s.bitSquare;
				square(&s, lsBit(pawns));
			}*/
		}
	}
  //Is move castling?
	else if (move->chessBoard->movingPiece.type == King) {
		//the king moves to the square occupied by its own rook in Chess960
		if (move->chessBoard->fen->isChess960 && move->chessBoard->piecesOnSquares[move->destinationSquare.name] == ((move->chessBoard->fen->sideToMove << 3) | Rook)) {
			if (move->destinationSquare.file == move->chessBoard->fen->castlingRook[1][move->chessBoard->fen->sideToMove])
				move->type |= MoveTypeCastlingQueenside;
			else if (move->destinationSquare.file == move->chessBoard->fen->castlingRook[0][move->chessBoard->fen->sideToMove])
				move->type |= MoveTypeCastlingKingside;
			else {
				printf("setUciMoveType() error: illegal castling move in chess 960: the %s rook on %s is not a castling one\n", move->chessBoard->fen->sideToMove == ColorWhite ? "white" : "black", squareName[move->destinationSquare.name]);
				exit(-1);
			}
		} else {
			int diff = move->sourceSquare.name - move->destinationSquare.name;
			if (abs(diff) == 2) {
				int dstKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
				if (move->destinationSquare.name == dstKingSquare[1][move->chessBoard->fen->sideToMove])
					move->type |= MoveTypeCastlingQueenside;
				else if (move->destinationSquare.name == dstKingSquare[0][move->chessBoard->fen->sideToMove])
					move->type |= MoveTypeCastlingKingside;
				else {
					printf("setUciMoveType() error: illegal %s king move from %s to %s; FEN %s\n", move->chessBoard->fen->sideToMove == ColorWhite ? "white" : "black", squareName[move->sourceSquare.name], squareName[move->destinationSquare.name], move->chessBoard->fen->fenString);
					exit(-1);
				}
			}
		}
	}
}

int initMove(struct Move * move, struct Board * board, const char * moveString) {
	if (!move || !board || !moveString) {
		printf("initMove() error: argument(s) must not be NULL\n");
		return 1;
	}
	memset(move, 0, sizeof(struct Move));
	board->capturedPiece = PieceNameNone;
	board->promoPiece = PieceNameNone;
	square(&(board->movingPiece.square), SquareNone);
	piece(&(board->movingPiece.square), &(board->movingPiece), PieceNameNone);

	move->chessBoard = board;
	move->castlingRook = FileNone;

	if (!parseUciMove(moveString)) {
		strncpy(move->sanMove, moveString, sizeof move->sanMove);
		move->uciMove[0] = '\0'; //this is not needed because of memset() call above
		if (validateSanMove(move)) {
			printf("initMove() error: validateSanMove failed\n");
			return 1;
		}
		if ((move->type & MoveTypeNull) != MoveTypeNull)
			san2uci(move);
	} else {
		strncpy(move->uciMove, moveString, sizeof move->uciMove);
		move->sanMove[0] = '\0'; //this is not needed because of memset() call above
		if (validateUciMove(move)) {
			printf("initMove() error: validateUciMove failed\n");
			return 1;
		}
		setUciMoveType(move);
		uci2san(move);
	}
	return 0;
}

bool promoMove(struct Board * board, int src, int dst) {
  if ((board->piecesOnSquares[src] & 7) == Pawn) {
    int src_rank = src / 8;
    int dst_rank = dst / 8;
    int pre_promo_rank = board->fen->sideToMove == ColorWhite ? Rank7 : Rank2;
    int promo_rank = board->fen->sideToMove == ColorWhite ? Rank8 : Rank1;
    if (src_rank == pre_promo_rank && dst_rank == promo_rank) return true;
  }
  return false;
}

//because normally src and dst squares are taken from board's occupation and legal moves,
//no validation is necessary here
void init_move(struct Move * move, struct Board * board, int src, int dst) {
	memset(move, 0, sizeof(struct Move));
	move->type = MoveTypeValid;
	square(&(move->sourceSquare), src);
	square(&(move->destinationSquare), dst);
	board->capturedPiece = PieceNameNone;
	if (board->piecesOnSquares[dst] && ((board->piecesOnSquares[dst] >> 3) == board->opponentColor)) 
	  move->type |= MoveTypeCapture;
	board->promoPiece = promoMove(board, src, dst) ? (board->fen->sideToMove == ColorWhite ? WhiteQueen : BlackQueen) : PieceNameNone; //deficiency - always assume promotion to queen
	if (board->promoPiece) move->type |= MoveTypePromotion;
	struct Square src_sq;
	square(&src_sq, src);
	piece(&src_sq, &(board->movingPiece), board->piecesOnSquares[src]);
	move->chessBoard = board;
	move->castlingRook = FileNone;
	strcat(move->uciMove, squareName[src]);
	strcat(move->uciMove, squareName[dst]);
	move->uciMove[4] = promoLetter[board->promoPiece & 7];
	if (board->movingPiece.type == Pawn) {
		//Is move en passant capture?
		if ((board->fen->enPassant < FileNone) && board->fen->enPassant + (board->fen->sideToMove == ColorWhite ? Rank6 << 3 : Rank3 << 3) == dst)
			move->type |= (MoveTypeCapture | MoveTypeEnPassant);
		//Is move en passant?
		else if (abs(move->sourceSquare.name - move->destinationSquare.name) == 16) {
			//all opponent pawns
			unsigned long long pawns = move->chessBoard->occupations[(move->chessBoard->opponentColor << 3) | Pawn];
			//opponent pawns on Rank 4 or 5 depending on the side to move
			if (move->chessBoard->fen->sideToMove == ColorWhite)
			  pawns &= RANK4;
			else pawns &= RANK5;
			//opponent pawns on adjacent files (adjacent to the moving pawn from its initial rank to rank 4 or 5)
			if (move->sourceSquare.file < FileH) pawns &= files_bb[move->sourceSquare.file + 1]; 
			if (move->sourceSquare.file > FileA) pawns &= files_bb[move->sourceSquare.file - 1];
			//if there are such opponent pawns, then the move is en passant
			if (pawns) move->type |= MoveTypeEnPassant;
		}
  }
  //Is move castling?
	else if (board->movingPiece.type == King) {
		//the king moves to the square occupied by its own rook in Chess960
		if (board->fen->isChess960 && board->piecesOnSquares[dst] == ((board->fen->sideToMove << 3) | Rook)) {
			if (move->destinationSquare.file == board->fen->castlingRook[1][board->fen->sideToMove])
				move->type |= MoveTypeCastlingQueenside;
			else if (move->destinationSquare.file == board->fen->castlingRook[0][board->fen->sideToMove])
				move->type |= MoveTypeCastlingKingside;
			else {
				printf("init_move() error: illegal castling move in chess 960: the %s rook on %s is not a castling one\n", board->fen->sideToMove == ColorWhite ? "white" : "black", squareName[dst]);
				exit(-1);
			}
		} else {
			if (abs(src - dst) == 2) {
				int dstKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
				if (dst == dstKingSquare[1][board->fen->sideToMove])
					move->type |= MoveTypeCastlingQueenside;
				else if (dst == dstKingSquare[0][board->fen->sideToMove])
					move->type |= MoveTypeCastlingKingside;
				else {
					printf("init_move() error: illegal %s king move from %s to %s; FEN %s\n", board->fen->sideToMove == ColorWhite ? "white" : "black", squareName[src], squareName[dst], board->fen->fenString);
  				exit(-1);
				}
			}
		}
	}
}

#ifdef __cplusplus
}
#endif
