#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wchar.h>
#include <locale.h>
#include "magic_bitboards.h"
#include "libchess.h"

//#ifdef __cplusplus
//extern "C" {
//#endif
#ifdef _WIN32
#include "windows.h"
#include "stdio.h"
#endif

void drawMoves(const Board& board, const Square sq, const uint64_t * movesFromSquares) {
	int squareIndex = SquareA1, sqStart = SquareA1;
	char buffer[8][256] = {};
	int rank = Rank1;
	
	uint64_t moves = movesFromSquares[sq];
	//const char * m = (board.sideToMove == ColorWhite && (board.piecesOnSquares[sq] >> 3) == ColorWhite) || (board.sideToMove == ColorBlack && (board.piecesOnSquares[sq] >> 3) == ColorBlack) ? " moves (x): " : " controlled squares (x): ";
	if ((board.sideToMove == ColorWhite && PC_COLOR(board.piecesOnSquares[sq]) == ColorWhite) || (board.sideToMove == ColorBlack && PC_COLOR(board.piecesOnSquares[sq]) == ColorBlack)) {
		printf("%s on %s %llx moves (x): \n", piece[board.piecesOnSquares[sq]], square[sq], moves);
		char s[6] = {};
		while ((squareIndex = lsBit(moves)) < SquareNone) {
			moves ^= (1ULL << squareIndex);
			for (int k = sqStart; k < squareIndex; k++) {
				if ((k + 1) % 8 == 0) {
					sprintf(s, "| %c |", pieceLetter[board.piecesOnSquares[k]]);
					strcat(buffer[rank++], s);
				}
				else {
					sprintf(s, "| %c ", pieceLetter[board.piecesOnSquares[k]]);
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
	
		for (int k = sqStart; k < SquareNone; k++) {
			if ((k + 1) % 8 == 0) {
				sprintf(s, "| %c |", pieceLetter[board.piecesOnSquares[k]]);
				strcat(buffer[rank++], s);
			}
			else {
				sprintf(s, "| %c ", pieceLetter[board.piecesOnSquares[k]]);
				strcat(buffer[rank], s);
			}
		}
		printf("+---+---+---+---+---+---+---+---+\n");
		for (signed char i = 7; i >= 0; i--) {
			printf("%s\n", buffer[i]);
			printf("+---+---+---+---+---+---+---+---+\n");
		}
	}
}

/// <summary>
/// Draws the chessboard and if displayMoves is true, then all leagal moves
/// </summary>
void writeDebug(const Board& board) {
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
	//HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	//DWORD mode;
	//GetConsoleMode(hOut, &mode);
	//SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	//CloseHandle(hOut);
#endif
  setlocale(LC_ALL, "en_US.UTF-8");
  wchar_t buffer[8][256] = {};
	wchar_t pieceLetter[] = {L'C', 0x2659, 0x2658, 0x2657, 0x2656, 0x2655, 0x2654, L' ', L'c', 0x265F, 0x265E, 0x265D, 0x265C, 0x265B, 0x265A, L'*'};
	int rank = Rank1;
	for (int i = SquareA1; i <= SquareH8; i++) {
		wchar_t s[16] = {};
		int	row = i / 8; // 0..7
		int	col = i % 8; // 0..7
		if ((i + 1) % 8 == 0) {
			if ((row % 2 == 0 && col % 2 == 0) || (row % 2 && col % 2))
			  swprintf(s, sizeof(s), L"\u2502\033[47m%lc\033[0m\u2502", pieceLetter[board.piecesOnSquares[i]]);			
			else
			  swprintf(s, sizeof(s), L"\u2502%lc\u2502", pieceLetter[board.piecesOnSquares[i]]);
			wcscat(buffer[rank++], s);
		}
		else {
			if ((row % 2 == 0 && col % 2 == 0) || (row % 2 && col % 2))
			  swprintf(s, sizeof(s), L"\u2502\033[47m%lc\033[0m", pieceLetter[board.piecesOnSquares[i]]);			
			else
			  swprintf(s, sizeof(s), L"\u2502%lc", pieceLetter[board.piecesOnSquares[i]]);
			wcscat(buffer[rank], s);
		}
	}

	printf("\u250C\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2510\n");
	for (signed char s = 7; s >= 0; s--) {
		printf("%ls\n", buffer[s]);
		printf("\u2514\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2518\n");
	}
}

int reconcile(const Board& board) {
	int err = 0;
	char fenString[MAX_FEN_STRING_LEN] = "";
	for (int i = SquareA1; i <= SquareH8; i++) {
		const Piece pn = board.piecesOnSquares[i];
		const int pt = PC_TYPE(pn) - 1;
		const Color color = PC_COLOR(pn);
	  if (pn != PieceNone && !(board.side[color] & board.pieceTypes[pt] & (1ULL << i))) {
		  printf("reconcile() error: piecesOnSquares[%s] %s does not match its occupations bitboard %llx, FEN %s\n", square[i], piece[pn], board.side[color] & board.pieceTypes[pt], board2fen(board, fenString));
		  err = 1;
		} 
	}
	for (int color = ColorWhite; color <= ColorBlack; color++) {
		for (int type = Pawn - 1; type <= King - 1; type++) {
  		Piece pn = PC(color, type + 1);
			uint64_t o = board.side[color] & board.pieceTypes[type];
			while (o) {
	  		Square s = lsBit(o);
				if (board.piecesOnSquares[s] != pn) {
					switch (pn) {
					case PieceWhite:
						if (PC_COLOR(board.piecesOnSquares[s]) == ColorWhite)
							break;
					case PieceBlack:
						if (PC_COLOR(board.piecesOnSquares[s]) == ColorBlack)
							break;
					default:
						printf("reconcile() error: %s piece in occupations %llx does not match one in piecesOnSquares[%s] %s, FEN %s\n", piece[pn], board.side[color] & board.pieceTypes[type], square[s], piece[board.piecesOnSquares[s]], board2fen(board, fenString));
						err = 1;
					}
				}
				o &= o - 1;
			}
		}
	}
	return err;
}

//returns FEN string
char * board2fen(const Board& board, char * fenString) {
    assert(fenString);

    char position[73] = {0};
    char *pos_ptr = position;
    for (int rank = Rank8; rank >= Rank1; rank--) {
        int empty_count = 0;
        for (int file = FileA; file <= FileH; file++) {
            Square sq = SQ(rank, file);
            Piece piece = board.piecesOnSquares[sq];
            if (piece == PieceNone) {
                empty_count++;
            } else {
                if (empty_count > 0) {
                    *pos_ptr++ = '0' + empty_count;
                    empty_count = 0;
                }
                *pos_ptr++ = pieceLetter[piece];
            }
        }
        if (empty_count > 0) {
            *pos_ptr++ = '0' + empty_count;
        }
        if (rank > 0) {
            *pos_ptr++ = '/';
        }
    }
    *pos_ptr = '\0';

    char castling[5] = {0};
    if (((unsigned int *)board.castlingRook)[0] == 0x08080808) {
        castling[0] = '-';
    } else {
        uint8_t idx = 0;
        for (int color = ColorWhite; color <= ColorBlack; color++) {
            if (board.isChess960) {
                if (board.castlingRook[color][0] != FileNone)
                    castling[idx++] = color == ColorWhite ? toupper(board.castlingRook[color][0] + 'a') : board.castlingRook[color][0] + 'a';
                if (board.castlingRook[color][1] != FileNone)
                    castling[idx++] = color == ColorWhite ? toupper(board.castlingRook[color][1] + 'a') : board.castlingRook[color][1] + 'a';
            } else {
                if (board.castlingRook[color][0] != FileNone)
                    castling[idx++] = (color == ColorWhite) ? 'K' : 'k';
                if (board.castlingRook[color][1] != FileNone) {
                    castling[idx++] = (color == ColorWhite) ? 'Q' : 'q';
                }
            }
        }
    }
    char en_passant[3] = {0};
    if (board.enPassant == FileNone) {
        en_passant[0] = '-';
    } else {
        en_passant[0] = board.enPassant + 'a';
        en_passant[1] = (board.sideToMove == ColorWhite) ? '6' : '3';
    }
    sprintf(fenString, "%s %c %s %s %d %d", position, fenColor[board.sideToMove], castling, en_passant, board.halfmoveClock, board.moveNumber);
    return fenString;
}

//helper function - used internally by enPassantMoveLegal(Board * board, const Square sq) and enPassantLegal(Board * board)
bool isEnPassantLegal(const Board& board) {
  int kingSq = lsBit(board.side[board.sideToMove] & board.pieceTypes[King - 1]);
  Rank kingRank = SQ_RANK(kingSq);
  for (int pt = Rook - 1; pt <= Queen - 1; pt++) {
    uint64_t oppRooksQueens = board.side[OPP_COLOR(board.sideToMove)] & board.pieceTypes[pt];
    while (oppRooksQueens) { //a loop for all opponent's rooks and queens
      const Square oppSq = lsBit(oppRooksQueens);
      if (kingRank != SQ_RANK(oppSq)) {
        oppRooksQueens &= oppRooksQueens - 1;
        continue; //king and opponent's rook or queen are not on the same rank, no check from en passant move is possible
      }
      //move the king towards opponent's rook or queen
      int dir = (oppSq > kingSq) ? 1 : -1;
      int shift = kingSq + dir;
      while (shift >= 0 && shift < 64 && SQ_RANK(shift) == kingRank) {
        if (board.piecesOnSquares[shift] != PieceNone) { //if there is something on the way
          if (shift == oppSq) return false; //if it is the rook or the queen, then en passant is illegal because of check
          break; //something other than the rook or the queen - not a problem
        }
        shift += dir; //continue moving the king towards opponent's rook or queen
      }
      oppRooksQueens &= oppRooksQueens - 1; //next opponent's rook or queen
    }
  }
  return true;
}

//returns en passant square (dst sq) if en passant move is legal or SquareNone otherwise
Square enPassantLegal(Board& board) {
  if (board.enPassant == FileNone) return SquareNone;
  Rank pawnRank, enPassantRank;
  if (board.sideToMove == ColorWhite) {
  	pawnRank = Rank5;
  	enPassantRank = Rank6;
  } else {
  	pawnRank = Rank4;
  	enPassantRank = Rank3;
  }
  Square src = SquareNone, dst = SquareNone, ep = SquareNone;
  const Piece capturingPawn = PC(board.sideToMove, Pawn);
  if (board.enPassant > FileA) {
  	src = SQ(pawnRank, board.enPassant - 1);
    if (board.piecesOnSquares[src] == capturingPawn) ep = (Square)(src + 1);
  }
  //if there are two capturing pawns on src and src2, then we should skip this check 
  //because the second capturing pawn will block the check even if the first pawn and en passant pawns are removed
  if (board.enPassant < FileH) {
  	const Square src2 = SQ(pawnRank, board.enPassant + 1);
    if (board.piecesOnSquares[src2] == capturingPawn) {
    	if (src != SquareNone) return SQ(enPassantRank, board.enPassant); //skip the legality check
    	else src = src2;
    	ep = (Square)(src - 1);
    }
  }
  if (ep != SquareNone) {
		const Piece capturedPawn = board.piecesOnSquares[ep];
		board.piecesOnSquares[src] = PieceNone; //temporarily remove moving pawn
		board.piecesOnSquares[ep] = PieceNone; //temporarily remove en passant pawn    	
    if (isEnPassantLegal(board)) dst = SQ(enPassantRank, board.enPassant);
		board.piecesOnSquares[src] = capturingPawn; //restore moving pawn
		board.piecesOnSquares[ep] = capturedPawn; //restore en passant pawn
  }
  return dst;
}


//returns en passant square if en passant capture from sq is legal or SquareNone otherwise
Square enPassantMoveLegal(Board& board, const Square sq) {
  if (board.enPassant == FileNone) return SquareNone;
  //need to remove the pawns first: en passant and capturing pawn from their squares
  Rank pawnRank, enPassantRank;
  if (board.sideToMove == ColorWhite) {
   pawnRank = Rank5;
   enPassantRank = Rank6;
  } else {
  	pawnRank = Rank4;
  	enPassantRank = Rank3;
  }
  Square ep, dst = SquareNone; //en passant pawn sq and capturing pawn dst sq
  if (SQ_RANK(sq) == pawnRank) {	
    const File sqFile = SQ_FILE(sq);
	  if (board.enPassant == sqFile - 1) ep = (Square)(sq - 1);
	  else if (board.enPassant == sqFile + 1) ep = (Square)(sq + 1);
	  else return SquareNone;
  } else return SquareNone;
  const Piece capturingPawn = board.piecesOnSquares[sq], capturedPawn = board.piecesOnSquares[ep];
	board.piecesOnSquares[sq] = PieceNone; //temporarily remove capturing pawn
	board.piecesOnSquares[ep] = PieceNone; //temporarily remove en passant pawn
  if (isEnPassantLegal(board)) dst = SQ(enPassantRank, board.enPassant);
	board.piecesOnSquares[sq] = capturingPawn; //restore capturing pawn
	board.piecesOnSquares[ep] = capturedPawn; //restore en passant pawn  	
  return dst;
}

//generates knight moves from a given square limited by board boundaries only
uint64_t generateKnightMoves(const Square sq) {
  uint64_t moves = 0;
  const File file = SQ_FILE(sq);
	if (file > FileB && file < FileG)
		moves = (sq > SquareC3) ? 0xA1100110AULL << (sq - SquareC3) : 0xA1100110AULL >> (SquareC3 - sq);
	else if (file == FileB)
		moves = (sq > SquareB3) ? 0x508000805ULL << (sq - SquareB3) : 0x508000805ULL >> (SquareB3 - sq);
	else if (file == FileG)
		moves = (sq > SquareG3) ? 0xA0100010A0ULL << (sq - SquareG3) : 0xA0100010A0ULL >> (SquareG3 - sq);
	else if (file == FileA)
		moves = (sq > SquareA3) ? 0x204000402ULL << (sq - SquareA3) : 0x204000402ULL >> (SquareA3 - sq);
	else if (file == FileH)
		moves = (sq > SquareH3) ? 0x4020002040ULL << (sq - SquareH3) : 0x4020002040ULL >> (SquareH3 - sq);
	return moves;
}

//generates king moves from a given square limited by board boundaries only
uint64_t generateKingMoves(const Square sq) {
  uint64_t moves = 0;
  const File file = SQ_FILE(sq);
	if (file > FileA && file < FileH) {
		moves = (sq > SquareB2) ? 0x070507ULL << (sq - SquareB2) : 0x070507ULL >> (SquareB2 - sq);
	}
	else if (file == FileA) {
		moves = (sq > SquareA2) ? 0x30203ULL << (sq - SquareA2) : 0x30203ULL >> (SquareA2 - sq);
	}
	else if (file == FileH) {
		moves = (sq > SquareH2) ? 0xC040C0ULL << (sq - SquareH2) : 0xC040C0ULL >> (SquareH2 - sq);
	}
	return moves;
}

uint64_t bishopPinFinder(const Board& board, const Color sideToMove, const Square bishopSquare, const Square kingSquare) {
	uint64_t d = 0;
	int ii = SQ_FILE(kingSquare), jj = SQ_RANK(kingSquare), shift = kingSquare;
	//King and opponent bishop (or queen) are on the same diagonal
	if (SQ_DIAG(bishopSquare) == 7 + jj - ii) {
		//We move the king along this diagonal towards the opponent bishop (or queen) until we bump into something.
		if (bishopSquare > kingSquare) {
			while (ii++ < FileH && jj++ < Rank8)
				if (board.piecesOnSquares[shift += 9] != PieceNone) { d = SQ_BIT(shift); break; }
		} else {
			while (ii-- > FileA && jj-- > Rank1)
				if (board.piecesOnSquares[shift -= 9] != PieceNone) { d = SQ_BIT(shift); break; }
		}
	}
	//King and opponent bishop (or queen) are on the same anti-diagonal
	else {
		if (SQ_ANTIDIAG(bishopSquare) == ii + jj) {
			//We move the king along this anti-diagonal towards the opponent bishop (or queen) until we bump into something.
			if (bishopSquare > kingSquare) {
				while (ii-- > FileA && jj++ < Rank8)
					if (board.piecesOnSquares[shift += 7] != PieceNone) { d = SQ_BIT(shift); break; }
			}
			else {
				while (ii++ < FileH && jj-- > Rank1)
					if (board.piecesOnSquares[shift -= 7] != PieceNone) { d = SQ_BIT(shift); break; }
			}
		}
	}
	//If that "something" is the same color as the king and if it is attacked (checked in caller)
	//by opponent bishop (or queen), then this "something" is pinned against the king
	return d & board.side[sideToMove];
}

uint64_t rookPinFinder(const Board& board, const Color sideToMove, const Square rookSquare, const Square kingSquare) {
	uint64_t d = 0;
	int ii = SQ_FILE(kingSquare), jj = SQ_RANK(kingSquare), shift = kingSquare; 
	//King and opponent rook (or queen) are on the same file
	if (SQ_FILE(rookSquare) == ii) {
		//We move the king along the file towards the opponent rook (or queen) until we bump into something.
		if (rookSquare > kingSquare) {
			while (jj++ < Rank8)
				if (board.piecesOnSquares[shift += 8] != PieceNone) { d = SQ_BIT(shift); break; }
		}
		else {
			while (jj-- > Rank1)
				if (board.piecesOnSquares[shift -= 8] != PieceNone) { d = SQ_BIT(shift); break; }
		}
	}
	//King and opponent rook (or queen) are on the same rank
	else if (SQ_RANK(rookSquare) == jj) {
		//We move the king along the rank towards the opponent rook (or queen) until we bump into something.
		if (rookSquare > kingSquare) {
			while (ii++ < FileH)
				if (board.piecesOnSquares[shift += 1] != PieceNone) { d = SQ_BIT(shift); break; }
		}
		else {
			while (ii-- > FileA)
				if (board.piecesOnSquares[shift -= 1] != PieceNone) { d = SQ_BIT(shift); break; }
		}
	}	
	//If that "something" is the same color as the king and if it is attacked (checked in caller)
	//by opponent rook (or queen), then this "something" is pinned against the king
	return d & board.side[sideToMove];
}

uint64_t pinnedBishopMoves(const Board& board, const Square sq, const Square pinnedBy) {
	const Diagonal diag = SQ_DIAG(sq);
	//if pinned on diagonal, then it can only move along it
	if (SQ_DIAG(pinnedBy) == diag) return get_bishop_moves(sq, board.side[ColorWhite] | board.side[ColorBlack]) & diag_bb[diag];
	//the same is for antidiagonal
	else {
  	const Antidiagonal antidiag = SQ_ANTIDIAG(sq);
		if (SQ_ANTIDIAG(pinnedBy) == antidiag) return get_bishop_moves(sq, board.side[ColorWhite] | board.side[ColorBlack]) & antidiag_bb[antidiag];
	}
	return 0;
}

uint64_t pinnedRookMoves(const Board& board, const Square sq, const Square pinnedBy) {
	const File file = SQ_FILE(sq);
	//if the rook is pinned on a file, it can only move along it
	if (SQ_FILE(pinnedBy) == file) return get_rook_moves(sq, board.side[ColorWhite] | board.side[ColorBlack]) & files_bb[file];
	//the same applies to a rank
	else {
		const Rank rank = SQ_RANK(sq);
		if (SQ_RANK(pinnedBy) == rank) return get_rook_moves(sq, board.side[ColorWhite] | board.side[ColorBlack]) & ranks_bb[rank];
	}
	return 0;
}

//returns moves
//TO DO: it can be further improved
uint64_t piece_moves(const PieceType pieceType, const Square sq, const MovesContext& ctx, const KingSquare& king, Board& board) {
	const int8_t pawnShifts[2][3] = { { 8, 7, 9 }, { -8, -9, -7 } }; // { { N, NW, NE}, {S, SW, SE} }
	const Rank pawnRanks[2][3] = { { Rank2, Rank5, Rank6 }, { Rank7, Rank4, Rank3 } };	
	int shift = 0;
	uint64_t moves = 0;
	//while (occupation) {
	  Square pinnedBy = SquareNone;
		//const Square sq = lsBit(occupation);
		const File sqFile = SQ_FILE(sq);
		const Rank sqRank = SQ_RANK(sq);
		const Diagonal sqDiag = (Diagonal)(7 + sqRank - sqFile);
		const Antidiagonal sqAntidiag = (Antidiagonal)(sqFile + sqRank);
		const uint64_t sqBit = 1ULL << sq;
		uint64_t d = 0;
		if (ctx.pinnedPieces & sqBit) {
 			d = ctx.pinningPieces;
 			while (d) {
 				const Square p = lsBit(d);
 				const File pFile = SQ_FILE(p);
 				const Rank pRank = SQ_RANK(p);
 				const Diagonal pDiag = (Diagonal)(7 + pRank - pFile);
 				const Antidiagonal pAntidiag = (Antidiagonal)(pFile + pRank);
 				if ((pFile == sqFile && pFile == king.file) || (pRank == sqRank && pRank == king.rank) || (pDiag == sqDiag && pDiag == king.diag) || (pAntidiag == sqAntidiag && pAntidiag == king.antidiag)) {
 				  pinnedBy = p;
 				  break;
 				}
   			d &= d - 1;
      }
    }
    d = 0;
		if (pinnedBy < SquareNone) {
			if (pieceType == Bishop) {
  			//if check or the bishop is pinned on a file or rank, then it cannot move
  			if (board.isCheck || SQ_FILE(pinnedBy) == sqFile || SQ_RANK(pinnedBy) == sqRank) {
  				//occupation &= occupation - 1;
  				//continue;
  				return 0;
  			}
			  d = pinnedBishopMoves(board, sq, pinnedBy);
			} else if (pieceType == Rook) {
  			//if check or the rook is pinned on a diagonal or anti-diagonal, then it cannot move
  			if (board.isCheck || SQ_DIAG(pinnedBy) == sqDiag || SQ_ANTIDIAG(pinnedBy) == sqAntidiag) {
  				//occupation &= occupation - 1;
  				//continue;
  				return 0;
  			}
			  d = pinnedRookMoves(board, sq, pinnedBy);
			} else if (pieceType == Queen) {
  			if (board.isCheck) {
  				//occupation &= occupation - 1;
  				//continue;
  				return 0;
  			}
  			//if pinned on a diagonal or anti-diagonal, then the queen can move like a bishop along this diagonal or anti-diagonal
  			if (SQ_DIAG(pinnedBy) == sqDiag || SQ_ANTIDIAG(pinnedBy) == sqAntidiag) d = pinnedBishopMoves(board, sq, pinnedBy);
  			//if pinned on a file or rank, then the queen can only move like a rook along this file or rank
  			else d = pinnedRookMoves(board, sq, pinnedBy);
			} else if (pieceType == Pawn) {
  			if (board.isCheck) {// this pawn can't do much
  			  //occupation &= occupation - 1;
  			  //continue;
  			  return 0;
  			}
  			if (SQ_DIAG(pinnedBy) == sqDiag) {
  				shift = board.sideToMove == ColorWhite ? sq + 9 : sq - 9;
  				const File enPassantFile = board.sideToMove == ColorWhite ? (File)(sqFile + 1) : (File)(sqFile - 1);
  				if ((board.sideToMove == ColorWhite && sqFile < FileH) || (board.sideToMove == ColorBlack && sqFile > FileA)) {
  					if (board.piecesOnSquares[shift] != PieceNone) {
  						if ((board.piecesOnSquares[shift] >> 3) == OPP_COLOR(board.sideToMove))
  							d |= (1ULL << shift);
  					}
  					else if (board.enPassant == enPassantFile && sqRank == pawnRanks[board.sideToMove][1])
  						d |= (1ULL << shift);
  				}
  			} else if (SQ_ANTIDIAG(pinnedBy) == sqAntidiag) {
  				shift = board.sideToMove == ColorWhite ? sq + 7 : sq - 7;
  				const File enPassantFile = board.sideToMove == ColorWhite ? (File)(sqFile - 1) : (File)(sqFile + 1);
  				if ((board.sideToMove == ColorWhite && sqFile > FileA) || (board.sideToMove == ColorBlack && sqFile < FileH)) {
  					if (board.piecesOnSquares[shift] != PieceNone) {
  						if (board.piecesOnSquares[shift] >> 3 == OPP_COLOR(board.sideToMove))
  							d |= (1ULL << shift);
  					}
  					else if (board.enPassant == enPassantFile && sqRank == pawnRanks[board.sideToMove][1])
  						d |= (1ULL << shift);
  				}
  			} else if (SQ_FILE(pinnedBy) == sqFile) {
  				shift = sq + pawnShifts[board.sideToMove][0];
  				if (board.piecesOnSquares[shift] == PieceNone) {
  					d |= (1ULL << shift);
  					if (sqRank == pawnRanks[board.sideToMove][0]) {
  						shift += pawnShifts[board.sideToMove][0];
  						if (board.piecesOnSquares[shift] == PieceNone) d |= (1ULL << shift);
  					}
  				}
  			}
  			else if (SQ_RANK(pinnedBy) == sqRank) { //not much can be done
  			  //occupation &= occupation - 1;
  			  //continue;
  			  return 0;
        }
  		} //end of if (pawn is pinned); if knight is pinned, it has no moves
			moves = d;
		} //end of if (pinned)
		//generate bishop/rook/queen/pawn moves from square <cp> limited by board boundaries and other chess pieces regardless of their color
		else {
   		d = 0;
		  if (pieceType == Bishop)
			  d = get_bishop_moves(sq, board.side[ColorWhite]| board.side[ColorBlack]);
			else if (pieceType == Rook)
			  d = get_rook_moves(sq, board.side[ColorWhite] | board.side[ColorBlack]);
			else if (pieceType == Queen) {
				uint64_t any = board.side[ColorWhite] | board.side[ColorBlack];
			  d = get_bishop_moves(sq, any);
			  d |= get_rook_moves(sq, any);			  
			} else if (pieceType == Pawn) {
  			//normal pawn moves (non-capturing)
  			shift = sq + pawnShifts[board.sideToMove][0]; //N or S
  			if (board.piecesOnSquares[shift] == PieceNone) {
  				d |= (1ULL << shift);
  				//double advance from rank 2 or 7
  				if (sqRank == pawnRanks[board.sideToMove][0]) { //rank2 or rank7
  					shift += pawnShifts[board.sideToMove][0];
  					if (board.piecesOnSquares[shift] == PieceNone) d |= (1ULL << shift);
  				}
  			}
  			//capturing pawn moves
  			if (sqFile > FileA) {
  				shift = sq + pawnShifts[board.sideToMove][1]; //NW (white) or SW (black)
          uint64_t bit_sq = 1ULL << shift;
          if (board.side[OPP_COLOR(board.sideToMove)] & bit_sq) d |= bit_sq; 
  			}
  			if (sqFile < FileH) {
  				shift = sq + pawnShifts[board.sideToMove][2]; //NE (white) or SE (black)
          uint64_t bit_sq = 1ULL << shift;
          if (board.side[OPP_COLOR(board.sideToMove)] & bit_sq) d |= bit_sq; 
  			}
  			const Square ep = enPassantMoveLegal(board, sq);
  			if (ep != SquareNone) d |= (1ULL << ep);
  			moves = d;
  			if (ctx.num_checkers == 1) { //we can ONLY capture or block a single checker - no other moves
  				if (ep != SquareNone) moves &= (1ULL << ep);
  				else moves &= (ctx.blockingSquares | (1ULL << ctx.checkerSquare)); //can we block or capture checker
  			}		  
			} else if (pieceType == Knight) d = generateKnightMoves(sq);
		} //end of if (not pinned)
    if (pieceType != Pawn) {
  		//piece legal moves, which exclude moves to the squares occupied by pieces with the same color
  		moves = d ^ (d & board.side[board.sideToMove]);
  		//if the king is in check, the legal moves are limited: we can either capture the  checker or block it
  		if (ctx.num_checkers == 1) moves &= (ctx.blockingSquares | (1ULL << ctx.checkerSquare));
		}
		board.num_moves += bitCount(moves);
		//occupation &= occupation - 1;
	//}
	return moves;
}

//returns attacked squares bitboard as well as pinned and pinning bitboards in MovesContext
//call it before calling generateMoves(), which takes both attackedSquares and MovesContext as input
uint64_t getAttackedSquares(const Board& board, MovesContext& movesContext) {
	const Color oppColor = OPP_COLOR(board.sideToMove);
	//movesContext.num_checkers = 0;
	//movesContext.checkerSquare = SquareNone;
	//movesContext.pinnedPieces = 0;
	//movesContext.pinningPieces = 0;
	//movesContext.blockingSquares = 0;

	uint64_t d, attackedSquares = 0;
	uint64_t opponentPawns = board.side[oppColor] & board.pieceTypes[Pawn - 1];
	uint64_t opponentKnights = board.side[oppColor] & board.pieceTypes[Knight - 1];
	uint64_t opponentBishops = board.side[oppColor] & board.pieceTypes[Bishop - 1];
	uint64_t opponentRooks = board.side[oppColor] & board.pieceTypes[Rook - 1];
	uint64_t opponentQueens = board.side[oppColor] & board.pieceTypes[Queen - 1];
	uint64_t any = board.side[ColorWhite] | board.side[ColorBlack];
	Square sq;

	//for opponent color sliding piece moves we need to temporary remove the king, 
	//so the rays can light through it
	//this is necessary to mark the squares behind the king as attacked, 
	//so that the king under the check of opponent ray piece, cannot step back
	//we only xor it out from any because only it is passed to get_bishop_moves() and get_root_moves()
	const uint64_t kingBitSq = board.side[board.sideToMove] & board.pieceTypes[King - 1];
	assert(kingBitSq);
	/*if (!kingBitSq) {
		char fen[MAX_FEN_STRING_LEN];
		reconcile(board);
		writeDebug(board);
		printf("getAttackedSquares() debug: kingSquare is none, fen %s\n", board2fen(board, fen));
		exit(1);
	}*/
	any ^= kingBitSq;
	const Square kingSquare = lsBit(kingBitSq);

	//find the squares attacked and defended by opponent bishops
	//and see if they pin anything against the king
	while (opponentBishops) {
  	sq = lsBit(opponentBishops);
		//generate opponent bishop moves limited by board boundaries and other pieces regardless of their color 
		uint64_t attacked = get_bishop_moves(sq, any);
		attackedSquares |= attacked;
		//find pinned by this bishop pieces
		if ((d = (bishopPinFinder(board, board.sideToMove, sq, kingSquare) & attacked))) {
      movesContext.pinnedPieces |= d;
   		movesContext.pinningPieces |= (1ULL << sq); //SQ_BIT macro does an extra check for sq != SquareNone, here we don't need it
		}
		//we should set the checkers and its square if any as well
		if (attacked & kingBitSq) {
			movesContext.num_checkers++;
			movesContext.checkerSquare = sq;
		}
		opponentBishops &= opponentBishops - 1;
	}
	//repeat the same process as described for opponent bishops, for opponent rooks
  while (opponentRooks) {
  	sq = lsBit(opponentRooks);
		uint64_t attacked = get_rook_moves(sq, any);
		attackedSquares |= attacked;
		if ((d = (rookPinFinder(board, board.sideToMove, sq, kingSquare) & attacked))) {
      movesContext.pinnedPieces |= d;
   		movesContext.pinningPieces |= (1ULL << sq);			
		}
		//we should set the checkers and its square if any as well
		if (attacked & kingBitSq) {
			movesContext.num_checkers++;
			movesContext.checkerSquare = sq;
		}
		opponentRooks &= opponentRooks - 1;
	}
	//repeat the same process as described for opponent bishops, for opponent queens
	while (opponentQueens) {
  	sq = lsBit(opponentQueens);
		uint64_t attacked = get_bishop_moves(sq, any) | get_rook_moves(sq, any);
		attackedSquares |= attacked;
		if ((d = (bishopPinFinder(board, board.sideToMove, sq, kingSquare) & attacked) | (rookPinFinder(board, board.sideToMove, sq, kingSquare) & attacked))) {
      movesContext.pinnedPieces |= d;
   		movesContext.pinningPieces |= (1ULL << sq);			
		}
		//we should set the checkers and its square if any as well
		if (attacked & kingBitSq) {
			movesContext.num_checkers++;
			movesContext.checkerSquare = sq;
		}
		opponentQueens &= opponentQueens - 1;
	}
	
	//find the squares attacked and defended by opponent pawns
	const int8_t pawnCapturingMoves[2][2] = { { 7, 9 }, { -9, -7 } };
	while (opponentPawns) {
		sq = lsBit(opponentPawns);
		d = 0;
		File ii = SQ_FILE(sq);
		Square shift = sq;
		//opponent pawn attacking, capturing and protecting moves; another words, just diagonal and anti-diagonal moves
		if (ii > FileA)	{
			shift = (Square)(sq + pawnCapturingMoves[oppColor][0]);
			d |= (1ULL << shift);
		}
		if (ii < FileH) {
			shift = (Square)(sq + pawnCapturingMoves[oppColor][1]);
			d |= (1ULL << shift);
		}
		attackedSquares |= d;
		if (d & kingBitSq) {
			movesContext.num_checkers++;
			movesContext.checkerSquare = sq;
		}
		opponentPawns &= opponentPawns - 1;
	}
	//find the squares attacked and defended by opponent knights
	while (opponentKnights) {
		sq = lsBit(opponentKnights);
		//generate opponent knight moves limited by board boudaries only
		uint64_t attacked = generateKnightMoves(sq);
		attackedSquares |= attacked;
		if (attacked & kingBitSq) {
			movesContext.num_checkers++;
			movesContext.checkerSquare = sq;
		}		
		opponentKnights &= opponentKnights - 1;
	}

	//generate opponent king moves limited by board boundaries only
	const Square opponentKingSquare = lsBit(board.side[oppColor] & board.pieceTypes[King - 1]);
  attackedSquares |= generateKingMoves(opponentKingSquare);
  return attackedSquares;
}

//returns only attacked squares, no pins
uint64_t getAttackedSquaresOnly(const Board& board) {
	const int shiftedColor = board.sideToMove << 3;
	const Color oppColor = OPP_COLOR(board.sideToMove);
	const int shiftedOpponentColor = oppColor << 3;

	uint64_t d, attackedSquares = 0;
	uint64_t opponentPawns = board.side[oppColor] & board.pieceTypes[Pawn - 1];
	uint64_t opponentKnights = board.side[oppColor] & board.pieceTypes[Knight - 1];
	uint64_t opponentBishops = board.side[oppColor] & board.pieceTypes[Bishop - 1];
	uint64_t opponentRooks = board.side[oppColor] & board.pieceTypes[Rook - 1];
	uint64_t opponentQueens = board.side[oppColor] & board.pieceTypes[Queen - 1];
	uint64_t any = board.side[ColorWhite]| board.side[ColorBlack];
	Square sq;

	//for opponent color sliding piece moves we need to temporary remove the king, 
	//so the rays can light through it
	//this is necessary to mark the squares behind the king as attacked, 
	//so that the king under the check of opponent ray piece, cannot step back
	//we only xor it out from any because only it is passed to get_bishop_moves() and get_root_moves()
	uint64_t kingBitSq = board.side[board.sideToMove] & board.pieceTypes[King - 1];
	any ^= kingBitSq;

	//find the squares attacked and defended by opponent bishops
	//and see if they pin anything against the king
	while (opponentBishops) {
  	sq = lsBit(opponentBishops);
		//generate opponent bishop moves limited by board boundaries and other pieces regardless of their color 
		attackedSquares |= get_bishop_moves(sq, any);
		opponentBishops &= opponentBishops - 1;
	}
	//repeat the same process as described for opponent bishops, for opponent rooks
  while (opponentRooks) {
  	sq = lsBit(opponentRooks);
		attackedSquares |= get_rook_moves(sq, any);
		opponentRooks &= opponentRooks - 1;
	}
	//repeat the same process as described for opponent bishops, for opponent queens
	while (opponentQueens) {
  	sq = lsBit(opponentQueens);
		attackedSquares |= get_bishop_moves(sq, any) | get_rook_moves(sq, any);
		opponentQueens &= opponentQueens - 1;
	}
	
	//find the squares attacked and defended by opponent pawns
	const int8_t pawnCapturingMoves[Color_NB][2] = { { 7, 9 }, { -9, -7 } };
	while (opponentPawns) {
		sq = lsBit(opponentPawns);
		d = 0;
		File ii = SQ_FILE(sq);
		Square shift = sq;
		//opponent pawn attacking, capturing and protecting moves; another words, just diagonal and anti-diagonal moves
		if (ii > FileA)	{
			shift = (Square)(sq + pawnCapturingMoves[oppColor][0]);
			d |= (1ULL << shift);
		}
		if (ii < FileH) {
			shift = (Square)(sq + pawnCapturingMoves[oppColor][1]);
			d |= (1ULL << shift);
		}
		attackedSquares |= d;
		opponentPawns &= opponentPawns - 1;
	}
	//find the squares attacked and defended by opponent knights
	while (opponentKnights) {
		sq = lsBit(opponentKnights);
		//generate opponent knight moves limited by board boudaries only
		attackedSquares |= generateKnightMoves(sq);
		opponentKnights &= opponentKnights - 1;
	}
	//generate opponent king moves limited by board boundaries only
	const Square opponentKingSquare = lsBit(board.side[oppColor] & board.pieceTypes[King - 1]);
  attackedSquares |= generateKingMoves(opponentKingSquare);
  return attackedSquares;
}

uint64_t kingMoves(Board& board, const Square kingSquare, const KingSquare& kingSq, MovesContext& movesContext, const uint64_t attackedSquares) {
  //Square kingSquare = lsBit(kingSq.bit);
	Color oppColor = OPP_COLOR(board.sideToMove);	
  //this is from the opponent point of view, meaning its defended pieces and the squares that it attacks
  //opponent defended pieces are used in calculation of sideToMove king moves in terms of
  //whether it can capture opponent's piece or not
	uint64_t defendedPieces = attackedSquares & board.side[oppColor];
	//generate king moves limited by board boundaries only
	uint64_t moves = generateKingMoves(kingSquare);
	//filter these moves to find the legal ones: 
	//the king cannot capture defended opponent's pieces
	//it can't go to a square occupied by other pieces of its color
	//and it can't go to a square attacked by opponent piece(s)
	moves ^= (moves & (defendedPieces | board.side[board.sideToMove] | attackedSquares));
	
	//is king checked?
	if (movesContext.num_checkers) board.isCheck = true;
	else board.isCheck = false;
	//if double check, no other moves rather than the king's move are possible
	if (movesContext.num_checkers > 1) {
  	board.num_moves = bitCount(moves);
		return moves;
	}
	if (board.isCheck) {
		//normal check by checker
    uint64_t none = ~(board.side[ColorWhite] | board.side[ColorBlack]);
		//if not checked by knight or pawn, calculate blocking squares
		assert(movesContext.checkerSquare < SquareNone);
		if ((board.piecesOnSquares[movesContext.checkerSquare] != PC(oppColor, Knight)) && board.piecesOnSquares[movesContext.checkerSquare] != PC(oppColor, Pawn)) {
			if (kingSq.diag == SQ_DIAG(movesContext.checkerSquare)) {
				if (kingSquare > movesContext.checkerSquare) {
					uint64_t bitSq = 1ULL << movesContext.checkerSquare;
					movesContext.blockingSquares = none & diag_bb[kingSq.diag] & ((kingSq.bit - 1) ^ (bitSq | (bitSq - 1)));
				} else {
					movesContext.blockingSquares = none & diag_bb[kingSq.diag] & (((1ULL << movesContext.checkerSquare) - 1) ^ (kingSq.bit | (kingSq.bit - 1)));
				}
			} else if (kingSq.antidiag == SQ_ANTIDIAG(movesContext.checkerSquare)) {
				if (kingSquare > movesContext.checkerSquare) {
					uint64_t bitSq = 1ULL << movesContext.checkerSquare;
					movesContext.blockingSquares = none & antidiag_bb[kingSq.antidiag] & ((kingSq.bit - 1) ^ (bitSq | (bitSq - 1)));							
				} else {
					movesContext.blockingSquares = none & antidiag_bb[kingSq.antidiag] & (((1ULL << movesContext.checkerSquare) - 1) ^ (kingSq.bit | (kingSq.bit - 1)));
				}
			} else if (kingSq.file == SQ_FILE(movesContext.checkerSquare)) {
				if (kingSquare > movesContext.checkerSquare) {
					uint64_t bitSq = 1ULL << movesContext.checkerSquare;
					movesContext.blockingSquares = none & files_bb[kingSq.file] & ((kingSq.bit - 1) ^ (bitSq | (bitSq - 1)));
				} else {
					movesContext.blockingSquares = none & files_bb[kingSq.file] & (((1ULL << movesContext.checkerSquare) - 1) ^ (kingSq.bit | (kingSq.bit - 1)));
				}
			} else if (kingSq.rank == SQ_RANK(movesContext.checkerSquare)) {
				if (kingSquare > movesContext.checkerSquare) {
					uint64_t bitSq = 1ULL << movesContext.checkerSquare;
					movesContext.blockingSquares = none & ranks_bb[kingSq.rank] & ((kingSq.bit - 1) ^ (bitSq | (bitSq - 1)));
				} else {
					movesContext.blockingSquares = none & ranks_bb[kingSq.rank] & (((1ULL << movesContext.checkerSquare) - 1) ^ (kingSq.bit | (kingSq.bit - 1)));
				}
			}
		}
	} else { //king is not checked
		//to complete legal king moves include castling
		const Square whiteBlack = board.sideToMove == ColorWhite ? SquareA1 : SquareA8; //first squares of the pieces' ranks		
		const Square castlingKingSquare[Color_NB][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
		//short castling moves
		if ((board.castlingRook[board.sideToMove][0] != FileNone) == CastlingKingside) {
			const uint64_t shortCastlingRookSquareBit = SQ_BIT(board.castlingRook[board.sideToMove][0] + whiteBlack);
			//empty squares between the king including king's square and its destination 
			//(for short castling: g1 or g8) should not be under attack
			const uint64_t shortKingSquares = (((1ULL << kingSq.file) - 1) ^ 127) << whiteBlack;
			const uint64_t shortRookSquares = (((1ULL << board.castlingRook[board.sideToMove][0]) - 1) ^ 31) << whiteBlack;
			uint64_t occupations = board.side[ColorWhite] | board.side[ColorBlack];
			//squares between the rook and its destination (for short castling f1 or f8) should be vacant (except the king and short castling rook for chess 960)
			occupations ^= (kingSq.bit | shortCastlingRookSquareBit);
			if ((shortKingSquares & attackedSquares) == 0 && (shortKingSquares & occupations) == 0 && (shortRookSquares & occupations) == 0) {
				if (board.isChess960) moves |= shortCastlingRookSquareBit;
				else moves |= (1ULL << castlingKingSquare[0][board.sideToMove]);
			}
		}
		//long castling moves
		if (board.castlingRook[board.sideToMove][1] != FileNone) {
			const uint64_t longCastlingRookSquareBit = SQ_BIT(board.castlingRook[board.sideToMove][1] + whiteBlack);
			//empty squares between the king including king's square and its destination (for long castling: c1 or c8) should not be under attack
			uint64_t longKingSquares = (((1ULL << (kingSq.file + 1)) - 1) ^ 3) << whiteBlack;
			uint64_t longRookSquares = (((1ULL << (board.castlingRook[board.sideToMove][1] + 1)) - 1) ^ 15) << whiteBlack;
			uint64_t occupations = board.side[ColorWhite] | board.side[ColorBlack];
			//squares between the rook and its destination (for long castling d1 or d8) should be vacant (except the king and long castling rook for chess 960)
			occupations ^= (kingSq.bit | longCastlingRookSquareBit);
			if ((longKingSquares & attackedSquares) == 0 && (longKingSquares & occupations) == 0 && (longRookSquares & occupations) == 0) {
				if (board.isChess960) moves |= longCastlingRookSquareBit;
				else moves |= (1ULL << castlingKingSquare[1][board.sideToMove]);
			}
		}
  }
  board.num_moves = bitCount(moves);  
  return moves;
}

Square getKingSquare(const Board& board, KingSquare& kingSq) {
	kingSq.bit = board.side[board.sideToMove] & board.pieceTypes[King - 1];
	Square kingSquare = lsBit(kingSq.bit);
	kingSq.file = SQ_FILE(kingSquare);
	kingSq.rank = SQ_RANK(kingSquare);
	kingSq.diag = (Diagonal)(7 + kingSq.rank - kingSq.file);
	kingSq.antidiag = (Antidiagonal)(kingSq.rank + kingSq.file);
	return kingSquare;
}

//generates legal moves and stores them in movesFromSquares[64] array
//call getAttackedSquares() first to init movesFromSquares, movesContext and calculate attackedSquares
//this may not be efficient, it's better to generate moves from a square and make those moves instead of saving them
//in large array and then loop over it
//this function is no longer needed - moves can be generated when needed using the logic in isCheckMateStaleMate()
//uint64_t movesFromSquares[64] array is too bulky to carry around
/*void generateMoves(Board& board, uint64_t * movesFromSquares) {
 	KingSquare kingSq;
	Square kingSquare = getKingSquare(board, kingSq);
	MovesContext movesContext = {};
	uint64_t attackedSquares = getAttackedSquares(board, movesContext);
	movesFromSquares[kingSquare] = kingMoves(board, kingSquare, kingSq, movesContext, attackedSquares);
	uint64_t occupations;
	if (movesContext.num_checkers > 1) goto exit;
	
	//legal other moves
	occupations = board.side[board.sideToMove] & board.pieceTypes[Bishop - 1];
	while (occupations) {
		Square sq = lsBit(occupations);
	  movesFromSquares[sq] = piece_moves(Bishop, sq, movesContext, kingSq, board);
	  occupations &= occupations - 1;
	}
	occupations = board.side[board.sideToMove] & board.pieceTypes[Rook - 1];
	while (occupations) {
		Square sq = lsBit(occupations);
	  movesFromSquares[sq] = piece_moves(Rook, sq, movesContext, kingSq, board);
	  occupations &= occupations - 1;
	}
	occupations = board.side[board.sideToMove] & board.pieceTypes[Queen - 1];
	while (occupations) {
		Square sq = lsBit(occupations);
	  movesFromSquares[sq] = piece_moves(Queen, sq, movesContext, kingSq, board);
	  occupations &= occupations - 1;
	}
	occupations = board.side[board.sideToMove] & board.pieceTypes[Pawn - 1];
	while (occupations) {
		Square sq = lsBit(occupations);
	  movesFromSquares[sq] = piece_moves(Pawn, sq, movesContext, kingSq, board);
	  occupations &= occupations - 1;
	}
	occupations = board.side[board.sideToMove] & board.pieceTypes[Knight - 1];
	while (occupations) {
		Square sq = lsBit(occupations);
	  movesFromSquares[sq] = piece_moves(Knight, sq, movesContext, kingSq, board);
	  occupations &= occupations - 1;
	}
  	
exit:
  board.isStaleMate = false; board.isMate = false;
 	if (!board.num_moves) {
		if (board.isCheck) {
			board.isMate = true;
			board.isCheck = false;
		}
		else board.isStaleMate = true;
	}
}*/

void isCheckMateStaleMate(Board& board) {
 	KingSquare kingSq;
	Square kingSquare = getKingSquare(board, kingSq);
	MovesContext movesContext = {};
	uint64_t attackedSquares = getAttackedSquares(board, movesContext);
	kingMoves(board, kingSquare, kingSq, movesContext, attackedSquares);
	uint64_t occupations;
	if (movesContext.num_checkers > 1) goto exit;
	
	//legal other moves
	occupations = board.side[board.sideToMove] & board.pieceTypes[Bishop - 1];
	while (occupations) {
		Square sq = lsBit(occupations);
	  piece_moves(Bishop, sq, movesContext, kingSq, board);
	  occupations &= occupations - 1;
	}
	occupations = board.side[board.sideToMove] & board.pieceTypes[Rook - 1];
	while (occupations) {
		Square sq = lsBit(occupations);
	  piece_moves(Rook, sq, movesContext, kingSq, board);
	  occupations &= occupations - 1;
	}
	occupations = board.side[board.sideToMove] & board.pieceTypes[Queen - 1];
	while (occupations) {
		Square sq = lsBit(occupations);
	  piece_moves(Queen, sq, movesContext, kingSq, board);
	  occupations &= occupations - 1;
	}
	occupations = board.side[board.sideToMove] & board.pieceTypes[Pawn - 1];
	while (occupations) {
		Square sq = lsBit(occupations);
	  piece_moves(Pawn, sq, movesContext, kingSq, board);
	  occupations &= occupations - 1;
	}
	occupations = board.side[board.sideToMove] & board.pieceTypes[Knight - 1];
	while (occupations) {
		Square sq = lsBit(occupations);
	  piece_moves(Knight, sq, movesContext, kingSq, board);
	  occupations &= occupations - 1;
	}
  	
exit:
  board.isStaleMate = false; board.isMate = false;
 	if (!board.num_moves) {
		if (board.isCheck) {
			board.isMate = true;
			board.isCheck = false;
		}
		else board.isStaleMate = true;
	}
}

//fast-forward a valid uci move on a given board without StateInfo
//returns capturedType for updateHash()
PieceType ff_move(Board& board, Move& move) {
  const Color oppColor = OPP_COLOR(board.sideToMove);
	const Piece movingPiece = board.piecesOnSquares[move.src];
	const PieceType mpType = PC_TYPE(board.piecesOnSquares[move.src]);

	//remove the piece from its source square
	uint64_t srcBit = (1ULL << move.src);
	board.side[board.sideToMove] ^= srcBit;
	board.pieceTypes[mpType - 1] ^= srcBit;
	board.piecesOnSquares[move.src] = PieceNone;

	//rook moves
	//need to find out if this is a castling rook and castling rights exist, to update castling rooks
	if ((mpType == Rook) && ((unsigned int *)board.castlingRook)[0] != 0x08080808) {
  	Castling castlingSide = CastlingNone; //0b00
		const Rank rookRank = board.sideToMove == ColorWhite ? Rank1 : Rank8;
		if (SQ_RANK(move.src) == rookRank) { //rook is on its initial rank
     	const File mpFile = SQ_FILE(move.src);
			if (mpFile == board.castlingRook[board.sideToMove][0]) castlingSide = CastlingKingside;
			else if (mpFile == board.castlingRook[board.sideToMove][1]) castlingSide = CastlingQueenside;
			if (castlingSide) {
				//update castling rooks
				board.castlingRook[board.sideToMove][castlingSide - 1] = FileNone;
			}
		}
	}
	move.type = MoveTypeNormal; //init type
	PieceType capturedType = PieceTypeNone; //init capturedType
	//normal capture
	uint64_t dstBit = 1ULL << move.dst;
	if (dstBit & board.side[oppColor]) {
		//printf("ff_move() debug: opponent occupations %llx, dstBit %llx\n", board.side[oppColor], dstBit);
		move.type = MoveTypeCapture;
		capturedType = PC_TYPE(board.piecesOnSquares[move.dst]);
		//debug
		if (capturedType == King) {
			char fen[MAX_FEN_STRING_LEN];
			board2fen(board, fen);
			printf("ff_move() error: captured king! fen %s, %s move %s%s\n", fen, piece[movingPiece], square[move.src], square[move.dst]);
			writeDebug(board);
			exit(1);
		}
		board.side[oppColor] ^= dstBit;
		board.pieceTypes[capturedType - 1] ^= dstBit;
		//if captured piece is a castling rook, then remove the castling rights of the opponent on that side
		if (capturedType == Rook) {
			const Rank rookRank = oppColor == ColorWhite ? Rank1 : Rank8;
			if (SQ_RANK(move.dst) == rookRank) {
	     	const File rookFile = SQ_FILE(move.dst);
				if (rookFile == board.castlingRook[oppColor][0]) //short castling opponent rook file
					board.castlingRook[oppColor][0] = FileNone;
				else if (rookFile == board.castlingRook[oppColor][1]) //long castling opponent rook file
					board.castlingRook[oppColor][1] = FileNone;
			}
		} 
	}

  //en passant capture and en passant move
	board.enPassant = FileNone;
	int diff;
	if (mpType == Pawn && move.type != MoveTypeCapture) {
  	diff = abs(move.src - move.dst);
		if (diff == 7 || diff == 9) { 
		  move.type = MoveTypeEnPassantCapture;
			const Square capturedPawnSquare = board.sideToMove == ColorWhite ? (Square)(move.dst - 8) : (Square)(move.dst + 8);
			uint64_t bitSq = 1ULL << capturedPawnSquare;
			board.side[oppColor] ^= bitSq;
			board.pieceTypes[Pawn - 1] ^= bitSq;
			board.piecesOnSquares[capturedPawnSquare] = PieceNone;
		} else if (diff == 16) {
			//all opponent pawns
			uint64_t pawns = board.side[oppColor] & board.pieceTypes[Pawn - 1];
	  	const File mpFile = SQ_FILE(move.src);
			//opponent pawns on Rank 4 or 5 depending on the side to move
			if (board.sideToMove == ColorWhite) pawns &= RANK4;
			else pawns &= RANK5;
			//opponent pawns on adjacent files (adjacent to the moving pawn from its initial rank to rank 4 or 5)
			if (mpFile == FileA) pawns &= files_bb[mpFile + 1];
			else if (mpFile == FileH) pawns &= files_bb[mpFile - 1];
			else pawns &= (files_bb[mpFile + 1] | files_bb[mpFile - 1]);
			//if there are such opponent pawns, then the move is en passant
			if (pawns) {
				move.type = MoveTypeEnPassant;
				board.enPassant = mpFile;
			}		
		}
	}
  
	//castling
	if (mpType == King) {
		//special case of chess 960 castling - in uci notation the king captures its own rook but moves to its standard dst
		if (board.isChess960 && board.piecesOnSquares[move.dst] == PC(board.sideToMove, Rook)) {
  		const File dstFile = SQ_FILE(move.dst);
			if (dstFile == board.castlingRook[board.sideToMove][0]) move.type = MoveTypeCastlingKingside;
			else if (dstFile == board.castlingRook[board.sideToMove][1]) move.type = MoveTypeCastlingQueenside;
			else {
				char fen[MAX_FEN_STRING_LEN] = "";
				printf("ff_move() error: illegal castling move in chess 960: the %s rook on %s is not a castling one, FEN %s\n", board.sideToMove == ColorWhite ? "white" : "black", square[move.dst], board2fen(board, fen));
				exit(-1);				
			}
		} else if (move.dst - move.src == 2) move.type = MoveTypeCastlingKingside;
		else if (move.dst - move.src == -2) move.type = MoveTypeCastlingQueenside;
		if (move.type == MoveTypeCastlingKingside || move.type == MoveTypeCastlingQueenside) {
			Square dstKingSquare, dstRookSquare, srcRookSquare;
			Piece pieceRook;
			if (board.sideToMove == ColorWhite) {
				pieceRook = WhiteRook;
				if (move.type == MoveTypeCastlingKingside) {
					dstKingSquare = SquareG1;
					dstRookSquare = SquareF1;
					srcRookSquare = SQ(Rank1, board.castlingRook[0][0]);
				} else { //MoveTypeCastlingQueenside
					dstKingSquare = SquareC1;
					dstRookSquare = SquareD1;
					srcRookSquare = SQ(Rank1, board.castlingRook[0][1]);
				}
			} else {
				pieceRook = BlackRook;
				if (move.type == MoveTypeCastlingKingside) {
					dstKingSquare = SquareG8;
					dstRookSquare = SquareF8;
					srcRookSquare = SQ(Rank8, board.castlingRook[1][0]);
				} else {
					dstKingSquare = SquareC8;
					dstRookSquare = SquareD8;
					srcRookSquare = SQ(Rank8, board.castlingRook[1][1]);
				}
			}
			//remove castling rook from its source square taking care of
			//Chess960 case of rook occupying king's destination square - make sure we are not removing (overwriting) the king with PieceNone! - this check is unnecessary because later we will overwrite it anyway with the king
			//if (srcRookSquare != dstKingSquare) board.piecesOnSquares[srcRookSquare] = PieceNone;
			board.piecesOnSquares[srcRookSquare] = PieceNone;
			//and put it to its destination square
			board.piecesOnSquares[dstRookSquare] = pieceRook;
			//update occupations
			//xor out the rook on its source square
			uint64_t bitSq = 1ULL << srcRookSquare;
			board.side[board.sideToMove] ^= bitSq;
			board.pieceTypes[Rook - 1] ^= bitSq;
			//add the rook to its destination square
			bitSq = 1ULL << dstRookSquare;
			board.side[board.sideToMove] |= bitSq;
			board.pieceTypes[Rook - 1] |= bitSq;
			//chess960 king destination update
	  	if (board.isChess960) move.dst = dstKingSquare;
		}		
		//set castling rook to none
		board.castlingRook[board.sideToMove][0] = FileNone;
		board.castlingRook[board.sideToMove][1] = FileNone;
	}
	
	//promotion
	if (move.promoType != PieceTypeNone) {
		board.piecesOnSquares[move.dst] = PC(board.sideToMove, move.promoType);
		board.side[board.sideToMove] |= dstBit;
		board.pieceTypes[move.promoType - 1] |= dstBit;
	} 
	//other move
	else {
		//move the piece to its destination
		board.piecesOnSquares[move.dst] = movingPiece;
		uint64_t bitSq = 1ULL << move.dst;
		board.side[board.sideToMove] |= bitSq;
		board.pieceTypes[mpType - 1] |= bitSq;
	}
				
	//increment halfmove clock if not a pawn's move and not a capture
	if (mpType != Pawn && move.type != MoveTypeCapture) board.halfmoveClock++;
	else board.halfmoveClock = 0;
	
	//increment move number if it was black's move
	if (board.sideToMove == ColorBlack) board.moveNumber++;
	
	//toggle SideToMove
	board.sideToMove = board.sideToMove == ColorWhite ? ColorBlack : ColorWhite;

	return capturedType;
}

//slightly heavier version of ff_move with saving the state in StateInfo, so that undo_move() can work
//returns captured type
PieceType do_move(Board& board, Move& move, StateInfo& state) {
  const Color oppColor = OPP_COLOR(board.sideToMove);
	const PieceType mpType = PC_TYPE(board.piecesOnSquares[move.src]);
  
  //copy piece in src square before removing it
  const Piece movingPiece = board.piecesOnSquares[move.src];
	//remove the piece from its source square
	uint64_t bitSq = 1ULL << move.src;
	board.side[board.sideToMove] ^= bitSq;
	board.pieceTypes[mpType - 1] ^= bitSq;
	board.piecesOnSquares[move.src] = PieceNone;

	//preserve castlingRook array for undo_move() - it simpler to do it unconditionally
	//because it may be changed in 3 different occasions: rook move, rook capture and castling
	((unsigned int *)state.castlingRook)[0] = ((unsigned int *)board.castlingRook)[0];

	//rook moves
	//need to find out if this is a castling rook and castling rights exist, to update castling rooks
	if ((mpType == Rook) && ((unsigned int *)board.castlingRook)[0] != 0x08080808) {
		const Rank rookRank = board.sideToMove == ColorWhite ? Rank1 : Rank8;
		if (SQ_RANK(move.src) == rookRank) { //rook is on its initial rank
     	const File mpFile = SQ_FILE(move.src); //need to find moving rook's file because we store it in castlingRook array
     	Castling castlingSide;
			if (mpFile == board.castlingRook[board.sideToMove][0]) castlingSide = CastlingKingside;
			else if (mpFile == board.castlingRook[board.sideToMove][1]) castlingSide = CastlingQueenside;
			else castlingSide = CastlingNone;
			if (castlingSide) {
				//update castling rooks
				board.castlingRook[board.sideToMove][castlingSide - 1] = FileNone;
			}
		}
	}
	move.type = MoveTypeNormal; //initialize/reset moving type
	state.capturedType = PieceTypeNone; //initialize/reset the captured type
	//normal capture
	uint64_t dstBit = 1ULL << move.dst;
	if (dstBit & board.side[oppColor]) { //or if (oppColor == PC_COLOR(board.piecesOnSquares[move.dst])
		move.type = MoveTypeCapture;
		//preserved captured type for undo_move()
		state.capturedType = PC_TYPE(board.piecesOnSquares[move.dst]);
		if (state.capturedType == King) {
			char fen[MAX_FEN_STRING_LEN];
			board2fen(board, fen);
			printf("do_move() error: captured king! fen %s, %s move %s%s\n", fen, piece[movingPiece], square[move.src], square[move.dst]);
			writeDebug(board);
			exit(1);
		}
		//remove the captured puece from occupations
		board.side[oppColor] ^= dstBit;
		board.pieceTypes[state.capturedType - 1] ^= dstBit;
		//if captured piece is a castling rook, then remove the castling rights of the opponent on that side
		if (state.capturedType == Rook) {
			const Rank rookRank = oppColor == ColorWhite ? Rank1 : Rank8;
			if (SQ_RANK(move.dst) == rookRank) {
	     	const File rookFile = SQ_FILE(move.dst);
				if (rookFile == board.castlingRook[oppColor][0]) {//short castling opponent rook file
					board.castlingRook[oppColor][0] = FileNone;
				}
				else if (rookFile == board.castlingRook[oppColor][1]) {//long castling opponent rook file
					board.castlingRook[oppColor][1] = FileNone;
				}
			}
		} 
	}
	
  //preserve en passant file for undo_move()
  state.enPassant = board.enPassant;
  //en passant capture and en passant move
	board.enPassant = FileNone; //init/reset board.enPassant
	int diff;
	if (mpType == Pawn && move.type != MoveTypeCapture) {
  	diff = abs(move.src - move.dst);
		if (diff == 7 || diff == 9) { //en passant capture
		  move.type = MoveTypeEnPassantCapture;
			const Square capturedPawnSquare = board.sideToMove == ColorWhite ? (Square)(move.dst - 8) : (Square)(move.dst + 8);
			bitSq = 1ULL << capturedPawnSquare;
			board.side[oppColor] ^= bitSq;
			board.pieceTypes[Pawn - 1] ^= bitSq;
			board.piecesOnSquares[capturedPawnSquare] = PieceNone;
		} else if (diff == 16) { //double pawn advance
			//all opponent pawns
			uint64_t pawns = board.side[oppColor] & board.pieceTypes[Pawn - 1];
	  	const File mpFile = SQ_FILE(move.src);
			//opponent pawns on Rank 4 or 5 depending on the side to move
			pawns &= board.sideToMove == ColorWhite ? RANK4 : RANK5;
			//opponent pawns on adjacent files (adjacent to the moving pawn from its initial rank to rank 4 or 5)
			if (mpFile == FileA) pawns &= files_bb[mpFile + 1];
			else if (mpFile == FileH) pawns &= files_bb[mpFile - 1];
			else pawns &= (files_bb[mpFile + 1] | files_bb[mpFile - 1]);
			//if there are such opponent pawns, then the move is en passant
			if (pawns) {
				move.type = MoveTypeEnPassant;
				board.enPassant = mpFile;
			}		
		}
	}
  
	//castling
	if (mpType == King) {
		//special case of chess 960 castling - in uci notation the king captures its own rook but moves to its standard dst
		if (board.isChess960 && board.piecesOnSquares[move.dst] == PC(board.sideToMove, Rook)) {
  		const File dstFile = SQ_FILE(move.dst);
			if (dstFile == board.castlingRook[board.sideToMove][0]) move.type = MoveTypeCastlingKingside;
			else if (dstFile == board.castlingRook[board.sideToMove][1]) move.type = MoveTypeCastlingQueenside;
			else {
				char fen[MAX_FEN_STRING_LEN] = "";
				printf("ff_move() error: illegal castling move in chess 960: the %s rook on %s is not a castling one, FEN %s\n", board.sideToMove == ColorWhite ? "white" : "black", square[move.dst], board2fen(board, fen));
				exit(-1);				
			}
		} else if (move.dst - move.src == 2) move.type = MoveTypeCastlingKingside;
		else if (move.src - move.dst == 2) move.type = MoveTypeCastlingQueenside;
		if (move.type == MoveTypeCastlingKingside || move.type == MoveTypeCastlingQueenside) {
			Square dstKingSquare, dstRookSquare, srcRookSquare;
			Piece pieceRook;
			if (board.sideToMove == ColorWhite) {
				pieceRook = WhiteRook;
				if (move.type == MoveTypeCastlingKingside) {
					dstKingSquare = SquareG1;
					dstRookSquare = SquareF1;
					srcRookSquare = SQ(Rank1, board.castlingRook[0][0]);
				} else { //MoveTypeCastlingQueenside
					dstKingSquare = SquareC1;
					dstRookSquare = SquareD1;
					srcRookSquare = SQ(Rank1, board.castlingRook[0][1]);
				}
			} else {
				pieceRook = BlackRook;
				if (move.type == MoveTypeCastlingKingside) {
					dstKingSquare = SquareG8;
					dstRookSquare = SquareF8;
					srcRookSquare = SQ(Rank8, board.castlingRook[1][0]);
				} else {
					dstKingSquare = SquareC8;
					dstRookSquare = SquareD8;
					srcRookSquare = SQ(Rank8, board.castlingRook[1][1]);
				}
			}
			//remove castling rook from its source square taking care of
			//Chess960 case of rook occupying king's destination square - make sure we are not removing (overwriting) the king with PieceNone! - this check seems to be unnecessary because later we would still put the king in its dst square
			//if (srcRookSquare != dstKingSquare) board.piecesOnSquares[srcRookSquare] = PieceNone;
			board.piecesOnSquares[srcRookSquare] = PieceNone;
			//and put it to its destination square
			board.piecesOnSquares[dstRookSquare] = pieceRook;
			//update occupations
			//xor out the rook on its source square
			bitSq = 1ULL << srcRookSquare;
			board.side[board.sideToMove] ^= bitSq;
			board.pieceTypes[Rook - 1] ^= bitSq;
			//add the rook to its destination square
			bitSq = 1ULL << dstRookSquare;
			board.side[board.sideToMove] |= bitSq;
			board.pieceTypes[Rook - 1] |= bitSq;
			//chess960 king destination update
	  	if (board.isChess960) move.dst = dstKingSquare;
		}
		//set castling rook to none
		board.castlingRook[board.sideToMove][0] = FileNone;
		board.castlingRook[board.sideToMove][1] = FileNone;
	}
	
	//promotion
	if (move.promoType != PieceTypeNone) {
		board.piecesOnSquares[move.dst] = PC(board.sideToMove, move.promoType);
		board.side[board.sideToMove] |= dstBit;
		board.pieceTypes[move.promoType - 1] |= dstBit;
	} 
	//other move
	else {
		//move the piece to its destination
		board.piecesOnSquares[move.dst] = movingPiece;
		//cannot use dstBit because of chess960 dst square update for king
		bitSq = 1ULL << move.dst;
		board.side[board.sideToMove] |= bitSq; 
		board.pieceTypes[mpType - 1] |= bitSq; 
	}
	
	//preserve halfmove clock for undo_move()
	state.halfmoveClock = board.halfmoveClock;
	//preserve check info
	state.isCheck = board.isCheck;
	//increment halfmove clock if not a pawn's move and not a capture
	if (mpType != Pawn && move.type != MoveTypeCapture) board.halfmoveClock++;
	else board.halfmoveClock = 0;
	
	//increment move number if it was black's move
	if (board.sideToMove == ColorBlack) board.moveNumber++;
	
	//toggle SideToMove
	board.sideToMove = board.sideToMove == ColorWhite ? ColorBlack : ColorWhite;

	return state.capturedType;
}

//even heavier version of do_move with updating DirtyPiece for NNUE incremental updates
//returns captured type
PieceType do_move_dp(Board& board, Move& move, StateInfo& state, Stockfish::DirtyPiece& dp) {
  const Color oppColor = OPP_COLOR(board.sideToMove);
	const PieceType mpType = PC_TYPE(board.piecesOnSquares[move.src]);
	assert(mpType != PieceTypeNone);
  
  //copy piece in src square before removing it
  dp.pc = board.piecesOnSquares[move.src];
  dp.from = move.src;
  dp.to = move.dst;
  dp.add_sq = SquareNone;
  dp.remove_sq = SquareNone;
	//remove the piece from its source square
	uint64_t bitSq = 1ULL << move.src;
	board.side[board.sideToMove] ^= bitSq;
	board.pieceTypes[mpType - 1] ^= bitSq;
	board.piecesOnSquares[move.src] = PieceNone;

	//preserve castlingRook array for undo_move() - it simpler to do it unconditionally
	//because it may be changed in 3 different occasions: rook move, rook capture and castling
	((unsigned int *)state.castlingRook)[0] = ((unsigned int *)board.castlingRook)[0];

	//rook moves
	//need to find out if this is a castling rook and castling rights exist, to update castling rooks
	if ((mpType == Rook) && ((unsigned int *)board.castlingRook)[0] != 0x08080808) {
		const Rank rookRank = board.sideToMove == ColorWhite ? Rank1 : Rank8;
		if (SQ_RANK(move.src) == rookRank) { //rook is on its initial rank
     	const File mpFile = SQ_FILE(move.src); //need to find moving rook's file because we store it in castlingRook array
     	Castling castlingSide;
			if (mpFile == board.castlingRook[board.sideToMove][0]) castlingSide = CastlingKingside;
			else if (mpFile == board.castlingRook[board.sideToMove][1]) castlingSide = CastlingQueenside;
			else castlingSide = CastlingNone;
			if (castlingSide) {
				//update castling rooks
				board.castlingRook[board.sideToMove][castlingSide - 1] = FileNone;
			}
		}
	}
	move.type = MoveTypeNormal; //initialize/reset moving type
	state.capturedType = PieceTypeNone; //initialize/reset the captured type
	//normal capture
	uint64_t dstBit = 1ULL << move.dst;
	if (dstBit & board.side[oppColor]) { //or if (oppColor == PC_COLOR(board.piecesOnSquares[move.dst])
	  dp.remove_sq = move.dst;
	  dp.remove_pc = board.piecesOnSquares[move.dst];
		move.type = MoveTypeCapture;
		//preserved captured type for undo_move()
		state.capturedType = PC_TYPE(board.piecesOnSquares[move.dst]);
		if (state.capturedType == King) {
			char fen[MAX_FEN_STRING_LEN];
			board2fen(board, fen);
			printf("do_move_dp() error: captured king! fen %s, %s move %s%s\n", fen, pieceType[mpType], square[move.src], square[move.dst]);
			writeDebug(board);
			exit(1);
		}		
		//remove the captured puece from occupations
		board.side[oppColor] ^= dstBit;
		board.pieceTypes[state.capturedType - 1] ^= dstBit;
		//if captured piece is a castling rook, then remove the castling rights of the opponent on that side
		if (state.capturedType == Rook) {
			const Rank rookRank = oppColor == ColorWhite ? Rank1 : Rank8;
			if (SQ_RANK(move.dst) == rookRank) {
	     	const File rookFile = SQ_FILE(move.dst);
				if (rookFile == board.castlingRook[oppColor][0]) {//short castling opponent rook file
					board.castlingRook[oppColor][0] = FileNone;
				}
				else if (rookFile == board.castlingRook[oppColor][1]) {//long castling opponent rook file
					board.castlingRook[oppColor][1] = FileNone;
				}
			}
		} 
	}
	
  //preserve en passant file for undo_move()
  state.enPassant = board.enPassant;
  //en passant capture and en passant move
	board.enPassant = FileNone; //init/reset board.enPassant
	int diff;
	if (mpType == Pawn && move.type != MoveTypeCapture) {
  	diff = abs(move.src - move.dst);
		if (diff == 7 || diff == 9) { //en passant capture
		  move.type = MoveTypeEnPassantCapture;
			const Square capturedPawnSquare = board.sideToMove == ColorWhite ? (Square)(move.dst - 8) : (Square)(move.dst + 8);
			dp.remove_sq = capturedPawnSquare;
			dp.remove_pc = PC(oppColor, Pawn);
			bitSq = 1ULL << capturedPawnSquare;
			board.side[oppColor] ^= bitSq;
			board.pieceTypes[Pawn - 1] ^= bitSq;
			board.piecesOnSquares[capturedPawnSquare] = PieceNone;
		} else if (diff == 16) { //double pawn advance
			//all opponent pawns
			uint64_t pawns = board.side[oppColor] & board.pieceTypes[Pawn - 1];
	  	const File mpFile = SQ_FILE(move.src);
			//opponent pawns on Rank 4 or 5 depending on the side to move
			pawns &= board.sideToMove == ColorWhite ? RANK4 : RANK5;
			//opponent pawns on adjacent files (adjacent to the moving pawn from its initial rank to rank 4 or 5)
			if (mpFile == FileA) pawns &= files_bb[mpFile + 1];
			else if (mpFile == FileH) pawns &= files_bb[mpFile - 1];
			else pawns &= (files_bb[mpFile + 1] | files_bb[mpFile - 1]);
			//if there are such opponent pawns, then the move is en passant
			if (pawns) {
				move.type = MoveTypeEnPassant;
				board.enPassant = mpFile;
			}		
		}
	}
  
	//castling
	if (mpType == King) {
		//special case of chess 960 castling - in uci notation the king captures its own rook but moves to its standard dst
		if (board.isChess960 && board.piecesOnSquares[move.dst] == PC(board.sideToMove, Rook)) {
  		const File dstFile = SQ_FILE(move.dst);
			if (dstFile == board.castlingRook[board.sideToMove][0]) move.type = MoveTypeCastlingKingside;
			else if (dstFile == board.castlingRook[board.sideToMove][1]) move.type = MoveTypeCastlingQueenside;
			else {
				char fen[MAX_FEN_STRING_LEN] = "";
				printf("ff_move() error: illegal castling move in chess 960: the %s rook on %s is not a castling one, FEN %s\n", board.sideToMove == ColorWhite ? "white" : "black", square[move.dst], board2fen(board, fen));
				exit(-1);				
			}
		} else if (move.dst - move.src == 2) move.type = MoveTypeCastlingKingside;
		else if (move.src - move.dst == 2) move.type = MoveTypeCastlingQueenside;
		if (move.type == MoveTypeCastlingKingside || move.type == MoveTypeCastlingQueenside) {
			Square dstKingSquare, dstRookSquare, srcRookSquare;
			Piece pieceRook;
			if (board.sideToMove == ColorWhite) {
				pieceRook = WhiteRook;
				if (move.type == MoveTypeCastlingKingside) {
					dstKingSquare = SquareG1;
					dstRookSquare = SquareF1;
					srcRookSquare = SQ(Rank1, board.castlingRook[0][0]);
				} else { //MoveTypeCastlingQueenside
					dstKingSquare = SquareC1;
					dstRookSquare = SquareD1;
					srcRookSquare = SQ(Rank1, board.castlingRook[0][1]);
				}
			} else {
				pieceRook = BlackRook;
				if (move.type == MoveTypeCastlingKingside) {
					dstKingSquare = SquareG8;
					dstRookSquare = SquareF8;
					srcRookSquare = SQ(Rank8, board.castlingRook[1][0]);
				} else {
					dstKingSquare = SquareC8;
					dstRookSquare = SquareD8;
					srcRookSquare = SQ(Rank8, board.castlingRook[1][1]);
				}
			}
			//remove castling rook from its source square taking care of
			//Chess960 case of rook occupying king's destination square - make sure we are not removing (overwriting) the king with PieceNone! - this check seems to be unnecessary because later we would still put the king in its dst square
			//if (srcRookSquare != dstKingSquare) board.piecesOnSquares[srcRookSquare] = PieceNone;
			board.piecesOnSquares[srcRookSquare] = PieceNone;
			//and put it to its destination square
			board.piecesOnSquares[dstRookSquare] = pieceRook;
			//update occupations
			//xor out the rook on its source square
			bitSq = 1ULL << srcRookSquare;
			board.side[board.sideToMove] ^= bitSq;
			board.pieceTypes[Rook - 1] ^= bitSq;
			//add the rook to its destination square
			bitSq = 1ULL << dstRookSquare;
			board.side[board.sideToMove] |= bitSq;
			board.pieceTypes[Rook - 1] |= bitSq;
			//chess960 king destination update
	  	if (board.isChess960) move.dst = dstKingSquare;
	  	dp.to = dstKingSquare;
      dp.remove_pc = dp.add_pc = pieceRook;
      dp.remove_sq = srcRookSquare;
      dp.add_sq = dstRookSquare;
		}
		//set castling rook to none
		board.castlingRook[board.sideToMove][0] = FileNone;
		board.castlingRook[board.sideToMove][1] = FileNone;
	}
	
	//promotion
	if (move.promoType != PieceTypeNone) {
		board.piecesOnSquares[move.dst] = PC(board.sideToMove, move.promoType);
		board.side[board.sideToMove] |= dstBit;
		board.pieceTypes[move.promoType - 1] |= dstBit;
		dp.add_pc = board.piecesOnSquares[move.dst];
		dp.add_sq = move.dst;
		dp.to = SquareNone;
	} 
	//other move
	else {
		//move the piece to its destination
		board.piecesOnSquares[move.dst] = dp.pc;
		//cannot use dstBit because of chess960 dst square update for king
		bitSq = 1ULL << move.dst;
		board.side[board.sideToMove] |= bitSq; 
		board.pieceTypes[mpType - 1] |= bitSq; 
	}
	
	//preserve halfmove clock for undo_move()
	state.halfmoveClock = board.halfmoveClock;
	//preserve check info
	state.isCheck = board.isCheck;
	//increment halfmove clock if not a pawn's move and not a capture
	if (mpType != Pawn && move.type != MoveTypeCapture) board.halfmoveClock++;
	else board.halfmoveClock = 0;
	
	//increment move number if it was black's move
	if (board.sideToMove == ColorBlack) board.moveNumber++;
	
	//toggle SideToMove
	board.sideToMove = board.sideToMove == ColorWhite ? ColorBlack : ColorWhite;

	return state.capturedType;
}

//reverse do_move()
void undo_move(Board& board, const Move& move, const StateInfo& state) {
	//toggle SideToMove
	board.sideToMove = board.sideToMove == ColorWhite ? ColorBlack : ColorWhite;

  const Color oppColor = OPP_COLOR(board.sideToMove);
	PieceType mpType = PC_TYPE(board.piecesOnSquares[move.dst]);
	const uint64_t dstBit = 1ULL << move.dst;
	
	//restore castlingRook array
	((unsigned int *)board.castlingRook)[0] = ((unsigned int *)state.castlingRook)[0];

  //restore en passant file
  board.enPassant = state.enPassant;

	//restore halfmove clock
	board.halfmoveClock = state.halfmoveClock;
	
	//restore check info
	board.isCheck = state.isCheck;

	//decrement move number if it was black's move
	if (board.sideToMove == ColorBlack) board.moveNumber--;

	//remove the piece from its destination
	if (move.type != MoveTypeCapture) board.piecesOnSquares[move.dst] = PieceNone;

	//remove the piece in dst square from occupations and update mpType to Pawn in case of promotion
	board.side[board.sideToMove] ^= dstBit;
	if (move.promoType != PieceTypeNone) {
		mpType = Pawn;
		board.pieceTypes[move.promoType - 1] ^= dstBit;
	} else board.pieceTypes[mpType - 1] ^= dstBit;
	
	//normal capture
	if (move.type == MoveTypeCapture) {
		//restore captured piece
		board.piecesOnSquares[move.dst] = PC(oppColor, state.capturedType);
	  board.side[oppColor] |= dstBit;
	  board.pieceTypes[state.capturedType - 1] |= dstBit;
	}
  //en passant capture
	else if (move.type == MoveTypeEnPassantCapture) {
	  	//restore en passant captured pawn
			const int capturedPawnSquare = board.sideToMove == ColorWhite ? move.dst - 8 : move.dst + 8;
			board.piecesOnSquares[capturedPawnSquare] = PC(oppColor, Pawn);
			uint64_t bitSq = 1ULL << capturedPawnSquare;
			board.side[oppColor] |= bitSq;
			board.pieceTypes[Pawn - 1] |= bitSq;
	}  
	//castling
	else if (move.type == MoveTypeCastlingKingside || move.type == MoveTypeCastlingQueenside) {
		Square dstKingSquare, dstRookSquare, srcRookSquare;
		Piece pieceRook;
		if (board.sideToMove == ColorWhite) {
			pieceRook = WhiteRook;
			if (move.type == MoveTypeCastlingKingside) {
				dstKingSquare = SquareG1;
				dstRookSquare = SquareF1;
				srcRookSquare = SQ(Rank1, board.castlingRook[0][0]);
			} else { //MoveTypeCastlingQueenside
				dstKingSquare = SquareC1;
				dstRookSquare = SquareD1;
				srcRookSquare = SQ(Rank1, board.castlingRook[0][1]);
			}
		} else {
			pieceRook = BlackRook;
			if (move.type == MoveTypeCastlingKingside) {
				dstKingSquare = SquareG8;
				dstRookSquare = SquareF8;
				srcRookSquare = SQ(Rank8, board.castlingRook[1][0]);
			} else { //MoveTypeCastlingQueenside
				dstKingSquare = SquareC8;
				dstRookSquare = SquareD8;
				srcRookSquare = SQ(Rank8, board.castlingRook[1][1]);
			}
		}
		//restore castling rook in its source square
		board.piecesOnSquares[srcRookSquare] = pieceRook;
		uint64_t bitSq = 1ULL << srcRookSquare;
		board.side[board.sideToMove] |= bitSq;
		board.pieceTypes[Rook - 1] |= bitSq;
		//and remove it from its destination square
		board.piecesOnSquares[dstRookSquare] = PieceNone;
		bitSq = 1ULL << dstRookSquare;
		board.side[board.sideToMove] ^= bitSq;
		board.pieceTypes[Rook - 1] ^= bitSq;
	}
	
	//restore the piece in its source square
	board.piecesOnSquares[move.src] = PC(board.sideToMove, mpType);
	uint64_t bitSq = 1ULL << move.src;
	board.side[board.sideToMove] |= bitSq;
	board.pieceTypes[mpType - 1] |= bitSq;		
}

//#ifdef __cplusplus
//}
//#endif
