#pragma warning(disable:4334)
#pragma warning(disable:4996)
#pragma warning(disable:4244)

#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "libchess.h"
/*
void drawPieceMoves(enum PieceType pc, enum SquareName sq, unsigned long moves) {
	enum SquareName squareIndex = SquareA1, sqStart = SquareA1;
	char buffer[8][255];
	enum Ranks rank = Rank1;

	for (enum Ranks r = Rank1; r <= Rank8; r++) buffer[r][0] = '\0';

	printf("%c on %s %lx\n", pieceLetter[pc], squareName[sq], moves);
	char s[6];
	while ((squareIndex = lsBit(moves)) < SquareNone) {
		moves ^= (1UL << squareIndex);
		for (enum SquareName k = sqStart; k < squareIndex; k++) {
			if ((k + 1) % 8 == 0) {
				sprintf(s, "| %c |", k == sq ? pieceLetter[pc] : ' ');
				strcat(buffer[rank++], s);
			}
			else {
				sprintf(s, "| %c ", k == sq ? pieceLetter[pc] : ' ');
				strcat(buffer[rank], s);
			}
		}
		if ((squareIndex + 1) % 8 == 0)
			strcat(buffer[rank++], "| x |");
		else
			strcat(buffer[rank], "| x ");
		sqStart = squareIndex;
		sqStart++;
	}
	for (enum SquareName k = sqStart; k < SquareNone; k++) {
		if ((k + 1) % 8 == 0) {
			sprintf(s, "| %c |", k == sq ? pieceLetter[pc] : ' ');
			strcat(buffer[rank++], s);
		}
		else {
			sprintf(s, "| %c ", k == sq ? pieceLetter[pc] : ' ');
			strcat(buffer[rank], s);
		}
	}
	printf("+---+---+---+---+---+---+---+---+\n");
	for (signed char i = 7; i >= 0; i--) {
		printf("%s\n", buffer[i]);
		printf("+---+---+---+---+---+---+---+---+\n");
	}
}
*/
/*
unsigned long diagMoves(struct Square * sq) {
	return (sq->diag >= 7) ? 0x8040201008040201UL << ((sq->diag - 7) << 3) : 0x8040201008040201UL >> ((7 - sq->diag) << 3);
}
unsigned long antiDiagMoves(struct Square * sq) {
	return (sq->antiDiag >= 7) ? 0x102040810204080UL << ((sq->antiDiag - 7) << 3) : 0x102040810204080UL >> ((7 - sq->antiDiag) << 3);
}
*/
///<summary>
/// naive chess piece (except pawns) move generator
/// moves are limited by board boundary only
/// may be used at the start to populate move arrays[64] of unsigned long
///<summary>
/*
unsigned long moveGenerator(enum PieceType pc, enum SquareName sq) {
	unsigned long d = 0;
	enum Files file = sq & 7;
	enum Ranks rank = sq >> 3;
	enum Diagonals diag = (7 + rank) - file;
	enum Antidiagonals antiDiag = file + rank;
	//unsigned long bitSquare = 1UL << sq;
	enum SquareName diff;

	switch (pc) {
	case Bishop:
		d = (diag >= 7) ? 0x8040201008040201UL << ((diag - 7) << 3) : 0x8040201008040201UL >> ((7 - diag) << 3);
		d |= (antiDiag >= 7) ? 0x102040810204080UL << ((antiDiag - 7) << 3) : 0x102040810204080UL >> ((7 - antiDiag) << 3);
		d ^= 1UL << sq;
		break;
	case Rook:
		d = ((0xFFUL << (sq & 56)) | (0x101010101010101UL << file)) ^ (1UL << sq);
		break;
	case Queen:
		d = (diag >= 7) ? 0x8040201008040201UL << ((diag - 7) << 3) : 0x8040201008040201UL >> ((7 - diag) << 3);
		d |= (antiDiag >= 7) ? 0x102040810204080UL << ((antiDiag - 7) << 3) : 0x102040810204080UL >> ((7 - antiDiag) << 3);
		d |= ((0xFFUL << (sq & 56)) | (0x101010101010101UL << file));
		d ^= 1UL << sq;
		break;
	case Knight:
		//if (file >= FileB && rank >= Rank3) d |= (1UL << (sq - 17));
		//if (file <= FileG && rank >= Rank3) d |= (1UL << (sq - 15));
		//if (file >= FileC && rank >= Rank2) d |= (1UL << (sq - 10));
		//if (file <= FileF && rank >= Rank2) d |= (1UL << (sq - 6));
		//if (file >= FileC && rank <= Rank7) d |= (1UL << (sq + 6));
		//if (file <= FileF && rank <= Rank7) d |= (1UL << (sq + 10));
		//if (file >= FileB && rank <= Rank6) d |= (1UL << (sq + 15));
		//if (file <= FileG && rank <= Rank6) d |= (1UL << (sq + 17));
		if (file > FileB && file < FileG)
			d = (sq > SquareC3) ? 0xA1100110AUL << (sq - SquareC3) : 0xA1100110AUL >> (SquareC3 - sq);
		else if (file == FileB)
			d = (sq > SquareB3) ? 0x508000805UL << (sq - SquareB3) : 0x508000805UL >> (SquareB3 - sq);
		else if (file == FileG)
			d = (sq > SquareG3) ? 0xA0100010A0UL << (sq - SquareG3) : 0xA0100010A0UL >> (SquareG3 - sq);
		else if (file == FileA)
			d = (sq > SquareA3) ? 0x204000402UL << (sq - SquareA3) : 0x204000402UL >> (SquareA3 - sq);
		else if (file == FileH)
			d = (sq > SquareH3) ? 0x4020002040UL << (sq - SquareH3) : 0x4020002040UL >> (SquareH3 - sq);		
		
		break;
	case King:
		if (file > FileA && file < FileH) {
			d = (sq > SquareB2) ? 0x070507UL << (sq - SquareB2) : 0x070507UL >> (SquareB2 - sq);
		}
		else if (file == FileA) {
			d = (sq > SquareA2) ? 0x30203UL << (sq - SquareA2) : 0x30203UL >> (SquareA2 - sq);
		}
		else if (file == FileH) {
			d = (sq > SquareH2) ? 0xC040C0UL << (sq - SquareH2) : 0xC040C0UL >> (SquareH2 - sq);
		}
				
		//if (file > FileA && rank > Rank1) d |= (1UL << (sq - 9));
		//if (rank > Rank1) d |= (1UL << (sq - 8));
		//if (file < FileH && rank > Rank1) d |= (1UL << (sq - 7));
		//if (file > FileA) d |= (1UL << (sq - 1));
		//if (file < FileH) d |= (1UL << (sq + 1));
		//if (file > FileA && rank < Rank8) d |= (1UL << (sq + 7));
		//if (rank < Rank8) d |= (1UL << (sq + 8));
		//if (file < FileH && rank < Rank8) d |= (1UL << (sq + 9));
		break;
	default:
		break;
	}
	drawPieceMoves(pc, sq, d);
	return d;
}
*/
/*
unsigned long moveGenerator(enum PieceType pc, struct Square * sq) {
	unsigned long d = 0;

	switch (pc) {
	case Bishop:
		d = (sq->diag >= 7) ? 0x8040201008040201UL << ((sq->diag - 7) << 3) : 0x8040201008040201UL >> ((7 - sq->diag) << 3);
		d |= (sq->antiDiag >= 7) ? 0x102040810204080UL << ((sq->antiDiag - 7) << 3) : 0x102040810204080UL >> ((7 - sq->antiDiag) << 3);
		d ^= sq->bitSquare;
		break;
	case Rook:
		d = ((0xFFUL << (sq->name & 56)) | (0x101010101010101UL << sq->file)) ^ (sq->bitSquare);
		break;
	case Queen:
		d = (sq->diag >= 7) ? 0x8040201008040201UL << ((sq->diag - 7) << 3) : 0x8040201008040201UL >> ((7 - sq->diag) << 3);
		d |= (sq->antiDiag >= 7) ? 0x102040810204080UL << ((sq->antiDiag - 7) << 3) : 0x102040810204080UL >> ((7 - sq->antiDiag) << 3);
		d |= ((0xFFUL << (sq->name & 56)) | (0x101010101010101UL << sq->file));
		d ^= 1UL << sq->name;
		break;
	case Knight:
		if (sq->file > FileB && sq->file < FileG)
			d = (sq->name > SquareC3) ? 0xA1100110AUL << (sq->name - SquareC3) : 0xA1100110AUL >> (SquareC3 - sq->name);
		else if (sq->file == FileB)
			d = (sq->name > SquareB3) ? 0x508000805UL << (sq->name - SquareB3) : 0x508000805UL >> (SquareB3 - sq->name);
		else if (sq->file == FileG)
			d = (sq->name > SquareG3) ? 0xA0100010A0UL << (sq->name - SquareG3) : 0xA0100010A0UL >> (SquareG3 - sq->name);
		else if (sq->file == FileA)
			d = (sq->name > SquareA3) ? 0x204000402UL << (sq->name - SquareA3) : 0x204000402UL >> (SquareA3 - sq->name);
		else if (sq->file == FileH)
			d = (sq->name > SquareH3) ? 0x4020002040UL << (sq->name - SquareH3) : 0x4020002040UL >> (SquareH3 - sq->name);
		break;
	case King:
		if (sq->file > FileA && sq->file < FileH) {
			d = (sq->name > SquareB2) ? 0x070507UL << (sq->name - SquareB2) : 0x070507UL >> (SquareB2 - sq->name);
		}
		else if (sq->file == FileA) {
			d = (sq->name > SquareA2) ? 0x30203UL << (sq->name - SquareA2) : 0x30203UL >> (SquareA2 - sq->name);
		}
		else if (sq->file == FileH) {
			d = (sq->name > SquareH2) ? 0xC040C0UL << (sq->name - SquareH2) : 0xC040C0UL >> (SquareH2 - sq->name);
		}
		break;
	default:
		break;
	}
	return d;
}
*/
///<summary>
/// draws a board with just specified piece names such as white pawns, for example
/// filling other squares with '0' or 'o' regardless if it is occupied by other piece or not
///</summary>
/*
void drawBoard(struct Board * board, enum PieceName piece) {
	enum SquareName squareIndex = SquareA1, sqStart = SquareA1;
	char buffer[8][255];
	enum Ranks rank = Rank1;
	char empty;
	if (piece != PieceNameNone) empty = '0';
	else empty = ' ';

	for (enum Ranks r = Rank1; r <= Rank8; r++) buffer[r][0] = '\0';

	//prints out the requested piece name
	if (piece != BlackKing && piece != WhiteKing)
		printf("%ss (%c)\n", pieceName[piece], pieceLetter[board->piecesOnSquares[piece]]);
	else
		printf("%s (%c)\n", pieceName[piece], pieceLetter[board->piecesOnSquares[piece]]);
	char s[6];
	unsigned long squares = board->occupations[piece];
	//iterates for all occupations (squares) of a piece name, such as white pawns, for example
	while ((squareIndex = lsBit(squares)) < SquareNone) {
		squares ^= (1UL << squareIndex);
		for (enum SquareName k = sqStart; k < squareIndex; k++) {
			if ((k + 1) % 8 == 0) {
				sprintf(s, "| %c |", empty);
				strcat(buffer[rank++], s);
			}
			else {
				sprintf(s, "| %c ", empty);
				strcat(buffer[rank], s);
			}
		}
		if ((squareIndex + 1) % 8 == 0) {
			sprintf(s, "| %c |", pieceLetter[board->piecesOnSquares[piece]]);
			strcat(buffer[rank++], s);
		}
		else {
			sprintf(s, "| %c ", pieceLetter[board->piecesOnSquares[piece]]);
			strcat(buffer[rank], s);
		}
		sqStart = ++squareIndex;
	}
	for (enum SquareName k = sqStart; k < SquareNone; k++) {
		if ((k + 1) % 8 == 0) {
			sprintf(s, "| %c |", empty);
			strcat(buffer[rank++], s);
		} else {
			sprintf(s, "| %c ", empty);
			strcat(buffer[rank], s);
		}
	}
	printf("+---+---+---+---+---+---+---+---+\n");
	for (signed char i = 7; i >= 0; i--) {
		printf("%s\n", buffer[i]);
		printf("+---+---+---+---+---+---+---+---+\n");
	}
}
*/

///<summary>
/// draws moves from a given square sq
/// it is called from writeDebug
///</summary>
void drawMoves(struct Board * board, enum SquareName sq) {
	enum SquareName squareIndex = SquareA1, sqStart = SquareA1;
	char buffer[8][255];
	enum Ranks rank = Rank1;
	unsigned long moves = board->movesFromSquares[sq];
	const char * m = (board->fen->sideToMove == ColorWhite && (board->piecesOnSquares[sq] >> 3) == ColorWhite) || (board->fen->sideToMove == ColorBlack && (board->piecesOnSquares[sq] >> 3) == ColorBlack) ? " moves (x): " : " controlled squares (x): ";

	for (enum Ranks r = Rank1; r <= Rank8; r++) buffer[r][0] = '\0';

	printf("%s on %s %s %lx\n", pieceName[board->piecesOnSquares[sq]], squareName[sq], m, moves);
	char s[6];
	while ((squareIndex = lsBit(moves)) < SquareNone) {
		moves ^= (1UL << squareIndex);
		for (enum SquareName k = sqStart; k < squareIndex; k++) {
			if ((k + 1) % 8 == 0) {
				sprintf(s, "| %c |", pieceLetter[board->piecesOnSquares[k]]);
				strcat(buffer[rank++], s);
			}
			else {
				sprintf(s, "| %c ", pieceLetter[board->piecesOnSquares[k]]);
				strcat(buffer[rank], s);
			}
		}
		if ((squareIndex + 1) % 8 == 0)
			strcat(buffer[rank++], "| x |");
		else
			strcat(buffer[rank], "| x ");
		sqStart = squareIndex;
		sqStart++;
	}
	for (enum SquareName k = sqStart; k < SquareNone; k++) {
		if ((k + 1) % 8 == 0) {
			sprintf(s, "| %c |", pieceLetter[board->piecesOnSquares[k]]);
			strcat(buffer[rank++], s);
		}
		else {
			sprintf(s, "| %c ", pieceLetter[board->piecesOnSquares[k]]);
			strcat(buffer[rank], s);
		}
	}
	printf("+---+---+---+---+---+---+---+---+\n");
	for (signed char i = 7; i >= 0; i--) {
		printf("%s\n", buffer[i]);
		printf("+---+---+---+---+---+---+---+---+\n");
	}
}

/// <summary>
/// Draws the chessboard and if displayMoves is true, then all leagal moves
/// </summary>
void writeDebug(struct Board * board, bool displayMoves) {
	char buffer[8][255];
	enum Ranks rank = Rank1;
	printf("chess board\n");
	for (enum Ranks r = Rank1; r <= Rank8; r++) buffer[r][0] = '\0';
	for (enum SquareName i = SquareA1; i <= SquareH8; i++) {
		char s[6];
		if ((i + 1) % 8 == 0) {
			sprintf(s, "| %c |", pieceLetter[board->piecesOnSquares[i]]);
			strcat(buffer[rank++], s);
		}
		else {
			sprintf(s, "| %c ", pieceLetter[board->piecesOnSquares[i]]);
			strcat(buffer[rank], s);
		}
	}

	printf("+---+---+---+---+---+---+---+---+\n");
	for (signed char s = 7; s >= 0; s--) {
		printf("%s\n", buffer[s]);
		printf("+---+---+---+---+---+---+---+---+\n");
	}
	if (displayMoves) {
		enum SquareName square;
		unsigned long d = board->occupations[PieceNameAny];
		while ((square = lsBit(d)) < 64) {
			d ^= (1UL << square);
			drawMoves(board, square);
		}
	}
	if (board->isMate) printf("%s\n", board->fen->sideToMove == ColorWhite ? "White is mated" : "Black is mated");
	else if (board->isStaleMate) printf("%s\n", board->fen->sideToMove == ColorWhite ? "White is stalemated" : "Black is stalemated");
	else if (board->isCheck) printf("%s\n", board->fen->sideToMove == ColorWhite ? "White is checked" : "Black is checked");
}

///<summary>
/// returns 0 if occupations reconcile with piecesOnSquares,
/// otherwise, non-zero error code
///</summary>
/*

int reconcile(struct Board * board) {
	int err = 0;
	for (enum SquareName i = SquareA1; i <= SquareH8; i++) {
		if (!(board->occupations[board->piecesOnSquares[i]] & (1UL << i))) {
			printf("reconcile() error: piecesOnSquares[%s] %s does not match its occupation bitboard %lx\n", squareName[i], pieceName[board->piecesOnSquares[i]], board->occupations[board->piecesOnSquares[i]]);
			err = 1;
		}
	}
	for (enum PieceName j = PieceNameNone; j <= PieceNameBlack; j++) {
		unsigned char s;
		unsigned long o = board->occupations[j];
		while ((s = lsBit(o)) < SquareNone) {
			if (board->piecesOnSquares[s] != j) {
				switch (j) {
				case PieceNameAny:
					if (board->piecesOnSquares[s] == PieceNameNone)
						break;
				case PieceNameWhite:
					if (board->piecesOnSquares[s] >> 3 == ColorWhite)
						break;
				case PieceNameBlack:
					if (board->piecesOnSquares[s] >> 3 == ColorBlack)
						break;
				default:
					printf("reconcile() error: %s piece in occupations does not match one in piecesOnSquares[%s] %s\n", pieceName[j], squareName[s], pieceName[board->piecesOnSquares[s]]);
					err = 1;
				}
			}
			o ^= 1UL << s;
		}
	}
	return err;
}
*/

void updateFen(struct Board * board) {
	for (signed char i = 7; i >= 0; i--) {
		bool prevSquareEmpty = false;
		unsigned char n = 0, r = 0;
		char fenRank[9];
		for (unsigned char j = 0; j <= 7; j++) {
			unsigned char s = (i << 3) | j;
			if (board->piecesOnSquares[s] == PieceNameNone) {
				if (!prevSquareEmpty) {
					prevSquareEmpty = true;
					n = 1;
				} else n++;
			} else {
				if (prevSquareEmpty) {
					fenRank[r++] = '0' + n;
					prevSquareEmpty = false;
				}
				fenRank[r++] = pieceLetter[board->piecesOnSquares[s]];
			}
		}
		if (prevSquareEmpty) fenRank[r++] = '0' + n;
		fenRank[r] = '\0';
		strncpy(board->fen->ranks[i], fenRank, strlen(fenRank) + 1);
	}
	fentostr(board->fen);
}

bool isEnPassantLegal(struct Board * board) {
	enum Files i;
	enum SquareName shift;
	struct Square kingSquare;
	struct Square opponentRookOrQueenSquare;
	square(&kingSquare, lsBit(board->occupations[(board->fen->sideToMove << 3) | King]));
	for (enum PieceType pt = Rook; pt <= Queen; pt++) {
		unsigned long opponentRooksOrQueens = board->occupations[(board->opponentColor << 3) | pt];
		square(&opponentRookOrQueenSquare, lsBit(opponentRooksOrQueens));
		// a loop for all opponent rooks (or queens)
		while (opponentRookOrQueenSquare.name < SquareNone) {
			// check if opponent rook and board->fen->sideToMove king are on the same rank
			if (kingSquare.rank != opponentRookOrQueenSquare.rank) {
				opponentRooksOrQueens ^= opponentRookOrQueenSquare.bitSquare;
				square(&opponentRookOrQueenSquare, lsBit(opponentRooksOrQueens));
				continue;
			}
			// if yes, then shift opponent rook (or queen) along it one way, then the other
			// to see if it attacks the king
			i = opponentRookOrQueenSquare.file; shift = opponentRookOrQueenSquare.name;
			while (i++ < FileH)
				// if there is something on the way but it is not the king, then break
				// otherwise, return enPassant is not legal - return false
				if (board->piecesOnSquares[shift += 1] != PieceNameNone) {
					if (shift == kingSquare.name) return false;
					break;
				}
			i = opponentRookOrQueenSquare.file; shift = opponentRookOrQueenSquare.name;
			while (i-- > FileA)
				if (board->piecesOnSquares[shift -= 1] != PieceNameNone) {
					if (shift == kingSquare.name) return false;
					break;
				}
			opponentRooksOrQueens ^= (1UL << opponentRookOrQueenSquare.name); //iterator
			square(&opponentRookOrQueenSquare, lsBit(opponentRooksOrQueens));
		}
	}
	return true;
}
//generates bishop moves from a given square limited by board boundaries and other chess pieces regardless of their color
unsigned long generateBishopMoves(struct Board * board, struct Square * sq) {
	unsigned long d = 0;
	enum Files i = sq->file;
	enum Ranks j = sq->rank;
	enum SquareName shift = sq->name;

	while (i++ < FileH && j++ < Rank8)
		if (board->piecesOnSquares[shift += 9] == PieceNameNone) d |= 1UL << shift; 
		else { d |= 1UL << shift; break; }
	i = sq->file; j = sq->rank; shift = sq->name;
	while (i-- > FileA && j-- > Rank1)
		if (board->piecesOnSquares[shift -= 9] == PieceNameNone) d |= 1UL << shift; 
		else { d |= 1UL << shift; break; }
	i = sq->file; j = sq->rank; shift = sq->name;
	while (i-- > FileA && j++ < Rank8)
		if (board->piecesOnSquares[shift += 7] == PieceNameNone) d |= 1UL << shift;
		else { d |= 1UL << shift; break; }
	i = sq->file; j = sq->rank; shift = sq->name;
	while (i++ < FileH && j-- > Rank1)
		if (board->piecesOnSquares[shift -= 7] == PieceNameNone) d |= 1UL << shift;
		else { d |= 1UL << shift; break; }
	return d;
}
//generates rook moves from a given square limited by board boundaries and other chess pieces regardless of their color
unsigned long generateRookMoves(struct Board * board, struct Square * sq) {
	//enum Files i;
	//enum Ranks j;
	int i;
	enum SquareName shift;
	unsigned long d = 0;
	//bitwise operations are slightly slower than array lookup - more operations I suppose
	//unsigned long t;
	//unsigned long o = board->occupations[PieceNameAny] ^ sq->bitSquare;
	//t = sq->bitSquare;
	//while (!((t & FILE_H) || (t & o))) t |= t << 1;
	//d |= t; t = sq->bitSquare;
	//while (!((t & FILE_A) || (t & o))) t |= t >> 1;
	//d |= t; t = sq->bitSquare;
	//while (!((t & RANK8) || (t & o))) t |= t << 8;
	//d |= t; t = sq->bitSquare;
	//while (!((t & RANK1) || (t & o))) t |= t >> 8;
	//d |= t;
	//return d ^ sq->bitSquare;
	i = sq->rank; shift = sq->name;
	while (i++ < Rank8)
		if (board->piecesOnSquares[shift += 8] == PieceNameNone) d |= 1UL << shift; else { d |= 1UL << shift; break; }
	i = sq->rank; shift = sq->name;
	while (i-- > Rank1)
		if (board->piecesOnSquares[shift -= 8] == PieceNameNone) d |= 1UL << shift; else { d |= 1UL << shift; break; }
	shift = sq->name; i = sq->file;
	while (i++ < FileH)
		if (board->piecesOnSquares[shift += 1] == PieceNameNone) d |= 1UL << shift; else { d |= 1UL << shift; break; }
	i = sq->file; shift = sq->name;
	while (i-- > FileA)
		if (board->piecesOnSquares[shift -= 1] == PieceNameNone) d |= 1UL << shift; else { d |= 1UL << shift; break; }
	return d;
}
//generates knight moves from a given square limited by board boundaries only
unsigned long generateKnightMoves(struct Square * sq) {
	if (sq->file > FileB && sq->file < FileG)
		return (sq->name > SquareC3) ? 0xA1100110AUL << (sq->name - SquareC3) : 0xA1100110AUL >> (SquareC3 - sq->name);
	else if (sq->file == FileB)
		return (sq->name > SquareB3) ? 0x508000805UL << (sq->name - SquareB3) : 0x508000805UL >> (SquareB3 - sq->name);
	else if (sq->file == FileG)
		return (sq->name > SquareG3) ? 0xA0100010A0UL << (sq->name - SquareG3) : 0xA0100010A0UL >> (SquareG3 - sq->name);
	else if (sq->file == FileA)
		return (sq->name > SquareA3) ? 0x204000402UL << (sq->name - SquareA3) : 0x204000402UL >> (SquareA3 - sq->name);
	else if (sq->file == FileH)
		return (sq->name > SquareH3) ? 0x4020002040UL << (sq->name - SquareH3) : 0x4020002040UL >> (SquareH3 - sq->name);
	else return 0;
}
//generates king moves from a given square limited by board boundaries only
unsigned long generateKingMoves(struct Square * sq) {
	if (sq->file > FileA && sq->file < FileH) {
		return (sq->name > SquareB2) ? 0x070507UL << (sq->name - SquareB2) : 0x070507UL >> (SquareB2 - sq->name);
	}
	else if (sq->file == FileA) {
		return (sq->name > SquareA2) ? 0x30203UL << (sq->name - SquareA2) : 0x30203UL >> (SquareA2 - sq->name);
	}
	else if (sq->file == FileH) {
		return (sq->name > SquareH2) ? 0xC040C0UL << (sq->name - SquareH2) : 0xC040C0UL >> (SquareH2 - sq->name);
	}
	else return 0;
}
unsigned long rookPinFinder(struct Board * board, struct Square * sq, struct Square * kingSquare) {
	unsigned long d = 0;
	enum Files ii = kingSquare->file; 
	enum Ranks jj = kingSquare->rank; 
	enum SquareName shift = kingSquare->name;
	//King and opponent rook (or queen) are on the same file
	if (sq->file == kingSquare->file) {
		//We move the king along the file towards the opponent rook (or queen) until we bump into something.
		//Later in is_pinned() function we'll see if that "something" is the same color as the king and if it is attacked
		//by opponent rook (or queen). If it is, then this "something" is pinned against the king
		if (sq->name > kingSquare->name) {
			while (jj++ < Rank8)
				if (board->piecesOnSquares[shift += 8] != PieceNameNone) { d |= 1UL << shift; break; }
		}
		else {
			while (jj-- > Rank1)
				if (board->piecesOnSquares[shift -= 8] != PieceNameNone) { d |= 1UL << shift; break; }
		}
	}
	//King and opponent rook (or queen) are on the same rank
	else if (sq->rank == kingSquare->rank) {
		//We move the king along the rank towards the opponent rook (or queen) until we bump into something.
		//Later in is_pinned() function we'll see if that "something" is the same color as the king and if it is attacked
		//by opponet rook (or queen). If it is, then this "something" is pinned against the king
		if (sq->name > kingSquare->name) {
			while (ii++ < FileH)
				if (board->piecesOnSquares[shift += 1] != PieceNameNone) { d |= 1UL << shift; break; }
		}
		else {
			while (ii-- > FileA)
				if (board->piecesOnSquares[shift -= 1] != PieceNameNone) { d |= 1UL << shift; break; }
		}
	}
	
	return d;
}
unsigned long bishopPinFinder(struct Board * board, struct Square * sq, struct Square * kingSquare) {
	unsigned long d = 0;
	enum Files ii = kingSquare->file; 
	enum Ranks jj = kingSquare->rank; 
	enum SquareName shift = kingSquare->name;
	//King and opponent bishop (or queen) are on the same diagonal
	if (sq->diag == kingSquare->diag) {
		//We move the king along this diagonal towards the opponent bishop (or queen) until we bump into something.
		//Later in is_pinned() function we'll see if that "something" is the same color as the king and if it is attacked
		//by opponent bishop (or queen). If it is, then this "something" is pinned against the king
		if (sq->name > kingSquare->name) {
			while (ii++ < FileH && jj++ < Rank8)
				if (board->piecesOnSquares[shift += 9] != PieceNameNone) { d |= 1UL << shift; break; }
		}
		else {
			while (ii-- > FileA && jj-- > Rank1)
				if (board->piecesOnSquares[shift -= 9] != PieceNameNone) { d |= 1UL << shift; break; }
		}
	}
	//King and opponent bishop (or queen) are on the same anti-diagonal
	else if (sq->antiDiag == kingSquare->antiDiag) {
		//We move the king along this anti-diagonal towards the opponent bishop (or queen) until we bump into something.
		//Later in is_pinned() function we'll see if that "something" is the same color as the king and if it is attacked
		//by opponent bishop (or queen). If it is, then this "something" is pinned against the king
		if (sq->name > kingSquare->name) {
			while (ii-- > FileA && jj++ < Rank8)
				if (board->piecesOnSquares[shift += 7] != PieceNameNone) { d |= 1UL << shift; break; }
		}
		else {
			while (ii++ < FileH && jj-- > Rank1)
				if (board->piecesOnSquares[shift -= 7] != PieceNameNone) { d |= 1UL << shift; break; }
		}
	}
	return d;
}
unsigned long pinnedBishopMoves(struct Board * board, struct Square * sq, struct Square * pinnedBy) {
	unsigned long d = 0;
	enum Files ii = sq->file;
	enum Ranks jj = sq->rank;
	enum SquareName shift = sq->name;
	//if pinned on diagonal, then it can only move along it
	if (pinnedBy->diag == sq->diag) {
		while (ii++ < FileH && jj++ < Rank8)
			if (board->piecesOnSquares[shift += 9] == PieceNameNone) d |= 1UL << shift; else { d |= 1UL << shift; break; }
		ii = sq->file; jj = sq->rank; shift = sq->name;
		while (ii-- > FileA && jj-- > Rank1)
			if (board->piecesOnSquares[shift -= 9] == PieceNameNone) d |= 1UL << shift; else { d |= 1UL << shift; break; }
	}
	//the same is for antidiagonal
	else if (pinnedBy->antiDiag == sq->antiDiag) {
		while (ii-- > FileA && jj++ < Rank8)
			if (board->piecesOnSquares[shift += 7] == PieceNameNone) d |= 1UL << shift; else { d |= 1UL << shift; break; }
		ii = sq->file; jj = sq->rank; shift = sq->name;
		while (ii++ < FileH && jj-- > Rank1)
			if (board->piecesOnSquares[shift -= 7] == PieceNameNone) d |= 1UL << shift; else { d |= 1UL << shift; break; }
	}
	return d;
}
unsigned long pinnedRookMoves(struct Board * board, struct Square * sq, struct Square * pinnedBy) {
	unsigned long d = 0;
	enum Files ii = sq->file;
	enum Ranks jj = sq->rank;
	enum SquareName shift = sq->name;
	//if the rook is pinned on a file, it can only move along it
	if (pinnedBy->file == sq->file) {
		while (jj++ < Rank8)
			if (board->piecesOnSquares[shift += 8] == PieceNameNone) d |= 1UL << shift; else { d |= 1UL << shift; break; }
		jj = sq->rank; shift = sq->name;
		while (jj-- > Rank1)
			if (board->piecesOnSquares[shift -= 8] == PieceNameNone) d |= 1UL << shift; else { d |= 1UL << shift; break; }
	}
	//the same applies to a rank
	else if (pinnedBy->rank == sq->rank) {
		while (ii++ < FileH)
			if (board->piecesOnSquares[shift += 1] == PieceNameNone) d |= 1UL << shift; else { d |= 1UL << shift; break; }
		ii = sq->file; shift = sq->name;
		while (ii-- > FileA)
			if (board->piecesOnSquares[shift -= 1] == PieceNameNone) d |= 1UL << shift; else { d |= 1UL << shift; break; }
	}
	return d;
}

void generateMoves(struct Board * board) {
	enum SquareName whiteBlack[] = { SquareA1, SquareA8 }; //first squares of the pieces' ranks
	unsigned long d = 0, defendedPieces = 0, blockingSquares = 0, checker = 0, checkers = 0, moves = 0, attackedSquares = 0;
	struct Square pinnedBy, attackerSquare, sq;
	square(&pinnedBy, SquareNone);
	square(&attackerSquare, SquareNone);
	square(&sq, SquareNone);
	memset(board->movesFromSquares, 0, sizeof board->movesFromSquares);
	unsigned short squarePinsSquare[8];
	memset(squarePinsSquare, 0, sizeof squarePinsSquare);
	unsigned char pinCounter = 0;
	enum Files ii;
	enum Ranks jj;
	enum SquareName shift;
	enum SquareName shiftedColor = board->fen->sideToMove << 3;
	enum SquareName shiftedOpponentColor = board->opponentColor << 3;
	enum SquareName pinnedPieceSquare = SquareNone;
	board->isCheck = false; board->isStaleMate = false; board->isMate = false;

	struct ChessPiece king;
	struct Square kingSquare;
	enum PieceName _king = shiftedColor | King;
	square(&kingSquare, lsBit(board->occupations[_king]));
	piece(&kingSquare, &king, _king);
	struct ChessPiece opponentKing;
	struct Square opponentKingSquare;
	enum PieceName _oking = shiftedOpponentColor | King;
	square(&opponentKingSquare, lsBit(board->occupations[_oking]));
	piece(&opponentKingSquare, &opponentKing, _oking);

	//for opponent color sliding piece moves we need to temporary remove the king, 
	//so the rays can light through it
	//this is necessary to mark the squares behind the king as attacked, 
	//so that the king under the check of opponent ray piece, cannot step back
	board->piecesOnSquares[king.square.name] = PieceNameNone;

	unsigned long opponentBishops = board->occupations[shiftedOpponentColor | Bishop];
	unsigned long opponentRooks = board->occupations[shiftedOpponentColor | Rook];
	unsigned long opponentQueens = board->occupations[shiftedOpponentColor | Queen];
	unsigned long opponentPawns = board->occupations[shiftedOpponentColor | Pawn];
	unsigned long opponentKnights = board->occupations[shiftedOpponentColor | Knight];
	unsigned long opponentAny = board->occupations[shiftedOpponentColor | PieceTypeAny];
	unsigned long bishops = board->occupations[shiftedColor | Bishop];
	unsigned long rooks = board->occupations[shiftedColor | Rook];
	unsigned long queens = board->occupations[shiftedColor | Queen];
	unsigned long pawns = board->occupations[shiftedColor | Pawn];
	unsigned long knights = board->occupations[shiftedColor | Knight];

	//find the squares attacked and defended by opponent bishops
	//and see if they pin anything against the king
	square(&sq, lsBit(opponentBishops));
	while (sq.name < SquareNone) {
		//generate opponent bishop moves limited by board boundaries and other pieces regardless of their color 
		board->movesFromSquares[sq.name] = generateBishopMoves(board, &sq);
		attackedSquares |= board->movesFromSquares[sq.name];
		//find pinned by this bishop pieces
		//we move from the board->fen->sideToMove king toward the opponent 
		//bishop if it's on the same diagonal (NE-SW) or antidiagonal (NW-SE)
		//we stop at the first non-empty square regardless of whether 
		//it is occupied by black or white
		d = bishopPinFinder(board, &sq, &(king.square));
		//if the bishop also attacks this square and the piece on this square 
		//has board->fen->sideToMove color, then it is pinned
		d &= board->movesFromSquares[sq.name];
		d &= board->occupations[shiftedColor | PieceTypeAny];
		//compact pinning and pinned squares in one unsigned short number, 
		//where upper 6 bits represent the pinning square 
		//and the lower 6 bits - pinned one and store this number in the array 
		//squarePinsSquare[8]
		if ((pinnedPieceSquare = lsBit(d)) < SquareNone)
			squarePinsSquare[pinCounter++] = (((unsigned short)sq.name << 6) | (unsigned short)pinnedPieceSquare);
		opponentBishops ^= sq.bitSquare;
		square(&sq, lsBit(opponentBishops));
	}
	//repeat the same process as described for opponent bishops for opponent rooks
	square(&sq, lsBit(opponentRooks));
	while (sq.name < SquareNone) {
		board->movesFromSquares[sq.name] = generateRookMoves(board, &sq);
		attackedSquares |= board->movesFromSquares[sq.name];
		d = rookPinFinder(board, &sq, &(king.square));
		d &= board->movesFromSquares[sq.name];
		d &= board->occupations[shiftedColor | PieceTypeAny];
		if ((pinnedPieceSquare = lsBit(d)) < SquareNone)
			squarePinsSquare[pinCounter++] = (((unsigned short)sq.name << 6) | (unsigned short)pinnedPieceSquare);
		opponentRooks ^= sq.bitSquare;
		square(&sq, lsBit(opponentRooks));
	}
	//repeat the same process as described for opponent bishops for opponent queens
	square(&sq, lsBit(opponentQueens));
	while (sq.name < SquareNone) {
		board->movesFromSquares[sq.name] = generateBishopMoves(board, &sq) | generateRookMoves(board, &sq);
		attackedSquares |= board->movesFromSquares[sq.name];
		d = bishopPinFinder(board, &sq, &(king.square)) | rookPinFinder(board, &sq, &(king.square));
		d &= board->movesFromSquares[sq.name];
		d &= board->occupations[shiftedColor | PieceTypeAny];
		if ((pinnedPieceSquare = lsBit(d)) < SquareNone)
			squarePinsSquare[pinCounter++] = (unsigned short)(((unsigned short)sq.name << 6) | (unsigned short)pinnedPieceSquare);
		opponentQueens ^= sq.bitSquare;
		square(&sq, lsBit(opponentQueens));
	}
	//we are done with opponent ray piece attacked squares, now we can restore the king
	board->piecesOnSquares[king.square.name] = (shiftedColor | King);

	//find the squares attacked and defended by opponent pawns
	char pawnCapturingMoves[2][2] = { { 7, 9 }, { -9, -7 } };
	square(&sq, lsBit(opponentPawns));
	while (sq.name < SquareNone) {
		d = 0;
		ii = sq.file; shift = sq.name;
		//opponent pawn attacking, capturing and protecting moves; another words, just diagonal and anti-diagonal moves
		if (ii > FileA)	{
			shift = sq.name + pawnCapturingMoves[board->opponentColor][0];
			d |= (1UL << shift);
		}
		if (ii < FileH) {
			shift = sq.name + pawnCapturingMoves[board->opponentColor][1];
			d |= (1UL << shift);
		}
		board->movesFromSquares[sq.name] = d;
		attackedSquares |= d;
		opponentPawns ^= sq.bitSquare;
		square(&sq, lsBit(opponentPawns));
	}
	//find the squares attacked and defended by opponent knights
	square(&sq, lsBit(opponentKnights));
	while (sq.name < SquareNone) {
		//generate opponent knight moves limited by board boudaries only
		unsigned long knight_moves = generateKnightMoves(&sq);
		d = knight_moves & board->occupations[shiftedOpponentColor | PieceTypeAny];
		d |= knight_moves & (~board->occupations[shiftedOpponentColor | PieceTypeAny]);
		board->movesFromSquares[sq.name] = d;
		attackedSquares |= d;
		opponentKnights ^= sq.bitSquare;
		square(&sq, lsBit(opponentKnights));
	}

	//generate opponent king moves limited by board boundaries only
	unsigned long opponentKingMoves = generateKingMoves(&(opponentKing.square));
	//opponent pieces protected by their king
	d = opponentKingMoves & board->occupations[shiftedOpponentColor | PieceTypeAny];
	//empty squares controlled and board->fen->sideToMove pieces attacked by opponnent king
	d |= opponentKingMoves & (~board->occupations[shiftedOpponentColor | PieceTypeAny]);
	board->movesFromSquares[opponentKing.square.name] = d;
	attackedSquares |= d;

	defendedPieces = attackedSquares & board->occupations[shiftedOpponentColor | PieceTypeAny];

	//checkers
	if ((attackedSquares & king.square.bitSquare) > 0) {
		square(&sq, lsBit(opponentAny));
		while (sq.name < SquareNone) {
			if ((board->movesFromSquares[sq.name] & king.square.bitSquare) > 0) checkers |= sq.bitSquare;
			opponentAny ^= sq.bitSquare;
			square(&sq, lsBit(opponentAny));
		}
	}
	
	//generate king moves limited by board boundaries only
	board->movesFromSquares[king.square.name] = generateKingMoves(&(king.square));
	//filter these moves to find the legal ones: 
	//the king cannot capture defended opponent's pieces
	board->movesFromSquares[king.square.name] ^= board->movesFromSquares[king.square.name] & defendedPieces;
	//it can't go to a square occupied by other pieces of its color
	board->movesFromSquares[king.square.name] ^= board->occupations[shiftedColor | PieceTypeAny] & board->movesFromSquares[king.square.name];
	//and it can't go to a square attacked by opponent piece(s)
	board->movesFromSquares[king.square.name] ^= attackedSquares & board->movesFromSquares[king.square.name];

	//to complete legal king moves include castling
	enum SquareName castlingKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
	enum PieceName _rook = shiftedColor | Rook;
	//short castling moves
	if ((((board->fen->castlingRights >> (board->fen->sideToMove << 1)) & 3) & CastlingSideKingside) > 0) {
		struct ChessPiece shortCastlingRook;
		struct Square shortCastlingRookSquare;
		square(&shortCastlingRookSquare, board->fen->castlingRook[0][board->fen->sideToMove] + whiteBlack[board->fen->sideToMove]);
		piece(&shortCastlingRookSquare, &shortCastlingRook, _rook);
		//empty squares between the king including king's square and its destination 
		//(for short castling: g1 or g8) should not be under attack
		unsigned long shortKingSquares = (((1UL << king.square.file) - 1) ^ 127) << whiteBlack[board->fen->sideToMove];
		unsigned long shortRookSquares = (((1UL << board->fen->castlingRook[0][board->fen->sideToMove]) - 1) ^ 31) << whiteBlack[board->fen->sideToMove];
		unsigned long occupations = board->occupations[PieceNameAny];
		//shortKingSquares--;
		//shortKingSquares ^= 127;
		//shortKingSquares <<= whiteBlack[board->fen->sideToMove];
		//squares between the rook and its destination (for short castling f1 or f8) should be vacant (except the king and short castling rook for chess 960)
		//shortRookSquares--;
		//shortRookSquares ^= 31;
		//shortRookSquares <<= whiteBlack[board->fen->sideToMove];
		occupations ^= (king.square.bitSquare | shortCastlingRook.square.bitSquare);
		if ((shortKingSquares & attackedSquares) == 0 && (shortKingSquares & occupations) == 0 && (shortRookSquares & occupations) == 0) {
			if (board->fen->isChess960) {
				board->movesFromSquares[king.square.name] |= shortCastlingRook.square.bitSquare;
			}
			else {
				board->movesFromSquares[king.square.name] |= (1UL << castlingKingSquare[0][board->fen->sideToMove]);
			}
		}
	}
	//long castling moves
	if ((((board->fen->castlingRights >> (board->fen->sideToMove << 1)) & 3) & CastlingSideQueenside) > 0) {
		struct ChessPiece longCastlingRook;
		struct Square longCastlingRookSquare;
		square(&longCastlingRookSquare, board->fen->castlingRook[1][board->fen->sideToMove] + whiteBlack[board->fen->sideToMove]);
		piece(&longCastlingRookSquare, &longCastlingRook, _rook);
		//empty squares between the king including king's square and its destination (for long castling: c1 or c8) should not be under attack
		unsigned long longKingSquares = (((1UL << (king.square.file + 1)) - 1) ^ 3) << whiteBlack[board->fen->sideToMove];
		unsigned long longRookSquares = (((1UL << (board->fen->castlingRook[1][board->fen->sideToMove] + 1)) - 1) ^ 15) << whiteBlack[board->fen->sideToMove];
		unsigned long occupations = board->occupations[PieceNameAny];
		//longKingSquares--;
		//longKingSquares ^= 3;
		//longKingSquares <<= whiteBlack[board->fen->sideToMove];
		//squares between the rook and its destination (for long castling d1 or d8) should be vacant (except the king and long castling rook for chess 960)
		//longRookSquares--;
		//longRookSquares ^= 15;
		//longRookSquares <<= whiteBlack[board->fen->sideToMove];
		occupations ^= (king.square.bitSquare | longCastlingRook.square.bitSquare);
		if ((longKingSquares & attackedSquares) == 0 && (longKingSquares & occupations) == 0 && (longRookSquares & occupations) == 0) {
			if (board->fen->isChess960) {
				board->movesFromSquares[king.square.name] |= longCastlingRook.square.bitSquare;
			}
			else {
				board->movesFromSquares[king.square.name] |= (1UL << castlingKingSquare[1][board->fen->sideToMove]);
			}
		}
	}
	moves |= board->movesFromSquares[king.square.name];

	//is king checked?
	if (board->isCheck = board->occupations[shiftedColor | King] & attackedSquares) {
		//if double check, no other moves rather than the king's move are possible
		unsigned char bits = bitCount(checkers);
		if (bits > 1) goto exit;
		//normal check by checker
		else if (bits == 1) {
			square(&attackerSquare, lsBit(checkers));
			checker = checkers;
			//if not checked by knight or pawn, calculate blocking squares
			if ((board->piecesOnSquares[attackerSquare.name] != (shiftedOpponentColor | Knight)) && board->piecesOnSquares[attackerSquare.name] != (shiftedOpponentColor | Pawn)) {
				d = 0;
				ii = attackerSquare.file; jj = attackerSquare.rank; shift = attackerSquare.name;
				if (king.square.diag == attackerSquare.diag) {
					if (king.square.name > attackerSquare.name) {
						while (ii++ < FileH && jj++ < Rank8)
							if (board->piecesOnSquares[shift += 9] == PieceNameNone) d |= 1UL << shift; else break;
					}
					else {
						while (ii-- > FileA && jj-- > Rank1)
							if (board->piecesOnSquares[shift -= 9] == PieceNameNone) d |= 1UL << shift; else break;
					}
				}
				else if (king.square.antiDiag == attackerSquare.antiDiag) {
					if (king.square.name > attackerSquare.name) {
						while (ii-- > FileA && jj++ < Rank8)
							if (board->piecesOnSquares[shift += 7] == PieceNameNone) d |= 1UL << shift; else break;
					}
					else {
						while (ii++ < FileH && jj-- > Rank1)
							if (board->piecesOnSquares[shift -= 7] == PieceNameNone) d |= 1UL << shift; else break;
					}
				}
				else if (king.square.file == attackerSquare.file) {
					if (king.square.name > attackerSquare.name) {
						while (jj++ < Rank8)
							if (board->piecesOnSquares[shift += 8] == PieceNameNone) d |= 1UL << shift; else break;
					}
					else {
						while (jj-- > Rank1)
							if (board->piecesOnSquares[shift -= 8] == PieceNameNone) d |= 1UL << shift; else break;
					}
				}
				else if (king.square.rank == attackerSquare.rank) {
					if (king.square.name > attackerSquare.name) {
						while (ii++ < FileH)
							if (board->piecesOnSquares[shift += 1] == PieceNameNone) d |= 1UL << shift; else { d |= 1UL << shift; break; }
					}
					else {
						while (ii-- > FileA)
							if (board->piecesOnSquares[shift -= 1] == PieceNameNone) d |= 1UL << shift; else { d |= 1UL << shift; break; }
					}
				}
				blockingSquares = d;
			}
		}
	}

	//legal other moves

	//bishop moves
	square(&sq, lsBit(bishops));
	while (sq.name < SquareNone) {
		square(&pinnedBy, SquareNone);
		for (unsigned char c = 0; c < pinCounter; c++) {
			if ((squarePinsSquare[c] & 63) == sq.name) {
				square(&pinnedBy, (enum SquareName)(squarePinsSquare[c] >> 6));
				break;
			}
		}
		if (pinnedBy.name < SquareNone) {
			//if there is check, then the bishop can't help since it can't move to block or capture the checker
			if (board->isCheck || (pinnedBy.file == sq.file) || (pinnedBy.rank == sq.rank)) {
				bishops ^= sq.bitSquare;
				square(&sq, lsBit(bishops));
				continue;
			}
			//if pinned on a file or rank, then the bishop cannot move
			d = pinnedBishopMoves(board, &sq, &pinnedBy);
		}
		//generate bishop moves from square <cp> limited by board boundaries and other chess pieces regardless of their color
		else d = generateBishopMoves(board, &sq);

		//white bishop legal moves, which exclude moves to the squares occupied by pieces with the same color
		board->movesFromSquares[sq.name] = d ^ (d & board->occupations[shiftedColor | PieceTypeAny]);
		//if the white king is in check, the legal moves are limited: we can either capture the  checker or block it
		if (blockingSquares > 0) {
			board->movesFromSquares[sq.name] &= (blockingSquares | checker);
		}
		//if we can't block it, then we can only try to capture it
		else if (checker > 0) {
			board->movesFromSquares[sq.name] &= checker;
		}
		moves |= board->movesFromSquares[sq.name];
		bishops ^= sq.bitSquare;
		square(&sq, lsBit(bishops));
	}

	//rook moves
	square(&sq, lsBit(rooks));
	while (sq.name < SquareNone) {
		square(&pinnedBy, SquareNone);
		for (unsigned char c = 0; c < pinCounter; c++) {
			if ((squarePinsSquare[c] & 63) == sq.name) {
				square(&pinnedBy, (enum SquareName)(squarePinsSquare[c] >> 6));
				break;
			}
		}
		if (pinnedBy.name < SquareNone) {
			//if check or the rook is pinned on a diagonal or anti-diagonal, then it cannot move
			if (board->isCheck || (pinnedBy.diag == sq.diag) || (pinnedBy.antiDiag == sq.antiDiag)) {
				rooks ^= sq.bitSquare;
				square(&sq, lsBit(rooks));
				continue;
			}
			d = pinnedRookMoves(board, &sq, &pinnedBy);
		}
		else d = generateRookMoves(board, &sq);
		board->movesFromSquares[sq.name] = d ^ (d & board->occupations[shiftedColor | PieceTypeAny]);
		if (blockingSquares > 0) board->movesFromSquares[sq.name] &= (blockingSquares | checker);
		else if (checker > 0) board->movesFromSquares[sq.name] &= checker;
		moves |= board->movesFromSquares[sq.name];		
		rooks ^= sq.bitSquare;
		square(&sq, lsBit(rooks));
	}

	//queen moves
	square(&sq, lsBit(queens));
	while (sq.name < SquareNone) {
		square(&pinnedBy, SquareNone);
		for (unsigned char c = 0; c < pinCounter; c++) {
			if ((squarePinsSquare[c] & 63) == sq.name) {
				square(&pinnedBy, (enum SquareName)(squarePinsSquare[c] >> 6));
				break;
			}
		}
		if (pinnedBy.name < SquareNone) {
			if (board->isCheck) {
				queens ^= sq.bitSquare;
				square(&sq, lsBit(queens));
				continue;
			}
			//if pinned on a diagonal or anti-diagonal, then the queen can move as a bishop along this diagonal or anti-diagonal
			if ((pinnedBy.diag == sq.diag) || (pinnedBy.antiDiag == sq.antiDiag))
				d = pinnedBishopMoves(board, &sq, &pinnedBy);
			//if pinned on a file or rank, then the queen can only move like a rook along this file or rank
			else d = pinnedRookMoves(board, &sq, &pinnedBy);
		} else {
			d = generateBishopMoves(board, &sq);
			d |= generateRookMoves(board, &sq);
		}
		board->movesFromSquares[sq.name] = d ^ (d & board->occupations[shiftedColor | PieceTypeAny]);
		if (blockingSquares > 0) board->movesFromSquares[sq.name] &= (blockingSquares | checker);
		else if (checker > 0) board->movesFromSquares[sq.name] &= checker;
		moves |= board->movesFromSquares[sq.name];
		queens ^= sq.bitSquare;
		square(&sq, lsBit(queens));
	}
	
	//pawn moves
	signed char pawnShifts[3][3] = { { 8, 7, 9 }, { -8, -9, -7 } };
	enum Ranks pawnRanks[3][3] = { { Rank2, Rank5, Rank6 }, { Rank7, Rank4, Rank3 } };
	square(&sq, lsBit(pawns));
	while (sq.name < SquareNone) {
		d = 0;
		ii = sq.file; jj = sq.rank; shift = sq.name;
		square(&pinnedBy, SquareNone);
		for (unsigned char c = 0; c < pinCounter; c++) {
			if ((squarePinsSquare[c] & 63) == sq.name) {
				square(&pinnedBy, (enum SquareName)(squarePinsSquare[c] >> 6));
				break;
			}
		}
		if (pinnedBy.name < SquareNone) {
			if (board->isCheck) goto next;
			if (pinnedBy.diag == sq.diag) {
				switch (board->fen->sideToMove) {
				case ColorWhite:
					if (ii < FileH) {
						shift = sq.name + 9;
						if (board->piecesOnSquares[shift] != PieceNameNone) {
							if (board->piecesOnSquares[shift] >> 3 == board->opponentColor)
								d |= 1UL << shift;
						}
						else if (board->fen->enPassant == sq.file + 1 && sq.rank == Rank5)
							d |= 1UL << shift;
					}
					break;
				case ColorBlack:
					if (ii > FileA) {
						shift = sq.name - 9;
						if (board->piecesOnSquares[shift] != PieceNameNone) {
							if (board->piecesOnSquares[shift] >> 3 == board->opponentColor)
								d |= 1UL << shift;
						}
						else if (board->fen->enPassant == sq.file - 1 && sq.rank == Rank4)
							d |= 1UL << shift;
					}
					break;
				}
			}
			else if (pinnedBy.antiDiag == sq.antiDiag) {
				switch (board->fen->sideToMove) {
				case ColorWhite:
					if (ii > FileA) {
						shift = sq.name + 7;
						if (board->piecesOnSquares[shift] != PieceNameNone) {
							if (board->piecesOnSquares[shift] >> 3 == board->opponentColor)
								d |= 1UL << shift;
						}
						else if (board->fen->enPassant == sq.file - 1 && sq.rank == Rank5)
							d |= 1UL << shift;
					}
					break;
				case ColorBlack:
					if (ii < FileH) {
						shift = sq.name - 7;
						if (board->piecesOnSquares[shift] != PieceNameNone) {
							if (board->piecesOnSquares[shift] >> 3 == board->opponentColor)
								d |= 1UL << shift;
						}
						else if (board->fen->enPassant == sq.file + 1 && sq.rank == Rank4)
							d |= 1UL << shift;
					}
					break;
				}
			}
			else if (pinnedBy.file == sq.file) {
				shift = sq.name + pawnShifts[board->fen->sideToMove][0];
				if (board->piecesOnSquares[shift] == PieceNameNone) {
					d |= 1UL << shift;
					if (jj == pawnRanks[board->fen->sideToMove][0]) {
						shift = (shift + pawnShifts[board->fen->sideToMove][0]);
						if (board->piecesOnSquares[shift] == PieceNameNone) d |= 1UL << shift;
					}
				}
			}
			else if (pinnedBy.rank == sq.rank) goto next;
			board->movesFromSquares[sq.name] = d;
		}
		else {
			//normal pawn moves (non-capturing)
			shift = sq.name + pawnShifts[board->fen->sideToMove][0];
			if (board->piecesOnSquares[shift] == PieceNameNone) {
				d |= 1UL << shift;
				//double advance from rank 2
				if (jj == pawnRanks[board->fen->sideToMove][0]) {
					shift += pawnShifts[board->fen->sideToMove][0];
					if (board->piecesOnSquares[shift] == PieceNameNone) d |= 1UL << shift;
				}
			}
			//capturing pawn moves
			if (ii > FileA) {
				shift = sq.name + pawnShifts[board->fen->sideToMove][1];
				if (board->piecesOnSquares[shift] != PieceNameNone) {
					if (board->piecesOnSquares[shift] >> 3 == board->opponentColor) 
						d |= 1UL << shift;
				}
				else if (board->fen->enPassant == sq.file - 1 && sq.rank == pawnRanks[board->fen->sideToMove][1]) {
					//make sure there is no discover check from a queen or a rook
					board->piecesOnSquares[sq.name] = PieceNameNone;
					board->piecesOnSquares[sq.name - 1] = PieceNameNone;
					if (isEnPassantLegal(board)) d |= 1UL << shift;
					board->piecesOnSquares[sq.name] = shiftedColor | Pawn; 
					board->piecesOnSquares[sq.name - 1] = shiftedOpponentColor | Pawn;
				}
			}
			if (ii < FileH) {
				shift = sq.name + pawnShifts[board->fen->sideToMove][2];
				if (board->piecesOnSquares[shift] != PieceNameNone) {
					if (board->piecesOnSquares[shift] >> 3 == board->opponentColor) 
						d |= 1UL << shift;
				}
				else if (board->fen->enPassant == sq.file + 1 && sq.rank == pawnRanks[board->fen->sideToMove][1]) {
					//make sure there is no discover check from a queen or a rook
					board->piecesOnSquares[sq.name] = PieceNameNone; 
					board->piecesOnSquares[sq.name + 1] = PieceNameNone;
					if (isEnPassantLegal(board)) d |= 1UL << shift;
					board->piecesOnSquares[sq.name] = shiftedColor | Pawn;
					board->piecesOnSquares[sq.name + 1] = shiftedOpponentColor | Pawn;
				}
			}
			board->movesFromSquares[sq.name] = d;
			if (blockingSquares > 0) 
				board->movesFromSquares[sq.name] &= (blockingSquares | checker);
			else if (checker > 0) {
				//if checker is en passant pawn that can be captured
				if (board->fen->enPassant == attackerSquare.file && attackerSquare.rank == pawnRanks[board->fen->sideToMove][1] && sq.rank == pawnRanks[board->fen->sideToMove][1] && ((sq.file == board->fen->enPassant - 1) || (sq.file == board->fen->enPassant + 1)))
					board->movesFromSquares[sq.name] &= 1UL << ((pawnRanks[board->fen->sideToMove][2] << 3) + board->fen->enPassant);
				else board->movesFromSquares[sq.name] &= checker;
			}
		}
		moves |= board->movesFromSquares[sq.name];
next:
		pawns ^= sq.bitSquare;
		square(&sq, lsBit(pawns));
	}

	//knight moves
	square(&sq, lsBit(knights));
	while (sq.name < SquareNone) {
		square(&pinnedBy, SquareNone);
		for (unsigned char c = 0; c < pinCounter; c++) {
			if ((squarePinsSquare[c] & 63) == sq.name) {
				square(&pinnedBy, (enum SquareName)(squarePinsSquare[c] >> 6));
				break;
			}
		}
		if (pinnedBy.name == SquareNone) {
			d = generateKnightMoves(&sq);
			board->movesFromSquares[sq.name] = d ^ (d & board->occupations[shiftedColor | PieceTypeAny]);
			if (blockingSquares > 0) board->movesFromSquares[sq.name] &= (blockingSquares | checker);
			else if (checker > 0) board->movesFromSquares[sq.name] &= checker;
			moves |= board->movesFromSquares[sq.name];
		}
		knights ^= sq.bitSquare;
		square(&sq, lsBit(knights));
	}

exit:
	if (moves == 0) {
		if (board->isCheck) {
			board->isMate = true;
			board->isCheck = false;
		}
		else board->isStaleMate = true;
	}
}

void makeMove(struct Move * move) {
	//null move
	if ((move->type & MoveTypeNull) && (move->type & MoveTypeValid)) {
		move->chessBoard->fen->sideToMove = move->chessBoard->fen->sideToMove == ColorWhite ? ColorBlack : ColorWhite;
		if (move->chessBoard->fen->sideToMove == ColorWhite) {
			move->chessBoard->fen->moveNumber++;
			move->chessBoard->opponentColor = ColorBlack;
		} else move->chessBoard->opponentColor = ColorWhite;
		generateMoves(move->chessBoard);
		goto exit;
	}
	
	//remove the piece from its source square
	move->chessBoard->occupations[move->chessBoard->movingPiece.name] ^= move->chessBoard->movingPiece.square.bitSquare;
	move->chessBoard->piecesOnSquares[move->chessBoard->movingPiece.square.name] = PieceTypeNone;
	
	//rook moves
	if (move->chessBoard->movingPiece.type == Rook) {
		//need to find out if this is a castling rook to update FEN castling rights and rooks
		enum Ranks rookRank[2] = { Rank1, Rank8 };
		enum CastlingSide castlingSide = CastlingSideNone;
		if (move->chessBoard->movingPiece.square.rank == rookRank[move->chessBoard->fen->sideToMove]) {
			if (move->chessBoard->movingPiece.square.file == move->chessBoard->fen->castlingRook[0][move->chessBoard->fen->sideToMove]) //short castling
				castlingSide = CastlingSideKingside;
			else if (move->chessBoard->movingPiece.square.file == move->chessBoard->fen->castlingRook[1][move->chessBoard->fen->sideToMove]) //long castling
				castlingSide = CastlingSideQueenside;
			if (castlingSide != CastlingSideNone) {
				//revoke castling rights
				move->chessBoard->fen->castlingRights ^= castlingSide << ((move->chessBoard->fen->sideToMove) << 1);
				//set castling rook to none
				move->chessBoard->fen->castlingRook[castlingSide - 1][move->chessBoard->fen->sideToMove] = FileNone;
			}
		}
	}

	//capture
	if (move->type & MoveTypeCapture) {
		if (move->type & MoveTypeEnPassant) {
			struct Square capturedPawnSquare;
			signed char offset[2] = { -8, 8 };
			square(&capturedPawnSquare, move->destinationSquare.name + offset[move->chessBoard->fen->sideToMove]);
			enum PieceName capturedPiece = (move->chessBoard->opponentColor << 3) | Pawn;
			move->chessBoard->occupations[capturedPiece] ^= capturedPawnSquare.bitSquare;
			move->chessBoard->piecesOnSquares[capturedPawnSquare.name] = PieceTypeNone;
		} else {
			enum PieceName capturedPiece = move->chessBoard->piecesOnSquares[move->destinationSquare.name];
			move->chessBoard->occupations[move->chessBoard->piecesOnSquares[move->destinationSquare.name]] ^= move->destinationSquare.bitSquare;
			//if captured piece is a castling rook, then remove the castling rights of the opponent on that side
			if ((capturedPiece & 7) == Rook) {
				unsigned char whiteBlack[2] = { 0, 56 };
				if (move->destinationSquare.name == move->chessBoard->fen->castlingRook[0][move->chessBoard->opponentColor] + whiteBlack[move->chessBoard->opponentColor]) {
					move->chessBoard->fen->castlingRights &= CastlingRightsWhiteBothBlackBoth ^ (CastlingSideKingside << (move->chessBoard->opponentColor << 1));
					move->chessBoard->fen->castlingRook[0][move->chessBoard->opponentColor] = FileNone;
				}
				else if (move->destinationSquare.name == move->chessBoard->fen->castlingRook[1][move->chessBoard->opponentColor] + whiteBlack[move->chessBoard->opponentColor]) {
					move->chessBoard->fen->castlingRights &= CastlingRightsWhiteBothBlackBoth ^ (CastlingSideQueenside << (move->chessBoard->opponentColor << 1));
					move->chessBoard->fen->castlingRook[1][move->chessBoard->opponentColor] = FileNone;
				}
			}
		}
	}

	//castling
	enum SquareName dstKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
	unsigned char castlingSide = 0;
	if (move->chessBoard->movingPiece.type == King) {
		if ((move->type & MoveTypeCastlingQueenside) || (move->type & MoveTypeCastlingKingside)) {
			castlingSide = (((move->type & (MoveTypeCastlingKingside | MoveTypeCastlingQueenside)) - 1) >> 2);
			unsigned char rookOffset[2] = { 0, 56 };
			enum SquareName dstRookSquare[2][2] = { { SquareF1, SquareF8 }, { SquareD1, SquareD8 } };
			enum SquareName srcRookSquare = move->chessBoard->fen->castlingRook[castlingSide][move->chessBoard->fen->sideToMove] + rookOffset[move->chessBoard->fen->sideToMove];
			enum PieceName rookName = (move->chessBoard->fen->sideToMove << 3) | Rook;
			//remove castling rook from its source square taking care of
			//Chess960 case of rook occupying king's destination square - make sure we are not removing (overwriting) the king with PieceNone!
			if (srcRookSquare != dstKingSquare[castlingSide][move->chessBoard->fen->sideToMove]) {
				move->chessBoard->piecesOnSquares[srcRookSquare] = PieceNameNone;
			}
			//and put it to its destination square
			move->chessBoard->piecesOnSquares[dstRookSquare[castlingSide][move->chessBoard->fen->sideToMove]] = rookName;
			//update occupations
			//xor out the rook on its source square
			move->chessBoard->occupations[rookName] ^= (1UL << srcRookSquare);
			//move->chessBoard->occupations[PieceNameNone] |= (1UL << srcRookSquare);
			//add the rook on its destination square
			move->chessBoard->occupations[rookName] |= (1UL << dstRookSquare[castlingSide][move->chessBoard->fen->sideToMove]);
			//move->chessBoard->occupations[PieceNameNone] ^= (1UL << dstRookSquare[castlingSide][move->chessBoard->fen->sideToMove]);
		}
		//update FEN castling rights and rooks
		move->chessBoard->fen->castlingRights &= CastlingSideBoth << (move->chessBoard->opponentColor << 1);
		move->chessBoard->fen->castlingRook[0][move->chessBoard->fen->sideToMove] = FileNone;
		move->chessBoard->fen->castlingRook[1][move->chessBoard->fen->sideToMove] = FileNone;
	}
	
	//promotion
	if (move->type & MoveTypePromotion) {
		move->chessBoard->piecesOnSquares[move->destinationSquare.name] = move->chessBoard->promoPiece;
		move->chessBoard->occupations[move->chessBoard->promoPiece] |= move->destinationSquare.bitSquare;
	} 
	
	//other move
	else {
		//move the piece to its destination
		//special case of chess 960 castling
		if (move->chessBoard->fen->isChess960 && ((move->type & MoveTypeCastlingQueenside) || (move->type & MoveTypeCastlingKingside))) {
			square(&(move->destinationSquare), dstKingSquare[castlingSide][move->chessBoard->fen->sideToMove]);
		}
		square(&(move->chessBoard->movingPiece.square), move->destinationSquare.name);
		move->chessBoard->piecesOnSquares[move->destinationSquare.name] = move->chessBoard->movingPiece.name;
		move->chessBoard->occupations[move->chessBoard->movingPiece.name] |= move->destinationSquare.bitSquare;
	}
	
	//update occupations for all pieces
	move->chessBoard->occupations[PieceNameWhite] = move->chessBoard->occupations[WhiteBishop] | move->chessBoard->occupations[WhiteKing] | move->chessBoard->occupations[WhiteKnight] | move->chessBoard->occupations[WhitePawn] | move->chessBoard->occupations[WhiteQueen] | move->chessBoard->occupations[WhiteRook];
	move->chessBoard->occupations[PieceNameBlack] = move->chessBoard->occupations[BlackBishop] | move->chessBoard->occupations[BlackKing] | move->chessBoard->occupations[BlackKnight] | move->chessBoard->occupations[BlackPawn] | move->chessBoard->occupations[BlackQueen] | move->chessBoard->occupations[BlackRook];
	move->chessBoard->occupations[PieceNameAny] = move->chessBoard->occupations[PieceNameWhite] | move->chessBoard->occupations[PieceNameBlack];
	move->chessBoard->occupations[PieceNameNone] = ~move->chessBoard->occupations[PieceNameAny];
	
	//set FEN en passant file if any
	if ((move->type & MoveTypeEnPassant) && !(move->type & MoveTypeCapture))
		move->chessBoard->fen->enPassant = move->chessBoard->movingPiece.square.file;
	else move->chessBoard->fen->enPassant = FileNone;
	
	//increment halfmove clock if not a pawn's move and not a capture
	if (move->chessBoard->movingPiece.type != Pawn && !(move->type & MoveTypeCapture)) 
		move->chessBoard->fen->halfmoveClock++;
	else move->chessBoard->fen->halfmoveClock = 0;
	
	//increment move number if it was black's move
	if (move->chessBoard->fen->sideToMove == ColorBlack) 
		move->chessBoard->fen->moveNumber++;
	
	//toggle FEN SideToMove and opponentColor
	if (move->chessBoard->fen->sideToMove == ColorWhite) {
		move->chessBoard->fen->sideToMove = ColorBlack;
		move->chessBoard->opponentColor = ColorWhite;
	} else {
		move->chessBoard->fen->sideToMove = ColorWhite;
		move->chessBoard->opponentColor = ColorBlack;
	}
	
	//update FEN
	updateFen(move->chessBoard);
	
	//generate opponent's moves
	generateMoves(move->chessBoard);
exit:
	if (move->chessBoard->isCheck) {
		if (!(strrchr(move->sanMove, '+'))) strcat(move->sanMove, "+");
	}
	else if (move->chessBoard->isMate) 
		if (!(strrchr(move->sanMove, '#'))) strcat(move->sanMove, "#");
}

int fentoboard(struct Fen * fen, struct Board * board) {
	memset(board, 0, sizeof(struct Board));
	for (unsigned char i = Rank1; i <= Rank8; i++) {
		unsigned char j = 0;
		for (unsigned char c = FileA; c <= FileH; c++) {
			char symbols[] = ".PNBRQK..pnbrqk";
			unsigned char row = i << 3, idx = row | j, t;
			if (fen->ranks[i][c] == '\0') break;
			char * found = strchr(symbols, fen->ranks[i][c]);
			if (found) {
				char s = found - symbols;
				if (s > 0 && s < 15) {
					board->occupations[s] |= 1UL << idx;
					board->piecesOnSquares[idx] = (enum PieceName)s;
				}
			} else if (isdigit(fen->ranks[i][c])) {
				for (unsigned char k = j; k < j + fen->ranks[i][c] - '0'; k++) {
					t = row | k;
					board->piecesOnSquares[t] = PieceNameNone;
					board->occupations[PieceNameNone] |= 1UL << t;
				}
				j += (fen->ranks[i][c] - '1');
			} else {
				printf("Invalid character is found in FEN %s string: %c\n", fen->fenString, fen->ranks[i][c]);
				return 1;
			}
			j++;
		}
	}
	board->occupations[PieceNameWhite] = board->occupations[WhiteBishop] | board->occupations[WhiteKing] | board->occupations[WhiteKnight] | board->occupations[WhitePawn] | board->occupations[WhiteQueen] | board->occupations[WhiteRook];
	board->occupations[PieceNameBlack] = board->occupations[BlackBishop] | board->occupations[BlackKing] | board->occupations[BlackKnight] | board->occupations[BlackPawn] | board->occupations[BlackQueen] | board->occupations[BlackRook];
	board->occupations[PieceNameAny] = board->occupations[PieceNameWhite] | board->occupations[PieceNameBlack];
	board->occupations[PieceNameNone] = ~board->occupations[PieceNameAny];
	board->fen = fen;
	board->opponentColor = fen->sideToMove == ColorWhite ? ColorBlack : ColorWhite;
	board->plyNumber = fen->sideToMove == ColorWhite ? (fen->moveNumber << 1) - 1 : fen->moveNumber << 1;
	
	generateMoves(board);

	return 0;
}
