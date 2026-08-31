#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <array>
#include <time.h>
#include <wchar.h>
#include <locale.h>
//#include "magic_bitboards.h"
#include "nnue/bitboard.h"
#include "libchess.h"
#ifdef _WIN32
#include "windows.h"
#include "stdio.h"
#endif

void drawMoves(const Board& board, const Square startSq, uint64_t moves) {
  Piece pc = board.piecesOnSquares[startSq];
  if (PC_COLOR(pc) != board.sideToMove) {
    printf("Piece on %s is %s but side to move is %s\n", square[startSq], piece[pc], color[board.sideToMove]);
    return;
  }
  printf("%s on %s moves (x): %llx\n", piece[pc], square[startSq], moves);
  printf("+---+---+---+---+---+---+---+---+\n");
  // Standard chess printing: Rank 8 down to Rank 1
  for (int r = 7; r >= 0; --r) {
    printf("|");
    for (int f = 0; f <= 7; ++f) {
      Square currentSq = static_cast<Square>(r * 8 + f);      
      if (currentSq == startSq) {
        // Highlight the piece itself (e.g., in brackets or capital)
        printf(" %c*", pieceLetter[board.piecesOnSquares[currentSq]]);
      } else if (SQ_BIT(currentSq) & moves) {
        // This square is a valid move
        printf(" x ");
      } else {
        // Just print the piece or an empty space
        char p = pieceLetter[board.piecesOnSquares[currentSq]];
        //printf(" %c ", (p == ' ') ? '.' : p); // Use . for empty squares
        printf(" %c ", p); // Use . for empty squares
      }
      printf("|");
    }
    printf(" %d\n+---+---+---+---+---+---+---+---+\n", r + 1);
  }
  printf("  a   b   c   d   e   f   g   h\n");
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
	for (Square i = SquareA1; i <= SquareH8; ++i) {
		wchar_t s[16] = {};
		Rank row = SQ_RANK(i); // 0..7
		File col = SQ_FILE(i); // 0..7
		if (SQ_FILE(i + 1) == FileA) {
			if (((row & 1) == 0 && (col & 1) == 0) || ((row & 1) && (col & 1)))
			  swprintf(s, sizeof(s), L"\u2502\033[47m%lc\033[0m\u2502", pieceLetter[board.piecesOnSquares[i]]);			
			else
			  swprintf(s, sizeof(s), L"\u2502%lc\u2502", pieceLetter[board.piecesOnSquares[i]]);
			wcscat(buffer[rank++], s);
		}
		else {
			if (((row & 1) == 0 && (col & 1) == 0) || ((row & 1) && (col & 1)))
			  swprintf(s, sizeof(s), L"\u2502\033[47m%lc\033[0m", pieceLetter[board.piecesOnSquares[i]]);			
			else
			  swprintf(s, sizeof(s), L"\u2502%lc", pieceLetter[board.piecesOnSquares[i]]);
			wcscat(buffer[rank], s);
		}
	}
	printf("\u250C\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2510\n");
	for (signed char s = 7; s >= 0; --s) {
		printf("%ls\n", buffer[s]);
		printf("\u2514\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2518\n");
	}
	wchar_t w[16] = {};
	wchar_t b[16] = {};
	int idx_w = 0, idx_b = 0;
	for (PieceType pt = Pawn; pt < King; ++pt) {
		uint8_t white = bitCount(board.side[ColorWhite] & board.pieceTypes[pt - 1]);
		uint8_t black = bitCount(board.side[ColorBlack] & board.pieceTypes[pt - 1]);
		if (white >= black) {
		  for (int i = 0; i < white - black; ++i) {
		  	b[idx_b++] = L' ';
		  	b[idx_b++] = pieceLetter[PC(ColorBlack, pt)];
		  }
		} else {
		  for (int i = 0; i < black - white; ++i) {
		  	w[idx_w++] = L' ';
		  	w[idx_w++] = pieceLetter[PC(ColorWhite, pt)];
		  }
		}
	}
	printf("%ls\u2502%ls\n", w, b);
}

int reconcile(const Board& board) {
	int err = 0;
	char fenString[MAX_FEN_STRING_LEN] = "";
	for (int i = SquareA1; i <= SquareH8; ++i) {
		const Piece pn = board.piecesOnSquares[i];
		const int pt = PC_TYPE(pn) - 1;
		const Color color = PC_COLOR(pn);
	  if (pn != PieceNone && !(board.side[color] & board.pieceTypes[pt] & (1ULL << i))) {
		  printf("reconcile() error: piecesOnSquares[%s] %s does not match its occupations bitboard %llx, FEN %s\n", square[i], piece[pn], board.side[color] & board.pieceTypes[pt], board2fen(board, fenString));
		  err = 1;
		} 
	}
	for (Color color = ColorWhite; color <= ColorBlack; ++color) {
		for (int type = Pawn - 1; type <= King - 1; ++type) {
  		Piece pn = PC(color, static_cast<PieceType>(type + 1));
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
  for (int rank = Rank8; rank >= Rank1; --rank) { //cannot decrement Rank type because it is uint8_t and wraps to 255 after decrement 0 - 1 - looping infinnitely!
    int empty_count = 0;
    for (File file = FileA; file <= FileH; ++file) {
      Square sq = SQ(Rank(rank), file);
      Piece piece = board.piecesOnSquares[sq];
      if (piece == PieceNone) {
        ++empty_count;
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
	if (!board.castlingRooks) {		
    castling[0] = '-';
	} else {		
    uint8_t idx = 0;
		for (Color color = ColorWhite; color <= ColorBlack; ++color) {
      uint64_t bb = board.castlingRooks & board.side[color];
	    while (bb) {
	    	const Square sq = popMSB(bb);
	    	if (board.isChess960)
	    	  castling[idx++] = SQ_RANK(sq) == Rank1 ? toupper(static_cast<char>(SQ_FILE(sq)) + 'a') : static_cast<char>(SQ_FILE(sq)) + 'a';
	    	else {
	    		if (sq == SquareH1) castling[idx++] = 'K';
	    		else if (sq == SquareA1) castling[idx++] = 'Q';
	    		else if (sq == SquareH8) castling[idx++] = 'k';
	    		else if (sq == SquareA8) castling[idx++] = 'q';
	    		else {
	    			printf("board2fen() error: unknown rook square %s, board.castlingRooks %llx\n", square[sq], board.castlingRooks);
	    		}
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

// Pass the capturing pawn's src square (sq) and the captured pawn's square (ep)
inline bool isEnPassantLegal(const Board& board, const Square sq, const Square ep) {
    Square kingSq = kingSquare(board, board.sideToMove);
    if (SQ_RANK(kingSq) != SQ_RANK(sq)) return true;
    //remove both pawns
    uint64_t occ = occupations(board);
    occ ^= (SQ_BIT(sq) | SQ_BIT(ep)); 
    //uint64_t rankAttacks = get_rook_moves(kingSq, occ) & ranks_bb[SQ_RANK(kingSq)];
    uint64_t rankAttacks = Stockfish::attacks_bb<Stockfish::ROOK>(static_cast<Stockfish::Square>(kingSq), occ) & ranks_bb[SQ_RANK(kingSq)];
    uint64_t enemySliders = board.side[OPP_COLOR(board.sideToMove)] & (board.pieceTypes[Rook - 1] | board.pieceTypes[Queen - 1]);
    return (rankAttacks & enemySliders) == 0;
}

//returns en passant square (dst sq) if en passant move is legal or SquareNone otherwise
//this function does not check for pins!
Square legalEnPassantMove(const Board& board) {
  if (board.enPassant == FileNone) return SquareNone;
  auto pawnRank = ep_ranks[board.sideToMove].first;
  auto enPassantRank = ep_ranks[board.sideToMove].second;
  const Piece capturingPawn = PC(board.sideToMove, Pawn);
  Square ep = SQ(pawnRank, board.enPassant); //to be captured pawn square
  bool valid = false;
  // Check if the pawn to the left can capture
  if (board.enPassant > FileA) {
    Square srcL = SQ(pawnRank, static_cast<File>(board.enPassant - 1));
    if (board.piecesOnSquares[srcL] == capturingPawn) {
      if (isEnPassantLegal(board, srcL, ep)) valid = true;
    }
  }
  // Check if the pawn to the right can capture
  if (!valid && board.enPassant < FileH) {
    Square srcR = SQ(pawnRank, static_cast<File>(board.enPassant + 1));
    if (board.piecesOnSquares[srcR] == capturingPawn) {
      if (isEnPassantLegal(board, srcR, ep)) valid = true;
    }
  }
  return valid ? SQ(enPassantRank, board.enPassant) : SquareNone;
}

//returns en passant square if en passant capture from sq is legal or SquareNone otherwise
Square legalEnPassantMoveFromSq(const Board& board, const Square sq) {
    if (board.enPassant == FileNone) return SquareNone;  
    const uint64_t pawnRanks[] = { RANK5, RANK4 };
    if (!(SQ_BIT(sq) & pawnRanks[board.sideToMove])) return SquareNone;
    File sqFile = SQ_FILE(sq);
    if (sqFile != board.enPassant - 1 && sqFile != board.enPassant + 1) return SquareNone;
    Square ep = SQ(SQ_RANK(sq), board.enPassant); 
    if (!isEnPassantLegal(board, sq, ep)) return SquareNone;
    const Rank enPassantRank[] = { Rank6, Rank3 };
    return SQ(enPassantRank[board.sideToMove], board.enPassant);
}

//generates knight moves from a given square limited by board boundaries only
//it seems to be faster to get knight moves from array[64]
/*inline constexpr uint64_t generateKnightMoves(const Square sq) {
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
}*/

//generates king moves from a given square limited by board boundaries only
/*inline constexpr uint64_t generateKingMoves(const Square sq) {
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
}*/

// Logic for a single square and color
/*inline constexpr uint64_t generatePawnAttacks(Color color, Square sq) {
  uint64_t attacks = 0;
  uint64_t bit = SQ_BIT(sq);
  if (color == ColorWhite) {
    // White attacks: Up-Left (-1 + 8 = 7) and Up-Right (1 + 8 = 9)
    if (SQ_FILE(sq) > FileA) attacks |= (bit << 7);
    if (SQ_FILE(sq) < FileH) attacks |= (bit << 9);
  } else {
    // Black attacks: Down-Left (-1 - 8 = -9) and Down-Right (1 - 8 = -7)
    if (SQ_FILE(sq) > FileA) attacks |= (bit >> 9);
    if (SQ_FILE(sq) < FileH) attacks |= (bit >> 7);
  }
  return attacks;
}*/

// Array generator function
/*template <uint64_t (*GenFunc)(Square)>
constexpr std::array<uint64_t, 64> initMoves() {
  std::array<uint64_t, 64> table{};
  for (Square sq = SquareA1; sq < SquareNone; ++sq) {
    table[sq] = GenFunc(sq);
  }
  return table;
}*/

// Global move tables
//constexpr auto KnightMoves = initMoves<generateKnightMoves>();
//constexpr auto KingMoves = initMoves<generateKingMoves>();
// 2 rows (colors), 64 columns (squares)
/*constexpr std::array<std::array<uint64_t, 64>, 2> PawnAttacks = [] {
  std::array<std::array<uint64_t, 64>, 2> arr{}; //the first arg square (column) is the fastest index, color (row) is the second arg
  for (int c = 0; c < 2; ++c) {
    for (int s = 0; s < 64; ++s) {
      arr[c][s] = generatePawnAttacks(static_cast<Color>(c), static_cast<Square>(s)); //this is correct PawnAttacks[color][square]
    }
  }
  return arr;
}();*/

/*static const std::array<std::array<uint64_t, 64>, 64> RayFrom = [] {
  init_magic_bitboards();
  std::array<std::array<uint64_t, 64>, 64> arr{}; //the first arg square (column) is the fastest index, square (row) is the second arg
  for (int s1 = 0; s1 < 64; ++s1) {
    for (PieceType pt : {Bishop, Rook}) {
      for (int s2 = 0; s2 < 64; ++s2) {
        uint64_t moves = pt == Bishop ? get_bishop_moves(s1, 0) : get_rook_moves(s1, 0);
        if (moves & SQ_BIT(s2)) {
          arr[s1][s2] = pt == Bishop ? get_bishop_moves(s1, 0) & (get_bishop_moves(s2, SQ_BIT(s1)) | s2) : get_rook_moves(s1, 0) & (get_rook_moves(s2, SQ_BIT(s1)) | s2);
        }
      }
    }
  }
  cleanup_magic_bitboards();
  return arr;
}();*/


//return piece moves (except king) not taking into account pins, which will be filtered later
uint64_t getMoves(const Board& board, const Square sq, const PieceType pt) {
  if (pt == Pawn) {
		//capture moves
		//uint64_t moves = PawnAttacks[board.sideToMove][sq] & board.side[OPP_COLOR(board.sideToMove)];
		uint64_t moves = Stockfish::PseudoAttacks[board.sideToMove][sq] & board.side[OPP_COLOR(board.sideToMove)];
		//en passant moves
		Square ep = legalEnPassantMoveFromSq(board, sq); //does not include pinned pawn - filtered later
		if (ep != SquareNone) moves |= SQ_BIT(ep);
    //pawn push
		uint64_t push = board.sideToMove == ColorWhite ? (SQ_BIT(sq) << 8) : SQ_BIT(sq) >> 8;
		push &= ~(occupations(board));
    //double push 
		uint64_t double_push = 0;
    const Rank pawnRanks[2] = { Rank2, Rank7 };
    if (SQ_RANK(sq) == pawnRanks[board.sideToMove]) {
    	double_push = board.sideToMove == ColorWhite ? push << 8 : push >> 8;
    	double_push &= ~(occupations(board));
    }
		moves |= (push | double_push);
		return moves;
  } else if (pt == Rook) {
  	//return get_rook_moves(sq, occupations(board)) & ~board.side[board.sideToMove];
  	return Stockfish::attacks_bb<Stockfish::ROOK>(static_cast<Stockfish::Square>(sq), occupations(board)) & ~board.side[board.sideToMove];
  } else if (pt == Bishop) {
  	//return get_bishop_moves(sq, occupations(board)) & ~board.side[board.sideToMove];
  	return Stockfish::attacks_bb<Stockfish::BISHOP>(static_cast<Stockfish::Square>(sq), occupations(board)) & ~board.side[board.sideToMove];
  } else if (pt == Knight) {  	
  	//return KnightMoves[sq] & ~board.side[board.sideToMove];
  	return Stockfish::PseudoAttacks[Knight][sq] & ~board.side[board.sideToMove];
  }	else if (pt == Queen) {
		//return (get_bishop_moves(sq, occupations(board)) | get_rook_moves(sq, occupations(board))) & ~board.side[board.sideToMove];
		return (Stockfish::attacks_bb<Stockfish::BISHOP>(static_cast<Stockfish::Square>(sq), occupations(board)) | Stockfish::attacks_bb<Stockfish::ROOK>(static_cast<Stockfish::Square>(sq), occupations(board))) & ~board.side[board.sideToMove];
  } else return 0;
}

//much improved universal pin finder that relies in LineBetween table, which excludes start sq and includes end sq
//it returns pinned and pinning bitmasks
std::pair<uint64_t, uint64_t> pinFinder(const Board& board, const Square kingSq) {
  Color oppColor = OPP_COLOR(board.sideToMove);
  uint64_t pinnedMask = 0, pinningMask = 0;
  //get opponent sliding pieces moves that potentialy hit our king (using "reverse attack" trick), i.e.
  //moves that are on the same rank, file or diag/antidiag as the king
  /*uint64_t potentialPinners = 
    (get_rook_moves(kingSq, 0) & (board.side[oppColor] & (board.pieceTypes[Rook - 1] | board.pieceTypes[Queen - 1]))) |
    (get_bishop_moves(kingSq, 0) & (board.side[oppColor] & (board.pieceTypes[Bishop - 1] | board.pieceTypes[Queen - 1])));*/
  uint64_t potentialPinners = 
    (Stockfish::attacks_bb<Stockfish::ROOK>(static_cast<Stockfish::Square>(kingSq), 0) & (board.side[oppColor] & (board.pieceTypes[Rook - 1] | board.pieceTypes[Queen - 1]))) |
    (Stockfish::attacks_bb<Stockfish::BISHOP>(static_cast<Stockfish::Square>(kingSq), 0) & (board.side[oppColor] & (board.pieceTypes[Bishop - 1] | board.pieceTypes[Queen - 1])));  
  const uint64_t occ = occupations(board);
  const uint64_t us = board.side[board.sideToMove];
  while (potentialPinners) {
    Square sq = popLSB(potentialPinners);
    //BetweenBB excludes kingSq but includes sq, so XOR the pinner out and count every
    //piece left standing between the king and it - ENEMY pieces included. Counting only
    //our own pieces here reports a false pin whenever an enemy piece blocks the ray.
    uint64_t blockers = (Stockfish::BetweenBB[kingSq][sq] ^ SQ_BIT(sq)) & occ;
    // Exactly one piece between king and pinner, and it is ours -> it is pinned
    if (bitCount(blockers) == 1 && (blockers & us)) {
      pinnedMask |= blockers;
      pinningMask |= SQ_BIT(sq);
    }
  }
  return std::make_pair(pinnedMask, pinningMask);
}

std::pair<uint64_t, uint64_t> checkMask(const Board& board, Square kingSq, uint64_t checkers) {
    if (bitCount(checkers) > 1) return std::make_pair(0ULL, 0ULL);
    Square checkerSq = lsBit(checkers);
    uint64_t mask = checkers;    
    uint64_t ep_mask = 0;
    uint64_t sliders = board.side[OPP_COLOR(board.sideToMove)] & (board.pieceTypes[Bishop - 1] | board.pieceTypes[Rook - 1] | board.pieceTypes[Queen - 1]);
    if (sliders & checkers) mask = Stockfish::BetweenBB[kingSq][checkerSq];// | checkers; //including checker - checkerSq is included in BetweenBB
    else if (board.enPassant != FileNone && legalEnPassantMove(board) != SquareNone) { //enPassant is set for any double pawn advance, we must ensure that it is a legal capture!
      auto pawnRank = ep_ranks[board.sideToMove].first; //capturing pawn src rank
      auto enPassantRank = ep_ranks[board.sideToMove].second; //capturing pawn dst rank
      Square ep = SQ(pawnRank, board.enPassant); //to be captured pawn square
      // If the checking piece is indeed that double-pushed pawn, add the EP square to the mask.
      if (checkerSq == ep) ep_mask = SQ_BIT(SQ(enPassantRank, board.enPassant)); //this is wrong! - en passant capture mask should be separate from general check mask and should only be applied to pawn captures! Otherwise, it can be attempted to be captured by a non-pawn piece, which is illegal!
    }
    return std::make_pair(mask, ep_mask);
}

uint64_t piece_moves(Board& board, const PieceType pt, const Square sq, const Square kingSq, const uint64_t pinned, const uint64_t pinning, const uint64_t check_mask, const uint64_t ep_mask) {
	uint64_t moves = getMoves(board, sq, pt);
	//if (pinned & SQ_BIT(sq)) moves &= LineThrough[kingSq][sq];
	if (pinned & SQ_BIT(sq)) moves &= Stockfish::LineBB[kingSq][sq];
  if (pt == Pawn) moves &= (check_mask | ep_mask);
  else moves &= check_mask;
  //A pawn landing on the promotion rank is FOUR legal moves (N, B, R, Q); expanding them is the
  //caller's job (test_pos.cpp:154, expand() in test_smp.cpp / creatica-shared-root.cpp), so the
  //extra three have to be added here. Without this a position whose only legal move is a
  //promotion reports num_moves == 1 and the "forced move" branch auto-plays an unsearched
  //promotion instead of running MCTS over the four choices.
  if (pt == Pawn) board.num_moves += 3 * bitCount(moves & base_rank_bb[OPP_COLOR(board.sideToMove)]);
  board.num_moves += bitCount(moves);
  return moves;
}

//returns only attacked squares, no pins, no checkers (call getCheckers() and pinFinder() separately)
uint64_t getAttackedSquaresOnly(const Board& board) {
	const Color oppColor = OPP_COLOR(board.sideToMove);
	uint64_t attackedSquares = 0;
	uint64_t any = occupations(board) ^ (board.side[board.sideToMove] & board.pieceTypes[King - 1]);
	uint64_t bitboard = board.side[oppColor] & board.pieceTypes[Bishop - 1];
	while (bitboard) {
  	Square sq = popLSB(bitboard);
		//attackedSquares |= get_bishop_moves(sq, any);
		attackedSquares |= Stockfish::attacks_bb<Stockfish::BISHOP>(static_cast<Stockfish::Square>(sq), any);
	}
	bitboard = board.side[oppColor] & board.pieceTypes[Rook - 1];
  while (bitboard) {
  	Square sq = popLSB(bitboard);
		//attackedSquares |= get_rook_moves(sq, any);
		attackedSquares |= Stockfish::attacks_bb<Stockfish::ROOK>(static_cast<Stockfish::Square>(sq), any);
	}
	bitboard = board.side[oppColor] & board.pieceTypes[Queen - 1];
	while (bitboard) {
  	Square sq = popLSB(bitboard);
		//attackedSquares |= get_bishop_moves(sq, any) | get_rook_moves(sq, any);
		attackedSquares |= Stockfish::attacks_bb<Stockfish::BISHOP>(static_cast<Stockfish::Square>(sq), any) | Stockfish::attacks_bb<Stockfish::ROOK>(static_cast<Stockfish::Square>(sq), any);
	}
	bitboard = board.side[oppColor] & board.pieceTypes[Pawn - 1];
	while (bitboard) {
		Square sq = popLSB(bitboard);
		//attackedSquares |= PawnAttacks[oppColor][sq];
		attackedSquares|= Stockfish::PseudoAttacks[oppColor][sq];
	}
	bitboard = board.side[oppColor] & board.pieceTypes[Knight - 1];
	while (bitboard) {
		Square sq = popLSB(bitboard);
		//attackedSquares |= KnightMoves[sq];
		attackedSquares |= Stockfish::PseudoAttacks[Knight][sq];
	}
  //attackedSquares |= KingMoves[kingSquare(board, oppColor)];
  attackedSquares |= Stockfish::PseudoAttacks[King][kingSquare(board, oppColor)];
  return attackedSquares;
}

/*uint64_t attacks_bb(Piece pc, Square s, uint64_t occupied) {
    switch (PC_TYPE(pc)) {
      case Pawn: return PawnAttacks[PC_COLOR(pc)][s];
      case Knight: return KnightMoves[s];
      case Bishop: return get_bishop_moves(s, occupied);
      case Rook: return get_rook_moves(s, occupied);
      case Queen: return get_bishop_moves(s, occupied) | get_rook_moves(s, occupied);
      case King: return KingMoves[s];
      default: return 0;
    }
}*/


//returns both attackers and defenders
/*uint64_t attackers_to(const Board& board, const Square sq, const uint64_t occupied) {
  return (get_rook_moves(sq, occupied) & (board.pieceTypes[Rook - 1] | board.pieceTypes[Queen - 1])) |
         (get_bishop_moves(sq, occupied) & (board.pieceTypes[Bishop - 1] | board.pieceTypes[Queen - 1])) |
         (KnightMoves[sq] & board.pieceTypes[Knight - 1]) |
         (PawnAttacks[ColorBlack][sq] & board.side[ColorWhite] & board.pieceTypes[Pawn - 1]) |
         (PawnAttacks[ColorWhite][sq] & board.side[ColorBlack] & board.pieceTypes[Pawn - 1]) |
         (KingMoves[sq] & board.pieceTypes[King - 1]);
}*/

uint64_t attackers_to(const Board& board, const Square sq, const uint64_t occupied) {
  return (Stockfish::attacks_bb<Stockfish::ROOK>(static_cast<Stockfish::Square>(sq), occupied) & (board.pieceTypes[Rook - 1] | board.pieceTypes[Queen - 1])) |
         (Stockfish::attacks_bb<Stockfish::BISHOP>(static_cast<Stockfish::Square>(sq), occupied) & (board.pieceTypes[Bishop - 1] | board.pieceTypes[Queen - 1])) |
         (Stockfish::PseudoAttacks[Knight][sq] & board.pieceTypes[Knight - 1]) |
         (Stockfish::PseudoAttacks[ColorBlack][sq] & board.side[ColorWhite] & board.pieceTypes[Pawn - 1]) |
         (Stockfish::PseudoAttacks[ColorWhite][sq] & board.side[ColorBlack] & board.pieceTypes[Pawn - 1]) |
         (Stockfish::PseudoAttacks[King][sq] & board.pieceTypes[King - 1]);
}


//Static Exchange Evaluation (SEE)
//move has been made!
//returns the result from board.sideToMove perspective
// test cases: 
// 1. pawn has captured knight (nnue sees -300), then pawn captures pawn (net result -200) => the side to move loses 200
// 2. knight has captured pawn (nnue sees -100), then pawn captures knight (net result 200) => the side to move wins 200
//returns the score from side to move perspective: -200 in first case and 200 in the second case
//correction logic is unclear... nnue returns absolute value, get_see relative to the exchange
//the correction logic of evaluate_nnue() result is
// if (score = get_see() > 0) res = evaluate_nnue() = score;
// if (score = get_see() < 0) res = evaluate_nnue() += (evaluate_nnue() - score);
/*int get_see(const Board& board, const Move move, const PieceType capturedType) {
  int score[32]; // Stack to track the exchange sequence
  int depth = 0;  
  PieceType attacker = PC_TYPE(board.piecesOnSquares[move.dst]); //1. pawn; 2. knight
  PieceType victim = capturedType; // 1. knight; 2. pawn
  PieceType pieceOnDst = attacker; // 1. pawn; 2. knight
  score[0] = pieceValueCP[victim]; // 1. knight(300); 2. pawn(100)
  if (move.promoType != PieceTypeNone) {
      pieceOnDst = move.promoType; // The piece sitting on the square is now the promoted piece
      // Add the material gained by promoting the pawn
      score[0] += pieceValueCP[move.promoType] - pieceValueCP[Pawn];
  }
  Color side = board.sideToMove;
  uint64_t occupied = occupations(board);  
  while (true) {
    // Find all pieces attacking the dst square.
    // The mask `& occupied` ensures we only consider pieces currently on the board.
    uint64_t attackers = attackers_to(board, move.dst, occupied) & board.side[side] & occupied;
    
    if (!attackers) break;
    
    // Get the LVA (Least Valuable Attacker: Pawn, then Knight, then Bishop...)
    PieceType lva_type = PieceTypeNone;
    for (PieceType pt = Pawn; pt <= King; ++pt) { 
      const uint64_t pt_attackers = attackers & board.pieceTypes[pt - 1]; 
      if (pt_attackers) {
        lva_type = pt; //1. pawn, 2. pawn 
        occupied ^= SQ_BIT(lsBit(pt_attackers)); // Remove the LVA from the occupied bitboard
        break; 
      }
    }
    //lva_type cannot be PieceTypeNone here because attackers != 0    
    depth++;
    // The score at this depth is the value of the piece sitting on the dst square 
    // minus the score the opponent could achieve up to the previous depth.
    score[depth] = pieceValueCP[pieceOnDst] - score[depth - 1]; //1. 100 - 300 = -200; 2. 300 - 100 = 200
    
    // Alpha-beta style pruning: if the side to move can stop the capture sequence and be ahead, they will.
    if (std::max(-score[depth - 1], score[depth]) < 0) break; //1. -300 < -200 = -200 < 0 -> break (winning); 2. -100 < 200 = 200 > 0 -> continue (losing)
    
    side = OPP_COLOR(side);
    pieceOnDst = lva_type; // The piece that just captured now sits on the destination square //2. pawn
  }
  
  // Evaluate the score stack from bottom up (Minimax unrolling)
  while (depth > 0) {
    score[depth - 1] = -std::max(-score[depth - 1], score[depth]); //1. -200; 2. 200
    depth--;
  }  
  return score[0]; //1. -200; 2. 200
}*/

//same as above except the move has not been made
//the correction logic of evaluate_nnue() result is
// if (score = get_see() < 0) res = evaluate_nnue() += score;
/*int get_see(const Board& board, Move move) {
  int score[32]; // Stack to track the exchange sequence
  int depth = 0;  
  
  PieceType attacker = PC_TYPE(board.piecesOnSquares[move.src]);
  PieceType victim = PC_TYPE(board.piecesOnSquares[move.dst]);
  PieceType pieceOnDst = attacker;
  
  uint64_t occupied = occupations(board) ^ SQ_BIT(move.src); 

  // 1. Handle En Passant
  // If a pawn moves diagonally to an empty square, it is an en passant capture.
  if (attacker == Pawn && victim == PieceTypeNone && SQ_FILE(move.src) != SQ_FILE(move.dst)) {
      victim = Pawn;
      // The captured pawn is on the same rank as the attacking pawn, but on the destination file
      Square epCapturedSq = SQ(SQ_RANK(move.src), SQ_FILE(move.dst));
      occupied ^= SQ_BIT(epCapturedSq); // Remove the captured pawn from the occupied bitboard
  }

  // Base gain is the value of the victim
  score[0] = pieceValueCP[victim];

  // 2. Handle Promotions
  if (move.promoType != PieceTypeNone) {
      pieceOnDst = move.promoType; // The piece sitting on the square is now the promoted piece
      // Add the material gained by promoting the pawn
      score[0] += pieceValueCP[move.promoType] - pieceValueCP[Pawn];
  }

  Color side = OPP_COLOR(board.sideToMove); 

  while (true) {
    // Find all pieces attacking the dst square.
    // The mask `& occupied` ensures we only consider pieces currently on the board.
    uint64_t attackers = attackers_to(board, move.dst, occupied) & board.side[side] & occupied;
    
    if (!attackers) break;
    
    // Get the LVA (Least Valuable Attacker: Pawn, then Knight, then Bishop...)
    PieceType lva_type = PieceTypeNone;
    for (PieceType pt = Pawn; pt <= King; ++pt) { 
      const uint64_t pt_attackers = attackers & board.pieceTypes[pt - 1]; 
      if (pt_attackers) {
        lva_type = pt;
        occupied ^= SQ_BIT(lsBit(pt_attackers)); // Remove the LVA from the occupied bitboard
        break; 
      }
    }
    
    depth++;
    // The score at this depth is the value of the piece sitting on the square 
    // minus the score the opponent could achieve up to the previous depth.
    score[depth] = pieceValueCP[pieceOnDst] - score[depth - 1];    
    
    // Alpha-beta style pruning: if the side to move can stop the capture sequence and be ahead, they will.
    if (std::max(-score[depth - 1], score[depth]) < 0) break;
    
    side = OPP_COLOR(side);
    pieceOnDst = lva_type; // The piece that just captured now sits on the destination square
  }
  
  // Evaluate the score stack from bottom up (Minimax unrolling)
  while (depth > 0) {
    score[depth - 1] = -std::max(-score[depth - 1], score[depth]);
    depth--;
  }
  
  return score[0];
}*/

//The "Reverse" Attack Logic to quickly calculate the checkers
/*uint64_t getCheckers(const Board& board, const Square kingSq) {
  uint64_t occ = occupations(board);
  return ((get_rook_moves(kingSq, occ) & (board.pieceTypes[Rook - 1] | board.pieceTypes[Queen - 1])) |
         (get_bishop_moves(kingSq, occ) & (board.pieceTypes[Bishop - 1] | board.pieceTypes[Queen - 1])) |
         (KnightMoves[kingSq] & board.pieceTypes[Knight - 1]) |
         (PawnAttacks[board.sideToMove][kingSq] & board.pieceTypes[Pawn - 1])) & board.side[OPP_COLOR(board.sideToMove)];    
}*/

uint64_t getCheckers(const Board& board, const Square kingSq) {
  uint64_t occ = occupations(board);
  return ((Stockfish::attacks_bb<Stockfish::ROOK>(static_cast<Stockfish::Square>(kingSq), occ) & (board.pieceTypes[Rook - 1] | board.pieceTypes[Queen - 1])) |
         (Stockfish::attacks_bb<Stockfish::BISHOP>(static_cast<Stockfish::Square>(kingSq), occ) & (board.pieceTypes[Bishop - 1] | board.pieceTypes[Queen - 1])) |
         (Stockfish::PseudoAttacks[Knight][kingSq] & board.pieceTypes[Knight - 1]) |
         (Stockfish::PseudoAttacks[board.sideToMove][kingSq] & board.pieceTypes[Pawn - 1])) & board.side[OPP_COLOR(board.sideToMove)];    
}

uint64_t castlingMoves(const Board& board, const Square kingSq, uint64_t attackedSquares) {
  uint64_t moves = 0;
  uint64_t rooks = board.castlingRooks & board.side[board.sideToMove];
  if (!rooks) return moves; // No rights left at all

  // Kingside (Highest index rook)
  Square kRookSq = msBit(rooks); 
  // We must ensure the rook we found is actually a Kingside rook.
  // In Standard Chess, this is File H. In 960, we compare to King position.
  if (kRookSq > kingSq) { 
    const auto& cp = CastlingPath[board.sideToMove][0];
    //printf("castlingMoves() debug: cp.path %llx, cp.checkZone %llx\n", cp.path, cp.checkZone);
    if (!(cp.path & occupations(board)) && !(cp.checkZone & attackedSquares)) {
      //moves = board.isChess960 ? SQ_BIT(castlingRookSquare[board.sideToMove][0]) : SQ_BIT(castlingKingSquare[board.sideToMove][0]);
      moves = board.isChess960 ? SQ_BIT(kRookSq) : SQ_BIT(castlingKingSquare[board.sideToMove][0]);
    }
  }
  // Queenside (Highest index rook)
  Square qRookSq = lsBit(rooks); 
  // We must ensure the rook we found is actually a Queenside rook.
  // In Standard Chess, this is File A. In 960, we compare to King position.
  if (qRookSq < kingSq) { 
    const auto& cp = CastlingPath[board.sideToMove][1];
    //printf("castlingMoves() debug: cp.path %llx, cp.checkZone %llx\n", cp.path, cp.checkZone);
    if (!(cp.path & occupations(board)) && !(cp.checkZone & attackedSquares)) {
      //moves |= board.isChess960 ? SQ_BIT(castlingRookSquare[board.sideToMove][1]) : SQ_BIT(castlingKingSquare[board.sideToMove][1]);
      moves |= board.isChess960 ? SQ_BIT(qRookSq) : SQ_BIT(castlingKingSquare[board.sideToMove][1]);
    }
  } 
 	return moves;
}

//legal king moves
//also calculates blocking squares for checks
std::pair<uint64_t, uint64_t> king_moves(Board& board, Square kingSq, uint64_t attackedSquares) {
  
  //debug: check if opponent's king is under attack - it must not be!
  //flip sideToMove temporarily 
  //board.sideToMove = OPP_COLOR(board.sideToMove);
  //get squares attacked by actual sideToMove
  //uint64_t attackedSquares2 = getAttackedSquaresOnly(board);
  //if sideToMove attacks the opponent's king, it can simply capture it, which is illegal position
  //if (attackedSquares2 & board.side[board.sideToMove] & board.pieceTypes[King - 1]) {
  //	char fen[MAX_FEN_STRING_LEN];
  	//restore sideToMove to output the correct FEN
  //  board.sideToMove = OPP_COLOR(board.sideToMove); 
  //	printf("king_moves() debug: opponent's king is checked! fen %s\n", board2fen(board, fen));
  //	exit(1);
  //}
  //restore sideToMove
  //board.sideToMove = OPP_COLOR(board.sideToMove);
  //end of debug
  
  board.isMate = false; board.isStaleMate = false;
  //uint64_t moves = KingMoves[kingSq] & ~(board.side[board.sideToMove] | attackedSquares);
  uint64_t moves = Stockfish::PseudoAttacks[King][kingSq] & ~(board.side[board.sideToMove] | attackedSquares);
  uint64_t checkers = getCheckers(board, kingSq);
  if (checkers) {
  	board.isCheck = true;
    if (bitCount(checkers) > 1) {
    	board.num_moves = bitCount(moves);
      return std::make_pair(moves, checkers);
    }
  } else {
  	board.isCheck = false;
  	uint64_t castling_moves = castlingMoves(board, kingSq, attackedSquares);
  	moves |= castling_moves;
  }
	board.num_moves = bitCount(moves);
  return std::make_pair(moves, checkers);
}

//returns king_moves, pinned, pinning, checkers, kingSquare
std::tuple<uint64_t, uint64_t, uint64_t, uint64_t, Square> kingMoves(Board& board) {
  Square kingSq = kingSquare(board, board.sideToMove);
	uint64_t attackedSquares = getAttackedSquaresOnly(board);
	auto [moves, checkers] = king_moves(board, kingSq, attackedSquares);
  auto [pinned, pinning] = pinFinder(board, kingSq);
  return std::make_tuple(moves, pinned, pinning, checkers, kingSq);
}

void isCheckMateStaleMate(Board& board) {
	auto [moves, pinned, pinning, checkers, kingSq] = kingMoves(board);
  if (bitCount(checkers) <= 1) {
    auto [check_mask, ep_mask] = checkers ? checkMask(board, kingSq, checkers) : std::make_pair(0xffffffffffffffffULL, 0ULL);
  	//legal other moves
    for (PieceType pt = Pawn; pt <= Queen; ++pt) {
    	uint64_t occupations = board.side[board.sideToMove] & board.pieceTypes[pt - 1];
    	while (occupations) {
    		Square sq = popLSB(occupations);
    	  piece_moves(board, pt, sq, kingSq, pinned, pinning, check_mask, ep_mask);
    	}
    }
    /*occupations = board.side[board.sideToMove] & board.pieceTypes[Rook - 1];
  	while (occupations) {
  		Square sq = popLSB(occupations);
  	  piece_moves(board, Rook, sq, kingSq, pinned, pinning, check_mask, ep_mask);
  	}
  	occupations = board.side[board.sideToMove] & board.pieceTypes[Queen - 1];
  	while (occupations) {
  		Square sq = popLSB(occupations);
  	  piece_moves(board, Queen, sq, kingSq, pinned, pinning, check_mask, ep_mask);
  	}
  	occupations = board.side[board.sideToMove] & board.pieceTypes[Pawn - 1];
  	while (occupations) {
  		Square sq = popLSB(occupations);
  	  piece_moves(board, Pawn, sq, kingSq, pinned, pinning, check_mask, ep_mask);
  	}
  	occupations = board.side[board.sideToMove] & board.pieceTypes[Knight - 1];
  	while (occupations) {
  		Square sq = popLSB(occupations);
  	  piece_moves(board, Knight, sq, kingSq, pinned, pinning, check_mask, ep_mask);
  	}*/
  }
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
	uint64_t srcBit = SQ_BIT(move.src);
	board.side[board.sideToMove] ^= srcBit;
	board.pieceTypes[mpType - 1] ^= srcBit;
	board.piecesOnSquares[move.src] = PieceNone;

	//the above commented branched code is replaced with these 4 simple lines, which also take care of castling:
  const uint64_t castlingRooks = board.castlingRooks; //preserve for castling section
  board.castlingRights &= CastlingRights[move.src];
  board.castlingRights &= CastlingRights[move.dst];
  board.castlingRooks &= CastlingRooks[move.src];
  board.castlingRooks &= CastlingRooks[move.dst];

	move.type = MoveTypeNormal; //init type
	PieceType capturedType = PieceTypeNone; //init capturedType
	//normal capture
	uint64_t dstBit = SQ_BIT(move.dst);
	if (dstBit & board.side[oppColor]) {
		move.type = MoveTypeCapture;
		capturedType = PC_TYPE(board.piecesOnSquares[move.dst]);
		//debug
		/*if (capturedType == King) {
			char fen[MAX_FEN_STRING_LEN];
			board2fen(board, fen);
			printf("ff_move() error: captured king! fen %s, %s move %s%s\n", fen, piece[movingPiece], square[move.src], square[move.dst]);
			writeDebug(board);
			exit(1);
		}*/
		board.side[oppColor] ^= dstBit;
		board.pieceTypes[capturedType - 1] ^= dstBit;
	}

  //en passant capture
	board.enPassant = FileNone;
	const int direction = (board.sideToMove == ColorWhite) ? 8 : -8;
	// If it's a diagonal move to an empty square, it MUST be En Passant.
	if (mpType == Pawn && SQ_FILE(move.src) != SQ_FILE(move.dst) && capturedType == PieceTypeNone) {
    Square capturedPawnSquare = static_cast<Square>(move.dst - direction);
		const uint64_t bitSq = SQ_BIT(capturedPawnSquare);
		board.side[oppColor] ^= bitSq;
		board.pieceTypes[Pawn - 1] ^= bitSq;
		board.piecesOnSquares[capturedPawnSquare] = PieceNone;
	  move.type = MoveTypeEnPassantCapture;
	  capturedType = Pawn;
  }
  //en passant move
	const int double_push = (board.sideToMove == ColorWhite) ? 16 : -16;
	if (mpType == Pawn && move.dst == static_cast<Square>(move.src + double_push)) { //set en passant unconditionally
		move.type = MoveTypeEnPassant;
		board.enPassant = SQ_FILE(move.src);
  }
  
	//castling
	if (mpType == King) {
		//special case of chess 960 castling - in uci notation the king captures its own rook but moves to its standard dst
    if (board.isChess960 && (castlingRooks & SQ_BIT(move.dst))) {
      if (move.dst > move.src) move.type = MoveTypeCastlingKingside;
      else move.type = MoveTypeCastlingQueenside;
    }
		else if (move.dst - move.src == 2) move.type = MoveTypeCastlingKingside;
		else if (move.src - move.dst == 2) move.type = MoveTypeCastlingQueenside;
		Square srcRookSquare = SquareNone;
		if (move.type == MoveTypeCastlingKingside) {
			srcRookSquare = msBit(castlingRooks & base_rank_bb[board.sideToMove]);		
		} else if (move.type == MoveTypeCastlingQueenside) {
			srcRookSquare = lsBit(castlingRooks & base_rank_bb[board.sideToMove]);
		}
		if (srcRookSquare != SquareNone) { 
			//remove castling rook from its source square taking care of
			board.piecesOnSquares[srcRookSquare] = PieceNone;
			//and put it to its destination square
			board.piecesOnSquares[castlingRookSquare[board.sideToMove][move.type - 1]] = castlingRook[board.sideToMove];
			//update occupations
			//xor out the rook on its source square
			uint64_t bitSq = SQ_BIT(srcRookSquare);
			board.side[board.sideToMove] ^= bitSq;
			board.pieceTypes[Rook - 1] ^= bitSq;
			//add the rook to its destination square
			bitSq = SQ_BIT(castlingRookSquare[board.sideToMove][move.type - 1]);
			board.side[board.sideToMove] |= bitSq;
			board.pieceTypes[Rook - 1] |= bitSq;
			//chess960 king destination update
	  	if (board.isChess960) move.dst = castlingKingSquare[board.sideToMove][move.type - 1];
		}		
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
		//replaced "|=" with "^|", which is tiny faster
		board.side[board.sideToMove] ^= bitSq; 
		board.pieceTypes[mpType - 1] ^= bitSq; 
	}
				
  //branchless halfmoveClock update
	bool resetClock = (mpType == Pawn) | (capturedType != PieceTypeNone);
  board.halfmoveClock = (board.halfmoveClock + 1) & -(!resetClock);
  	
	//increment move number if it was black's move
	if (board.sideToMove == ColorBlack) ++board.moveNumber;
	
	//toggle SideToMove
	board.sideToMove = board.sideToMove == ColorWhite ? ColorBlack : ColorWhite;

	return capturedType;
}

//slightly heavier version of ff_move with saving the state in StateInfo, so that undo_move() can work
//returns captured type
PieceType do_move(Board& board, Move& move, StateInfo& state) {
  const Color oppColor = OPP_COLOR(board.sideToMove);
  
  //copy piece in src square before removing it
  const Piece movingPiece = board.piecesOnSquares[move.src];
	const PieceType mpType = PC_TYPE(movingPiece);
	//remove the piece from its source square
	uint64_t bitSq = SQ_BIT(move.src);
	board.side[board.sideToMove] ^= bitSq;
	board.pieceTypes[mpType - 1] ^= bitSq;
	board.piecesOnSquares[move.src] = PieceNone;

  //preserve num_moves
  state.num_moves = board.num_moves;

  //preserve castling
  state.castlingRooks = board.castlingRooks;
  state.castlingRights = board.castlingRights;
  
	board.castlingRights &= CastlingRights[move.src];
  board.castlingRights &= CastlingRights[move.dst];
  board.castlingRooks &= CastlingRooks[move.src];
  board.castlingRooks &= CastlingRooks[move.dst];

	move.type = MoveTypeNormal; //initialize/reset moving type
	state.capturedType = PieceTypeNone; //initialize/reset the captured type
	//normal capture
	uint64_t dstBit = SQ_BIT(move.dst);
	if (dstBit & board.side[oppColor]) { //or if (oppColor == PC_COLOR(board.piecesOnSquares[move.dst])
		move.type = MoveTypeCapture;
		//preserved captured type for undo_move()
		state.capturedType = PC_TYPE(board.piecesOnSquares[move.dst]);
		//for debugging - remove later
		/*if (state.capturedType == King) {
			char fen[MAX_FEN_STRING_LEN];
			board2fen(board, fen);
			printf("do_move() error: captured king! fen %s, %s move %s%s\n", fen, piece[movingPiece], square[move.src], square[move.dst]);
			writeDebug(board);
			exit(1);
		}*/
		//remove the captured puece from occupations
		board.side[oppColor] ^= dstBit;
		board.pieceTypes[state.capturedType - 1] ^= dstBit;
	}
	
  //preserve en passant file for undo_move()
  state.enPassant = board.enPassant;
  //en passant capture
	board.enPassant = FileNone; //init/reset board.enPassant
	const int direction = (board.sideToMove == ColorWhite) ? 8 : -8;
	// If it's a diagonal move to an empty square, it MUST be En Passant.
	if (mpType == Pawn && SQ_FILE(move.src) != SQ_FILE(move.dst) && state.capturedType == PieceTypeNone) {
    Square capturedPawnSquare = static_cast<Square>(move.dst - direction);
		bitSq = SQ_BIT(capturedPawnSquare);
		board.side[oppColor] ^= bitSq;
		board.pieceTypes[Pawn - 1] ^= bitSq;
		board.piecesOnSquares[capturedPawnSquare] = PieceNone;
	  move.type = MoveTypeEnPassantCapture;
	  state.capturedType = Pawn;
  }
  //en passant move - set en passant unconditionally
	const int double_push = (board.sideToMove == ColorWhite) ? 16 : -16;
	if (mpType == Pawn && move.dst == static_cast<Square>(move.src + double_push)) {
		move.type = MoveTypeEnPassant;
		board.enPassant = SQ_FILE(move.src);
  }  
	//castling
	if (mpType == King) {
    if (board.isChess960 && (state.castlingRooks & SQ_BIT(move.dst))) {
      if (move.dst > move.src) move.type = MoveTypeCastlingKingside;
      else move.type = MoveTypeCastlingQueenside;
    } else if (move.dst - move.src == 2) move.type = MoveTypeCastlingKingside;
		else if (move.src - move.dst == 2) move.type = MoveTypeCastlingQueenside;
		Square srcRookSquare = SquareNone;
		if (move.type == MoveTypeCastlingKingside) {
			srcRookSquare = msBit(state.castlingRooks & base_rank_bb[board.sideToMove]);		
		} else if (move.type == MoveTypeCastlingQueenside) {
			srcRookSquare = lsBit(state.castlingRooks & base_rank_bb[board.sideToMove]);
		}
		if (srcRookSquare != SquareNone) { 
			//remove castling rook from its source square taking care of
			board.piecesOnSquares[srcRookSquare] = PieceNone;
			//and put it to its destination square
			board.piecesOnSquares[castlingRookSquare[board.sideToMove][move.type - 1]] = castlingRook[board.sideToMove];
			//update occupations
			//xor out the rook on its source square
			uint64_t bitSq = SQ_BIT(srcRookSquare);
			board.side[board.sideToMove] ^= bitSq;
			board.pieceTypes[Rook - 1] ^= bitSq;
			//xor in the rook on its destination square
			bitSq = SQ_BIT(castlingRookSquare[board.sideToMove][move.type - 1]);
			board.side[board.sideToMove] ^= bitSq;
			board.pieceTypes[Rook - 1] ^= bitSq;
			//chess960 king destination update
	  	if (board.isChess960) move.dst = castlingKingSquare[board.sideToMove][move.type - 1];
		}
	}
	
	//promotion
	if (move.promoType != PieceTypeNone) {
		board.piecesOnSquares[move.dst] = PC(board.sideToMove, move.promoType);
		//replaced "|=" with "^|", which is tiny faster
		board.side[board.sideToMove] ^= dstBit;
		board.pieceTypes[move.promoType - 1] ^= dstBit;
	} 
	//other move
	else {
		//move the piece to its destination
		board.piecesOnSquares[move.dst] = movingPiece;
		//cannot use dstBit because of chess960 dst square update for king
		bitSq = SQ_BIT(move.dst);
		//replaced "|=" with "^|", which is tiny faster
		board.side[board.sideToMove] ^= bitSq; 
		board.pieceTypes[mpType - 1] ^= bitSq; 
	}
	
	//preserve halfmove clock for undo_move()
	state.halfmoveClock = board.halfmoveClock;
	//preserve check info
	state.isCheck = board.isCheck;
	state.isMate = board.isMate;
	state.isStaleMate = board.isStaleMate;

	//branchless halfmoveClock update
	bool resetClock = (mpType == Pawn) | (state.capturedType != PieceTypeNone);
  board.halfmoveClock = (board.halfmoveClock + 1) & -(!resetClock);

	//increment move number if it was black's move
	if (board.sideToMove == ColorBlack) ++board.moveNumber;
	
	//toggle SideToMove
	board.sideToMove = board.sideToMove == ColorWhite ? ColorBlack : ColorWhite;

	return state.capturedType;
}

namespace Stockfish {
template<bool PutPiece, bool ComputeRay>
void update_piece_threats(Board& board, Piece pc, Square s, DirtyThreats * const dts, [[maybe_unused]] Bitboard noRaysContaining) {
    const Bitboard occupied     = board.side[ColorWhite] | board.side[ColorBlack];
    const Bitboard rookQueens   = board.pieceTypes[Rook - 1] | board.pieceTypes[Queen - 1];
    const Bitboard bishopQueens = board.pieceTypes[Bishop - 1] | board.pieceTypes[Queen - 1];
    const Bitboard knights      = board.pieceTypes[Knight - 1];
    const Bitboard kings        = board.pieceTypes[King - 1];
    const Bitboard whitePawns   = board.side[ColorWhite] & board.pieceTypes[Pawn - 1];
    const Bitboard blackPawns   = board.side[ColorBlack] & board.pieceTypes[Pawn - 1];

    const Bitboard rAttacks = Stockfish::attacks_bb<Stockfish::ROOK>(static_cast<Stockfish::Square>(s), occupied); //attacks_bb<ROOK>(s, occupied);
    const Bitboard bAttacks = Stockfish::attacks_bb<Stockfish::BISHOP>(static_cast<Stockfish::Square>(s), occupied); //attacks_bb<BISHOP>(s, occupied);

    Bitboard threatened = attacks_bb(pc, s, occupied) & occupied;
    Bitboard sliders    = (rookQueens & rAttacks) | (bishopQueens & bAttacks);
    //Bitboard incoming_threats = (PseudoAttacks[KNIGHT][s] & knights) | (attacks_bb<PAWN>(s, WHITE) & blackPawns) | (attacks_bb<PAWN>(s, BLACK) & whitePawns) | (PseudoAttacks[KING][s] & kings);
    Bitboard incoming_threats = (Stockfish::PseudoAttacks[Knight][s] & knights) | (Stockfish::PseudoAttacks[ColorWhite][s] & blackPawns) | (Stockfish::PseudoAttacks[ColorBlack][s] & whitePawns) | (Stockfish::PseudoAttacks[King][s] & kings);

    while (threatened)
    {
        Square threatenedSq = pop_lsb(threatened);
        Piece threatenedPc = static_cast<Stockfish::Piece>(board.piecesOnSquares[threatenedSq]);

        assert(threatenedSq != s);
        assert(threatenedPc);

        add_dirty_threat<PutPiece>(dts, pc, threatenedPc, s, threatenedSq);
    }

    if constexpr (ComputeRay) {
        while (sliders) {
            Square sliderSq = pop_lsb(sliders);
            Piece slider = static_cast<Stockfish::Piece>(board.piecesOnSquares[sliderSq]);
            const Bitboard ray = Stockfish::RayPassBB[sliderSq][s] & ~Stockfish::BetweenBB[sliderSq][s];
            //const Bitboard rayFrom = RayFrom[sliderSq][s];
            //const Bitboard lineBetween = LineBetween[sliderSq][s];
            //const Bitboard ray = RayFrom[sliderSq][s] & ~LineBetween[sliderSq][s];
            //const Bitboard ray = rayFrom & ~lineBetween;
            const Bitboard discovered = ray & (rAttacks | bAttacks) & occupied;
            assert(bitCount(discovered) <= 1);
            if (discovered && (Stockfish::RayPassBB[sliderSq][s] & noRaysContaining) != noRaysContaining) {
                const Square threatenedSq = lsb(discovered);
                const Piece  threatenedPc = static_cast<Stockfish::Piece>(board.piecesOnSquares[threatenedSq]);
                add_dirty_threat<!PutPiece>(dts, slider, threatenedPc, sliderSq, threatenedSq);
            }
            add_dirty_threat<PutPiece>(dts, slider, pc, sliderSq, s);
        }
    }
    else incoming_threats |= sliders;

    while (incoming_threats) {
        Square srcSq = pop_lsb(incoming_threats);
        Piece srcPc = static_cast<Stockfish::Piece>(board.piecesOnSquares[srcSq]);
        assert(srcSq != s);
        assert(srcPc != PieceNone);
        add_dirty_threat<PutPiece>(dts, srcPc, pc, srcSq, s);
    }
}
}

//even heavier version of do_move with updating DirtyPiece and DirtyThreats for NNUE incremental updates
//returns captured type
PieceType do_move_dp(Board& board, Move& move, StateInfo& state, Stockfish::DirtyPiece& dp, Stockfish::DirtyThreats& dts) {
  const Color oppColor = OPP_COLOR(board.sideToMove);
	const PieceType mpType = PC_TYPE(board.piecesOnSquares[move.src]);
	assert(mpType != PieceTypeNone);
	move.type = MoveTypeNormal; //initialize/reset moving type
	state.capturedType = PieceTypeNone; //initialize/reset the captured type

  //copy piece in src square before removing it
  dp.pc = static_cast<Stockfish::Piece>(board.piecesOnSquares[move.src]);
  dp.from = static_cast<Stockfish::Square>(move.src);
  dp.to = static_cast<Stockfish::Square>(move.dst);
  dp.add_sq = static_cast<Stockfish::Square>(SquareNone);
  dp.remove_sq = static_cast<Stockfish::Square>(SquareNone);
  dts.us = static_cast<Stockfish::Color>(board.sideToMove);
  dts.prevKsq = static_cast<Stockfish::Square>(kingSquare(board, board.sideToMove));
  dts.threatenedSqs = dts.threateningSqs = 0;

  //preserve num_moves
  state.num_moves = board.num_moves;
  //preserve castling
  state.castlingRooks = board.castlingRooks;
  state.castlingRights = board.castlingRights;
  //printf("do_move_dp() debug: state.castlingRooks %llx, state.castlingRights %x, move.dst %s\n", state.castlingRooks, state.castlingRights, square[move.dst]);
	Square srcRookSquare = SquareNone;
	if (mpType == King) {
    if (board.isChess960 && (state.castlingRooks & SQ_BIT(move.dst))) {
      if (move.dst > move.src) {
        move.type = MoveTypeCastlingKingside;
  			srcRookSquare = msBit(state.castlingRooks & base_rank_bb[board.sideToMove]);
      } else {
        move.type = MoveTypeCastlingQueenside;
  			srcRookSquare = lsBit(state.castlingRooks & base_rank_bb[board.sideToMove]);
      }
		} else if (move.dst - move.src == 2) {
		  move.type = MoveTypeCastlingKingside;
			srcRookSquare = msBit(state.castlingRooks & base_rank_bb[board.sideToMove]);		
		} else if (move.src - move.dst == 2) {
		  move.type = MoveTypeCastlingQueenside;
			srcRookSquare = lsBit(state.castlingRooks & base_rank_bb[board.sideToMove]);
		}
	}

  //preserve en passant file for undo_move()
  state.enPassant = board.enPassant;
  //en passant capture
	board.enPassant = FileNone; //init/reset board.enPassant
	const int direction = (board.sideToMove == ColorWhite) ? 8 : -8;
	// If it's a diagonal move to an empty square, it MUST be En Passant.
	if (mpType == Pawn && SQ_FILE(move.src) != SQ_FILE(move.dst) && board.piecesOnSquares[move.dst] == PieceNone) {
    const Square capturedPawnSquare = static_cast<Square>(move.dst - direction);
	  //update_piece_threats<false>(board, static_cast<Stockfish::Piece>(PC(oppColor, Pawn)), static_cast<Stockfish::Square>(capturedPawnSquare), &dts);
		//uint64_t bitSq = SQ_BIT(capturedPawnSquare);
		//board.side[oppColor] ^= bitSq;
		//board.pieceTypes[Pawn - 1] ^= bitSq;
		//board.piecesOnSquares[capturedPawnSquare] = PieceNone;
	  move.type = MoveTypeEnPassantCapture;
	  state.capturedType = Pawn;
	  //the captured pawn is NOT on move.dst, so the normal-capture branch below cannot
	  //report it. Without these two lines HalfKA keeps a phantom pawn after every ep
	  //capture (the threats side is fine, which is what made this subtle). HEAD set both.
	  dp.remove_sq = static_cast<Stockfish::Square>(capturedPawnSquare);
	  dp.remove_pc = static_cast<Stockfish::Piece>(PC(oppColor, Pawn));
	  remove_piece(board, static_cast<Stockfish::Square>(capturedPawnSquare), &dts);
  }
  //if (move.type >= MoveTypeNormal) update_piece_threats<false>(board, static_cast<Stockfish::Piece>(dp.pc), static_cast<Stockfish::Square>(move.src), &dts);
	  
	//printf("do_move_dp() debug: castlingRights %hhx\n", board.castlingRights);
	board.castlingRights &= CastlingRights[move.src];
	//printf("do_move_dp() debug: %s move from %s, castlingRights %hhx : %hhx\n", pieceType[mpType], square[move.src], board.castlingRights, CastlingRights[move.src]);
  board.castlingRights &= CastlingRights[move.dst];
	//printf("do_move_dp() debug: %s move to %s, castlingRights %hhx : %hhx\n", pieceType[mpType], square[move.dst], board.castlingRights, CastlingRights[move.dst]);
  board.castlingRooks &= CastlingRooks[move.src];
  board.castlingRooks &= CastlingRooks[move.dst];
  
	//normal capture
	uint64_t dstBit = SQ_BIT(move.dst);
	if (dstBit & board.side[oppColor]) {
	  const Piece captured = board.piecesOnSquares[move.dst];
	  //this should be called before the piece is removed from its src!
	  //update_piece_threats<false>(board, dp.pc, move.src, &dts); 
	  dp.remove_sq = static_cast<Stockfish::Square>(move.dst);
	  dp.remove_pc = static_cast<Stockfish::Piece>(captured);
		move.type = MoveTypeCapture;
		//preserved captured type for undo_move()
		state.capturedType = PC_TYPE(captured);
		remove_piece(board, dp.from, &dts);
		swap_piece(board, dp.to, dp.pc, &dts);
	  //remove the piece from its source square
  	//uint64_t bitSq = SQ_BIT(move.src);
  	//board.side[board.sideToMove] ^= bitSq;
  	//board.pieceTypes[mpType - 1] ^= bitSq;
  	//board.piecesOnSquares[move.src] = PieceNone;
		//update_piece_threats<false>(board, dp.pc, move.dst, &dts);
		//for debugging - remove later
		/*if (state.capturedType == King) {
			char fen[MAX_FEN_STRING_LEN];
			board2fen(board, fen);
			printf("do_move_dp() error: captured king! fen %s, %s move %s%s\n", fen, pieceType[mpType], square[move.src], square[move.dst]);
			writeDebug(board);
			exit(1);
		}*/
		//remove the captured puece from occupations
		//board.side[oppColor] ^= dstBit;
		//board.pieceTypes[state.capturedType - 1] ^= dstBit;
		//update_piece_threats<false, false>(board, static_cast<Stockfish::Piece>(captured), static_cast<Stockfish::Square>(move.dst), &dts);
		//add piece that captured one above to its dst
		//board.piecesOnSquares[move.dst] = static_cast<Piece>(dp.pc);
		//board.side[board.sideToMove] ^= dstBit; 
		//board.pieceTypes[mpType - 1] ^= dstBit; 
    //update_piece_threats<true, false>(board, dc.pc, move.dst, &dts);
	} else {
	  //remove the piece from its source square
  	//uint64_t bitSq = SQ_BIT(move.src);
  	//board.side[board.sideToMove] ^= bitSq;
  	//board.pieceTypes[mpType - 1] ^= bitSq;
  	//board.piecesOnSquares[move.src] = PieceNone;
	}
	
  //en passant move - set en passant unconditionally
	const int double_push = (board.sideToMove == ColorWhite) ? 16 : -16;
	if (mpType == Pawn && move.dst == move.src + double_push) {
		move.type = MoveTypeEnPassant;
		board.enPassant = SQ_FILE(move.src);
  }  
	//castling
	if (srcRookSquare != SquareNone) {
	  move.dst = castlingKingSquare[board.sideToMove][move.type - 1];
  	dp.to = static_cast<Stockfish::Square>(move.dst);
    dp.remove_pc = dp.add_pc = static_cast<Stockfish::Piece>(castlingRook[board.sideToMove]);
    dp.remove_sq = static_cast<Stockfish::Square>(srcRookSquare);
    dp.add_sq = static_cast<Stockfish::Square>(castlingRookSquare[board.sideToMove][move.type - 1]);
	  //update_piece_threats<false>(board, dp.pc, static_cast<Stockfish::Square>(move.src), &dts);
	  remove_piece(board, dp.from, &dts);
	  //writeDebug(board);
	  remove_piece(board, dp.remove_sq, &dts);
	  //writeDebug(board);
    //update_piece_threats<true>(board, dp.pc, static_cast<Stockfish::Square>(move.dst), &dts);
    //we need to remove rook before running next line
		//remove castling rook from its source square taking care of
		//board.piecesOnSquares[srcRookSquare] = PieceNone;
		//xor out the rook on its source square
		//uint64_t bitSq = SQ_BIT(srcRookSquare);
		//board.side[board.sideToMove] ^= bitSq;
		//board.pieceTypes[Rook - 1] ^= bitSq;
	  //update_piece_threats<false>(board, static_cast<Stockfish::Piece>(castlingRook[board.sideToMove]), static_cast<Stockfish::Square>(srcRookSquare), &dts);
		
		//and put it to its destination square
		//board.piecesOnSquares[castlingRookSquare[board.sideToMove][move.type - 1]] = castlingRook[board.sideToMove];
		//update occupations
		//xor in the rook on its destination square
		//bitSq = SQ_BIT(castlingRookSquare[board.sideToMove][move.type - 1]);
		//board.side[board.sideToMove] ^= bitSq;
		//board.pieceTypes[Rook - 1] ^= bitSq;
    //update_piece_threats<true>(board, dp.add_pc, dp.add_sq, &dts);
    put_piece(board, dp.pc, dp.to, &dts);
	  //writeDebug(board);
    put_piece(board, dp.add_pc, dp.add_sq, &dts);
	  //writeDebug(board);
	}
	//other than castling move and normal captures but including en passant captures
	else if (move.type != MoveTypeCapture) {
		//move the piece to its destination
		//board.piecesOnSquares[move.dst] = static_cast<Piece>(dp.pc);
		//cannot use dstBit because of chess960 dst square update for king
		//bitSq = SQ_BIT(move.dst);
		//board.side[board.sideToMove] ^= bitSq; 
		//board.pieceTypes[mpType - 1] ^= bitSq; 
		//if (move.type != MoveTypeEnPassantCapture) update_piece_threats<true, false>(board, dp.pc, static_cast<Stockfish::Square>(move.dst), &dts);
		//else update_piece_threats<true>(board, static_cast<Stockfish::Piece>(PC(static_cast<Color>(board.sideToMove), Pawn)), static_cast<Stockfish::Square>(move.dst), &dts, SQ_BIT(move.src) | SQ_BIT(move.dst));
		move_piece(board, dp.from, dp.to, &dts);
	}
	
	//promotion
	if (move.promoType != PieceTypeNone) {
	  const Piece promo = PC(board.sideToMove, move.promoType);
		//board.piecesOnSquares[move.dst] = promo;
		//board.side[board.sideToMove] ^= dstBit;
		//board.pieceTypes[move.promoType - 1] ^= dstBit;
		//update_piece_threats<true, false>(board, static_cast<Stockfish::Piece>(promo), static_cast<Stockfish::Square>(move.dst), &dts);
		dp.add_pc = static_cast<Stockfish::Piece>(promo);
		swap_piece(board, dp.to, dp.add_pc, &dts);
		dp.add_sq = dp.to;
		dp.to = static_cast<Stockfish::Square>(SquareNone);
	} 
	
	//preserve halfmove clock for undo_move()
	state.halfmoveClock = board.halfmoveClock;
	//preserve check info
	state.isCheck = board.isCheck;
	state.isMate = board.isMate;
	state.isStaleMate = board.isStaleMate;
  
  //branchless halfmoveClock update
	bool resetClock = (mpType == Pawn) | (state.capturedType != PieceTypeNone);
  board.halfmoveClock = (board.halfmoveClock + 1) & -(!resetClock);
	
	//increment move number if it was black's move
	if (board.sideToMove == ColorBlack) ++board.moveNumber;
	
	//toggle SideToMove
	board.sideToMove = board.sideToMove == ColorWhite ? ColorBlack : ColorWhite;
  //debugging - remove later
	/*if (!(board.side[board.sideToMove] & board.pieceTypes[King - 1]) || !(board.side[OPP_COLOR(board.sideToMove)] & board.pieceTypes[King - 1])) {
		char fen[MAX_FEN_STRING_LEN];
		board2fen(board, fen);
		printf("do_move_dp() error: no king after %s %s move %s%s! fen %s, chess960 %s\n", pieceType[mpType], moveType[move.type], square[move.src], square[move.dst], fen, board.isChess960 ? "true" : "false");
		writeDebug(board);
		reconcile(board);
		exit(1);
	}*/
  //board.sideToMove has already been toggled above, so reading it here would give the
  //OPPONENT's king. FullThreats::requires_refresh compares ksq against prevKsq (which
  //was captured for the mover), so both must be the same colour - key off dts.us.
  dts.ksq = static_cast<Stockfish::Square>(kingSquare(board, static_cast<Color>(dts.us)));
	return state.capturedType;
}

//reverse do_move()
void undo_move(Board& board, const Move& move, const StateInfo& state) {
	//toggle SideToMove
	board.sideToMove = board.sideToMove == ColorWhite ? ColorBlack : ColorWhite;

  const Color oppColor = OPP_COLOR(board.sideToMove);
	PieceType mpType = PC_TYPE(board.piecesOnSquares[move.dst]);
	const uint64_t dstBit = SQ_BIT(move.dst);
	
	//restore num_moves
	board.num_moves = state.num_moves;
	
	//restore castling
  board.castlingRooks = state.castlingRooks;
  board.castlingRights = state.castlingRights;
  
  //restore en passant file
  board.enPassant = state.enPassant;

	//restore halfmove clock
	board.halfmoveClock = state.halfmoveClock;
	
	//restore check info
	board.isCheck = state.isCheck;
	board.isMate = state.isMate;
	board.isStaleMate = state.isStaleMate;

	//decrement move number if it was black's move
	if (board.sideToMove == ColorBlack) --board.moveNumber;

	//remove the piece from its destination
	if (move.type != MoveTypeCapture) board.piecesOnSquares[move.dst] = PieceNone;

	//remove the piece in dst square from occupations and update mpType to Pawn in case of promotion
	board.side[board.sideToMove] ^= dstBit;
	if (move.promoType != PieceTypeNone) {
		mpType = Pawn;
		board.pieceTypes[move.promoType - 1] ^= dstBit;
	} else board.pieceTypes[mpType - 1] ^= dstBit;
	Square srcRookSquare = SquareNone;
	//normal capture
	if (move.type == MoveTypeCapture) {
		//restore captured piece
		board.piecesOnSquares[move.dst] = PC(oppColor, state.capturedType);
	  board.side[oppColor] ^= dstBit;
	  board.pieceTypes[state.capturedType - 1] ^= dstBit;
	}
  //en passant capture
	else if (move.type == MoveTypeEnPassantCapture) {
	  	//restore en passant captured pawn
			const int capturedPawnSquare = board.sideToMove == ColorWhite ? move.dst - 8 : move.dst + 8;
			board.piecesOnSquares[capturedPawnSquare] = PC(oppColor, Pawn);
			uint64_t bitSq = SQ_BIT(capturedPawnSquare);
			board.side[oppColor] ^= bitSq;
			board.pieceTypes[Pawn - 1] ^= bitSq;
	}  
	//castling
	else if (move.type == MoveTypeCastlingKingside) {
		srcRookSquare = msBit(board.castlingRooks & base_rank_bb[board.sideToMove]);
	} else if (move.type == MoveTypeCastlingQueenside) {
		srcRookSquare = lsBit(board.castlingRooks & base_rank_bb[board.sideToMove]);
	}
	if (srcRookSquare != SquareNone) {
		//restore castling rook in its source square
		board.piecesOnSquares[srcRookSquare] = castlingRook[board.sideToMove];
		uint64_t bitSq = SQ_BIT(srcRookSquare);
		board.side[board.sideToMove] ^= bitSq;
		board.pieceTypes[Rook - 1] ^= bitSq;
		//and remove it from its destination square
		board.piecesOnSquares[castlingRookSquare[board.sideToMove][move.type - 1]] = PieceNone;
		bitSq = SQ_BIT(castlingRookSquare[board.sideToMove][move.type - 1]);
		board.side[board.sideToMove] ^= bitSq;
		board.pieceTypes[Rook - 1] ^= bitSq;
	}
	
	//restore the piece in its source square
	board.piecesOnSquares[move.src] = PC(board.sideToMove, mpType);
	uint64_t bitSq = SQ_BIT(move.src);
	board.side[board.sideToMove] ^= bitSq;
	board.pieceTypes[mpType - 1] ^= bitSq;		
}
