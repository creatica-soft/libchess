#pragma warning(disable:4334)
#pragma warning(disable:4996)
#pragma warning(disable:4244)

#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "magic_bitboards.h"
#include "libchess.h"

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

enum Color squareColor(enum SquareName sqName) {
  if ((sqName & 7) % 2) {
		if ((sqName >> 3) % 2) return ColorBlack;
		else return ColorWhite;
	} 
	else {
		if ((sqName >> 3) % 2) return ColorWhite;
		else return ColorBlack;
	}
}

int randomNumber(const int min, const int max) {
    const int range = max - min + 1;
    const unsigned int max_random = 0xFFFFFFFFU; // Maximum value of random()
    const unsigned int threshold = (max_random / range) * range;
    unsigned int num;
    do {
        num = arc4random_uniform(max + 1);
    } while (num >= threshold); // Reject numbers causing bias
    return (num % range) + min; // Adjust to desired range
}

void generateEndGame(enum PieceName * pn, int numberOfPieces, enum Color sideToMove, enum CastlingRightsEnum castlingRights, enum Files enPassant, struct Board * board) {
  srandom(time(NULL));
  int bottomRandNumber, topRandNumber;
  enum SquareName sn;
  struct Fen * f;
  int roundNumber = 0;
	do {
	  do {
	  	//printf("round %d\n", roundNumber++);
	  	f = board->fen;
		  memset(board, 0, sizeof(struct Board));
		  board->fen = f;
	  	memset(board->fen, 0, sizeof(struct Fen));
		  board->occupations[PieceNameNone] = 0xffffffffffffffffUL;
		  //printf("occupations[PieceNameNone] %lx\n", board->occupations[PieceNameNone]);
		  for (int i = 0; i < numberOfPieces; i++) {
		  	enum SquareName kingSquare = SquareNone;
		  	enum PieceName opponentKing = PieceNameNone;
		  	bottomRandNumber = 0;
		  	topRandNumber = bitCount(board->occupations[PieceNameNone]) - 1;
		  	switch (pn[i]) {
		  		case WhitePawn:
		  		case BlackPawn:
		  			bottomRandNumber = 8;
		  		  topRandNumber -= 8;
		  		break;
		  		case WhiteKing:
		  			topRandNumber -= 32;
		  		break;
		  		case BlackKing:
		  			bottomRandNumber = 32;
		  		break;
		  		default: break;
		  	}
		  	sn = (enum SquareName)(randomNumber(bottomRandNumber, topRandNumber));
		  	//printf("bottomRandNum %d, topRandNum %d, sn %s pn[%d] %s\n", bottomRandNumber, topRandNumber, squareName[sn], i, pieceName[pn[i]]);
		  	enum Color color = squareColor(sn);
			  switch(pn[i]) {
		  		case WhiteBishop:
		  		case BlackBishop:
		  		  if (bitCount(board->occupations[pn[i]]) == 1) {
		  		  	//place the second bishop on the square of diff color
		  		  	if (squareColor(lsBit(board->occupations[pn[i]])) == color) {
 			  		  	do {
			  		  	  if (sn < 63) sn++;
			  		  	  else sn = squareColor(sn) == ColorWhite ? 0 : 1;
		  		  	  } while(board->piecesOnSquares[sn] != PieceNameNone || squareColor(lsBit(board->occupations[pn[i]])) == squareColor(sn));
		  		  	}
		  		  } else {
		  		  	while(board->piecesOnSquares[sn] != PieceNameNone) {
			  		  	if (sn < 63) sn++;
			  		  	else sn = 0;		  		  		
		  		  	}
		  		  }
		  		break;
		  		case WhiteKing:
		  		case BlackKing:
		  			opponentKing = ((pn[i] >> 3) == ColorWhite) ? ((ColorBlack << 3) | King) : ((ColorWhite << 3) | King);
		  		  if (bitCount(board->occupations[opponentKing]) == 1) {
		  		  	kingSquare = lsBit(board->occupations[opponentKing]);
		  		  	while(board->piecesOnSquares[sn] != PieceNameNone) {
			  		  	int diff = kingSquare - sn;
			  		  	switch (diff) {
			  		  		case -9:
			  		  		case -1:
			  		  		case 7:
				  		  		if (sn < 61) sn += 3;
				  		  		else sn = 0;
			  		  		break;
			  		  		case -8:
			  		  		case 8:
				  		  		if (sn < 62) sn += 2;
				  		  		else sn = 0;
			  		  		break;
			  		  		case -7:
			  		  		case 1:
			  		  		case 9:
				  		  		if (sn < 63) sn += 1;
				  		  		else sn = 0;
			  		  		break;
			  		  		default:
				  		  		if (sn < 63) sn += 1;
				  		  		else sn = 0;			  		  			
			  		  		break;
			  		  	}
		  		  	}
	   		  	}
	   		  break;
	  			case WhitePawn:
	  			case BlackPawn:
	  				while (board->piecesOnSquares[sn] != PieceNameNone) {
			  			if (sn < 56) sn++;
			  			else sn = bottomRandNumber;
		  			} 
		  		break;
		  		default:
	  				while (board->piecesOnSquares[sn] != PieceNameNone) {
  			  		if (sn < 63) sn++;
	  		  		else sn = 0;
	  		  	}
		  		break;
				}
		  	//printf("squareName %s\n", squareName[sn]);
		  	board->piecesOnSquares[sn] = pn[i];
		  	board->occupations[pn[i]] |= (1UL << sn);
		  	board->occupations[PieceNameNone] ^= (1UL << sn);
		  	//printf("occupations[PieceNameNone] %lx\n", board->occupations[PieceNameNone]);
		  }
		  //reconcile(board);
		  board->fen->moveNumber = 40;
		  board->plyNumber = sideToMove == ColorWhite ? board->fen->moveNumber * 2 - 2 : board->fen->moveNumber * 2 - 1;
		  board->opponentColor = sideToMove == ColorWhite ? ColorBlack : ColorWhite;
		  board->fen->castlingRights = castlingRights;
		  board->fen->enPassant = enPassant;
		  board->fen->sideToMove = sideToMove;
		  board->fen->halfmoveClock = 0;
		  updateFen(board);
		  generateMoves(board);
	  } while(board->isCheck || board->isStaleMate || board->isMate);
		board->opponentColor = sideToMove;
		board->fen->sideToMove = sideToMove == ColorWhite ? ColorBlack : ColorWhite;
		
		board->occupations[PieceNameWhite] = board->occupations[WhiteBishop] | board->occupations[WhiteKing] | board->occupations[WhiteKnight] | board->occupations[WhitePawn] | board->occupations[WhiteQueen] | board->occupations[WhiteRook];
		board->occupations[PieceNameBlack] = board->occupations[BlackBishop] | board->occupations[BlackKing] | board->occupations[BlackKnight] | board->occupations[BlackPawn] | board->occupations[BlackQueen] | board->occupations[BlackRook];
		board->occupations[PieceNameAny] = board->occupations[PieceNameWhite] | board->occupations[PieceNameBlack];
		//board->occupations[PieceNameNone] = ~board->occupations[PieceNameAny]; 
			
		updateFen(board);
		//printf("fen %s\n", board->fen->fenString);
		generateMoves(board);
		//printf("generate moves returned\n");
	} while(board->isCheck || board->isStaleMate || board->isMate);
  //writeDebug(board, true);
}

bool isEnPassantLegal(struct Board * board) {
	enum Files i;
	enum SquareName shift;
	struct Square kingSquare;
	struct Square opponentRookOrQueenSquare;
	unsigned char enPassantShift = board->fen->sideToMove == ColorWhite ? 16 : 40;
	board->fen->enPassantLegalBit = 0;

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
	board->fen->enPassantLegalBit = 1UL << (board->fen->enPassant + enPassantShift);
	return true;
}

//these two functions are based on the modern ray piece move generation technique 
//known as magic bitboards (see magic_bitboards.c)
//to use them, they must be initialized by calling init_magic_bitboards();
//and freed at the end by calling cleanup_magic_bitboards();
unsigned long generate_bishop_moves(struct Board * board, enum SquareName sn) {
  return get_bishop_moves(sn, board->occupations[PieceNameAny]);
}
unsigned long generate_rook_moves(struct Board * board, enum SquareName sn) {
  return get_rook_moves(sn, board->occupations[PieceNameAny]);
}

//generates bishop moves from a given square limited by board boundaries and other chess pieces 
//regardless of their color. If it's the same color, it is defended; if it's an opposite color, it's attacked
//this function is deprecated in favor of generate_bishop_moves()
/*
unsigned long generateBishopMoves(struct Board * board, struct Square * sq) {
	unsigned long d = 0;
	enum Files i = sq->file;
	enum Ranks j = sq->rank;
	enum SquareName shift = sq->name;
	while (i++ < FileH && j++ < Rank8) {
	  shift += 9;
	  d |= (1UL << shift);
		if (board->piecesOnSquares[shift] != PieceNameNone) break;
	}
	
	i = sq->file; j = sq->rank; shift = sq->name;
	while (i-- > FileA && j-- > Rank1) {
		shift -= 9;
		d |= (1UL << shift); 
		if (board->piecesOnSquares[shift] != PieceNameNone) break;
	}
	i = sq->file; j = sq->rank; shift = sq->name;
	while (i-- > FileA && j++ < Rank8) {
		shift += 7;
		d |= (1UL << shift);
		if (board->piecesOnSquares[shift] != PieceNameNone) break;
	}
	i = sq->file; j = sq->rank; shift = sq->name;
	while (i++ < FileH && j-- > Rank1) {
		shift -= 7;
		d |= (1UL << shift);
		if (board->piecesOnSquares[shift] != PieceNameNone) break;
	}
	return d;
}
*/

//generates rook moves from a given square limited by board boundaries and other chess pieces regardless of their color
//this function is deprecated in favor of generate_rook_moves()
/*
unsigned long generateRookMoves(struct Board * board, struct Square * sq) {
	int i;
	enum SquareName shift;
	unsigned long d = 0;
	i = sq->rank; shift = sq->name;
	while (i++ < Rank8) {
		shift += 8;
		d |= (1UL << shift); 
		if (board->piecesOnSquares[shift] != PieceNameNone) break; 
	}
	i = sq->rank; shift = sq->name;
	while (i-- > Rank1) {
		shift -= 8;
		d |= (1UL << shift);
		if (board->piecesOnSquares[shift] != PieceNameNone) break; 
	}
	shift = sq->name; i = sq->file;
	while (i++ < FileH) {
		shift++;
		d |= (1UL << shift);
		if (board->piecesOnSquares[shift] != PieceNameNone) break; 
	}
	i = sq->file; shift = sq->name;
	while (i-- > FileA) {
		shift--;
		d |= 1UL << shift;
		if (board->piecesOnSquares[shift] != PieceNameNone) break; 
	}
	return d;
}
*/
//generates knight moves from a given square limited by board boundaries only
unsigned long generateKnightMoves(struct Board * board, struct Square * sq) {
	bool opponent = ((board->occupations[(board->fen->sideToMove << 3) | PieceTypeAny]) & sq->bitSquare) ? false : true;
  unsigned long moves = 0;
	if (sq->file > FileB && sq->file < FileG) {
		moves = (sq->name > SquareC3) ? 0xA1100110AUL << (sq->name - SquareC3) : 0xA1100110AUL >> (SquareC3 - sq->name);
	}
	else if (sq->file == FileB) {
		moves = (sq->name > SquareB3) ? 0x508000805UL << (sq->name - SquareB3) : 0x508000805UL >> (SquareB3 - sq->name);
	}
	else if (sq->file == FileG)
		moves = (sq->name > SquareG3) ? 0xA0100010A0UL << (sq->name - SquareG3) : 0xA0100010A0UL >> (SquareG3 - sq->name);
	else if (sq->file == FileA)
		moves = (sq->name > SquareA3) ? 0x204000402UL << (sq->name - SquareA3) : 0x204000402UL >> (SquareA3 - sq->name);
	else if (sq->file == FileH)
		moves = (sq->name > SquareH3) ? 0x4020002040UL << (sq->name - SquareH3) : 0x4020002040UL >> (SquareH3 - sq->name);
	return moves;
}

//generates king moves from a given square limited by board boundaries only
unsigned long generateKingMoves(struct Board * board, struct Square * sq) {
  unsigned long moves = 0;
	if (sq->file > FileA && sq->file < FileH) {
		moves = (sq->name > SquareB2) ? 0x070507UL << (sq->name - SquareB2) : 0x070507UL >> (SquareB2 - sq->name);
	}
	else if (sq->file == FileA) {
		moves = (sq->name > SquareA2) ? 0x30203UL << (sq->name - SquareA2) : 0x30203UL >> (SquareA2 - sq->name);
	}
	else if (sq->file == FileH) {
		moves = (sq->name > SquareH2) ? 0xC040C0UL << (sq->name - SquareH2) : 0xC040C0UL >> (SquareH2 - sq->name);
	}
	return moves;
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
		while (ii++ < FileH && jj++ < Rank8) {
			shift += 9;
			d |= (1UL << shift);
			if (board->piecesOnSquares[shift] != PieceNameNone) break; 
		}
		ii = sq->file; jj = sq->rank; shift = sq->name;
		while (ii-- > FileA && jj-- > Rank1) {
			shift -= 9;
			d |= (1UL << shift);
			if (board->piecesOnSquares[shift] != PieceNameNone) break; 
		}
	}
	//the same is for antidiagonal
	else if (pinnedBy->antiDiag == sq->antiDiag) {
		while (ii-- > FileA && jj++ < Rank8) {
			shift += 7;
			d |= (1UL << shift);
			if (board->piecesOnSquares[shift] != PieceNameNone) break; 
		}
		ii = sq->file; jj = sq->rank; shift = sq->name;
		while (ii++ < FileH && jj-- > Rank1) {
			shift -= 7;
			d |= (1UL << shift);
			if (board->piecesOnSquares[shift] != PieceNameNone) break; 
		}
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
		while (jj++ < Rank8) {
			shift += 8;
			d |= (1UL << shift);
			if (board->piecesOnSquares[shift] != PieceNameNone) break; 
		}
		jj = sq->rank; shift = sq->name;
		while (jj-- > Rank1) {
			shift -= 8;
			d |= (1UL << shift);
			if (board->piecesOnSquares[shift] != PieceNameNone) break; 
		}
	}
	//the same applies to a rank
	else if (pinnedBy->rank == sq->rank) {
		while (ii++ < FileH) {
			shift++;
			d |= (1UL << shift);
			if (board->piecesOnSquares[shift] != PieceNameNone) break; 
		}
		ii = sq->file; shift = sq->name;
		while (ii-- > FileA) {
			shift--;
			d |= (1UL << shift);
			if (board->piecesOnSquares[shift] != PieceNameNone) break;
		}
	}
	return d;
}

void generateMoves(struct Board * board) {
	enum SquareName whiteBlack[] = { SquareA1, SquareA8 }; //first squares of the pieces' ranks
	unsigned long d = 0, checker = 0, moves = 0, attackedSquares = 0;
	struct Square pinnedBy, attackerSquare, sq, p;
	square(&pinnedBy, SquareNone);
	square(&attackerSquare, SquareNone);
	square(&sq, SquareNone);
	memset(board->movesFromSquares, 0, sizeof board->movesFromSquares);
	memset(board->sideToMoveMoves, 0, sizeof board->sideToMoveMoves);
	enum Files ii;
	enum Ranks jj;
	enum SquareName sn;
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
  board->blockingSquares = 0;
  board->checkers = 0;
  board->pinnedPieces = 0;
  board->pinningPieces = 0;
  
	//for opponent color sliding piece moves we need to temporary remove the king, 
	//so the rays can light through it
	//this is necessary to mark the squares behind the king as attacked, 
	//so that the king under the check of opponent ray piece, cannot step back
	board->piecesOnSquares[king.square.name] = PieceNameNone;
	//board->occupations[_king] = 0UL;
	//board->occupations[shiftedColor | PieceTypeAny] ^= king.square.bitSquare;
	board->occupations[PieceNameAny] ^= king.square.bitSquare;

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
	unsigned long any = board->occupations[shiftedColor | PieceTypeAny];

	//find the squares attacked and defended by opponent bishops
	//and see if they pin anything against the king
	//square(&sq, lsBit(opponentBishops));
	//while (sq.name < SquareNone) {
	while (opponentBishops) {
  	sn = __builtin_ctzl(opponentBishops);
  	square(&sq, sn);
		//generate opponent bishop moves limited by board boundaries and other pieces regardless of their color 
		//board->movesFromSquares[sq.name] = generateBishopMoves(board, &sq);
		board->movesFromSquares[sn] = generate_bishop_moves(board, sn);
		attackedSquares |= board->movesFromSquares[sn];
		//find pinned by this bishop pieces
		//in bishopPinFinder() we move from the board->fen->sideToMove king toward the opponent 
		//bishop if it's on the same diagonal (NE-SW) or antidiagonal (NW-SE)
		//we stop at the first non-empty square (d) regardless of whether 
		//it is occupied by black or white; 0 means no pin
		d = bishopPinFinder(board, &sq, &(king.square));
		//if the bishop also attacks this square and the piece on this square 
		//has board->fen->sideToMove color, then it is pinned
		d &= board->movesFromSquares[sq.name];
		d &= board->occupations[shiftedColor | PieceTypeAny];
		//in squarePinsSquare() we compact pinning and pinned squares in one unsigned short number, 
		//where upper 6 bits represent the pinning square 
		//and the lower 6 bits - pinned one and store this number in the array 
		//squarePinsSquare[8]
		if ((pinnedPieceSquare = lsBit(d)) < SquareNone) {
      board->pinnedPieces |= d;
   		board->pinningPieces |= sq.bitSquare;			
		}
		opponentBishops &= opponentBishops - 1;
	}
	//repeat the same process as described for opponent bishops, for opponent rooks
  while (opponentRooks) {
  	sn = __builtin_ctzl(opponentRooks);
  	square(&sq, sn);
		board->movesFromSquares[sn] = generate_rook_moves(board, sn);
		attackedSquares |= board->movesFromSquares[sq.name];
		d = rookPinFinder(board, &sq, &(king.square));
		d &= board->movesFromSquares[sq.name];
		d &= board->occupations[shiftedColor | PieceTypeAny];
		if ((pinnedPieceSquare = lsBit(d)) < SquareNone) {
      board->pinnedPieces |= d;
   		board->pinningPieces |= sq.bitSquare;			
		}
		opponentRooks &= opponentRooks - 1;
	}
	//repeat the same process as described for opponent bishops, for opponent queens
	while (opponentQueens) {
  	sn = __builtin_ctzl(opponentQueens);
  	square(&sq, sn);
		board->movesFromSquares[sn] = generate_bishop_moves(board, sn) | generate_rook_moves(board, sn);
		attackedSquares |= board->movesFromSquares[sq.name];
		d = bishopPinFinder(board, &sq, &(king.square)) | rookPinFinder(board, &sq, &(king.square));
		d &= board->movesFromSquares[sq.name];
		d &= board->occupations[shiftedColor | PieceTypeAny];
		if ((pinnedPieceSquare = lsBit(d)) < SquareNone) {
      board->pinnedPieces |= d;
   		board->pinningPieces |= sq.bitSquare;			
		}
		opponentQueens &= opponentQueens - 1;
	}
	//we are done with opponent ray piece attacked squares, now we can restore the king
	board->piecesOnSquares[king.square.name] = (shiftedColor | King);
	board->occupations[PieceNameAny] |= king.square.bitSquare;

	//find the squares attacked and defended by opponent pawns
	char pawnCapturingMoves[2][2] = { { 7, 9 }, { -9, -7 } };
	while (opponentPawns) {
		sn = __builtin_ctzl(opponentPawns);
		square(&sq, sn);
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
		opponentPawns &= opponentPawns - 1;
	}
	//find the squares attacked and defended by opponent knights
	while (opponentKnights) {
		sn = __builtin_ctzl(opponentKnights);
		square(&sq, sn);
		//generate opponent knight moves limited by board boudaries only
		unsigned long knight_moves = generateKnightMoves(board, &sq);
		board->movesFromSquares[sq.name] = knight_moves;
		attackedSquares |= knight_moves;
		opponentKnights &= opponentKnights - 1;
	}

	//generate opponent king moves limited by board boundaries only
	unsigned long opponentKingMoves = generateKingMoves(board, &(opponentKing.square));
	board->movesFromSquares[opponentKing.square.name] = opponentKingMoves;
  attackedSquares |= opponentKingMoves;
  
  //this is from the opponent point of view, meaning its defended pieces and the pieces that it attacks
  //opponent defended pieces are used in calculation of sideToMove king moves in terms of
  //whether it can capture opponent's piece or not
	board->defendedPieces = attackedSquares & board->occupations[shiftedOpponentColor | PieceTypeAny];
	//attacked pieces are not used in generateMoves()
  board->attackedPieces = attackedSquares & board->occupations[shiftedColor | PieceTypeAny];
    
	//checkers
	if ((attackedSquares & king.square.bitSquare) > 0) {
		while (opponentAny) {
			sn = __builtin_ctzl(opponentAny);
			if ((board->movesFromSquares[sn] & king.square.bitSquare) > 0) board->checkers |= (1UL << sn);
			opponentAny &= opponentAny - 1;
		}
	}
	
	//generate king moves limited by board boundaries only
	board->movesFromSquares[king.square.name] = generateKingMoves(board, &(king.square));
	//filter these moves to find the legal ones: 
	//the king cannot capture defended opponent's pieces
	board->movesFromSquares[king.square.name] ^= board->movesFromSquares[king.square.name] & board->defendedPieces;
	//it can't go to a square occupied by other pieces of its color
	board->movesFromSquares[king.square.name] ^= board->occupations[shiftedColor | PieceTypeAny] & board->movesFromSquares[king.square.name];
	//and it can't go to a square attacked by opponent piece(s)
	board->movesFromSquares[king.square.name] ^= attackedSquares & board->movesFromSquares[king.square.name];
	
	moves |= board->movesFromSquares[king.square.name];
	//is king checked?
	if ((board->isCheck = board->occupations[shiftedColor | King] & attackedSquares)) {
		//if double check, no other moves rather than the king's move are possible
		unsigned char bits = bitCount(board->checkers);
		if (bits > 1) goto exit;
		//normal check by checker
		else if (bits == 1) {
			square(&attackerSquare, lsBit(board->checkers));
			checker = board->checkers;
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
				board->blockingSquares = d;
			}
		}
	} else { //king is not checked
		//to complete legal king moves include castling
		enum SquareName castlingKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
		enum PieceName _rook = shiftedColor | Rook;
		//short castling moves
		if ((((board->fen->castlingRights >> (board->fen->sideToMove << 1)) & 3) & CastlingSideKingside) == CastlingSideKingside) {
			struct Square shortCastlingRookSquare;
			square(&shortCastlingRookSquare, board->fen->castlingRook[0][board->fen->sideToMove] + whiteBlack[board->fen->sideToMove]);
			struct ChessPiece shortCastlingRook;
			piece(&shortCastlingRookSquare, &shortCastlingRook, _rook);
			//empty squares between the king including king's square and its destination 
			//(for short castling: g1 or g8) should not be under attack
			unsigned long shortKingSquares = (((1UL << king.square.file) - 1) ^ 127) << whiteBlack[board->fen->sideToMove];
			unsigned long shortRookSquares = (((1UL << board->fen->castlingRook[0][board->fen->sideToMove]) - 1) ^ 31) << whiteBlack[board->fen->sideToMove];
			unsigned long occupations = board->occupations[PieceNameAny];
			//squares between the rook and its destination (for short castling f1 or f8) should be vacant (except the king and short castling rook for chess 960)
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
		if ((((board->fen->castlingRights >> (board->fen->sideToMove << 1)) & 3) & CastlingSideQueenside) == CastlingSideQueenside) {
			struct ChessPiece longCastlingRook;
			struct Square longCastlingRookSquare;
			square(&longCastlingRookSquare, board->fen->castlingRook[1][board->fen->sideToMove] + whiteBlack[board->fen->sideToMove]);
			piece(&longCastlingRookSquare, &longCastlingRook, _rook);
			//empty squares between the king including king's square and its destination (for long castling: c1 or c8) should not be under attack
			unsigned long longKingSquares = (((1UL << (king.square.file + 1)) - 1) ^ 3) << whiteBlack[board->fen->sideToMove];
			unsigned long longRookSquares = (((1UL << (board->fen->castlingRook[1][board->fen->sideToMove] + 1)) - 1) ^ 15) << whiteBlack[board->fen->sideToMove];
			unsigned long occupations = board->occupations[PieceNameAny];
			//squares between the rook and its destination (for long castling d1 or d8) should be vacant (except the king and long castling rook for chess 960)
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
  }
  
	//legal other moves

	//bishop moves
	while (bishops) {
		sn = __builtin_ctzl(bishops);
		square(&sq, sn);
		square(&pinnedBy, SquareNone);
		//alternative algorithm to find pinnedBy square
		if (board->pinnedPieces & sq.bitSquare) {
 			d = board->pinningPieces;
 			while (d) {
 				enum SquareName s = __builtin_ctzl(d);
 				square(&p, s);
 				if ((p.file == sq.file && p.file == kingSquare.file) || (p.rank == sq.rank && p.rank == kingSquare.rank) || (p.diag == sq.diag && p.diag == kingSquare.diag) || (p.antiDiag == sq.antiDiag && p.antiDiag == kingSquare.antiDiag)) {
 				  square(&pinnedBy, p.name);
 				  break;
 				}
   			d &= d - 1;
      }
    }
		if (pinnedBy.name < SquareNone) {
			//if there is check, then the bishop can't help since it can't move to block or capture the checker
			if (board->isCheck || (pinnedBy.file == sq.file) || (pinnedBy.rank == sq.rank)) {
				bishops &= bishops - 1;
				continue;
			}
			//if pinned on a file or rank, then the bishop cannot move
			d = pinnedBishopMoves(board, &sq, &pinnedBy);
		}
		//generate bishop moves from square <cp> limited by board boundaries and other chess pieces regardless of their color
		else {
			d = generate_bishop_moves(board, sn);
		}

		//bishop legal moves, which exclude moves to the squares occupied by pieces with the same color
		board->movesFromSquares[sq.name] = d ^ (d & board->occupations[shiftedColor | PieceTypeAny]);
		//if the king is in check, the legal moves are limited: we can either capture the  checker or block it
		if (checker > 0) {
			board->movesFromSquares[sq.name] &= (board->blockingSquares | checker);
		}
		moves |= board->movesFromSquares[sq.name];
		bishops &= bishops - 1;
	}

	//rook moves
	while (rooks) {
		sn = __builtin_ctzl(rooks);
		square(&sq, sn);
		square(&pinnedBy, SquareNone);
		//alternative algorithm to find pinnedBy square
		if (board->pinnedPieces & sq.bitSquare) {
 			d = board->pinningPieces;
 			while (d) {
 				enum SquareName s = __builtin_ctzl(d);
 				square(&p, s);
 				if ((p.file == sq.file && p.file == kingSquare.file) || (p.rank == sq.rank && p.rank == kingSquare.rank) || (p.diag == sq.diag && p.diag == kingSquare.diag) || (p.antiDiag == sq.antiDiag && p.antiDiag == kingSquare.antiDiag)) {
 				  square(&pinnedBy, p.name);
 				  break;
 				}
   			d &= d - 1;
      }
    }		
		if (pinnedBy.name < SquareNone) {
			//if check or the rook is pinned on a diagonal or anti-diagonal, then it cannot move
			if (board->isCheck || (pinnedBy.diag == sq.diag) || (pinnedBy.antiDiag == sq.antiDiag)) {
				rooks &= rooks - 1;
				continue;
			}
			d = pinnedRookMoves(board, &sq, &pinnedBy);
		}
		else {
			d = generate_rook_moves(board, sn);
		}
		board->movesFromSquares[sq.name] = d ^ (d & board->occupations[shiftedColor | PieceTypeAny]);
		if (checker > 0) board->movesFromSquares[sq.name] &= (board->blockingSquares | checker);
		moves |= board->movesFromSquares[sq.name];		
		rooks &= rooks - 1;
	}

	//queen moves
	while (queens) {
		sn = __builtin_ctzl(queens);
		square(&sq, sn);
		square(&pinnedBy, SquareNone);
		//alternative algorithm to find pinnedBy square
		if (board->pinnedPieces & sq.bitSquare) {
 			d = board->pinningPieces;
 			while (d) {
 				enum SquareName s = __builtin_ctzl(d);
 				square(&p, s);
 				if ((p.file == sq.file && p.file == kingSquare.file) || (p.rank == sq.rank && p.rank == kingSquare.rank) || (p.diag == sq.diag && p.diag == kingSquare.diag) || (p.antiDiag == sq.antiDiag && p.antiDiag == kingSquare.antiDiag)) {
 				  square(&pinnedBy, p.name);
 				  break;
 				}
   			d &= d - 1;
      }
    }		
		if (pinnedBy.name < SquareNone) {
			if (board->isCheck) {
				queens &= queens - 1;
				continue;
			}
			//if pinned on a diagonal or anti-diagonal, then the queen can move as a bishop along this diagonal or anti-diagonal
			if ((pinnedBy.diag == sq.diag) || (pinnedBy.antiDiag == sq.antiDiag)) {
				d = pinnedBishopMoves(board, &sq, &pinnedBy);
			}
			//if pinned on a file or rank, then the queen can only move like a rook along this file or rank
			else {
				d = pinnedRookMoves(board, &sq, &pinnedBy);
			}
		} else {
			d = generate_bishop_moves(board, sn);
			d |= generate_rook_moves(board, sn);
		}
		board->movesFromSquares[sq.name] = d ^ (d & board->occupations[shiftedColor | PieceTypeAny]);
		if (checker > 0) board->movesFromSquares[sq.name] &= (board->blockingSquares | checker);
		moves |= board->movesFromSquares[sq.name];
		queens &= queens - 1;
	}
	
	//pawn moves
	signed char pawnShifts[3][3] = { { 8, 7, 9 }, { -8, -9, -7 } };
	enum Ranks pawnRanks[3][3] = { { Rank2, Rank5, Rank6 }, { Rank7, Rank4, Rank3 } };
	while (pawns) {
		sn = __builtin_ctzl(pawns);
		square(&sq, sn);
		ii = sq.file; jj = sq.rank; shift = sq.name;
		square(&pinnedBy, SquareNone);
		//alternative algorithm to find pinnedBy square
		if (board->pinnedPieces & sq.bitSquare) {
 			d = board->pinningPieces;
 			while (d) {
 				enum SquareName s = __builtin_ctzl(d);
 				square(&p, s);
 				if ((p.file == sq.file && p.file == kingSquare.file) || (p.rank == sq.rank && p.rank == kingSquare.rank) || (p.diag == sq.diag && p.diag == kingSquare.diag) || (p.antiDiag == sq.antiDiag && p.antiDiag == kingSquare.antiDiag)) {
 				  square(&pinnedBy, p.name);
 				  break;
 				}
   			d &= d - 1;
      }
    }
    d = 0;		
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
				default:
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
				default:
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
			if (checker > 0) {
				//if checker is en passant pawn that can be captured
				if (board->fen->enPassant == attackerSquare.file && attackerSquare.rank == pawnRanks[board->fen->sideToMove][1] && sq.rank == pawnRanks[board->fen->sideToMove][1] && ((sq.file == board->fen->enPassant - 1) || (sq.file == board->fen->enPassant + 1))) {
					board->movesFromSquares[sq.name] &= 1UL << ((pawnRanks[board->fen->sideToMove][2] << 3) + board->fen->enPassant);
				}
				else {
					board->movesFromSquares[sq.name] &= (board->blockingSquares | checker);
				}
			}
		}
		moves |= board->movesFromSquares[sq.name];
next:
		pawns &= pawns - 1;
	}

	//knight moves
	while (knights) {
		sn = __builtin_ctzl(knights);
		square(&sq, sn);
		square(&pinnedBy, SquareNone);
		//alternative algorithm to find pinnedBy square
		if (board->pinnedPieces & sq.bitSquare) {
 			d = board->pinningPieces;
 			while (d) {
 				enum SquareName s = __builtin_ctzl(d);
 				square(&p, s);
 				if ((p.file == sq.file && p.file == kingSquare.file) || (p.rank == sq.rank && p.rank == kingSquare.rank) || (p.diag == sq.diag && p.diag == kingSquare.diag) || (p.antiDiag == sq.antiDiag && p.antiDiag == kingSquare.antiDiag)) {
 				  square(&pinnedBy, p.name);
 				  break;
 				}
   			d &= d - 1;
      }
    }		
		if (pinnedBy.name == SquareNone) {
			d = generateKnightMoves(board, &sq);
			board->movesFromSquares[sq.name] = d ^ (d & board->occupations[shiftedColor | PieceTypeAny]);
			if (checker > 0) board->movesFromSquares[sq.name] &= (board->blockingSquares | checker);
			moves |= board->movesFromSquares[sq.name];
		}
		knights &= knights - 1;
	}

exit:
	while (any) {
		sn = __builtin_ctzl(any);
		board->sideToMoveMoves[sn] = board->movesFromSquares[sn];
  	any &= any - 1;
  }
  //printf("generateMoves(): moves %lu\n", moves);
  board->attackedSquares = moves;
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
	move->chessBoard->piecesOnSquares[move->chessBoard->movingPiece.square.name] = PieceNameNone;
	
	//rook moves
	//need to find out if this is a castling rook to update FEN castling rights and rooks
	if (move->chessBoard->movingPiece.type == Rook && (move->chessBoard->fen->castlingRights & (CastlingSideBoth << ((move->chessBoard->fen->sideToMove) << 1)))) {
		enum Ranks rookRank[2] = { Rank1, Rank8 };
		enum CastlingSide castlingSide = CastlingSideNone;
		if (move->chessBoard->movingPiece.square.rank == rookRank[move->chessBoard->fen->sideToMove]) {
			if (move->chessBoard->movingPiece.square.file == move->chessBoard->fen->castlingRook[0][move->chessBoard->fen->sideToMove]) //short castling
				castlingSide = CastlingSideKingside;
			else if (move->chessBoard->movingPiece.square.file == move->chessBoard->fen->castlingRook[1][move->chessBoard->fen->sideToMove]) //long castling
				castlingSide = CastlingSideQueenside;
			if (castlingSide != CastlingSideNone) {
				//revoke castling rights
				move->chessBoard->fen->castlingRights ^= (castlingSide << ((move->chessBoard->fen->sideToMove) << 1));
				move->chessBoard->fen->castlingRook[castlingSide - 1][move->chessBoard->fen->sideToMove] = FileNone;
			}
		}
	}

	//capture
	if (move->type & MoveTypeCapture) {
		enum PieceName capturedPiece = PieceNameNone;
		if (move->type & MoveTypeEnPassant) {
			struct Square capturedPawnSquare;
			signed char offset[2] = { -8, 8 };
			square(&capturedPawnSquare, move->destinationSquare.name + offset[move->chessBoard->fen->sideToMove]);
			capturedPiece = (move->chessBoard->opponentColor << 3) | Pawn;
			move->chessBoard->occupations[capturedPiece] ^= capturedPawnSquare.bitSquare;
			move->chessBoard->piecesOnSquares[capturedPawnSquare.name] = PieceNameNone;
		} else {
			capturedPiece = move->chessBoard->piecesOnSquares[move->destinationSquare.name];
			move->chessBoard->occupations[move->chessBoard->piecesOnSquares[move->destinationSquare.name]] ^= move->destinationSquare.bitSquare;
			//if captured piece is a castling rook, then remove the castling rights of the opponent on that side
			if ((capturedPiece & 7) == Rook) {
				unsigned char whiteBlack[2] = { 0, 56 };
				enum CastlingRightsEnum cr = CastlingRightsWhiteNoneBlackNone;
				if (move->destinationSquare.name == move->chessBoard->fen->castlingRook[0][move->chessBoard->opponentColor] + whiteBlack[move->chessBoard->opponentColor]) {
					cr = CastlingSideKingside << (move->chessBoard->opponentColor << 1);
					if (move->chessBoard->fen->castlingRights & cr) {
						move->chessBoard->fen->castlingRights ^= cr;
						move->chessBoard->fen->castlingRook[0][move->chessBoard->opponentColor] = FileNone;
					}
				}
				else if (move->destinationSquare.name == move->chessBoard->fen->castlingRook[1][move->chessBoard->opponentColor] + whiteBlack[move->chessBoard->opponentColor]) {
					cr = CastlingSideQueenside << (move->chessBoard->opponentColor << 1);
					if (move->chessBoard->fen->castlingRights & cr) {
						move->chessBoard->fen->castlingRights ^= (CastlingSideQueenside << (move->chessBoard->opponentColor << 1));
						move->chessBoard->fen->castlingRook[1][move->chessBoard->opponentColor] = FileNone;
					}
				}
			}
		}
		move->chessBoard->capturedPiece = capturedPiece;
	}

	//castling
	enum SquareName dstKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
	enum CastlingSide castlingSide = CastlingSideNone;
	if (move->chessBoard->movingPiece.type == King) {
		if (((move->type & MoveTypeCastlingQueenside) == MoveTypeCastlingQueenside) || ((move->type & MoveTypeCastlingKingside) == MoveTypeCastlingKingside)) {
			castlingSide = (((move->type & (MoveTypeCastlingKingside | MoveTypeCastlingQueenside))) >> 2);
			unsigned char rookOffset[2] = { 0, 56 };
			enum SquareName dstRookSquare[2][2] = { { SquareF1, SquareF8 }, { SquareD1, SquareD8 } };
			enum SquareName srcRookSquare = move->chessBoard->fen->castlingRook[castlingSide - 1][move->chessBoard->fen->sideToMove] + rookOffset[move->chessBoard->fen->sideToMove];
			enum PieceName rookName = (move->chessBoard->fen->sideToMove << 3) | Rook;
			//remove castling rook from its source square taking care of
			//Chess960 case of rook occupying king's destination square - make sure we are not removing (overwriting) the king with PieceNone!
			if (srcRookSquare != dstKingSquare[castlingSide - 1][move->chessBoard->fen->sideToMove]) {
				move->chessBoard->piecesOnSquares[srcRookSquare] = PieceNameNone;
			}
			//and put it to its destination square
			move->chessBoard->piecesOnSquares[dstRookSquare[castlingSide - 1][move->chessBoard->fen->sideToMove]] = rookName;
			//update occupations
			//xor out the rook on its source square
			move->chessBoard->occupations[rookName] ^= (1UL << srcRookSquare);
			//add the rook on its destination square
			move->chessBoard->occupations[rookName] |= (1UL << dstRookSquare[castlingSide - 1][move->chessBoard->fen->sideToMove]);
  		//copy original castling rook for Zobrist hash calculation, then
	  	move->castlingRook = move->chessBoard->fen->castlingRook[castlingSide - 1][move->chessBoard->fen->sideToMove];
		}
		//update FEN castling rights, i.e. revoke castling rights for the sideToMove and keep castling rights for the opponent if any
		move->chessBoard->fen->castlingRights &= (CastlingSideBoth << (move->chessBoard->opponentColor << 1));
		//set castling rook to none
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
		if (move->chessBoard->fen->isChess960 && (((move->type & MoveTypeCastlingQueenside) == MoveTypeCastlingQueenside) || ((move->type & MoveTypeCastlingKingside) == MoveTypeCastlingKingside))) {
			square(&(move->destinationSquare), dstKingSquare[castlingSide - 1][move->chessBoard->fen->sideToMove]);
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

  //increment plyNumber
  move->chessBoard->plyNumber++;
	
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
	board->fen = fen;
	board->opponentColor = fen->sideToMove == ColorWhite ? ColorBlack : ColorWhite;
	board->plyNumber = fen->sideToMove == ColorWhite ? (fen->moveNumber << 1) - 1 : fen->moveNumber << 1;
	generateMoves(board);
	return 0;
}

struct Board * cloneBoard(struct Board * src) {
    struct Board * dst = malloc(sizeof(struct Board));
    if (!dst) {
        fprintf(stderr, "cloneBoard() error: malloc(Board) returned NULL: %s\n", strerror(errno));
        return NULL;
    }
    *dst = *src; // Shallow copy first

    // Deep copy pointers
    //Fen
    if (src->fen) {
        dst->fen = malloc(sizeof(struct Fen));
        if (!dst->fen) {
            free(dst);
            fprintf(stderr, "cloneBoard() error: malloc(Fen) returned NULL: %s\n", strerror(errno));
            return NULL;
        }
        *dst->fen = *src->fen; // Copy FEN contents
    }
    // Zobrist Hash
    if (src->zh) {
        dst->zh = malloc(sizeof(struct ZobristHash));
        if (!dst->zh) {
        	  free(dst->fen); 
            free(dst);
            fprintf(stderr, "cloneBoard() error: malloc(ZobristHash) returned NULL: %s\n", strerror(errno));
            return NULL;
        }
        *dst->zh = *src->zh; // Copy ZobristHash contents
    }
    return dst;
}

void freeBoard(struct Board * board) {
    if (board) {
        if (board->fen) free(board->fen);
        if (board->zh) free(board->zh);
        free(board);
    }
}
