#pragma warning(disable:4334)
#pragma warning(disable:4996)

#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "libchess.h"

///<summary>
/// returns string representation in the first argument
/// of a bit field move type given in a secondary argument
///</summary>
void getMoveType(char * mvType, unsigned char type) {
	//unsigned char N = bitCount((unsigned long)type);
	mvType[0] = '\0';
	unsigned char i;
	strcat(mvType, moveType[0]);
	while (type) {
		i = genLSBit(type);
		strcat(mvType, " | ");
		strcat(mvType, moveType[i + 1]);
		type ^= 1 << i;
	}
}

/* enum SquareName string2square(char * s) {
	unsigned char r = atoi(s);
	int f = s[0] - 'a';
	return ((r - 1) << 3) + f;
}*/

unsigned char moveCandidateScore(struct Square * sq, enum Files srcFile, enum Ranks srcRank)
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

bool parseUciMove(char * move) {
	// regex "^[a-h][1-8][a-h][1-8][bnqr]?$"
	char promo[] = "bnqr";
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

int validateSanMove(struct Move * move) {
	if (strcmp(move->sanMove, "--") == 0) {
		move->type = MoveTypeNull | MoveTypeValid;
		return 0;
	}
	char sanMove[12];
	char mvType[94];
	size_t len = strlen(move->sanMove);
	strncpy(sanMove, move->sanMove, len + 1);
	if (sanMove[len - 1] == '+' || sanMove[len - 1] == '#')
		sanMove[len - 1] = '\0';
	enum CastlingSide castlingSide = CastlingSideNone;
	if (strcmp(sanMove, "O-O-O") == 0)
		castlingSide = CastlingSideQueenside;
	else if (strcmp(sanMove, "O-O") == 0)
		castlingSide = CastlingSideKingside;
	//promo regex "[a-h](?:x[a-h])?[18]\=[BNQR]"
	char pieces[] = "NBRQ";
	if (sanMove[0] >= 'a' && sanMove[0] <= 'h') {
		if (sanMove[1] == '1' || sanMove[1] == '8') {
			if (sanMove[2] == '=') {
				char * idx = strchr(pieces, sanMove[3]);
				if (idx) {
					move->chessBoard->promoPiece = (move->chessBoard->fen->sideToMove << 3) | (idx - pieces + 2);
					move->type |= MoveTypePromotion;
					sanMove[2] = '\0';
				}
				else {
					getMoveType(mvType, move->type);
					printf("validateSanMove() error: Unknown promotion piece %u %s %s, move type %s\n", move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", sanMove, mvType);
					writeDebug(move->chessBoard, false);
					return 1;
				}
			}
		} else {
			if (sanMove[1] == 'x')
				if (sanMove[2] >= 'a' && sanMove[2] <= 'h')
					if (sanMove[3] == '1' || sanMove[3] == '8') {
						if (sanMove[4] == '=') {
							char * idx = strchr(pieces, sanMove[5]);
							if (idx) {
								move->chessBoard->promoPiece = (move->chessBoard->fen->sideToMove << 3) | (idx - pieces + 2);
								move->type |= MoveTypePromotion | MoveTypeCapture;
								sanMove[4] = '\0';
							}
							else {
								getMoveType(mvType, move->type);
								printf("validateSanMove() error: Unknown promotion piece %u %s %s, move type %s\n", move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", sanMove, mvType);
								writeDebug(move->chessBoard, false);
								return 1;
							}
						}
					}
		}
	}

	//castling
	if (castlingSide != CastlingSideNone) {
		move->type |= (castlingSide << 2);
		enum SquareName kingCastlingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
		enum PieceName kingPc = move->chessBoard->fen->sideToMove << 3 | King;
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
				printf("%s king move from %s to %s is not possible, move type %s\n", move->chessBoard->fen->sideToMove == ColorWhite ? "White" : "Black", squareName[king.square.name], squareName[kingCastlingSquare[castlingSide - 1][move->chessBoard->fen->sideToMove]], mvType);
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

	enum Files dstFile = FileNone;
	enum Files srcFile = FileNone;
	enum Ranks dstRank = RankNone;
	enum Ranks srcRank = RankNone;

	for (signed char i = (signed char)strlen(sanMove) - 1; i >= 0; i--) {
		if (sanMove[i] >= '1' && sanMove[i] <= '8') {
			enum Ranks x = sanMove[i] - '1';
			if (dstRank == RankNone) dstRank = x;
			else srcRank = x;
		} else if (sanMove[i] >= 'a' && sanMove[i] <= 'h') {
			enum Files x = sanMove[i] - 'a';
			if (dstFile == FileNone) dstFile = x;
			else srcFile = x;
		} else {
			char pieces[] = ".xNBRQK";
			char * idx = strchr(pieces, sanMove[i]);
			if (idx) {
				long pieceIndex = idx - pieces;
				if (pieceIndex > 1)
					piece(&(move->chessBoard->movingPiece.square), &(move->chessBoard->movingPiece), move->chessBoard->fen->sideToMove << 3 | pieceIndex);
				else if (pieceIndex == 1) {
					move->type |= MoveTypeCapture;
				}
			} else {
				printf("Unknown char ('%u') in move %u%s%s\n", sanMove[i], move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? "." : "...", sanMove);
				writeDebug(move->chessBoard, false);
				return 1;
			}
		}
	}
	square(&(move->destinationSquare), (dstRank << 3) | dstFile);
	if (move->destinationSquare.name >= SquareNone) {
		printf("dstRank or dstFile are not set in move %u%s%s\n", move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? "." : "...", sanMove);
		writeDebug(move->chessBoard, false);
		return 1;
	}
	if (move->chessBoard->movingPiece.name == PieceNameNone) {
		piece(&(move->chessBoard->movingPiece.square), &(move->chessBoard->movingPiece), move->chessBoard->fen->sideToMove == ColorWhite ? WhitePawn : BlackPawn);
		if (!(move->type & MoveTypeCapture)) {
			srcFile = move->destinationSquare.file;
			//detect EnPassant move
			enum Ranks dstRanks[] = { Rank4, Rank5 };
			enum Ranks srcRanks[] = { Rank2, Rank7 };
			enum SquareName srcSquare = (srcRanks[move->chessBoard->fen->sideToMove] << 3) + srcFile;
			if (move->destinationSquare.rank == dstRanks[move->chessBoard->fen->sideToMove]) {
				if ((move->chessBoard->movesFromSquares[srcSquare] & move->destinationSquare.bitSquare) > 0) {
					struct Square oppositePawnSquare;
					unsigned long oppositePawns = move->chessBoard->occupations[move->chessBoard->opponentColor << 3 | Pawn];
					square(&oppositePawnSquare, lsBit(oppositePawns));
					while (oppositePawnSquare.name < SquareNone) {
						if (oppositePawnSquare.rank == dstRanks[move->chessBoard->fen->sideToMove] && ((oppositePawnSquare.file == srcFile + 1 && srcFile < FileH) || (oppositePawnSquare.file == srcFile - 1 && srcFile > FileA))) {
							move->type |= (MoveTypeValid | MoveTypeEnPassant);
							memcpy(&(move->sourceSquare), &srcSquare, sizeof(struct Square));
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
		square(&(move->chessBoard->movingPiece.square), (srcRank << 3) + srcFile);
	else {
		//find move candidates
		struct Square * moveCandidates[10];

		unsigned long cp = move->chessBoard->occupations[move->chessBoard->movingPiece.name];
		struct Square * s;
		unsigned char n = 0;
		s = malloc(sizeof(struct Square));
		square(s, lsBit(cp));
		while (s->name < SquareNone) {
			if ((move->chessBoard->movesFromSquares[s->name] & move->destinationSquare.bitSquare) > 0) {
				if ((move->chessBoard->movingPiece.type == Pawn && s->file == srcFile) || move->chessBoard->movingPiece.type == King) {
					square(&(move->chessBoard->movingPiece.square), s->name);
					free(s);
					s = NULL;
					break;
				} else moveCandidates[n++] = s;
			} 
			cp ^= (1UL << s->name);
			if (!cp) {
				if (n)
					if (moveCandidates[n - 1] != s) {
						free(s);
						s = NULL;
					}
				break;
			}
			if (n)
				if (moveCandidates[n - 1] == s)
					s = malloc(sizeof(struct Square));
			square(s, lsBit(cp));
		}
		if (n == 0) free(s);
		if (move->chessBoard->movingPiece.square.name == SquareNone) {
			unsigned char t, maxT = 0;
			bool ambiguousMove = false;
			for (unsigned char i = 0; i < n; i++) {
				t = moveCandidateScore(moveCandidates[i], srcFile, srcRank);
				if (t > maxT) {
					maxT = t;
					square(&(move->chessBoard->movingPiece.square), moveCandidates[i]->name);
					ambiguousMove = false;
				} else if (t == maxT) ambiguousMove = true;
				free(moveCandidates[i]);
			}
			if (ambiguousMove) {
				getMoveType(mvType, move->type);
				printf("validateSanMove() error: ambiguous move %s, moveType %s\n", move->sanMove, mvType);
				writeDebug(move->chessBoard, false);
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
	if (!(move->type & MoveTypeValid))
	{
		if ((move->chessBoard->movesFromSquares[move->chessBoard->movingPiece.square.name] & move->destinationSquare.bitSquare) > 0) {
			square(&(move->sourceSquare), move->chessBoard->movingPiece.square.name);
			move->type |= MoveTypeValid;
		}
		else {
			printf("%s move from %s to %s is illegal in %u%s%s; FEN %s\n", pieceName[move->chessBoard->movingPiece.name], squareName[move->chessBoard->movingPiece.square.name], squareName[move->destinationSquare.name], move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? "." : "...", sanMove, move->chessBoard->fen->fenString);
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
		printf("The source square %s is empty in move from %s to %s in %u%s%s; FEN %s\n", squareName[move->chessBoard->movingPiece.name], squareName[move->sourceSquare.name], squareName[move->destinationSquare.name], move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? "." : "...", move->sanMove, move->chessBoard->fen->fenString);
		return 1;
	}
	if ((move->chessBoard->movesFromSquares[move->sourceSquare.name] & move->destinationSquare.bitSquare) > 0)
		move->type |= MoveTypeValid;
	else {
		printf("%s move from %s to %s is illegal in %u%s%s; legal moves %lu\n", pieceName[move->chessBoard->movingPiece.name], squareName[move->sourceSquare.name], squareName[move->destinationSquare.name], move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? "." : "...", move->sanMove, move->chessBoard->movesFromSquares[move->sourceSquare.name]);
		return 1;
	}
	return 0;
}

void san2uci(struct Move * move) {
	enum CastlingSide castling = ((move->type & (MoveTypeCastlingKingside | MoveTypeCastlingQueenside)) - 1) >> 3;
	struct Square s;
	if (castling != CastlingSideNone && move->chessBoard->fen->isChess960)
	{
		//need to move the king on the kingside rook
		enum Ranks r[2] = { Rank1, Rank8 };
		unsigned long rooks = move->chessBoard->occupations[move->chessBoard->movingPiece.color << 3 | Rook];
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
		if (move->type & MoveTypePromotion) {
			move->uciMove[4] = promoLetter[move->chessBoard->promoPiece & 7];
			move->uciMove[5] = '\0';
			return;
		}
		move->uciMove[4] = '\0';
	}
}

void uci2san(struct Move * move) {
	enum CastlingSide castling = ((move->type & (MoveTypeCastlingKingside | MoveTypeCastlingQueenside))) >> 2;
	if (castling == CastlingSideQueenside) {
		strcpy(move->sanMove, "O-O-O");
		return;
	} else if (castling == CastlingSideKingside) {
		strcpy(move->sanMove, "O-O");
		return;
	}
	unsigned char i = 0;
	if (move->chessBoard->movingPiece.type != Pawn)
		move->sanMove[i++] = pieceLetter[move->chessBoard->movingPiece.type];
	else if (move->type & MoveTypeCapture)
		move->sanMove[i++] = move->chessBoard->movingPiece.square.file + 'a';

	if (move->chessBoard->movingPiece.type != Pawn && move->chessBoard->movingPiece.type != King) {
		//find move candidates
		struct Square * moveCandidates[10];
		unsigned long cp = move->chessBoard->occupations[move->chessBoard->movingPiece.name];
		struct Square * s;
		s = malloc(sizeof(struct Square));
		square(s, lsBit(cp));
		unsigned char idx = 0;
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
					s = malloc(sizeof(struct Square));
			square(s, lsBit(cp));
		}
		if (idx == 0) free(s);
		for (unsigned char j = 0; j < idx; j++) {
			if (move->chessBoard->movingPiece.square.name == moveCandidates[j]->name) continue;
			if (move->chessBoard->movingPiece.square.rank == moveCandidates[j]->rank)
				move->sanMove[i++] = move->chessBoard->movingPiece.square.file + 'a';
			else if (move->chessBoard->movingPiece.square.file == moveCandidates[j]->file)
				move->sanMove[i++] = move->chessBoard->movingPiece.square.rank + '1';
			else move->sanMove[i++] = move->chessBoard->movingPiece.square.file + 'a';
			free(moveCandidates[j]);
		}
	}
	if (move->type & MoveTypeCapture) move->sanMove[i++] = 'x';
	move->sanMove[i++] = move->destinationSquare.file + 'a';
	move->sanMove[i++] = move->destinationSquare.rank + '1';

	if (move->type & MoveTypePromotion) {
		move->sanMove[i++] = '=';
		move->sanMove[i++] = promoLetter[move->chessBoard->promoPiece & 7];
	}
	move->sanMove[i++] = '\0';
	//Move has to be made in order to figure out if it checks, mates or leads to stale mate
}

void setUciMoveType(struct Move * move) {
	//Is move a capture? destination square is occupied by a piece of the opposite color
	//need to take care of chess 960 castling move where king captures its own rook!
	if (move->chessBoard->piecesOnSquares[move->destinationSquare.name] != PieceNameNone && (move->chessBoard->piecesOnSquares[move->destinationSquare.name] >> 3) != move->chessBoard->fen->sideToMove)
		move->type |= MoveTypeCapture;

	//Is move a pormotion?
	if (strlen(move->uciMove) == 5) {
		move->type |= MoveTypePromotion;
		char promo[] = "..nbrq";
		char * idx = strchr(promo, move->uciMove[5]);
		if (idx) 
			move->chessBoard->promoPiece = (move->chessBoard->fen->sideToMove << 3 | (idx - promo));
		else
			printf("Unknown promotion piece %u%s%s\n", move->chessBoard->fen->moveNumber, move->chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move->uciMove);
	} else if (move->chessBoard->movingPiece.type == Pawn)
	{
		//Is move en passant capture?
		if ((move->chessBoard->fen->enPassant < FileNone) && move->chessBoard->fen->enPassant + (move->chessBoard->fen->sideToMove == ColorWhite ? Rank6 << 3 : Rank3 << 3) == move->destinationSquare.name)
			move->type |= (MoveTypeCapture | MoveTypeEnPassant);
		//Is move en passant?
		else if ((move->chessBoard->movingPiece.square.name ^ move->destinationSquare.name) == 16)
		{
			struct Square s;
			unsigned long pawns = move->chessBoard->occupations[(move->chessBoard->opponentColor << 3) | Pawn];
			enum Ranks ranks[2] = { Rank4, Rank5 };
			square(&s, lsBit(pawns));
			while (s.name < SquareNone)
			{
				if (s.rank == ranks[move->chessBoard->fen->sideToMove] && ((s.file == move->sourceSquare.file + 1 && move->sourceSquare.file < FileH) || (s.file == move->sourceSquare.file - 1 && move->sourceSquare.file > FileA)))
				{
					move->type |= MoveTypeEnPassant;
					break;
				}
				pawns ^= s.bitSquare;
				square(&s, lsBit(pawns));
			}
		}
	}

//Is move castling?
	else if (move->chessBoard->movingPiece.type == King)
	{
		//the king moves to the square occupied by its own rook in Chess960
		if (move->chessBoard->fen->isChess960 && move->chessBoard->piecesOnSquares[move->destinationSquare.name] == ((move->chessBoard->fen->sideToMove << 3) | Rook))
		{
			if (move->destinationSquare.file == move->chessBoard->fen->castlingRook[1][move->chessBoard->fen->sideToMove])
				move->type |= MoveTypeCastlingQueenside;
			else if (move->destinationSquare.file == move->chessBoard->fen->castlingRook[0][move->chessBoard->fen->sideToMove])
				move->type |= MoveTypeCastlingKingside;
			else {
				printf("Illegal castling move in chess 960: the %s rook on %s is not a castling one\n", move->chessBoard->fen->sideToMove == ColorWhite ? "white" : "black", squareName[move->destinationSquare.name]);
				return;
			}
		}
		else
		{
			char diff = move->sourceSquare.name - move->destinationSquare.name;
			if (abs(diff) == 2) {
				enum SquareName dstKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
				if (move->destinationSquare.name == dstKingSquare[1][move->chessBoard->fen->sideToMove])
					move->type |= MoveTypeCastlingQueenside;
				else if (move->destinationSquare.name == dstKingSquare[0][move->chessBoard->fen->sideToMove])
					move->type |= MoveTypeCastlingKingside;
				else {
					printf("Illegal %s king move from %s to %s; FEN %s\n", move->chessBoard->fen->sideToMove == ColorWhite ? "white" : "black", squareName[move->sourceSquare.name], squareName[move->destinationSquare.name], move->chessBoard->fen->fenString);
					return;
				}
			}
		}
	}
}

int initMove(struct Move * move, struct Board * board, char * moveString) {
	if (!move || !board || !moveString) {
		printf("initMove() error: argument(s) must not be NULL\n");
		return 1;
	}
	memset(move, 0, sizeof(struct Move));
	board->capturedPiece = PieceNameNone;
	board->promoPiece = PieceNameNone;
	board->isCheck = false;
	board->isMate = false;
	board->isStaleMate = false;
	square(&(board->movingPiece.square), SquareNone);
	piece(&(board->movingPiece.square), &(board->movingPiece), PieceTypeNone);

	move->chessBoard = board;

	if (!parseUciMove(moveString)) {
		strncpy(move->sanMove, moveString, sizeof move->sanMove);
		move->uciMove[0] = '\0';
		if (validateSanMove(move)) {
			printf("initMove() error: validateSanMove failed\n");
			return 1;
		}
		if (!(move->type & MoveTypeNull))
			san2uci(move);
	} else {
		strncpy(move->uciMove, moveString, sizeof move->uciMove);
		move->sanMove[0] = '\0';
		if (validateUciMove(move)) {
			printf("initMove() error: validateUciMove failed\n");
			return 1;
		}
		setUciMoveType(move);
		uci2san(move);
	}
	return 0;
}

