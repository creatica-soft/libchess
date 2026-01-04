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

#ifdef __cplusplus
extern "C" {
#endif
#ifdef _WIN32
#include "windows.h"
#include "stdio.h"
#endif

struct Square {
  int file = FileNone;
  int rank = RankNone;
  int diag = DiagonalNone;
  int antidiag = AntidiagonalNone;
  unsigned long long bit = 0;
};

struct MovesContext {
  int shiftedColor;
  int shiftedOpponentColor;
  unsigned long long pinnedPieces;
  unsigned long long pinningPieces;
  unsigned long long blockingSquares;
  unsigned long long checker;
  int checkerSquare;
};

void drawMoves(const struct Board * board, const int sq) {
	int squareIndex = SquareA1, sqStart = SquareA1;
	char buffer[8][256] = {};
	int rank = Rank1;
	
	unsigned long long moves = board->movesFromSquares[sq];
	const char * m = (board->fen->sideToMove == ColorWhite && (board->piecesOnSquares[sq] >> 3) == ColorWhite) || (board->fen->sideToMove == ColorBlack && (board->piecesOnSquares[sq] >> 3) == ColorBlack) ? " moves (x): " : " controlled squares (x): ";

	printf("%s on %s %s %llx\n", pieceName[board->piecesOnSquares[sq]], squareName[sq], m, moves);
	char s[6] = {};
	while ((squareIndex = lsBit(moves)) < SquareNone) {
		moves ^= (1ULL << squareIndex);
		for (int k = sqStart; k < squareIndex; k++) {
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
	for (int k = sqStart; k < SquareNone; k++) {
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
void writeDebug(const struct Board * board, bool displayMoves) {
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
	wchar_t pieceLetter[] = {L' ', 0x2659, 0x2658, 0x2657, 0x2656, 0x2655, 0x2654, L'C', L'*', 0x265F, 0x265E, 0x265D, 0x265C, 0x265B, 0x265A, L'c'};
	int rank = Rank1;
	//printf("chess board\n");
	//for (int r = Rank1; r <= Rank8; r++) buffer[r][0] = '\0';
	for (int i = SquareA1; i <= SquareH8; i++) {
		wchar_t s[16] = {};
		int	row = i / 8; // 0..7
		int	col = i % 8; // 0..7
		if ((i + 1) % 8 == 0) {
			if ((row % 2 == 0 && col % 2 == 0) || (row % 2 && col % 2))
			  swprintf(s, sizeof(s), L"\u2502\033[47m%lc\033[0m\u2502", pieceLetter[board->piecesOnSquares[i]]);			
			else
			  swprintf(s, sizeof(s), L"\u2502%lc\u2502", pieceLetter[board->piecesOnSquares[i]]);
			wcscat(buffer[rank++], s);
		}
		else {
			if ((row % 2 == 0 && col % 2 == 0) || (row % 2 && col % 2))
			  swprintf(s, sizeof(s), L"\u2502\033[47m%lc\033[0m", pieceLetter[board->piecesOnSquares[i]]);			
			else
			  swprintf(s, sizeof(s), L"\u2502%lc", pieceLetter[board->piecesOnSquares[i]]);
			wcscat(buffer[rank], s);
		}
	}

	printf("\u250C\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2510\n");
	for (signed char s = 7; s >= 0; s--) {
		printf("%ls\n", buffer[s]);
		printf("\u2514\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2518\n");
	}
	if (displayMoves) {
		int  square;
		unsigned long long d = board->occupations[PieceNameAny];
		while ((square = lsBit(d)) < 64) {
			d ^= (1ULL << square);
			drawMoves(board, square);
		}
	}
}

int reconcile(struct Board * board) {
	int err = 0;
	for (int i = SquareA1; i <= SquareH8; i++) {
		if (!(board->occupations[board->piecesOnSquares[i]] & (1ULL << i))) {
			printf("reconcile() error: piecesOnSquares[%s] %s does not match its occupation bitboard %llx, fen %s\n", squareName[i], pieceName[board->piecesOnSquares[i]], board->occupations[board->piecesOnSquares[i]], board->fen->fenString);
			err = 1;
		}
	}
	for (int j = PieceNameNone; j <= PieceNameBlack; j++) {
		unsigned long long o = board->occupations[j];
		while (o) {
  		unsigned char s = lsBit(o);
			if (board->piecesOnSquares[s] != j) {
				switch (j) {
				case PieceNameAny:
					if (board->piecesOnSquares[s] == PieceNameNone)
						break;
				case PieceNameWhite:
					if (PC_COLOR(board->piecesOnSquares[s]) == ColorWhite)
						break;
				case PieceNameBlack:
					if (PC_COLOR(board->piecesOnSquares[s]) == ColorBlack)
						break;
				default:
					printf("reconcile() error: %s piece in occupations does not match one in piecesOnSquares[%s] %s, fen %s\n", pieceName[j], squareName[s], pieceName[board->piecesOnSquares[s]], board->fen->fenString);
					err = 1;
				}
			}
			o &= o - 1;
		}
	}
	return err;
}

//updates board->fen->fenString only
void updateFen(struct Board * board) {
    assert(board && board->fen);

    char position[73] = {0};
    char *pos_ptr = position;
    for (int rank = 7; rank >= 0; rank--) {
        int empty_count = 0;
        for (int file = 0; file <= 7; file++) {
            int sq = SQ(rank, file);
            int piece = board->piecesOnSquares[sq];
            if (piece == PieceNameNone) {
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
    if (board->fen->castlingRights == CastlingRightsWhiteNoneBlackNone) {
        castling[0] = '-';
    } else {
        unsigned char idx = 0;
        for (int color = ColorWhite; color <= ColorBlack; color++) {
            if (board->fen->isChess960) {
                if ((board->fen->castlingRights >> (color << 1)) & CastlingSideKingside) {
                    castling[idx++] = (color == ColorWhite) ? toupper(board->fen->castlingRook[0][0] + 'a') : board->fen->castlingRook[0][1] + 'a';
                }
                if ((board->fen->castlingRights >> (color << 1)) & CastlingSideQueenside) {
                    castling[idx++] = (color == ColorWhite) ? toupper(board->fen->castlingRook[1][0] + 'a') : board->fen->castlingRook[1][1] + 'a';
                }
            } else {
                if ((board->fen->castlingRights >> (color << 1)) & CastlingSideKingside) {
                    castling[idx++] = (color == ColorWhite) ? 'K' : 'k';
                }
                if ((board->fen->castlingRights >> (color << 1)) & CastlingSideQueenside) {
                    castling[idx++] = (color == ColorWhite) ? 'Q' : 'q';
                }
            }
        }
    }
    char en_passant[3] = {0};
    if (board->fen->enPassant == FileNone) {
        en_passant[0] = '-';
    } else {
        en_passant[0] = board->fen->enPassant + 'a';
        en_passant[1] = (board->fen->sideToMove == ColorWhite) ? '6' : '3';
    }
    sprintf(board->fen->fenString, "%s %c %s %s %d %d", position, fenColor[board->fen->sideToMove], castling, en_passant, board->fen->halfmoveClock, board->fen->moveNumber);
}

//returns en passant bit square if en passant move is legal or 0 otherwise
unsigned long long enPassantLegalBit(struct Board * board) {
  int kingSq = lsBit(board->occupations[PC(board->fen->sideToMove, King)]);
  int kingRank = SQ_RANK(kingSq);
  int enPassantRank = board->fen->sideToMove == ColorWhite ? Rank6 : Rank3;

  for (int pt = Rook; pt <= Queen; pt++) {
    unsigned long long oppRooksQueens = board->occupations[PC(OPP_COLOR(board->fen->sideToMove), pt)];
    while (oppRooksQueens) { //a loop for all opponent's rooks and queens
      int oppSq = lsBit(oppRooksQueens);
      if (kingRank != SQ_RANK(oppSq)) {
        oppRooksQueens &= oppRooksQueens - 1;
        continue; //king and opponent's rook or queen are not on the same rank, no check from en passant move is possible
      }
      //move the king towards opponent's rook or queen
      int dir = (oppSq > kingSq) ? 1 : -1;
      int shift = kingSq + dir;
      while (shift >= 0 && shift < 64 && SQ_RANK(shift) == kingRank) {
        if (board->piecesOnSquares[shift] != PieceNameNone) { //if there is something on the way
          if (shift == oppSq) return 0; //if it is the rook or the queen, then en passant is illegal because of check
          break; //something other than the rook or the queen - not a problem
        }
        shift += dir; //continue moving the king towards opponent's rook or queen
      }
      oppRooksQueens &= oppRooksQueens - 1; //next opponent's rook or queen
    }
  }
  return SQ_BIT(SQ(enPassantRank, board->fen->enPassant)); //en passant move is legal
}

//these two functions are based on the modern ray piece move generation technique 
//known as magic bitboards (see magic_bitboards.c)
//to use them, they must be initialized by calling init_magic_bitboards();
//and freed at the end by calling cleanup_magic_bitboards();
//we call get_bishop_moves() and get_root_moves() directly without these wrappers
//unsigned long long generate_bishop_moves(struct Board * board, int  sn) {
//  return get_bishop_moves(sn, board->occupations[PieceNameAny]);
//}
//unsigned long long generate_rook_moves(struct Board * board, int  sn) {
//  return get_rook_moves(sn, board->occupations[PieceNameAny]);
//}

//generates knight moves from a given square limited by board boundaries only
unsigned long long generateKnightMoves(struct Board * board, const int sq) {
  unsigned long long moves = 0;
  const int file = SQ_FILE(sq);
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
unsigned long long generateKingMoves(struct Board * board, const int sq) {
  unsigned long long moves = 0;
  const int file = SQ_FILE(sq);
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

unsigned long long rookPinFinder(struct Board * board, const int rookSquare, const int kingSquare) {
	unsigned long long d = 0;
	int ii = SQ_FILE(kingSquare), jj = SQ_RANK(kingSquare), shift = kingSquare; 
	//King and opponent rook (or queen) are on the same file
	if (SQ_FILE(rookSquare) == ii) {
		//We move the king along the file towards the opponent rook (or queen) until we bump into something.
		if (rookSquare > kingSquare) {
			while (jj++ < Rank8)
				if (board->piecesOnSquares[shift += 8] != PieceNameNone) { d = SQ_BIT(shift); break; }
		}
		else {
			while (jj-- > Rank1)
				if (board->piecesOnSquares[shift -= 8] != PieceNameNone) { d = SQ_BIT(shift); break; }
		}
	}
	//King and opponent rook (or queen) are on the same rank
	else if (SQ_RANK(rookSquare) == jj) {
		//We move the king along the rank towards the opponent rook (or queen) until we bump into something.
		if (rookSquare > kingSquare) {
			while (ii++ < FileH)
				if (board->piecesOnSquares[shift += 1] != PieceNameNone) { d = SQ_BIT(shift); break; }
		}
		else {
			while (ii-- > FileA)
				if (board->piecesOnSquares[shift -= 1] != PieceNameNone) { d = SQ_BIT(shift); break; }
		}
	}	
	//If that "something" is the same color as the king and if it is attacked
	//by opponet rook (or queen), then this "something" is pinned against the king
	return d & board->movesFromSquares[rookSquare] & board->occupations[PC(board->fen->sideToMove, PieceTypeAny)];
}

unsigned long long bishopPinFinder(struct Board * board, const int bishopSquare, const int kingSquare) {
	unsigned long long d = 0;
	int  ii = SQ_FILE(kingSquare), jj = SQ_RANK(kingSquare), shift = kingSquare;
	//King and opponent bishop (or queen) are on the same diagonal
	if (SQ_DIAG(bishopSquare) == 7 + jj - ii) {
		//We move the king along this diagonal towards the opponent bishop (or queen) until we bump into something.
		if (bishopSquare > kingSquare) {
			while (ii++ < FileH && jj++ < Rank8)
				if (board->piecesOnSquares[shift += 9] != PieceNameNone) { d = SQ_BIT(shift); break; }
		} else {
			while (ii-- > FileA && jj-- > Rank1)
				if (board->piecesOnSquares[shift -= 9] != PieceNameNone) { d = SQ_BIT(shift); break; }
		}
	}
	//King and opponent bishop (or queen) are on the same anti-diagonal
	else {
		if (SQ_ANTIDIAG(bishopSquare) == ii + jj) {
			//We move the king along this anti-diagonal towards the opponent bishop (or queen) until we bump into something.
			if (bishopSquare > kingSquare) {
				while (ii-- > FileA && jj++ < Rank8)
					if (board->piecesOnSquares[shift += 7] != PieceNameNone) { d = SQ_BIT(shift); break; }
			}
			else {
				while (ii++ < FileH && jj-- > Rank1)
					if (board->piecesOnSquares[shift -= 7] != PieceNameNone) { d = SQ_BIT(shift); break; }
			}
		}
	}
	//If that "something" is the same color as the king and if it is attacked
	//by opponent bishop (or queen), then this "something" is pinned against the king
	return d & board->movesFromSquares[bishopSquare] & board->occupations[PC(board->fen->sideToMove, PieceTypeAny)];
}

unsigned long long pinnedBishopMoves(struct Board * board, const int sq, const int pinnedBy) {
	const int diag = SQ_DIAG(sq);
	//if pinned on diagonal, then it can only move along it
	if (SQ_DIAG(pinnedBy) == diag) return get_bishop_moves(sq, board->occupations[PieceNameAny]) & diag_bb[diag];
	//the same is for antidiagonal
	else {
  	const int antidiag = SQ_ANTIDIAG(sq);
		if (SQ_ANTIDIAG(pinnedBy) == antidiag) return get_bishop_moves(sq, board->occupations[PieceNameAny]) & antidiag_bb[antidiag];
	}
	return 0;
}

unsigned long long pinnedRookMoves(struct Board * board, const int sq, const int pinnedBy) {
	const int file = SQ_FILE(sq);
	//if the rook is pinned on a file, it can only move along it
	if (SQ_FILE(pinnedBy) == file) return get_rook_moves(sq, board->occupations[PieceNameAny]) & files_bb[file];
	//the same applies to a rank
	else {
		const int rank = SQ_RANK(sq);
		if (SQ_RANK(pinnedBy) == rank) return get_rook_moves(sq, board->occupations[PieceNameAny]) & ranks_bb[rank];
	}
	return 0;
}

void piece_moves(int pieceType, unsigned long long occupation, struct MovesContext * ctx, struct Square * king, struct Board * board) {
	const signed char pawnShifts[3][3] = { { 8, 7, 9 }, { -8, -9, -7 } }; // { { N, NW, NE}, {S, SW, SE} }
	const int pawnRanks[3][3] = { { Rank2, Rank5, Rank6 }, { Rank7, Rank4, Rank3 } };	
	int shift = 0;
	while (occupation) {
	  int pinnedBy = SquareNone;
		int sq = lsBit(occupation);
		const int sqFile = SQ_FILE(sq);
		const int sqRank = SQ_RANK(sq);
		const int sqDiag = SQ_DIAG(sq);
		const int sqAntidiag = SQ_ANTIDIAG(sq);
		const unsigned long long sqBit = SQ_BIT(sq);
		unsigned long long d = 0;
		if (ctx->pinnedPieces & sqBit) {
 			d = ctx->pinningPieces;
 			while (d) {
 				int p = lsBit(d);
 				const int pFile = SQ_FILE(p);
 				const int pRank = SQ_RANK(p);
 				const int pDiag = SQ_DIAG(p);
 				const int pAntidiag = SQ_ANTIDIAG(p);
 				if ((pFile == sqFile && pFile == king->file) || (pRank == sqRank && pRank == king->rank) || (pDiag == sqDiag && pDiag == king->diag) || (pAntidiag == sqAntidiag && pAntidiag == king->antidiag)) {
 				  pinnedBy = p;
 				  break;
 				}
   			d &= d - 1;
      }
    }
    d = 0;
		if (pinnedBy < SquareNone) {
			if (pieceType == Bishop) {
  			//if there is check, then the bishop can't help since it can't move to block or capture the checker
  			if (board->isCheck || SQ_FILE(pinnedBy) == sqFile || SQ_RANK(pinnedBy) == sqRank) {
  				occupation &= occupation - 1;
  				continue;
  			}
  			//if pinned on a file or rank, then the bishop cannot move
			  d = pinnedBishopMoves(board, sq, pinnedBy);
			}
			else if (pieceType == Rook) {
  			//if check or the rook is pinned on a diagonal or anti-diagonal, then it cannot move
  			if (board->isCheck || SQ_DIAG(pinnedBy) == sqDiag || SQ_ANTIDIAG(pinnedBy) == sqAntidiag) {
  				occupation &= occupation - 1;
  				continue;
  			}
			  d = pinnedRookMoves(board, sq, pinnedBy);
			}
			else if (pieceType == Queen) {
  			if (board->isCheck) {
  				occupation &= occupation - 1;
  				continue;
  			}
  			//if pinned on a diagonal or anti-diagonal, then the queen can move like a bishop along this diagonal or anti-diagonal
  			if (SQ_DIAG(pinnedBy) == sqDiag || SQ_ANTIDIAG(pinnedBy) == sqAntidiag) d = pinnedBishopMoves(board, sq, pinnedBy);
  			//if pinned on a file or rank, then the queen can only move like a rook along this file or rank
  			else d = pinnedRookMoves(board, sq, pinnedBy);
			} else if (pieceType == Pawn) {
			  //d = 0;
  			if (board->isCheck) {// this pawn can't do much
  			  occupation &= occupation - 1;
  			  continue;
  			}
  			if (SQ_DIAG(pinnedBy) == sqDiag) {
  				shift = board->fen->sideToMove == ColorWhite ? sq + 9 : sq - 9;
  				int  enPassantFile = board->fen->sideToMove == ColorWhite ? sqFile + 1 : sqFile - 1;
  				if ((board->fen->sideToMove == ColorWhite && sqFile < FileH) || (board->fen->sideToMove == ColorBlack && sqFile > FileA)) {
  					if (board->piecesOnSquares[shift] != PieceNameNone) {
  						if ((board->piecesOnSquares[shift] >> 3) == OPP_COLOR(board->fen->sideToMove))
  							d |= SQ_BIT(shift);
  					}
  					else if (board->fen->enPassant == enPassantFile && sqRank == pawnRanks[board->fen->sideToMove][1])
  						d |= SQ_BIT(shift);
  				}
  			} else if (SQ_ANTIDIAG(pinnedBy) == sqAntidiag) {
  				shift = board->fen->sideToMove == ColorWhite ? sq + 7 : sq - 7;
  				int  enPassantFile = board->fen->sideToMove == ColorWhite ? sqFile - 1 : sqFile + 1;
  				if ((board->fen->sideToMove == ColorWhite && sqFile > FileA) || (board->fen->sideToMove == ColorBlack && sqFile < FileH)) {
  					if (board->piecesOnSquares[shift] != PieceNameNone) {
  						if (board->piecesOnSquares[shift] >> 3 == OPP_COLOR(board->fen->sideToMove))
  							d |= SQ_BIT(shift);
  					}
  					else if (board->fen->enPassant == enPassantFile && sqRank == pawnRanks[board->fen->sideToMove][1])
  						d |= SQ_BIT(shift);
  				}
  			} else if (SQ_FILE(pinnedBy) == sqFile) {
  				shift = sq + pawnShifts[board->fen->sideToMove][0];
  				if (board->piecesOnSquares[shift] == PieceNameNone) {
  					d |= SQ_BIT(shift);
  					if (sqRank == pawnRanks[board->fen->sideToMove][0]) {
  						shift += pawnShifts[board->fen->sideToMove][0];
  						if (board->piecesOnSquares[shift] == PieceNameNone) d |= SQ_BIT(shift);
  					}
  				}
  			}
  			else if (SQ_RANK(pinnedBy) == sqRank) { //goto next; //not much can be done
  			  occupation &= occupation - 1;
  			  continue;
        }
  		} //end of if (pawn is pinned); if knight is pinned, it has no moves
			board->movesFromSquares[sq] = d;
		} //end of if (pinned)
		//generate bishop moves from square <cp> limited by board boundaries and other chess pieces regardless of their color
		else {
   		d = 0;
		  if (pieceType == Bishop)
			  d = get_bishop_moves(sq, board->occupations[PieceNameAny]);
			else if (pieceType == Rook)
			  d = get_rook_moves(sq, board->occupations[PieceNameAny]);
			else if (pieceType == Queen) {
			  d = get_bishop_moves(sq, board->occupations[PieceNameAny]);
			  d |= get_rook_moves(sq, board->occupations[PieceNameAny]);			  
			} else if (pieceType == Pawn) {
			  //d = 0;
  			//normal pawn moves (non-capturing)
  			shift = sq + pawnShifts[board->fen->sideToMove][0]; //N or S
  			if (board->piecesOnSquares[shift] == PieceNameNone) {
  				d |= SQ_BIT(shift);
  				//double advance from rank 2 or 7
  				if (sqRank == pawnRanks[board->fen->sideToMove][0]) { //rank2 or rank7
  					shift += pawnShifts[board->fen->sideToMove][0];
  					if (board->piecesOnSquares[shift] == PieceNameNone) d |= SQ_BIT(shift);
  				}
  			}
  			//capturing pawn moves
  			if (sqFile > FileA) {
  				shift = sq + pawnShifts[board->fen->sideToMove][1]; //NW (white) or SW (black)
          unsigned long long bit_sq = SQ_BIT(shift);
          if (board->occupations[PC(OPP_COLOR(board->fen->sideToMove), PieceTypeAny)] & bit_sq) d |= bit_sq; 
  				else if ((board->fen->enPassant == (sqFile - 1)) && (sqRank == pawnRanks[board->fen->sideToMove][1])) { //rank5 or rank4
  					//make sure there is no discover check from a queen or a rook
  					board->piecesOnSquares[sq] = PieceNameNone; //temporarily remove moving pawn
  					board->piecesOnSquares[sq - 1] = PieceNameNone; //temporarily remove en passant pawn
  					if (enPassantLegalBit(board)) d |= bit_sq;
  					board->piecesOnSquares[sq] = ctx->shiftedColor | Pawn; //restore moving pawn
  					board->piecesOnSquares[sq - 1] = ctx->shiftedOpponentColor | Pawn; //restore en passant pawn
  				}
  			}
  			if (sqFile < FileH) {
  				shift = sq + pawnShifts[board->fen->sideToMove][2]; //NE (white) or SE (black)
          unsigned long long bit_sq = SQ_BIT(shift);
          if (board->occupations[PC(OPP_COLOR(board->fen->sideToMove), PieceTypeAny)] & bit_sq) d |= bit_sq; 
  				else if ((board->fen->enPassant == (sqFile + 1)) && (sqRank == pawnRanks[board->fen->sideToMove][1])) {
  					//make sure there is no discover check from a queen or a rook
  					board->piecesOnSquares[sq] = PieceNameNone; //temporarily remove moving pawn
  					board->piecesOnSquares[sq + 1] = PieceNameNone; //temporarily remove en passant pawn
  					if (enPassantLegalBit(board)) d |= bit_sq;
  					board->piecesOnSquares[sq] = ctx->shiftedColor | Pawn; //restore moving pawn
  					board->piecesOnSquares[sq + 1] = ctx->shiftedOpponentColor | Pawn; //restore en passant pawn
  				}
  			}
  			board->movesFromSquares[sq] = d;
  			if (ctx->checker > 0) {
  				//if checker is en passant pawn, can it be captured
  				if (board->fen->enPassant == SQ_FILE(ctx->checkerSquare) &&
  				    SQ_RANK(ctx->checkerSquare) == pawnRanks[board->fen->sideToMove][1] && //rank5 or rank4
  				    sqRank == pawnRanks[board->fen->sideToMove][1] && 
  				    ((sqFile == board->fen->enPassant - 1) || (sqFile == board->fen->enPassant + 1)))
  					board->movesFromSquares[sq] &= SQ_BIT(SQ(pawnRanks[board->fen->sideToMove][2], board->fen->enPassant)); //rank6 or rank3
  				else board->movesFromSquares[sq] &= (ctx->blockingSquares | ctx->checker); //can we block or capture checker
  			}		  
			} else if (pieceType == Knight) d = generateKnightMoves(board, sq);
		} //end of if (not pinned)
    if (pieceType != Pawn) {
  		//piece legal moves, which exclude moves to the squares occupied by pieces with the same color
  		board->movesFromSquares[sq] = d ^ (d & board->occupations[ctx->shiftedColor | PieceTypeAny]);
  		//if the king is in check, the legal moves are limited: we can either capture the  checker or block it
  		if (ctx->checker > 0) board->movesFromSquares[sq] &= (ctx->blockingSquares | ctx->checker);
		}
		board->moves |= board->movesFromSquares[sq];
		occupation &= occupation - 1;
	}
}

//generates legal moves
void generateMoves(struct Board * board) {
	unsigned long long d = 0, attackedSquares = 0;
	int sq;
	board->moves = 0;
	memset(board->movesFromSquares, 0, sizeof board->movesFromSquares);
	struct MovesContext movesContext;
	movesContext.shiftedColor = board->fen->sideToMove << 3;
	movesContext.shiftedOpponentColor = OPP_COLOR(board->fen->sideToMove) << 3;
	movesContext.checker = 0;
	movesContext.pinnedPieces = 0;
	movesContext.pinningPieces = 0;
	movesContext.blockingSquares = 0;
	board->isCheck = false; board->isStaleMate = false; board->isMate = false;

	struct ChessPiece king;
	int _king = movesContext.shiftedColor | King;
	int kingSquare = lsBit(board->occupations[_king]);
	struct Square kingSq;
	kingSq.file = SQ_FILE(kingSquare);
	kingSq.rank = SQ_RANK(kingSquare);
	kingSq.diag = SQ_DIAG(kingSquare);
	kingSq.antidiag = SQ_ANTIDIAG(kingSquare);
	kingSq.bit = SQ_BIT(kingSquare);
	PC_INIT(&king, _king, kingSquare);
	struct ChessPiece opponentKing;
	int _oking = movesContext.shiftedOpponentColor | King;
	int opponentKingSquare = lsBit(board->occupations[_oking]);
	PC_INIT(&opponentKing, _oking, opponentKingSquare);
	//for opponent color sliding piece moves we need to temporary remove the king, 
	//so the rays can light through it
	//this is necessary to mark the squares behind the king as attacked, 
	//so that the king under the check of opponent ray piece, cannot step back
	//we only xor it out from PieceNameAny occupations array member because only it is passed to get_bishop_moves() and get_root_moves()
	board->occupations[PieceNameAny] ^= kingSq.bit;

	unsigned long long opponentBishops = board->occupations[movesContext.shiftedOpponentColor | Bishop];
	unsigned long long opponentRooks = board->occupations[movesContext.shiftedOpponentColor | Rook];
	unsigned long long opponentQueens = board->occupations[movesContext.shiftedOpponentColor | Queen];
	unsigned long long opponentPawns = board->occupations[movesContext.shiftedOpponentColor | Pawn];
	unsigned long long opponentKnights = board->occupations[movesContext.shiftedOpponentColor | Knight];
	unsigned long long opponentAny = board->occupations[movesContext.shiftedOpponentColor | PieceTypeAny];
	unsigned long long bishops = board->occupations[movesContext.shiftedColor | Bishop];
	unsigned long long rooks = board->occupations[movesContext.shiftedColor | Rook];
	unsigned long long queens = board->occupations[movesContext.shiftedColor | Queen];
	unsigned long long pawns = board->occupations[movesContext.shiftedColor | Pawn];
	unsigned long long knights = board->occupations[movesContext.shiftedColor | Knight];
	unsigned long long any = board->occupations[movesContext.shiftedColor | PieceTypeAny];

	//find the squares attacked and defended by opponent bishops
	//and see if they pin anything against the king
	while (opponentBishops) {
  	sq = lsBit(opponentBishops);
		//generate opponent bishop moves limited by board boundaries and other pieces regardless of their color 
		board->movesFromSquares[sq] = get_bishop_moves(sq, board->occupations[PieceNameAny]);
		attackedSquares |= board->movesFromSquares[sq];
		//find pinned by this bishop pieces
		if ((d = bishopPinFinder(board, sq, king.square))) {
      movesContext.pinnedPieces |= d;
   		movesContext.pinningPieces |= (1ULL << sq); //SQ_BIT macro does an extra check for sq != SquareNone, here we don't need it
		}
		opponentBishops &= opponentBishops - 1;
	}
	//repeat the same process as described for opponent bishops, for opponent rooks
  while (opponentRooks) {
  	sq = lsBit(opponentRooks);
		board->movesFromSquares[sq] = get_rook_moves(sq, board->occupations[PieceNameAny]);
		attackedSquares |= board->movesFromSquares[sq];
		if ((d = rookPinFinder(board, sq, king.square))) {
      movesContext.pinnedPieces |= d;
   		movesContext.pinningPieces |= (1ULL << sq);			
		}
		opponentRooks &= opponentRooks - 1;
	}
	//repeat the same process as described for opponent bishops, for opponent queens
	while (opponentQueens) {
  	sq = lsBit(opponentQueens);
		board->movesFromSquares[sq] = get_bishop_moves(sq, board->occupations[PieceNameAny]) | get_rook_moves(sq, board->occupations[PieceNameAny]);
		attackedSquares |= board->movesFromSquares[sq];
		if ((d = bishopPinFinder(board, sq, king.square) | rookPinFinder(board, sq, king.square))) {
      movesContext.pinnedPieces |= d;
   		movesContext.pinningPieces |= (1ULL << sq);			
		}
		opponentQueens &= opponentQueens - 1;
	}
	//we are done with opponent ray piece attacked squares, now we can restore the king
	board->occupations[PieceNameAny] |= kingSq.bit;

	//find the squares attacked and defended by opponent pawns
	const char pawnCapturingMoves[2][2] = { { 7, 9 }, { -9, -7 } };
	while (opponentPawns) {
		sq = lsBit(opponentPawns);
		d = 0;
		int ii = SQ_FILE(sq), shift = sq;
		//opponent pawn attacking, capturing and protecting moves; another words, just diagonal and anti-diagonal moves
		if (ii > FileA)	{
			shift = sq + pawnCapturingMoves[OPP_COLOR(board->fen->sideToMove)][0];
			d |= (1ULL << shift);
		}
		if (ii < FileH) {
			shift = sq + pawnCapturingMoves[OPP_COLOR(board->fen->sideToMove)][1];
			d |= (1ULL << shift);
		}
		board->movesFromSquares[sq] = d;
		attackedSquares |= d;
		opponentPawns &= opponentPawns - 1;
	}
	//find the squares attacked and defended by opponent knights
	while (opponentKnights) {
		sq = lsBit(opponentKnights);
		//generate opponent knight moves limited by board boudaries only
		unsigned long long knight_moves = generateKnightMoves(board, sq);
		board->movesFromSquares[sq] = knight_moves;
		attackedSquares |= knight_moves;
		opponentKnights &= opponentKnights - 1;
	}

	//generate opponent king moves limited by board boundaries only
	unsigned long long opponentKingMoves = generateKingMoves(board, opponentKing.square);
	board->movesFromSquares[opponentKing.square] = opponentKingMoves;
  attackedSquares |= opponentKingMoves;
  
  //this is from the opponent point of view, meaning its defended pieces and the pieces that it attacks
  //opponent defended pieces are used in calculation of sideToMove king moves in terms of
  //whether it can capture opponent's piece or not
	unsigned long long defendedPieces = attackedSquares & board->occupations[movesContext.shiftedOpponentColor | PieceTypeAny];
    
	//checkers
	int num_checker = 0;
	if ((attackedSquares & kingSq.bit) > 0) {
		while (opponentAny) {
			sq = lsBit(opponentAny);
			if ((board->movesFromSquares[sq] & kingSq.bit) > 0) { 
				movesContext.checker |= (1ULL << sq); 
				num_checker++;
		  }
			opponentAny &= opponentAny - 1;
		}
	}
	
	//generate king moves limited by board boundaries only
	board->movesFromSquares[king.square] = generateKingMoves(board, king.square);
	//filter these moves to find the legal ones: 
	//the king cannot capture defended opponent's pieces
	//it can't go to a square occupied by other pieces of its color
	//and it can't go to a square attacked by opponent piece(s)
	board->movesFromSquares[king.square] ^= board->movesFromSquares[king.square] & (defendedPieces | board->occupations[movesContext.shiftedColor | PieceTypeAny] | attackedSquares);
	
	board->moves |= board->movesFromSquares[king.square];
	//is king checked?
	if ((board->isCheck = board->occupations[movesContext.shiftedColor | King] & attackedSquares)) {
		//if double check, no other moves rather than the king's move are possible
		if (num_checker > 1) goto exit;
		//normal check by checker
		else if (num_checker == 1) {
			movesContext.checkerSquare = lsBit(movesContext.checker);
			//if not checked by knight or pawn, calculate blocking squares
			if ((board->piecesOnSquares[movesContext.checkerSquare] != (movesContext.shiftedOpponentColor | Knight)) && board->piecesOnSquares[movesContext.checkerSquare] != (movesContext.shiftedOpponentColor | Pawn)) {
				if (kingSq.diag == SQ_DIAG(movesContext.checkerSquare)) {
					if (king.square > movesContext.checkerSquare) {
						unsigned long long bitSq = 1ULL << movesContext.checkerSquare;
						movesContext.blockingSquares = board->occupations[PieceNameNone] & diag_bb[kingSq.diag] & ((kingSq.bit - 1) ^ (bitSq | (bitSq - 1)));
					}
					else {
						movesContext.blockingSquares = board->occupations[PieceNameNone] & diag_bb[kingSq.diag] & (((1ULL << movesContext.checkerSquare) - 1) ^ (kingSq.bit | (kingSq.bit - 1)));
					}
				}
				else if (kingSq.antidiag == SQ_ANTIDIAG(movesContext.checkerSquare)) {
					if (king.square > movesContext.checkerSquare) {
						unsigned long long bitSq = 1ULL << movesContext.checkerSquare;
						movesContext.blockingSquares = board->occupations[PieceNameNone] & antidiag_bb[kingSq.antidiag] & ((kingSq.bit - 1) ^ (bitSq | (bitSq - 1)));							
					}
					else {
						movesContext.blockingSquares = board->occupations[PieceNameNone] & antidiag_bb[kingSq.antidiag] & (((1ULL << movesContext.checkerSquare) - 1) ^ (kingSq.bit | (kingSq.bit - 1)));
					}
				}
				else if (kingSq.file == SQ_FILE(movesContext.checkerSquare)) {
					if (king.square > movesContext.checkerSquare) {
						unsigned long long bitSq = 1ULL << movesContext.checkerSquare;
						movesContext.blockingSquares = board->occupations[PieceNameNone] & files_bb[kingSq.file] & ((kingSq.bit - 1) ^ (bitSq | (bitSq - 1)));
					}
					else {
						movesContext.blockingSquares = board->occupations[PieceNameNone] & files_bb[kingSq.file] & (((1ULL << movesContext.checkerSquare) - 1) ^ (kingSq.bit | (kingSq.bit - 1)));
					}
				}
				else if (kingSq.rank == SQ_RANK(movesContext.checkerSquare)) {
					if (king.square > movesContext.checkerSquare) {
						unsigned long long bitSq = 1ULL << movesContext.checkerSquare;
						movesContext.blockingSquares = board->occupations[PieceNameNone] & ranks_bb[kingSq.rank] & ((kingSq.bit - 1) ^ (bitSq | (bitSq - 1)));
					}
					else {
						movesContext.blockingSquares = board->occupations[PieceNameNone] & ranks_bb[kingSq.rank] & (((1ULL << movesContext.checkerSquare) - 1) ^ (kingSq.bit | (kingSq.bit - 1)));
					}
				}
			}
		}
	} else { //king is not checked
		//to complete legal king moves include castling
		const int whiteBlack[] = { SquareA1, SquareA8 }; //first squares of the pieces' ranks		
		const int castlingKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
		//short castling moves
		if ((((board->fen->castlingRights >> (board->fen->sideToMove << 1)) & 3) & CastlingSideKingside) == CastlingSideKingside) {
			const unsigned long long shortCastlingRookSquareBit = SQ_BIT(board->fen->castlingRook[0][board->fen->sideToMove] + whiteBlack[board->fen->sideToMove]);
			//empty squares between the king including king's square and its destination 
			//(for short castling: g1 or g8) should not be under attack
			const unsigned long long shortKingSquares = (((1ULL << kingSq.file) - 1) ^ 127) << whiteBlack[board->fen->sideToMove];
			const unsigned long long shortRookSquares = (((1ULL << board->fen->castlingRook[0][board->fen->sideToMove]) - 1) ^ 31) << whiteBlack[board->fen->sideToMove];
			unsigned long long occupations = board->occupations[PieceNameAny];
			//squares between the rook and its destination (for short castling f1 or f8) should be vacant (except the king and short castling rook for chess 960)
			occupations ^= (kingSq.bit | shortCastlingRookSquareBit);
			if ((shortKingSquares & attackedSquares) == 0 && (shortKingSquares & occupations) == 0 && (shortRookSquares & occupations) == 0) {
				if (board->fen->isChess960) board->movesFromSquares[king.square] |= shortCastlingRookSquareBit;
				else board->movesFromSquares[king.square] |= SQ_BIT(castlingKingSquare[0][board->fen->sideToMove]);
			}
		}
		//long castling moves
		if ((((board->fen->castlingRights >> (board->fen->sideToMove << 1)) & 3) & CastlingSideQueenside) == CastlingSideQueenside) {
			const unsigned long long longCastlingRookSquareBit = SQ_BIT(board->fen->castlingRook[1][board->fen->sideToMove] + whiteBlack[board->fen->sideToMove]);
			//empty squares between the king including king's square and its destination (for long castling: c1 or c8) should not be under attack
			unsigned long long longKingSquares = (((1ULL << (kingSq.file + 1)) - 1) ^ 3) << whiteBlack[board->fen->sideToMove];
			unsigned long long longRookSquares = (((1ULL << (board->fen->castlingRook[1][board->fen->sideToMove] + 1)) - 1) ^ 15) << whiteBlack[board->fen->sideToMove];
			unsigned long long occupations = board->occupations[PieceNameAny];
			//squares between the rook and its destination (for long castling d1 or d8) should be vacant (except the king and long castling rook for chess 960)
			occupations ^= (kingSq.bit | longCastlingRookSquareBit);
			if ((longKingSquares & attackedSquares) == 0 && (longKingSquares & occupations) == 0 && (longRookSquares & occupations) == 0) {
				if (board->fen->isChess960) board->movesFromSquares[king.square] |= longCastlingRookSquareBit;
				else board->movesFromSquares[king.square] |= SQ_BIT(castlingKingSquare[1][board->fen->sideToMove]);
			}
		}
		board->moves |= board->movesFromSquares[king.square];
  }
  
	//legal other moves
  piece_moves(Bishop, bishops, &movesContext, &kingSq, board);
  piece_moves(Rook, rooks, &movesContext, &kingSq, board);
  piece_moves(Queen, queens, &movesContext, &kingSq, board);
  piece_moves(Pawn, pawns, &movesContext, &kingSq, board);
  piece_moves(Knight, knights, &movesContext, &kingSq, board);
  	
exit:
 	if (board->moves == 0) {
		if (board->isCheck) {
			board->isMate = true;
			board->isCheck = false;
		}
		else board->isStaleMate = true;
	}
}

void makeMove(struct Move * move) {
	int  castlingSide = CastlingSideNone;
  const int oppColor = OPP_COLOR(move->chessBoard->fen->sideToMove);
	const int mpFile = SQ_FILE(move->src);
	move->prevCastlingRights = move->chessBoard->fen->castlingRights;
  move->prevEnPassant = move->chessBoard->fen->enPassant;
  move->prevHalfmoveClock = move->chessBoard->fen->halfmoveClock;
  move->prevCastlingRook = move->castlingRook;

	//null move
	if ((move->type & MoveTypeNull) && (move->type & MoveTypeValid)) {
		move->chessBoard->fen->sideToMove = move->chessBoard->fen->sideToMove == ColorWhite ? ColorBlack : ColorWhite;
		if (move->chessBoard->fen->sideToMove == ColorWhite) {
			move->chessBoard->fen->moveNumber++;
		}
		generateMoves(move->chessBoard);
		goto exit;
	}
	//remove the piece from its source square
	move->chessBoard->occupations[move->movingPiece] ^= SQ_BIT(move->src);
	move->chessBoard->piecesOnSquares[move->src] = PieceNameNone;
  //fprintf(stderr, "makeMove() debug(): src %d, bitboard src %llu, %s occupation %llu\n", move->src, SQ_BIT(move->src), pieceName[move->movingPiece], move->chessBoard->occupations[move->movingPiece]);

	//rook moves
	//need to find out if this is a castling rook to update FEN castling rights and rooks
	if (PC_TYPE(move->movingPiece) == Rook && (move->chessBoard->fen->castlingRights & (CastlingSideBoth << ((move->chessBoard->fen->sideToMove) << 1)))) {
		const int  rookRank[2] = { Rank1, Rank8 };
		int  castlingSide = CastlingSideNone;
		if (SQ_RANK(move->src) == rookRank[move->chessBoard->fen->sideToMove]) {
			if (mpFile == move->chessBoard->fen->castlingRook[0][move->chessBoard->fen->sideToMove]) //short castling
				castlingSide = CastlingSideKingside;
			else if (mpFile == move->chessBoard->fen->castlingRook[1][move->chessBoard->fen->sideToMove]) //long castling
				castlingSide = CastlingSideQueenside;
			if (castlingSide != CastlingSideNone) {
		    // Store the affected one
		    if (castlingSide == CastlingSideKingside) {
		        move->castlingRook = move->chessBoard->fen->castlingRook[0][move->chessBoard->fen->sideToMove];
		    } else {  // Queenside
		        move->otherCastlingRook = move->chessBoard->fen->castlingRook[1][move->chessBoard->fen->sideToMove];
		    }				
				//revoke castling rights
				move->chessBoard->fen->castlingRights ^= (castlingSide << ((move->chessBoard->fen->sideToMove) << 1));
				move->chessBoard->fen->castlingRook[castlingSide - 1][move->chessBoard->fen->sideToMove] = FileNone;
			}
		}
	}
	//capture
	if (move->type & MoveTypeCapture) {
		//int capturedPiece = PieceNameNone;
		if (move->type & MoveTypeEnPassant) {
			const signed char offset[2] = { -8, 8 };
			const int capturedPawnSquare = move->dst + offset[move->chessBoard->fen->sideToMove];
			move->capturedPiece = PC(oppColor, Pawn);
			move->chessBoard->occupations[move->capturedPiece] ^= SQ_BIT(capturedPawnSquare);
			move->chessBoard->piecesOnSquares[capturedPawnSquare] = PieceNameNone;
		} else {
			move->capturedPiece = move->chessBoard->piecesOnSquares[move->dst];
			move->chessBoard->occupations[move->chessBoard->piecesOnSquares[move->dst]] ^= SQ_BIT(move->dst);
			//if captured piece is a castling rook, then remove the castling rights of the opponent on that side
			if (PC_TYPE(move->capturedPiece) == Rook) {
				const unsigned char whiteBlack[2] = { 0, 56 };
				int cr = CastlingRightsWhiteNoneBlackNone;
				if (move->dst == move->chessBoard->fen->castlingRook[0][oppColor] + whiteBlack[oppColor]) {
					cr = CastlingSideKingside << (oppColor << 1);
					if (move->chessBoard->fen->castlingRights & cr) {
						move->chessBoard->fen->castlingRights ^= cr;
            move->castlingRook = move->chessBoard->fen->castlingRook[0][oppColor];
						move->chessBoard->fen->castlingRook[0][oppColor] = FileNone;
					}
				}
				else if (move->dst == move->chessBoard->fen->castlingRook[1][oppColor] + whiteBlack[oppColor]) {
					cr = CastlingSideQueenside << (oppColor << 1);
					if (move->chessBoard->fen->castlingRights & cr) {
						move->chessBoard->fen->castlingRights ^= (CastlingSideQueenside << (oppColor << 1));
            move->otherCastlingRook = move->chessBoard->fen->castlingRook[1][oppColor];
						move->chessBoard->fen->castlingRook[1][oppColor] = FileNone;
					}
				}
			}
		}
		//move->capturedPiece = capturedPiece;
	}

	//castling
	if (PC_TYPE(move->movingPiece) == King) {
		if ((move->type & MoveTypeCastlingQueenside) || (move->type & MoveTypeCastlingKingside)) {
			castlingSide = (((move->type & (MoveTypeCastlingKingside | MoveTypeCastlingQueenside))) >> 2) - 1;
			const unsigned char rookOffset[2] = { 0, 56 };
			const int  dstKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
			const int dstRookSquare[2][2] = { { SquareF1, SquareF8 }, { SquareD1, SquareD8 } };
			const int srcRookSquare = move->chessBoard->fen->castlingRook[castlingSide][move->chessBoard->fen->sideToMove] + rookOffset[move->chessBoard->fen->sideToMove];
			const int rookName = PC(move->chessBoard->fen->sideToMove, Rook);
			//remove castling rook from its source square taking care of
			//Chess960 case of rook occupying king's destination square - make sure we are not removing (overwriting) the king with PieceNameNone!
			if (srcRookSquare != dstKingSquare[castlingSide][move->chessBoard->fen->sideToMove]) move->chessBoard->piecesOnSquares[srcRookSquare] = PieceNameNone;
			//and put it to its destination square
			move->chessBoard->piecesOnSquares[dstRookSquare[castlingSide][move->chessBoard->fen->sideToMove]] = rookName;
			//update occupations
			//xor out the rook on its source square
			move->chessBoard->occupations[rookName] ^= SQ_BIT(srcRookSquare);
			//add the rook on its destination square
			move->chessBoard->occupations[rookName] |= SQ_BIT(dstRookSquare[castlingSide][move->chessBoard->fen->sideToMove]);
  		//copy original castling rook for Zobrist hash calculation, then
	  	move->castlingRook = move->chessBoard->fen->castlingRook[castlingSide][move->chessBoard->fen->sideToMove];
	  	move->otherCastlingRook = move->chessBoard->fen->castlingRook[castlingSide == 0 ? 1 : 0][move->chessBoard->fen->sideToMove];
  		//special case of chess 960 castling
  		if (move->chessBoard->fen->isChess960) move->dst = dstKingSquare[castlingSide][move->chessBoard->fen->sideToMove];
		}
		//update FEN castling rights, i.e. revoke castling rights for the sideToMove and keep castling rights for the opponent if any
		move->chessBoard->fen->castlingRights &= (CastlingSideBoth << (oppColor << 1));
		//set castling rook to none
		move->chessBoard->fen->castlingRook[0][move->chessBoard->fen->sideToMove] = FileNone;
		move->chessBoard->fen->castlingRook[1][move->chessBoard->fen->sideToMove] = FileNone;
	}
	
	//promotion
	if (move->type & MoveTypePromotion) {
		move->chessBoard->piecesOnSquares[move->dst] = move->promoPiece;
		move->chessBoard->occupations[move->promoPiece] |= SQ_BIT(move->dst);
	} 
	//other move
	else {
		//move the piece to its destination
		//special case of chess 960 castling - moved into castling
		//if (move->chessBoard->fen->isChess960 && ((move->type & MoveTypeCastlingQueenside) || (move->type & MoveTypeCastlingKingside))) move->dst = dstKingSquare[castlingSide][move->chessBoard->fen->sideToMove];
		//move moving piece from its src sq to its dst sq - is it needed? - comment it out to test
		//move->src = move->dst;
		move->chessBoard->piecesOnSquares[move->dst] = move->movingPiece;
		move->chessBoard->occupations[move->movingPiece] |= SQ_BIT(move->dst);
    //fprintf(stderr, "makeMove() debug(): dst %d, bitboard dst %llu, %s occupation %llu\n", move->dst, SQ_BIT(move->dst), pieceName[move->movingPiece], move->chessBoard->occupations[move->movingPiece]);
	}
	
	//update occupations for all pieces
	move->chessBoard->occupations[PieceNameWhite] = move->chessBoard->occupations[WhiteBishop] | 
	                                                move->chessBoard->occupations[WhiteKing] | 
	                                                move->chessBoard->occupations[WhiteKnight] | 
	                                                move->chessBoard->occupations[WhitePawn] | 
	                                                move->chessBoard->occupations[WhiteQueen] | 
	                                                move->chessBoard->occupations[WhiteRook];
	move->chessBoard->occupations[PieceNameBlack] = move->chessBoard->occupations[BlackBishop] | 
	                                                move->chessBoard->occupations[BlackKing] | 
	                                                move->chessBoard->occupations[BlackKnight] | 
	                                                move->chessBoard->occupations[BlackPawn] | 
	                                                move->chessBoard->occupations[BlackQueen] | 
	                                                move->chessBoard->occupations[BlackRook];
	move->chessBoard->occupations[PieceNameAny] = move->chessBoard->occupations[PieceNameWhite] | 
	                                              move->chessBoard->occupations[PieceNameBlack];
	move->chessBoard->occupations[PieceNameNone] = ~move->chessBoard->occupations[PieceNameAny];
	
	//set FEN en passant file if any
	if ((move->type & MoveTypeEnPassant) && !(move->type & MoveTypeCapture))
		move->chessBoard->fen->enPassant = mpFile;
	else move->chessBoard->fen->enPassant = FileNone;
	
	//increment halfmove clock if not a pawn's move and not a capture
	if (PC_TYPE(move->movingPiece) != Pawn && !(move->type & MoveTypeCapture)) 
		move->chessBoard->fen->halfmoveClock++;
	else move->chessBoard->fen->halfmoveClock = 0;
	
	//increment move number if it was black's move
	if (move->chessBoard->fen->sideToMove == ColorBlack) 
		move->chessBoard->fen->moveNumber++;
	
	//toggle FEN SideToMove and opponentColor
	if (move->chessBoard->fen->sideToMove == ColorWhite) {
		move->chessBoard->fen->sideToMove = ColorBlack;
	} else {
		move->chessBoard->fen->sideToMove = ColorWhite;
	}
	//printf("makeMove() debug: movingPiece %s\n", pieceName[move->movingPiece]);
  //printf("makeMove() debug: capturedPiece %s\n", pieceName[move->capturedPiece]);
  //printf("makeMove() debug: src %s\n", squareName[move->src]);
  //printf("makeMove() debug: dest %s\n", squareName[move->dst]);

	//update FEN
	//writeDebug(move->chessBoard, false);
	updateFen(move->chessBoard);
	//fprintf(stderr, "makeMove() debug: fen after calling updateFen() is %s\n", move->chessBoard->fen->fenString);
	//generate opponent's moves
	generateMoves(move->chessBoard);
	
exit:
	if (move->chessBoard->isCheck) {
		if (!(strrchr(move->sanMove, '+'))) strcat(move->sanMove, "+");
	}
	else if (move->chessBoard->isMate) 
		if (!(strrchr(move->sanMove, '#'))) strcat(move->sanMove, "#");
		/*if (reconcile(move->chessBoard)) {
			fprintf(stderr, "makeMove() error: reconcile() failed\n");
			exit(1);
		}*/
}

void undoMove(struct Move *move) {
    // Reverse symbols (optional, not search-critical)
    //if (strrchr(move->sanMove, '+')) move->sanMove[strlen(move->sanMove) - 1] = '\0';  // Remove '+'
    //if (strrchr(move->sanMove, '#')) move->sanMove[strlen(move->sanMove) - 1] = '\0';  // Remove '#'

    // Restore sideToMove and opponentColor (toggle back)
    if (move->chessBoard->fen->sideToMove == ColorWhite) {
    	move->chessBoard->fen->sideToMove = ColorBlack;
      move->chessBoard->fen->moveNumber--;    	
    } else {
    	move->chessBoard->fen->sideToMove = ColorWhite;
    }
    if (move->type & MoveTypeNull) {
        generateMoves(move->chessBoard);
        return;
    }
    move->chessBoard->fen->halfmoveClock = move->prevHalfmoveClock;
    move->chessBoard->fen->enPassant = move->prevEnPassant;
    move->chessBoard->fen->castlingRights = move->prevCastlingRights;

    //printf("undoMove() debug: movingPiece %s\n", pieceName[movingPiece]);
    //printf("undoMove() debug: capturedPiece %s\n", pieceName[capturedPiece]);
    //printf("undoMove() debug: src %s\n", squareName[src]);
    //printf("undoMove() debug: dest %s\n", squareName[dest]);
    // Explicitly REMOVE the piece from the destination bitboard
    move->chessBoard->occupations[move->movingPiece] ^= SQ_BIT(move->dst);
    move->chessBoard->piecesOnSquares[move->dst] = PieceNameNone;
    // Explicitly ADD the piece to the source bitboard
    move->chessBoard->occupations[move->movingPiece] |= SQ_BIT(move->src);
    // Place the piece on the source square in the mail-box array
    move->chessBoard->piecesOnSquares[move->src] = move->movingPiece;
    //char mvType[94] = "";
    //getMoveType(mvType, move->type);
    //printf("move->type %s\n", mvType);
    
    // Handle promotion before capture, so captured piece during promotion could be restored
    if (move->type & MoveTypePromotion) {
        move->chessBoard->occupations[move->promoPiece] ^= SQ_BIT(move->dst); //remove promo piece
        move->chessBoard->occupations[move->movingPiece] ^= SQ_BIT(move->dst); //remove pawn
        move->chessBoard->piecesOnSquares[move->dst] = PieceNameNone;
    }
    //  Handle the capture after promotion, so captured piece during promotion could be restored
    if (move->type & MoveTypeCapture) {
        if (move->type & MoveTypeEnPassant) {
            const signed char offset = (move->chessBoard->fen->sideToMove == ColorWhite ? -8 : 8);
            const int capturedSq = move->dst + offset;            
            // The captured pawn is restored to its proper square
            move->chessBoard->piecesOnSquares[capturedSq] = move->capturedPiece;
            move->chessBoard->occupations[move->capturedPiece] |= SQ_BIT(capturedSq);
        } else {
            // A regular capture: restore the captured piece to the destination square
            move->chessBoard->piecesOnSquares[move->dst] = move->capturedPiece;
            move->chessBoard->occupations[move->capturedPiece] |= SQ_BIT(move->dst);
        }
    }
    
    // Reverse castling King/rook move
    if (PC_TYPE(move->movingPiece) == King && (move->type & (MoveTypeCastlingQueenside | MoveTypeCastlingKingside))) {
        const unsigned char rookOffset[2] = {0, 56};
        const int castlingSide = ((move->type & (MoveTypeCastlingKingside | MoveTypeCastlingQueenside)) >> 2) - 1;
				const int otherSide = (castlingSide == 0 ? 1 : 0);				
				move->chessBoard->fen->castlingRook[castlingSide][move->chessBoard->fen->sideToMove] = move->castlingRook;
				move->chessBoard->fen->castlingRook[otherSide][move->chessBoard->fen->sideToMove] = move->otherCastlingRook;        
        const int dstRookSquare[2][2] = {{SquareF1, SquareF8}, {SquareD1, SquareD8}}; 
        const int dstKingSquare[2][2] = {{SquareG1, SquareG8}, {SquareC1, SquareC8}};
        const int srcRookSquare = move->chessBoard->fen->castlingRook[castlingSide][move->chessBoard->fen->sideToMove] + rookOffset[move->chessBoard->fen->sideToMove];
		    const int rook = PC(move->chessBoard->fen->sideToMove, Rook);
		    const int king = PC(move->chessBoard->fen->sideToMove, King);
		    //const int originalKingSrc = move->src;  // King's original src (cached in Move)
		
		    // Chess960 overlap check: If rook src == king dst in forward, skip clear in undo too
		    //bool overlap = (move->chessBoard->fen->isChess960 && srcRookSquare == dstKingSquare[castlingSide][move->chessBoard->fen->sideToMove]);
		
		    // Restore rook FIRST: Clear current dst, add to src (XOR handles bits)
		    move->chessBoard->occupations[rook] ^= SQ_BIT(dstRookSquare[castlingSide][move->chessBoard->fen->sideToMove]);
		    move->chessBoard->piecesOnSquares[dstRookSquare[castlingSide][move->chessBoard->fen->sideToMove]] = PieceNameNone;
		    move->chessBoard->occupations[rook] |= SQ_BIT(srcRookSquare);
		    //if (!overlap) {
		        // No overlap: Explicitly set src (was cleared in forward)
		        //move->chessBoard->piecesOnSquares[srcRookSquare] = rook;
		    //} //else {
		        // Overlap: Src currently holds king — set rook after king move (below), but bits are set
		        // Forward skipped clear, so undo sets explicitly after king restore
		    //}
		
		    // Restore king LAST: Clear current dst (rook's old src or normal), add to original src
		    move->chessBoard->occupations[king] ^= SQ_BIT(move->dst);
		    move->chessBoard->piecesOnSquares[move->dst] = PieceNameNone;
		    move->chessBoard->occupations[king] |= SQ_BIT(move->src);
		    move->chessBoard->piecesOnSquares[move->src] = king;
		
		    // If overlap: Now set rook src (king is back, so safe to overwrite if needed—but in Chess960, rook src is now free post-king restore)
		    //if (overlap) {
		        move->chessBoard->piecesOnSquares[srcRookSquare] = rook;  // Explicit set for overlap
        //}    
    } else {
    	// Rook-move or capture revoke: Restore only affected (other remains FileNone)
      if (PC_TYPE(move->movingPiece) == Rook || PC_TYPE(move->capturedPiece) == Rook) {
        // Check which was revoked (based on forward logic, but simple: restore if non-None)
        if (move->castlingRook != FileNone) {
            move->chessBoard->fen->castlingRook[0][move->chessBoard->fen->sideToMove] = move->castlingRook;  // Was kingside
        }
        if (move->otherCastlingRook != FileNone) {
            move->chessBoard->fen->castlingRook[1][move->chessBoard->fen->sideToMove] = move->otherCastlingRook;  // Was queenside
        }
      }
    }
    move->castlingRook = move->prevCastlingRook;

    // Step 9: Recompute occupations for all (now pieces are restored)
    move->chessBoard->occupations[PieceNameWhite] = move->chessBoard->occupations[WhiteBishop] | 
                                                    move->chessBoard->occupations[WhiteKing] | 
                                                    move->chessBoard->occupations[WhiteKnight] | 
                                                    move->chessBoard->occupations[WhitePawn] | 
                                                    move->chessBoard->occupations[WhiteQueen] | 
                                                    move->chessBoard->occupations[WhiteRook];
    move->chessBoard->occupations[PieceNameBlack] = move->chessBoard->occupations[BlackBishop] | 
                                                    move->chessBoard->occupations[BlackKing] | 
                                                    move->chessBoard->occupations[BlackKnight] | 
                                                    move->chessBoard->occupations[BlackPawn] | 
                                                    move->chessBoard->occupations[BlackQueen] | 
                                                    move->chessBoard->occupations[BlackRook];
    move->chessBoard->occupations[PieceNameAny] = move->chessBoard->occupations[PieceNameWhite] | 
                                                  move->chessBoard->occupations[PieceNameBlack];
    move->chessBoard->occupations[PieceNameNone] = ~move->chessBoard->occupations[PieceNameAny];

    // Step 10: Update FEN and generate moves (for restored side)
    updateFen(move->chessBoard);
    if (reconcile(move->chessBoard)) {
    	fprintf(stderr, "undoMove() error: reconsile failed\n");
    	exit(1);
    }
    generateMoves(move->chessBoard);
}

//lighter variant of makeMove() for MCTS 
//without move generation, update FEN and check for null move and preserving the state for undoMove()
void make_move(struct Move * move) {
	int castlingSide = CastlingSideNone;
  const int oppColor = OPP_COLOR(move->chessBoard->fen->sideToMove);
	const int mpFile = SQ_FILE(move->src);
	//remove the piece from its source square
	move->chessBoard->occupations[move->movingPiece] ^= SQ_BIT(move->src);
	move->chessBoard->piecesOnSquares[move->src] = PieceNameNone;

	//rook moves
	//need to find out if this is a castling rook to update FEN castling rights and rooks
	if (PC_TYPE(move->movingPiece) == Rook && (move->chessBoard->fen->castlingRights & (CastlingSideBoth << ((move->chessBoard->fen->sideToMove) << 1)))) {
		const int  rookRank[2] = { Rank1, Rank8 };
		//int  castlingSide = CastlingSideNone;
		if (SQ_RANK(move->src) == rookRank[move->chessBoard->fen->sideToMove]) {
			if (mpFile == move->chessBoard->fen->castlingRook[0][move->chessBoard->fen->sideToMove]) //short castling
				castlingSide = CastlingSideKingside;
			else if (mpFile == move->chessBoard->fen->castlingRook[1][move->chessBoard->fen->sideToMove]) //long castling
				castlingSide = CastlingSideQueenside;
			if (castlingSide != CastlingSideNone) {
		    // Store the affected one
		    if (castlingSide == CastlingSideKingside) {
		        move->castlingRook = move->chessBoard->fen->castlingRook[0][move->chessBoard->fen->sideToMove];
		    } else {  // Queenside
		        move->otherCastlingRook = move->chessBoard->fen->castlingRook[1][move->chessBoard->fen->sideToMove];
		    }				
				//revoke castling rights
				move->chessBoard->fen->castlingRights ^= (castlingSide << ((move->chessBoard->fen->sideToMove) << 1));
				move->chessBoard->fen->castlingRook[castlingSide - 1][move->chessBoard->fen->sideToMove] = FileNone;
			}
		}
	}
	//capture
	if (move->type & MoveTypeCapture) {
		//int capturedPiece = PieceNameNone;
		if (move->type & MoveTypeEnPassant) {
			const signed char offset[2] = { -8, 8 };
			const int capturedPawnSquare = move->dst + offset[move->chessBoard->fen->sideToMove];
			move->capturedPiece = PC(oppColor, Pawn);
			move->chessBoard->occupations[move->capturedPiece] ^= SQ_BIT(capturedPawnSquare);
			move->chessBoard->piecesOnSquares[capturedPawnSquare] = PieceNameNone;
		} else {
			move->capturedPiece = move->chessBoard->piecesOnSquares[move->dst];
			move->chessBoard->occupations[move->chessBoard->piecesOnSquares[move->dst]] ^= SQ_BIT(move->dst);
			//if captured piece is a castling rook, then remove the castling rights of the opponent on that side
			if (PC_TYPE(move->capturedPiece) == Rook) {
				const unsigned char whiteBlack[2] = { 0, 56 };
				int cr = CastlingRightsWhiteNoneBlackNone;
				if (move->dst == move->chessBoard->fen->castlingRook[0][oppColor] + whiteBlack[oppColor]) {
					cr = CastlingSideKingside << (oppColor << 1);
					if (move->chessBoard->fen->castlingRights & cr) {
						move->chessBoard->fen->castlingRights ^= cr;
            move->castlingRook = move->chessBoard->fen->castlingRook[0][oppColor];
						move->chessBoard->fen->castlingRook[0][oppColor] = FileNone;
					}
				}
				else if (move->dst == move->chessBoard->fen->castlingRook[1][oppColor] + whiteBlack[oppColor]) {
					cr = CastlingSideQueenside << (oppColor << 1);
					if (move->chessBoard->fen->castlingRights & cr) {
						move->chessBoard->fen->castlingRights ^= cr;
            move->otherCastlingRook = move->chessBoard->fen->castlingRook[1][oppColor];
						move->chessBoard->fen->castlingRook[1][oppColor] = FileNone;
					}
				}
			}
		}
	}

	//castling
	if (PC_TYPE(move->movingPiece) == King) {
		if ((move->type & MoveTypeCastlingQueenside) || (move->type & MoveTypeCastlingKingside)) {
			castlingSide = (((move->type & (MoveTypeCastlingKingside | MoveTypeCastlingQueenside))) >> 2) - 1;
			const unsigned char rookOffset[2] = { 0, 56 };
			const int  dstKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
			const int dstRookSquare[2][2] = { { SquareF1, SquareF8 }, { SquareD1, SquareD8 } };
			const int srcRookSquare = move->chessBoard->fen->castlingRook[castlingSide][move->chessBoard->fen->sideToMove] + rookOffset[move->chessBoard->fen->sideToMove];
			const int rookName = PC(move->chessBoard->fen->sideToMove, Rook);
			//remove castling rook from its source square taking care of
			//Chess960 case of rook occupying king's destination square - make sure we are not removing (overwriting) the king with PieceNameNone!
			if (srcRookSquare != dstKingSquare[castlingSide][move->chessBoard->fen->sideToMove]) move->chessBoard->piecesOnSquares[srcRookSquare] = PieceNameNone;
			//and put it to its destination square
			move->chessBoard->piecesOnSquares[dstRookSquare[castlingSide][move->chessBoard->fen->sideToMove]] = rookName;
			//update occupations
			//xor out the rook on its source square
			move->chessBoard->occupations[rookName] ^= SQ_BIT(srcRookSquare);
			//add the rook on its destination square
			move->chessBoard->occupations[rookName] |= SQ_BIT(dstRookSquare[castlingSide][move->chessBoard->fen->sideToMove]);
  		//copy original castling rook for Zobrist hash calculation, then
	  	move->castlingRook = move->chessBoard->fen->castlingRook[castlingSide][move->chessBoard->fen->sideToMove];
	  	move->otherCastlingRook = move->chessBoard->fen->castlingRook[castlingSide == 0 ? 1 : 0][move->chessBoard->fen->sideToMove];
  		//special case of chess 960 castling
  		if (move->chessBoard->fen->isChess960) move->dst = dstKingSquare[castlingSide][move->chessBoard->fen->sideToMove];
		}
		//update FEN castling rights, i.e. revoke castling rights for the sideToMove and keep castling rights for the opponent if any
		move->chessBoard->fen->castlingRights &= (CastlingSideBoth << (oppColor << 1));
		//set castling rook to none
		move->chessBoard->fen->castlingRook[0][move->chessBoard->fen->sideToMove] = FileNone;
		move->chessBoard->fen->castlingRook[1][move->chessBoard->fen->sideToMove] = FileNone;
	}
	
	//promotion
	if (move->type & MoveTypePromotion) {
		move->chessBoard->piecesOnSquares[move->dst] = move->promoPiece;
		move->chessBoard->occupations[move->promoPiece] |= SQ_BIT(move->dst);
	} 
	//other move
	else {
		//move the piece to its destination
		move->chessBoard->piecesOnSquares[move->dst] = move->movingPiece;
		move->chessBoard->occupations[move->movingPiece] |= SQ_BIT(move->dst);
	}
	
	//update occupations for all pieces
	move->chessBoard->occupations[PieceNameWhite] = move->chessBoard->occupations[WhiteBishop] | 
	                                                move->chessBoard->occupations[WhiteKing] | 
	                                                move->chessBoard->occupations[WhiteKnight] | 
	                                                move->chessBoard->occupations[WhitePawn] | 
	                                                move->chessBoard->occupations[WhiteQueen] | 
	                                                move->chessBoard->occupations[WhiteRook];
	move->chessBoard->occupations[PieceNameBlack] = move->chessBoard->occupations[BlackBishop] | 
	                                                move->chessBoard->occupations[BlackKing] | 
	                                                move->chessBoard->occupations[BlackKnight] | 
	                                                move->chessBoard->occupations[BlackPawn] | 
	                                                move->chessBoard->occupations[BlackQueen] | 
	                                                move->chessBoard->occupations[BlackRook];
	move->chessBoard->occupations[PieceNameAny] = move->chessBoard->occupations[PieceNameWhite] | 
	                                              move->chessBoard->occupations[PieceNameBlack];
	move->chessBoard->occupations[PieceNameNone] = ~move->chessBoard->occupations[PieceNameAny];
	
	//set FEN en passant file if any
	if ((move->type & MoveTypeEnPassant) && !(move->type & MoveTypeCapture))
		move->chessBoard->fen->enPassant = mpFile;
	else move->chessBoard->fen->enPassant = FileNone;
	
	//increment halfmove clock if not a pawn's move and not a capture
	if (PC_TYPE(move->movingPiece) != Pawn && !(move->type & MoveTypeCapture)) 
		move->chessBoard->fen->halfmoveClock++;
	else move->chessBoard->fen->halfmoveClock = 0;
	
	//increment move number if it was black's move
	if (move->chessBoard->fen->sideToMove == ColorBlack) 
		move->chessBoard->fen->moveNumber++;
	
	//toggle FEN SideToMove and opponentColor
	if (move->chessBoard->fen->sideToMove == ColorWhite) {
		move->chessBoard->fen->sideToMove = ColorBlack;
	} else {
		move->chessBoard->fen->sideToMove = ColorWhite;
	}
}

//simplified version of undoMove() without null move, updateFen() and generateMoves()
void undo_move(struct Move *move) {
    // Restore sideToMove and opponentColor (toggle back)
    if (move->chessBoard->fen->sideToMove == ColorWhite) {
    	move->chessBoard->fen->sideToMove = ColorBlack;
      move->chessBoard->fen->moveNumber--;    	
    } else {
    	move->chessBoard->fen->sideToMove = ColorWhite;
    }
    move->chessBoard->fen->halfmoveClock = move->prevHalfmoveClock;
    move->chessBoard->fen->enPassant = move->prevEnPassant;
    move->chessBoard->fen->castlingRights = move->prevCastlingRights;

    // Explicitly REMOVE the piece from the destination bitboard
    move->chessBoard->occupations[move->movingPiece] ^= SQ_BIT(move->dst);
    move->chessBoard->piecesOnSquares[move->dst] = PieceNameNone;
    // Explicitly ADD the piece to the source bitboard
    move->chessBoard->occupations[move->movingPiece] |= SQ_BIT(move->src);
    // Place the piece on the source square
    move->chessBoard->piecesOnSquares[move->src] = move->movingPiece;
    
    // Handle promotion before capture, so captured piece during promotion could be restored
    if (move->type & MoveTypePromotion) {
        move->chessBoard->occupations[move->promoPiece] ^= SQ_BIT(move->dst); //remove promo piece
        move->chessBoard->occupations[move->movingPiece] ^= SQ_BIT(move->dst); //remove pawn
        move->chessBoard->piecesOnSquares[move->dst] = PieceNameNone;
    }
    //  Handle the capture after promotion, so captured piece during promotion could be restored
    if (move->type & MoveTypeCapture) {
        if (move->type & MoveTypeEnPassant) {
            const signed char offset = (move->chessBoard->fen->sideToMove == ColorWhite ? -8 : 8);
            const int capturedSq = move->dst + offset;            
            // The captured pawn is restored to its proper square
            move->chessBoard->piecesOnSquares[capturedSq] = move->capturedPiece;
            move->chessBoard->occupations[move->capturedPiece] |= SQ_BIT(capturedSq);
        } else {
            // A regular capture: restore the captured piece to the destination square
            move->chessBoard->piecesOnSquares[move->dst] = move->capturedPiece;
            move->chessBoard->occupations[move->capturedPiece] |= SQ_BIT(move->dst);
        }
    }
    
    // Reverse castling King/rook move
    if (PC_TYPE(move->movingPiece) == King && (move->type & (MoveTypeCastlingQueenside | MoveTypeCastlingKingside))) {
        const unsigned char rookOffset[2] = {0, 56};
        const int castlingSide = ((move->type & (MoveTypeCastlingKingside | MoveTypeCastlingQueenside)) >> 2) - 1;
				const int otherSide = (castlingSide == 0 ? 1 : 0);				
				move->chessBoard->fen->castlingRook[castlingSide][move->chessBoard->fen->sideToMove] = move->castlingRook;
				move->chessBoard->fen->castlingRook[otherSide][move->chessBoard->fen->sideToMove] = move->otherCastlingRook;        
        const int dstRookSquare[2][2] = {{SquareF1, SquareF8}, {SquareD1, SquareD8}}; 
        const int dstKingSquare[2][2] = {{SquareG1, SquareG8}, {SquareC1, SquareC8}};
        const int srcRookSquare = move->chessBoard->fen->castlingRook[castlingSide][move->chessBoard->fen->sideToMove] + rookOffset[move->chessBoard->fen->sideToMove];
		    const int rook = PC(move->chessBoard->fen->sideToMove, Rook);
		    const int king = PC(move->chessBoard->fen->sideToMove, King);
		
		    // Restore rook FIRST: Clear current dst, add to src (XOR handles bits)
		    move->chessBoard->occupations[rook] ^= SQ_BIT(dstRookSquare[castlingSide][move->chessBoard->fen->sideToMove]);
		    move->chessBoard->piecesOnSquares[dstRookSquare[castlingSide][move->chessBoard->fen->sideToMove]] = PieceNameNone;
		    move->chessBoard->occupations[rook] |= SQ_BIT(srcRookSquare);
		
		    // Restore king LAST: Clear current dst (rook's old src or normal), add to original src
		    move->chessBoard->occupations[king] ^= SQ_BIT(move->dst);
		    move->chessBoard->piecesOnSquares[move->dst] = PieceNameNone;
		    move->chessBoard->occupations[king] |= SQ_BIT(move->src);
		    move->chessBoard->piecesOnSquares[move->src] = king;
        move->chessBoard->piecesOnSquares[srcRookSquare] = rook;
    } else {
    	// Rook-move or capture revoke: Restore only affected (other remains FileNone)
      if (PC_TYPE(move->movingPiece) == Rook || PC_TYPE(move->capturedPiece) == Rook) {
        // Check which was revoked (based on forward logic, but simple: restore if non-None)
        if (move->castlingRook != FileNone) {
            move->chessBoard->fen->castlingRook[0][move->chessBoard->fen->sideToMove] = move->castlingRook;  // Was kingside
        }
        if (move->otherCastlingRook != FileNone) {
            move->chessBoard->fen->castlingRook[1][move->chessBoard->fen->sideToMove] = move->otherCastlingRook;  // Was queenside
        }
      }
    }
    move->castlingRook = move->prevCastlingRook;

    // Step 9: Recompute occupations for all (now pieces are restored)
    move->chessBoard->occupations[PieceNameWhite] = move->chessBoard->occupations[WhiteBishop] | 
                                                    move->chessBoard->occupations[WhiteKing] | 
                                                    move->chessBoard->occupations[WhiteKnight] | 
                                                    move->chessBoard->occupations[WhitePawn] | 
                                                    move->chessBoard->occupations[WhiteQueen] | 
                                                    move->chessBoard->occupations[WhiteRook];
    move->chessBoard->occupations[PieceNameBlack] = move->chessBoard->occupations[BlackBishop] | 
                                                    move->chessBoard->occupations[BlackKing] | 
                                                    move->chessBoard->occupations[BlackKnight] | 
                                                    move->chessBoard->occupations[BlackPawn] | 
                                                    move->chessBoard->occupations[BlackQueen] | 
                                                    move->chessBoard->occupations[BlackRook];
    move->chessBoard->occupations[PieceNameAny] = move->chessBoard->occupations[PieceNameWhite] | 
                                                  move->chessBoard->occupations[PieceNameBlack];
    move->chessBoard->occupations[PieceNameNone] = ~move->chessBoard->occupations[PieceNameAny];
}

//fast-forward a valid uci move on a given board without init_move(), move will be set for updateHash() to work
void ff_move(struct Board * board, struct Move * move, const int src, const int dst, const int promo) {
  move->chessBoard = board;
  move->src = src;
  move->dst = dst; //in chess960 castling dst is castling rook square, not dst king's square, so we update it laster
  move->type = MoveTypeValid;
  
  const int oppColor = OPP_COLOR(board->fen->sideToMove);
	move->movingPiece = board->piecesOnSquares[src];
	const int mpType = PC_TYPE(move->movingPiece);
	//strcat(move->uciMove, squareName[src]);
	//strcat(move->uciMove, squareName[dst]);

	//remove the piece from its source square
	board->occupations[move->movingPiece] ^= SQ_BIT(src);
	board->piecesOnSquares[src] = PieceNameNone;

	//rook moves
	//need to find out if this is a castling rook and castling rights exist, to update FEN castling rights and rooks
	//mask two bits in castlingRights (bits 1-2 for white, bits 3-4 for black). CastlingSideBoth = 11
	if (mpType == Rook && (board->fen->castlingRights & (CastlingSideBoth << (board->fen->sideToMove << 1)))) {
  	int castlingSide = CastlingSideNone; //00
		const int  rookRank[2] = { Rank1, Rank8 };
		if (SQ_RANK(src) == rookRank[board->fen->sideToMove]) { //rook is on its initial rank
     	const int mpFile = SQ_FILE(src);
			if (mpFile == board->fen->castlingRook[0][board->fen->sideToMove]) //short castling rook file
				castlingSide = CastlingSideKingside; //01
			else if (mpFile == board->fen->castlingRook[1][board->fen->sideToMove]) //long castling rook file
				castlingSide = CastlingSideQueenside; //10
			if (castlingSide) {
				//revoke castling rights
				board->fen->castlingRights ^= (castlingSide << (board->fen->sideToMove << 1));
				//update castling rooks in FEN
				board->fen->castlingRook[castlingSide - 1][board->fen->sideToMove] = FileNone;
			}
		}
	}
	bool capture = false;
	
	//normal capture
	unsigned long long dstBit = SQ_BIT(dst);
	if (dstBit & board->occupations[PC(oppColor, PieceTypeAny)]) {
		capture = true;
		move->type |= MoveTypeCapture;
		move->capturedPiece = board->piecesOnSquares[dst];
		board->occupations[move->capturedPiece] ^= dstBit;
		//if captured piece is a castling rook, then remove the castling rights of the opponent on that side
		if (PC_TYPE(move->capturedPiece) == Rook) {
			const unsigned char whiteBlack[2] = { 0, 56 };
			int cr = CastlingRightsWhiteNoneBlackNone;
			if (dst == board->fen->castlingRook[0][oppColor] + whiteBlack[oppColor]) {
				cr = CastlingSideKingside << (oppColor << 1);
				if (board->fen->castlingRights & cr) {
					board->fen->castlingRights ^= cr;
					board->fen->castlingRook[0][oppColor] = FileNone;
					move->castlingRook = board->fen->castlingRook[0][oppColor];
				}
			}
			else if (dst == board->fen->castlingRook[1][oppColor] + whiteBlack[oppColor]) {
				cr = CastlingSideQueenside << (oppColor << 1);
				if (board->fen->castlingRights & cr) {
					board->fen->castlingRights ^= cr;
					board->fen->castlingRook[1][oppColor] = FileNone;
					move->castlingRook = board->fen->castlingRook[1][oppColor];
				}
			}
		}
	}

  //en passant capture and en passant move
	board->fen->enPassant = FileNone;
	int diff;
	if (mpType == Pawn && !capture) {
  	diff = abs(src - dst);
		if (diff == 7 || diff == 9) { 
  	//if ((board->fen->enPassant < FileNone) && board->fen->enPassant + (board->fen->sideToMove == ColorWhite ? Rank6 << 3 : Rank3 << 3) == dst) {
			capture = true;
		  move->type |= (MoveTypeCapture | MoveTypeEnPassant);
			const int sign = board->fen->sideToMove == ColorWhite ? -1 : 1;
			const int capturedPawnSquare = dst + sign * 8;
			move->capturedPiece = PC(oppColor, Pawn);
			board->occupations[move->capturedPiece] ^= SQ_BIT(capturedPawnSquare);
			board->piecesOnSquares[capturedPawnSquare] = PieceNameNone;
		} else if (diff == 16) {
			//all opponent pawns
			unsigned long long pawns = board->occupations[PC(oppColor, Pawn)];
	  	const int mpFile = SQ_FILE(src);
			//opponent pawns on Rank 4 or 5 depending on the side to move
			if (board->fen->sideToMove == ColorWhite) pawns &= RANK4;
			else pawns &= RANK5;
			//opponent pawns on adjacent files (adjacent to the moving pawn from its initial rank to rank 4 or 5)
			if (mpFile == FileA) pawns &= files_bb[mpFile + 1];
			else if (mpFile == FileH) pawns &= files_bb[mpFile - 1];
			else pawns &= (files_bb[mpFile + 1] | files_bb[mpFile - 1]);
			//if there are such opponent pawns, then the move is en passant
			if (pawns) {
				move->type |= MoveTypeEnPassant;
				board->fen->enPassant = mpFile;
			}		
		}
	}
  
	//castling
	if (mpType == King) {
		const int dstKingSquare[2][2] = { { SquareG1, SquareG8 }, { SquareC1, SquareC8 } };
		//special case of chess 960 castling - in uci notation the king moves to the square occupied by its own rook
		bool castling = false;
	  int castlingSide;
		if (board->fen->isChess960 && board->piecesOnSquares[dst] == PC(board->fen->sideToMove, Rook)) {
			castling = true;
  		const int dstFile = SQ_FILE(dst);
			if (dstFile == board->fen->castlingRook[0][board->fen->sideToMove]) {
				castlingSide = 0; //Kingside - unconventional to aovid subtracting 1
  			move->type |= MoveTypeCastlingKingside;
			}
			else if (dstFile == board->fen->castlingRook[1][board->fen->sideToMove]) {
				castlingSide = 1; //Queenside - unconventional to aovid subtracting 1
  			move->type |= MoveTypeCastlingQueenside;
			}
			else {
				updateFen(board);
				printf("ff_move() error: illegal castling move in chess 960: the %s rook on %s is not a castling one, fen %s\n", board->fen->sideToMove == ColorWhite ? "white" : "black", squareName[dst], board->fen->fenString);
				exit(-1);
			}
		} else if (abs(src - dst) == 2) {
			castling = true;
			if (dst == dstKingSquare[0][board->fen->sideToMove]) {
				castlingSide = 0; //Kingside - unconventional to aovid subtracting 1
  			move->type |= MoveTypeCastlingKingside;
			}
			else if (dst == dstKingSquare[1][board->fen->sideToMove]) {
				castlingSide = 1; //Queenside - unconventional to aovid subtracting 1
  			move->type |= MoveTypeCastlingQueenside;
			}
			else {
				updateFen(board);
				printf("ff_move() error: illegal %s king move from %s to %s; FEN %s\n", board->fen->sideToMove == ColorWhite ? "white" : "black", squareName[src], squareName[dst], board->fen->fenString);
				writeDebug(board, true);
				exit(-1);
			}
		}
		if (castling) {
			const unsigned char rookOffset[2] = { 0, 56 };
			const int dstRookSquare[2][2] = { { SquareF1, SquareF8 }, { SquareD1, SquareD8 } };
			const int srcRookSquare = board->fen->castlingRook[castlingSide][board->fen->sideToMove] + rookOffset[board->fen->sideToMove];
			const int rookName = PC(board->fen->sideToMove, Rook);
			//remove castling rook from its source square taking care of
			//Chess960 case of rook occupying king's destination square - make sure we are not removing (overwriting) the king with PieceNameNone!
			if (srcRookSquare != dstKingSquare[castlingSide][board->fen->sideToMove]) board->piecesOnSquares[srcRookSquare] = PieceNameNone;
			//and put it to its destination square
			board->piecesOnSquares[dstRookSquare[castlingSide][board->fen->sideToMove]] = rookName;
			//update occupations
			//xor out the rook on its source square
			board->occupations[rookName] ^= SQ_BIT(srcRookSquare);
			//add the rook to its destination square
			board->occupations[rookName] |= SQ_BIT(dstRookSquare[castlingSide][board->fen->sideToMove]);
			//update castling rook
			move->castlingRook = board->fen->castlingRook[castlingSide][board->fen->sideToMove];
			//chess960 king destination update
	  	if (board->fen->isChess960) move->dst = dstKingSquare[castlingSide][board->fen->sideToMove];
		}		
		//update FEN castling rights, i.e. revoke castling rights for the sideToMove and keep castling rights for the opponent if any
		board->fen->castlingRights &= (CastlingSideBoth << (oppColor << 1));
		//set castling rook to none
		board->fen->castlingRook[0][board->fen->sideToMove] = FileNone;
		board->fen->castlingRook[1][board->fen->sideToMove] = FileNone;
	}
	
	//promotion
	if (promo) {
		move->type |= MoveTypePromotion;
		move->promoPiece = PC(board->fen->sideToMove, promo);
		board->piecesOnSquares[dst] = move->promoPiece;
		board->occupations[move->promoPiece] |= dstBit;
  	move->uciMove[4] = uciPromoLetter[promo];	
	} 
	//other move
	else {
		//move the piece to its destination
		board->piecesOnSquares[move->dst] = move->movingPiece;
		board->occupations[move->movingPiece] |= SQ_BIT(move->dst);
	}
	
	//update occupations for all pieces
	board->occupations[PieceNameWhite] = board->occupations[WhiteBishop] | 
	                                     board->occupations[WhiteKing] | 
	                                     board->occupations[WhiteKnight] | 
	                                     board->occupations[WhitePawn] | 
	                                     board->occupations[WhiteQueen] | 
	                                     board->occupations[WhiteRook];
	board->occupations[PieceNameBlack] = board->occupations[BlackBishop] | 
	                                     board->occupations[BlackKing] | 
	                                     board->occupations[BlackKnight] | 
	                                     board->occupations[BlackPawn] | 
	                                     board->occupations[BlackQueen] | 
	                                     board->occupations[BlackRook];
	board->occupations[PieceNameAny] = board->occupations[PieceNameWhite] | 
	                                   board->occupations[PieceNameBlack];
	board->occupations[PieceNameNone] = ~board->occupations[PieceNameAny];
		
	//increment halfmove clock if not a pawn's move and not a capture
	if (mpType != Pawn && !capture) board->fen->halfmoveClock++;
	else board->fen->halfmoveClock = 0;
	
	//increment move number if it was black's move
	if (board->fen->sideToMove == ColorBlack) board->fen->moveNumber++;
	
	//toggle FEN SideToMove
	if (board->fen->sideToMove == ColorWhite) board->fen->sideToMove = ColorBlack;
	else board->fen->sideToMove = ColorWhite;
}

struct Board * cloneBoard(struct Board * src) {
    if (!src) return nullptr;
    struct Board * dst = new struct Board;
    *dst = *src;
    dst->fen = new struct Fen;
    if (src->fen) *dst->fen = *src->fen; 
    dst->zh = new struct ZobristHash;
    if (src->zh) *dst->zh = *src->zh; 
    return dst;
}

void freeBoard(struct Board * board) {
    if (board->fen) delete board->fen;
    if (board->zh) delete board->zh;
    delete board;
}

#ifdef __cplusplus
}
#endif
