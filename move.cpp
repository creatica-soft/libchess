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

int uci2move_idx(const char * uci_move, Move& move) {
  assert(uci_move);
  assert(strlen(uci_move) >= 4);
  move.src = SQ(uci_move[1] - '1', uci_move[0] - 'a');
  move.dst = SQ(uci_move[3] - '1', uci_move[2] - 'a');
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

char * idx2uci(const int move_idx, char * uci_move) {
  assert(uci_move);
  assert(sizeof(uci_move) >= 6);
  uci_move[0] = 0;
  const Square dst = (Square)(move_idx & 63);
  const Square src = (Square)((move_idx >> 6) & 63);
  const PieceType promo = (PieceType)((move_idx >> 12) & 7);
  strcat(uci_move, square[src]);
  strcat(uci_move, square[dst]);
  uci_move[4] = uciPromoLetter[promo]; //index 0, 1, 6 and 7 are '\0'
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

 	KingSquare kingSq;
	Square kingSquare = getKingSquare(board, kingSq);
	MovesContext movesContext = {};
	uint64_t attackedSquares = getAttackedSquares(board, movesContext);
	uint64_t king_moves = kingMoves(board, kingSquare, kingSq, movesContext, attackedSquares);

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
		//move.src = lsBit(board.side[board.sideToMove] & board.pieceTypes[King - 1]);
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
			const Rank rookRank = board.sideToMove == ColorWhite ? Rank1 : Rank8;
			const Square rookSquare = SQ(rookRank, board.castlingRook[board.sideToMove][castlingSide]);
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
					movingPiece = PC(board.sideToMove, pieceIndex);
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
     		  uint64_t moves = piece_moves(Pawn, srcSquare, movesContext, kingSq, board);
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
		} else if ((board.enPassant < FileNone) && board.enPassant + (board.sideToMove == ColorWhite ? Rank6 << 3 : Rank3 << 3) == move.dst) {
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
		uint64_t cp = board.side[board.sideToMove] & board.pieceTypes[pt - 1];
		//iterate over all squares where movingPiece.name are located
		while (cp) {
  		Square s = lsBit(cp);
  		uint64_t moves;
  		if (pt != King)
  		  moves = piece_moves(pt, s, movesContext, kingSq, board);
  		else moves = king_moves;
			if ((moves & SQ_BIT(move.dst))) {
  			//if moving piece is a pawn (its source file is known and equals to the square s.file) or a king
				if ((pt == Pawn && SQ_FILE(s) == srcFile) || pt == King) {
					//we are sure that square s is the source square
					move.src = s;
					break;
				} else { //otherwise add this square to moveCandidates squares
					if (n >= 10) {
				    printf("validateSanMove() error: too many move candidates for %s\n", sanMove);
				    writeDebug(board);
				    return 8;
					}				
					moveCandidates[n++] = s;
        }
			}
			cp &= cp - 1;
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
				printf("san2move() error: ambiguous move %s, moveType %s, srcRank %c, srcFile %c, max candidate rating %d, max candidates %d\n", sanMove, moveType[move.type], enumRanks[srcRank], enumFiles[srcFile], maxT, n);
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
	
	if (movesContext.num_checkers > 1 && PC_TYPE(movingPiece) != King) {
	  char fenString[MAX_FEN_STRING_LEN] = "";
		printf("san2move() error: %s move from %s to %s is illegal because king is checked twice. FEN %s\n", piece[movingPiece], square[move.src], square[move.dst], board2fen(board, fenString));
		writeDebug(board);
		//for (int i = 0; i < n; i++) printf("moveCandidate %s\n", square[moveCandidates[i]]);
		return 1;	  
	}

	uint64_t moves;
	if (PC_TYPE(movingPiece) != King)
	  moves = piece_moves(PC_TYPE(movingPiece), move.src, movesContext, kingSq, board);
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
	
 	KingSquare kingSq;
	Square kingSquare = getKingSquare(board, kingSq);
	MovesContext movesContext = {};
	uint64_t attackedSquares = getAttackedSquares(board, movesContext);
	kingMoves(board, kingSquare, kingSq, movesContext, attackedSquares);
	if (movesContext.num_checkers > 1 && mpType != King) {
	  char fenString[MAX_FEN_STRING_LEN] = "";
		printf("move2san() error: %s move from %s to %s is illegal; FEN %s\n", piece[movingPiece], square[move.src], square[move.dst], board2fen(board, fenString));
		writeDebug(board);
		//for (int i = 0; i < n; i++) printf("moveCandidate %s\n", square[moveCandidates[i]]);
		return nullptr;	  
	}
		
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
  		uint64_t moves = piece_moves(mpType, s, movesContext, kingSq, board);
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
