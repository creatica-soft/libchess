// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -Wl,-lchess,-rpath,/Users/ap/libchess -L /Users/ap/libchess -o test_pos test_pos.cpp

//NNUE full eval vs incremental eval test confirms that incremental is faster by one order of magnitude, i.e. about 10 times on average
//full eval is tens of microseconds per position vs incremental eval is just microseconds per position on M1

#include <algorithm>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include "nnue/nnue/nnue_accumulator.h"
//#include "tbprobe.h"
#include "libchess.h"

struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;    
};

void init_nnue(const char * nnue_file_big, const char * nnue_file_small);
void cleanup_nnue();
void init_nnue_context(NNUEContext& ctx);
void free_nnue_context(NNUEContext& ctx);
double evaluate_nnue(const Board& chess_board, NNUEContext& ctx);
void accumulator_stack_push(NNUEContext& ctx, Stockfish::DirtyPiece& dp);
void accumulator_stack_pop(NNUEContext& ctx);
void accumulator_stack_reset(NNUEContext& ctx);
std::string nnue_eval(const Board& board);
NNUEContext ctx;
Board board;
ZobristHash zh = {};
Zobrist z = {};

double test(std::string fenString) {
	if (fen2board(board, fenString.c_str())) {
		printf("test_pos error: fen2board() failed; FEN %s\n", fenString.c_str());
		return 1;
	}
	const int castlingRights = (board.castlingRook[0][0] != FileNone) | ((board.castlingRook[0][1] != FileNone) << 1) | ((board.castlingRook[1][0] != FileNone) << 2) | ((board.castlingRook[1][1] != FileNone) << 3);	
	char fenString2[MAX_FEN_STRING_LEN];
	board2fen(board, fenString2);
	if (strncmp(fenString.c_str(), fenString2, MAX_FEN_STRING_LEN) != 0) {
	  printf("test_pos() error: fenStrings are not the same \'%s\' != \'%s\', castlingRights %x\n", fenString.c_str(), fenString2, castlingRights);
	  return 1;
	}
	//writeDebug(board);
  if (reconcile(board)) return 1;
  /*for (int i = 0; i < 64; i++) {
    if (board.piecesOnSquares[i] != PieceNameNone) drawMoves(board, i, movesFromSquares);
  }*/
	getHash(zh, board, z);
	//printf("hash %llx\n", zh.hash);
	//struct MovesContext movesContext;
	//uint64_t movesFromSquares[64] = {};
	//generateMoves(board, movesContext, getAttackedSquares(board, movesContext), movesFromSquares);
	//generateMoves(board, movesFromSquares);
	isCheckMateStaleMate(board);
  if (board.isMate) printf("%s is mated\n", color[board.sideToMove]);
  else if (board.isStaleMate) printf("stalemate, %s has no moves\n", color[board.sideToMove]);
  else if (board.isCheck) printf("%s is checked\n", color[board.sideToMove]);
  else {
    accumulator_stack_reset(ctx);
    double eval = evaluate_nnue(board, ctx);
    printf("pos %s eval %f pawns\n", fenString.c_str(), eval);
  }
/*uint64_t any = board.side[board.sideToMove];
	Move move = {};
  double sum = 0, sum2 = 0;
  int move_num = 0;
  while (any) {
    move.src = lsBit(any);
    uint64_t moves = movesFromSquares[move.src];
    while (moves) {
      move_num++;
      move.dst = lsBit(moves);
      move.promoType = promoMove(board, move) ? Queen : PieceTypeNone;
      //move.type = MoveTypeNormal;
      ZobristHash tmp_hash = zh;
      const int mp = board.piecesOnSquares[move.src];
      auto start = std::chrono::high_resolution_clock::now();
      StateInfo state = {};
      Stockfish::DirtyPiece dp = {};
      const PieceType capturedType = do_move_dp(board, move, state, dp);
      printf("move piece %s, src %s, dst %s, promo %c, move_type %s, captured type %s, hash %llx\n", piece[mp], square[move.src], square[move.dst], uciPromoLetter[move.promoType], moveType[move.type], pieceType[capturedType], tmp_hash.hash);
      //writeDebug(board);
      if (reconcile(board)) return 1;
      updateHash(tmp_hash, board, move, capturedType, z);
      struct ZobristHash h = {};
      getHash(h, board, z);
      if (tmp_hash.hash != h.hash) {
        printf("error: updateHash() and getHash() produce different hashes %llx != %llx\n", tmp_hash.hash, h.hash);
        return 1;
      }
      accumulator_stack_push(ctx, dp);
      double eval = evaluate_nnue(board, ctx);
      undo_move(board, move, state);
      accumulator_stack_pop(ctx);
      double elapsed = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
      sum += elapsed;
      //printf("pos eval after move %s%s%c %f pawns\n", square[move.src], square[move.dst], uciPromoLetter[move.promoType], eval);
      NNUEContext ctx2;
      init_nnue_context(ctx2);
      auto start2 = std::chrono::high_resolution_clock::now();
      accumulator_stack_reset(ctx2);
      StateInfo state2 = {};
      const PieceType capturedType2 = do_move(board, move, state2);
      double eval2 = evaluate_nnue(board, ctx2);
      undo_move(board, move, state2);
      //double elapsed2 = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start2).count();
      //sum2 += elapsed2;
      //printf("elapsed %f, elapsed2 %f\n", elapsed, elapsed2);
      //printf("pos eval2 after move %s%s%c %f pawns\n", square[move.src], square[move.dst], uciPromoLetter[move.promoType], eval2);
      if (eval != eval2)
        printf("pos eval %f != eval2 %f after move %s%s%c\n", eval, eval2, square[move.src], square[move.dst], uciPromoLetter[move.promoType]);
      struct ZobristHash hh = {};
      getHash(hh, board, z);
      if (zh.hash != hh.hash) {
      //  writeDebug(board);
        reconcile(board);
        printf("error: hash after undo_move() is different %llx != %llx\n", zh.hash, hh.hash);
        return 1;
      }
      
  		//MovesContext movesContext;
  		//uint64_t movesFromSquares[64] = {};
  		//generateMoves(tmp_board, movesContext, getAttackedSquares(tmp_board, movesContext), movesFromSquares);    
      //if (tmp_board.isMate) printf("%s is mated\n", color[tmp_board.sideToMove]);
      //else if (tmp_board.isStaleMate) printf("stalemate, %s has no moves\n", color[tmp_board.sideToMove]);
      //else if (tmp_board.isCheck) printf("%s is checked\n", color[tmp_board.sideToMove]);
      //if (reconcile(tmp_board)) return 1;
      //for (int i = 0; i < 64; i++) {
      //  if (tmp_board.piecesOnSquares[i] != PieceNameNone) drawMoves(tmp_board, i, movesFromSquares);
      //}
      moves &= moves - 1;
    }
    any &= any - 1;
  }*/
  
      double sum = 0, sum2 = 0;
      int move_num = 0;
      Move move = {};
    	MovesContext movesContext = {};
     	KingSquare kingSq;
     	move.src = getKingSquare(board, kingSq);
  	  uint64_t moves = kingMoves(board, move.src, kingSq, movesContext, getAttackedSquares(board, movesContext));
  	  while (moves) {
        move_num++;
        move.dst = lsBit(moves);
        ZobristHash tmp_hash = zh;
        auto start = std::chrono::high_resolution_clock::now();
        StateInfo state = {};
        Stockfish::DirtyPiece dp = {};
        const PieceType capturedType = do_move_dp(board, move, state, dp);
        printf("move piece %s, src %s, dst %s, promo %c, move_type %s, captured type %s, hash %llx\n", piece[PC(board.sideToMove ^ 1, King)], square[move.src], square[move.dst], uciPromoLetter[move.promoType], moveType[move.type], pieceType[capturedType], tmp_hash.hash);
        //writeDebug(board);
        if (reconcile(board)) return 1;
        updateHash(tmp_hash, board, move, capturedType, z);
        ZobristHash h = {};
        getHash(h, board, z);
        if (tmp_hash.hash != h.hash) {
          printf("error: updateHash() and getHash() produce different hashes %llx != %llx\n", tmp_hash.hash, h.hash);
          return 1;
        }
        accumulator_stack_push(ctx, dp);
        isCheckMateStaleMate(board);
        if (!board.isCheck && !board.isMate && !board.isStaleMate) {
          double eval = evaluate_nnue(board, ctx);
        }
        undo_move(board, move, state);
        accumulator_stack_pop(ctx);
        double elapsed = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
        sum += elapsed;
        moves &= moves - 1;
      }
      if (movesContext.num_checkers > 1) {
        //if (board.num_moves == 0) board.isMate = true;
        goto exit;
      }
      
      for (PieceType pt = Queen; pt >= Pawn; pt = (PieceType)(pt - 1)) {
      	uint64_t occupations = board.side[board.sideToMove] & board.pieceTypes[pt - 1]; 
      	while (occupations) {
      	  move.src = lsBit(occupations);
  	      moves = piece_moves(pt, move.src, movesContext, kingSq, board);
      	  while (moves) {
      	    move.dst = lsBit(moves);
          	PieceType startPiece = PieceTypeNone, endPiece = PieceTypeNone;
          	if (promoMove(board, move)) {
          	  startPiece = Knight;
          	  endPiece = Queen;
          	}
        	  for (move.promoType = startPiece; move.promoType <= endPiece; move.promoType = (PieceType)(move.promoType + 1)) {
              ZobristHash tmp_hash = zh;
              auto start = std::chrono::high_resolution_clock::now();
              StateInfo state = {};
              Stockfish::DirtyPiece dp = {};
              const PieceType capturedType = do_move_dp(board, move, state, dp);
              printf("move piece %s, src %s, dst %s, promo %c, move_type %s, captured type %s, hash %llx\n", piece[PC(board.sideToMove ^ 1, pt)], square[move.src], square[move.dst], uciPromoLetter[move.promoType], moveType[move.type], pieceType[capturedType], tmp_hash.hash);
              //writeDebug(board);
              if (reconcile(board)) return 1;
              updateHash(tmp_hash, board, move, capturedType, z);
              ZobristHash h = {};
              getHash(h, board, z);
              if (tmp_hash.hash != h.hash) {
                printf("error: updateHash() and getHash() produce different hashes %llx != %llx\n", tmp_hash.hash, h.hash);
                return 1;
              }
              accumulator_stack_push(ctx, dp);
              isCheckMateStaleMate(board);
              if (!board.isCheck && !board.isMate && !board.isStaleMate) {
                double eval = evaluate_nnue(board, ctx);
              }
              undo_move(board, move, state);
              accumulator_stack_pop(ctx);
              double elapsed = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
              sum += elapsed;
        	    
        	  }
            moves &= moves - 1;
          }
          occupations &= occupations - 1;
        }
      }
exit:
  double avg = sum / move_num;
  printf("avg elapsed %f\n", avg);
  return avg;
}

int main(int argc, char ** argv) {
  zobristHash(z);
  char uciMove[6] = "";
  bool file_source = false;
  std::string fenString(startPos);
	init_magic_bitboards();
	init_nnue("nn-1c0000000000.nnue", "nn-37f18f62d772.nnue");
	init_nnue_context(ctx);
	if (argc == 2) {
	  std::string filename = argv[1];
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return 1;
    }
    file_source = true;
    std::string fenString;
    int lineNum = 1;
    double sum = 0;
    while (std::getline(file, fenString)) {
      //if (test(fenString)) break;
      sum += test(fenString);
      lineNum++;
    }
    std::cout << "Total Fen positions: " << lineNum << ", avg " << sum / lineNum << std::endl;
	}
	else if (argc >= 7) {
	  fenString = "";
	  for (int i = 1; i < 7; i++) {
	    fenString += argv[i];
	    if (i != 6) fenString += " ";
	   }
	}
	
	if (!file_source) test(fenString);
  free_nnue_context(ctx);
  cleanup_nnue();
  cleanup_magic_bitboards();
  return 0;
}
