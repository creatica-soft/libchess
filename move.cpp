#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <tuple>
#include "nnue/bitboard.h"
#include "libchess.h"

//#ifdef __cplusplus
//extern "C" {
//#endif

//THE UCI MOVE HELPERS THAT BELONG BESIDE uci2move_idx(), and until now lived twice over in the two
//engines. Both were private statics of creatica.cpp's class with a copy pasted into
//creatica_ab.cpp, and the copies drifted exactly as the note there predicted: creatica_ab gained
//eight diagnostic reasons while creatica kept a bare bool, and two explanatory comments were lost
//in the paste -- one of which made a correct line ("double check: king only") read as a bug, and
//another of which overstated which lichess games speak the king-takes-rook dialect.
//
//uci2move_idx() does no legality check whatsoever, which is what makes both of these necessary.

//THE KING-TAKES-ROOK CASTLING DIALECT. lichess sends castling as the king's move to the ROOK's
//square in games created FROM A POSITION -- standard rules, isChess960 false -- where libchess
//wants the king's two-square move. In a real Chess960 game the king-takes-rook form is what
//libchess itself wants (castlingMoves() generates it and do_move() recognises castling by exactly
//the castlingRooks test below), so rewriting it there would turn a castling move into a plain king
//move that is not even legal. Hence the isChess960 guard.
//
//Why it matters: uci2move_idx() took "e8a8" literally, the king was marched onto its own rook, and
//the Board left behind was illegal -- the next search read a king bitboard that made no sense and
//died in kingMoves(). That is what an "engine produced nothing for 60000 ms" stall looked like.
//
//PC_TYPE/PC_COLOR, never "& 7" or ">> 3": libchess.h defines a global operator&(T enum, int) that
//IGNORES its second operand and always masks with 1, so "mover & 7" silently evaluates to 0 for
//every piece. chess_types.h documents that operator as broken; the first version of this function
//stepped on it and never fired.
void normalise_castling(const Board& board, Move& move) {
    if (move.promoType != PieceTypeNone) return;
    if (board.isChess960) return;
    const Piece mover = static_cast<Piece>(board.piecesOnSquares[move.src]);
    const Piece onDst = static_cast<Piece>(board.piecesOnSquares[move.dst]);
    if (PC_TYPE(mover) != King || PC_TYPE(onDst) != Rook) return;   //not king-takes-rook
    if (PC_COLOR(mover) != PC_COLOR(onDst)) return;                 //enemy rook: a real capture
    if (!(board.castlingRooks & (1ULL << move.dst))) return;        //rook has no castling right
    const Square from = static_cast<Square>(move.src), to = static_cast<Square>(move.dst);
    const File f = SQ_FILE(to) > SQ_FILE(from) ? FileG : FileC;
    move.dst = SQ(SQ_RANK(from), f);
}

//IS THIS MOVE LEGAL ON THIS BOARD, and if not, WHY. Returns nullptr when legal.
//
//Returning the reason rather than a bool is the whole point. When this fires on a move from a
//driver that only sends legal moves, our board and theirs have DIVERGED, and the failure mode is
//the worst kind: the Board is corrupted, and the crash arrives in kingMoves() during a later
//search, far from the cause. A bare `false` threw away everything needed to find it.
const char * illegal_reason(Board& board, const Move& m) {
    if (m.src >= Square_NB || m.dst >= Square_NB) return "square index out of range";
    if (!((board.side[board.sideToMove] >> m.src) & 1ULL))
        return "no piece of the side to move on the from-square";
    auto [king_moves, pinned, pinning, checkers, kingSq] = kingMoves(board);
    if (m.src == kingSq)
        return ((king_moves >> m.dst) & 1ULL) ? nullptr
             : (checkers ? "king move does not escape check"
                         : "not a legal king move (occupied, or into check)");
    //DOUBLE CHECK: KING MOVES ONLY. Reached only for a non-king move, because every king move was
    //answered above. The position is legal; this move is not. Capturing one checker leaves the
    //other, blocking one leaves the other, and no single move does both -- if the capture square
    //were also on the second checker's line to the king then the captured piece was blocking that
    //line and the second piece was not checking at all. generate() stops in the same place.
    if (bitCount(checkers) > 1) return "double check: only a king move can be legal";
    auto [check_mask, ep_mask] = checkers ? checkMask(board, kingSq, checkers)
                                          : std::make_pair(0xffffffffffffffffULL, 0ULL);
    const PieceType pt = PC_TYPE(static_cast<Piece>(board.piecesOnSquares[m.src]));
    if (pt < Pawn || pt > Queen) return "from-square holds no piece type that moves";
    const uint64_t mv = piece_moves(board, pt, static_cast<Square>(m.src), kingSq,
                                    pinned, pinning, check_mask, ep_mask);
    if ((mv >> m.dst) & 1ULL) return nullptr;
    if ((pinned >> m.src) & 1ULL) return "piece is pinned and this move would expose the king";
    if (checkers) return "does not address the check (no capture of the checker, no block)";
    return "destination not reachable by that piece";
}

bool is_legal_move(Board& board, const Move& m) { return illegal_reason(board, m) == nullptr; }

int uci2move_idx(const char * uci_move, Move& move) {
  assert(uci_move);
  assert(strlen(uci_move) >= 4);
  move.src = SQ(static_cast<Rank>(uci_move[1] - '1'), static_cast<File>(uci_move[0] - 'a'));
  move.dst = SQ(static_cast<Rank>(uci_move[3] - '1'), static_cast<File>(uci_move[2] - 'a'));
  move.promoType = PieceTypeNone;
  int idx = (move.src << 6) | move.dst;
  if (strlen(uci_move) == 5) { //this will not include null-terminated char '\0'
  	const char promos[6] = "nbrq";
    const char * p = strchr(promos, uci_move[4]);
    if (p) {
      move.promoType = (PieceType)(p - promos + 2);
      idx |= (move.promoType << 12);
    } 
  }
  return idx;
}

Move& idx2move(const int move_idx, Move& move) {
  move.dst = (Square)(move_idx & 63);
  move.src = (Square)((move_idx >> 6) & 63);
  move.promoType = (PieceType)((move_idx >> 12) & 7);
  move.type = MoveType((move_idx >> 15) & 7);
  return move;
}

//uci_move must hold 6 chars: four for the squares, one for a promotion letter and the terminator.
//(sizeof(uci_move) is the size of the pointer, so it cannot check that.)
char * idx2uci(const int move_idx, char * uci_move) {
  assert(uci_move);
  uci_move[0] = 0;
  const Square dst = (Square)(move_idx & 63);
  const Square src = (Square)((move_idx >> 6) & 63);
  const PieceType promo = (PieceType)((move_idx >> 12) & 7);
  strcat(uci_move, square[src]);
  strcat(uci_move, square[dst]);
  uci_move[4] = uciPromoLetter[promo]; //index 0, 1, 6 and 7 are '\0'
  //TERMINATE AFTER THE PROMOTION LETTER. The letter overwrites the terminator strcat() left at [4], so
  //without this a promotion was followed by whatever the buffer held before. Callers with a local
  //buffer printed that: creatica's visit dump carried moves like "e7e8n" plus stray bytes.
  uci_move[5] = '\0';
  return uci_move;
}

bool promoMove(const Board& board, const Move& move) {
  if (PC_TYPE(board.piecesOnSquares[move.src]) == Pawn) { 
    const Rank pre_promo_rank = board.sideToMove == ColorWhite ? Rank7 : Rank2;
    const Rank promo_rank = board.sideToMove == ColorWhite ? Rank8 : Rank1;
    if (SQ_RANK(move.src) == pre_promo_rank && SQ_RANK(move.dst) == promo_rank) return true;
  }
  return false;
}

uint8_t moveCandidateScore(const Square sq, const File srcFile, const Rank srcRank) {
  const File file = SQ_FILE(sq);
  const Rank rank = SQ_RANK(sq);
	uint8_t score = 1;
	if (file == srcFile && rank == srcRank)
		score = 4;
	else if (rank == srcRank)
		score = 3;
	else if (file == srcFile)
		score = 2;
	return score;
}

int san2move(Board& board, const char * san_move, Move& move) {
  assert(san_move);
  move.src = SquareNone;
  move.dst = SquareNone;
  move.promoType = PieceTypeNone;
  move.type = MoveTypeNormal;
  Piece movingPiece = PieceNone;
	if (strcmp(san_move, "--") == 0) {
		move.type = MoveTypeNull;
		return 0;
	}
	char sanMove[12] = "";
	size_t len = strlen(san_move);
	if (len > 11) {
		printf("san2move() error: len(san_move) is greater than 11 (%zu). %u %s %s, move type %s\n", len, board.moveNumber, board.sideToMove == ColorWhite ? ". " : "... ", san_move, moveType[move.type]);
		writeDebug(board);
		return 1;
	}
	strncpy(sanMove, san_move, len + 1);
	if (sanMove[len - 1] == '+' || sanMove[len - 1] == '#')
		sanMove[len - 1] = '\0';
	if (strcmp(sanMove, "O-O-O") == 0)
		move.type = MoveTypeCastlingQueenside;
	else if (strcmp(sanMove, "O-O") == 0)
		move.type = MoveTypeCastlingKingside;
	const char pieces[] = "NBRQ";
	if (sanMove[0] >= 'a' && sanMove[0] <= 'h') {
		if (sanMove[1] == '1' || sanMove[1] == '8') {
			if (sanMove[2] == '=') {
				const char * idx = strchr(pieces, sanMove[3]);
				if (idx) {
					move.promoType = (PieceType)(idx - pieces + 2);
					sanMove[2] = '\0';
				}
				else {
					printf("san2move() error: unknown promotion piece %u %s %s, move type %s\n", board.moveNumber, board.sideToMove == ColorWhite ? ". " : "... ", sanMove, moveType[move.type]);
					writeDebug(board);
					return 2;
				}
			}
		} else {
			if (sanMove[1] == 'x') {
				if (sanMove[2] >= 'a' && sanMove[2] <= 'h') {
					if (sanMove[3] == '1' || sanMove[3] == '8') {
						if (sanMove[4] == '=') {
							const char * idx = strchr(pieces, sanMove[5]);
							if (idx) {
								move.promoType = (PieceType)(idx - pieces + 2);
								move.type = MoveTypeCapture;
								sanMove[4] = '\0';
							}
							else {
								printf("san2move() error: unknown promotion piece %u %s %s, move type %s\n", board.moveNumber, board.sideToMove == ColorWhite ? ". " : "... ", sanMove, moveType[move.type]);
								writeDebug(board);
								return 3;
							}
						}
					}
				}
		  }	
		}
	}

	auto [king_moves, pinned, pinning, checkers, kingSquare] = kingMoves(board);
  auto [check_mask, ep_mask] = checkers? checkMask(board, kingSquare, checkers) : std::make_pair(0xffffffffffffffffULL, 0ULL);
	//castling
	Square kingCastlingSquare = SquareNone;
	int castlingSide;
	if (move.type == MoveTypeCastlingKingside) {
	  kingCastlingSquare = board.sideToMove == ColorWhite ? SquareG1 : SquareG8;
	  castlingSide = 0;
	}
	else if (move.type == MoveTypeCastlingQueenside) {
	  kingCastlingSquare = board.sideToMove == ColorWhite ? SquareC1 : SquareC8;
	  castlingSide = 1;
	}
	if (kingCastlingSquare != SquareNone) {
		movingPiece = PC(board.sideToMove, King);
		move.src = kingSquare;

		if (!board.isChess960) {
			if (king_moves & SQ_BIT(kingCastlingSquare)) {
				move.dst = kingCastlingSquare;
			} else {
				printf("san2move() error: %s king move from %s to %s is not possible, move type %s\n", board.sideToMove == ColorWhite ? "White" : "Black", square[move.src], square[kingCastlingSquare], moveType[move.type]);
				writeDebug(board);
				return 4;
			}
		} else { //chess960
		  uint64_t rooks = board.castlingRooks & board.side[board.sideToMove];
		  if (!rooks) {
		    printf("san2move() error for chess960: no rooks are available for castling\n");
		    return 5;
		  }
			const Square rookSquare = castlingSide == 0 ? msBit(rooks) : lsBit(rooks);
			if (king_moves & SQ_BIT(rookSquare))
			  move.dst = rookSquare;
			else {
				printf("san2move() error for chess960: %s king move from %s to %s is not possible, move type %s\n", board.sideToMove == ColorWhite ? "White" : "Black", square[move.src], square[rookSquare], moveType[move.type]);
				writeDebug(board);
				return 5;			  
			}
		}
		return 0;
	}

	File dstFile = FileNone;
	File srcFile = FileNone;
	Rank dstRank = RankNone;
	Rank srcRank = RankNone;

	for (int i = (int)strlen(sanMove) - 1; i >= 0; i--) {
		if (sanMove[i] >= '1' && sanMove[i] <= '8') {
			const int x = sanMove[i] - '1';
			if (dstRank == RankNone) dstRank = (Rank)x;
			else srcRank = (Rank)x;
		} else if (sanMove[i] >= 'a' && sanMove[i] <= 'h') {
			const int x = sanMove[i] - 'a';
			if (dstFile == FileNone) dstFile = (File)x;
			else srcFile = (File)x;
		} else {
			const char pieces[] = ".xNBRQK";
			const char * idx = strchr(pieces, sanMove[i]);
			if (idx) {
				const int pieceIndex = idx - pieces;
				if (pieceIndex > 1)
					movingPiece = PC(board.sideToMove, static_cast<PieceType>(pieceIndex));
				else if (pieceIndex == 1) {
					move.type = MoveTypeCapture;
				}
			} else {
				printf("san2move() error: unknown char ('%c') in move %u%s%s\n", sanMove[i], board.moveNumber, board.sideToMove == ColorWhite ? "." : "...", sanMove);
				writeDebug(board);
				return 6;
			}
		}
	}
	move.dst = SQ(dstRank, dstFile);
	if (move.dst >= SquareNone) {
		printf("san2move() error: dstRank or dstFile are not set in move %u%s%s\n", board.moveNumber, board.sideToMove == ColorWhite ? "." : "...", sanMove);
		writeDebug(board);
		return 7;
	}
	if (movingPiece == PieceNone) {
		movingPiece = board.sideToMove == ColorWhite ? WhitePawn : BlackPawn;
		if (move.type != MoveTypeCapture) {
			srcFile = SQ_FILE(move.dst);
			//detect EnPassant move
			const Rank dstRanks[] = { Rank4, Rank5 };
			const Rank srcRanks[] = { Rank2, Rank7 };
			const Square srcSquare = SQ(srcRanks[board.sideToMove], srcFile);
			if (SQ_RANK(move.dst) == dstRanks[board.sideToMove]) {
  			if (board.piecesOnSquares[srcSquare] == PC(board.sideToMove, Pawn)) {
     		  uint64_t moves = piece_moves(board, Pawn, srcSquare, kingSquare, pinned, pinning, check_mask, ep_mask);
  				if ((moves & SQ_BIT(move.dst))) {
  					uint64_t opponentPawns = board.side[OPP_COLOR(board.sideToMove)] & board.pieceTypes[Pawn - 1];
  					while (opponentPawns) {
    					Square opponentPawnSquare = lsBit(opponentPawns);
  						if (SQ_RANK(opponentPawnSquare) == dstRanks[board.sideToMove] && ((SQ_FILE(opponentPawnSquare) == srcFile + 1 && srcFile < FileH) || (SQ_FILE(opponentPawnSquare) == srcFile - 1 && srcFile > FileA))) {
  							move.type = MoveTypeEnPassant;
  							move.src = srcSquare;
  							break;
  						}
  						opponentPawns &= opponentPawns - 1;
  					}
  				}
  			}
			}
		} else if ((board.enPassant < FileNone) && SQ(board.sideToMove == ColorWhite ? Rank6 : Rank3, board.enPassant) == move.dst) {
			move.type = MoveTypeEnPassant; // capture flag should have been set up already from "x" pattern in SAN move
			move.src = board.sideToMove == ColorWhite ? SQ_FILE(move.dst) > srcFile ? (Square)(move.dst - 9) : (Square)(move.dst - 7) : SQ_FILE(move.dst) > srcFile ? (Square)(move.dst + 7) : (Square)(move.dst + 9);
		}
	} 
	if (move.src == SquareNone && srcFile != FileNone && srcRank != RankNone)
		move.src = SQ(srcRank, srcFile);
	else {
   	int n = 0; //number of candidates
  	Square moveCandidates[10];
		//find move candidates
		//get a bitboard of squares where moving piece names (for example, white knights) are
		const PieceType pt = PC_TYPE(movingPiece);
		uint64_t pieces = board.side[board.sideToMove] & board.pieceTypes[pt - 1];
		//iterate over all squares where movingPiece.name are located
		while (pieces) {
  		Square srcSquare = popLSB(pieces);
  		uint64_t pc_moves;
  		if (pt != King)
  		  pc_moves = piece_moves(board, pt, srcSquare, kingSquare, pinned, pinning, check_mask, ep_mask);
  		else pc_moves = king_moves;
			if ((pc_moves & SQ_BIT(move.dst))) {
  			//if moving piece is a pawn (its source file is known and equals to the square SQ_FILE(srcSquare) or a king
				if ((pt == Pawn && SQ_FILE(srcSquare) == srcFile) || pt == King) {
					//we are sure that square srcSquare is the source square
					move.src = srcSquare;
					break;
				} else { //otherwise add this square to moveCandidates squares
					if (n >= 10) {
				    printf("validateSanMove() error: too many move candidates for %s\n", sanMove);
				    writeDebug(board);
				    return 8;
					}				
					moveCandidates[n++] = srcSquare;
        }
			}
		} //end of while() loop over squares
		//if moving piece source square is still unknown (i.e. its not a pawn or a king)
		if (move.src == SquareNone) {
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
			int maxTcandidates[5] = {0}; 
			for (int i = 0; i < n; i++) {
				t = moveCandidateScore(moveCandidates[i], srcFile, srcRank);
				maxTcandidates[t]++;
				if (t > maxT) {
					maxT = t;
					maxI = i;
				}				
			}
			if (maxTcandidates[maxT] == 1) {
  			move.src = moveCandidates[maxI];
  		}
			else {
			  char fen[MAX_FEN_STRING_LEN];
				printf("san2move() error: ambiguous move %s, movingPiece %s, moveType %s, srcRank %c, srcFile %c, max candidate rating %d, max candidates %d, fen %s\n", sanMove, piece[movingPiece], moveType[move.type], enumRanks[srcRank], enumFiles[srcFile], maxT, n, board2fen(board, fen));
				writeDebug(board);
				return 9;
			}
		}
	}
	if (move.src == SquareNone) {
		printf("san2move() error: source square not defined for the moving piece %s, move type %s\n", piece[movingPiece], moveType[move.type]);
		writeDebug(board);
		return 10;
	}
	
	if (bitCount(checkers) > 1 && PC_TYPE(movingPiece) != King) {
	  char fenString[MAX_FEN_STRING_LEN] = "";
		printf("san2move() error: %s move from %s to %s is illegal because king is checked twice. FEN %s\n", piece[movingPiece], square[move.src], square[move.dst], board2fen(board, fenString));
		writeDebug(board);
		//for (int i = 0; i < n; i++) printf("moveCandidate %s\n", square[moveCandidates[i]]);
		return 1;	  
	}

	uint64_t moves;
	if (PC_TYPE(movingPiece) != King) {
	  //printf("san2move() debug: check_mask %llx\n", check_mask);
	  moves = piece_moves(board, PC_TYPE(movingPiece), move.src, kingSquare, pinned, pinning, check_mask, ep_mask);
	}
	else moves = king_moves;
	
	if (!(moves & SQ_BIT(move.dst))) {
	  char fenString[MAX_FEN_STRING_LEN] = "";
		printf("san2move() error: %s move from %s to %s is illegal in %u%s%s; FEN %s\n", piece[movingPiece], square[move.src], square[move.dst], board.moveNumber, board.sideToMove == ColorWhite ? "." : "...", sanMove, board2fen(board, fenString));
		writeDebug(board);
		//for (int i = 0; i < n; i++) printf("moveCandidate %s\n", square[moveCandidates[i]]);
		return 11;
	}
	return 0;
}

//validates move and converts it to SAN move
char * move2san(Board& board, const Move& move, char * sanMove) {
  assert(sanMove);
	if (move.type == MoveTypeCastlingQueenside) {
		strcpy(sanMove, "O-O-O");
		return sanMove;
	} else if (move.type == MoveTypeCastlingKingside) {
		strcpy(sanMove, "O-O");
		return sanMove;
	}
	const Piece movingPiece = board.piecesOnSquares[move.src];
	const PieceType mpType = PC_TYPE(movingPiece);
	
	auto [king_moves, pinned, pinning, checkers, kingSquare] = kingMoves(board);
  
	if (bitCount(checkers) > 1 && mpType != King) {
	  char fenString[MAX_FEN_STRING_LEN] = "";
		printf("move2san() error: %s move from %s to %s is illegal; FEN %s\n", piece[movingPiece], square[move.src], square[move.dst], board2fen(board, fenString));
		writeDebug(board);
		//for (int i = 0; i < n; i++) printf("moveCandidate %s\n", square[moveCandidates[i]]);
		return nullptr;	  
	}
  auto [check_mask, ep_mask] = checkers? checkMask(board, kingSquare, checkers) : std::make_pair(0xffffffffffffffffULL, 0ULL);
	
	int i = 0;
	if (mpType != Pawn)
		sanMove[i++] = pieceLetter[mpType];
	else if (move.type == MoveTypeCapture)
		sanMove[i++] = SQ_FILE(move.src) + 'a';

	if (mpType != Pawn && mpType != King) {
		//find move candidates
		Square moveCandidates[10];
		uint64_t cp = board.side[board.sideToMove] & board.pieceTypes[mpType - 1];
		int idx = 0;
		while (cp) {
  		const Square s = lsBit(cp);
  		uint64_t moves = piece_moves(board, mpType, s, kingSquare, pinned, pinning, check_mask, ep_mask);
			if (moves & SQ_BIT(move.dst)) moveCandidates[idx++] = s;
			cp &= cp - 1;
		}
		for (int j = 0; j < idx; j++) {
			if (move.src == moveCandidates[j]) continue;
			if (SQ_RANK(move.src) == SQ_RANK(moveCandidates[j]))
				sanMove[i++] = SQ_FILE(move.src) + 'a';
			else if (SQ_FILE(move.src) == SQ_FILE(moveCandidates[j]))
				sanMove[i++] = SQ_RANK(move.src) + '1';
			else sanMove[i++] = SQ_FILE(move.src) + 'a';
		}
	}
	if (move.type == MoveTypeCapture) sanMove[i++] = 'x';
	sanMove[i++] = SQ_FILE(move.dst) + 'a';
	sanMove[i++] = SQ_RANK(move.dst) + '1';

	if (move.promoType != PieceTypeNone) {
		sanMove[i++] = '=';
		sanMove[i++] = promoLetter[move.promoType];
	}
	sanMove[i] = '\0';
	//Move has to be made in order to figure out if it checks, mates or leads to stale mate
	return sanMove;
}

//#ifdef __cplusplus
//}
//#endif
