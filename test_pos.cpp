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

#define STOCKFISH "/Users/ap/stockfish-macos-m1-apple-silicon"
#define MOVETIME 2000
#define DEPTH 0
#define HASH 256
#define THREADS 1
#define SYZYGY_PATH "/Users/ap/syzygy"
#define LOGGING false
#define LIMIT_STRENGTH false
#define ELO 2600
//eval1 vs eval2 is the SAME net incrementally vs from scratch - it must agree exactly
#define INCREMENTAL_EPS 1e-9
//eval2 vs eval3 is a different engine's full eval - Stockfish applies material damping
//and diverges proportionally, so allow an absolute band plus a relative one
#define REF_ABS_TOL 0.10
#define REF_REL_TOL 0.05

struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;    
};
struct Evaluation evaluation = {};
struct Evaluation * evaluations[1] = { nullptr };
struct Engine stockfish;

void init_nnue();
void cleanup_nnue();
void init_nnue_context(NNUEContext& ctx);
void free_nnue_context(NNUEContext& ctx);
double evaluate_nnue(const Board& chess_board, NNUEContext& ctx);
std::pair<Stockfish::DirtyPiece&, Stockfish::DirtyThreats&> accumulator_stack_push(NNUEContext& ctx);
void accumulator_stack_pop(NNUEContext& ctx);
void accumulator_stack_reset(NNUEContext& ctx);
std::string nnue_eval(const Board& board);
NNUEContext ctx, ctx2;
Board board;
ZobristHash zh = {};
Zobrist z = {};

int test(std::string fenString) {
	int failures = 0;
	if (fen2board(board, fenString.c_str())) {
		fprintf(stderr, "FAIL fen2board: FEN %s\n", fenString.c_str());
		return 1;
	}	
	char fenString2[MAX_FEN_STRING_LEN];
	board2fen(board, fenString2);
	if (strncmp(fenString.c_str(), fenString2, MAX_FEN_STRING_LEN) != 0) {
	  fprintf(stderr, "FAIL fen round-trip: not the same \'%s\' != \'%s\', castlingRights %x\n", fenString.c_str(), fenString2, board.castlingRights);
	  return 1;
	}
	//writeDebug(board);
  if (reconcile(board)) return 1;
	getHash(zh, board, z);
	//printf("hash %llx\n", zh.hash);
	isCheckMateStaleMate(board);
  //reset unconditionally: the move loop below pushes onto ctx whether or not the ROOT
  //was in check, so gating the reset on the else-branch left a stale stack behind
  accumulator_stack_reset(ctx);
  if (board.isMate) printf("%s is mated\n", color[board.sideToMove]);
  else if (board.isStaleMate) printf("stalemate, %s has no moves\n", color[board.sideToMove]);
  else if (board.isCheck) printf("%s is checked\n", color[board.sideToMove]);
  else {
    double eval = evaluate_nnue(board, ctx);
    printf("pos %s castlilngRights %hhx eval %f pawns\n", fenString.c_str(), board.castlingRights, eval);
  } 
  
  newGame(stockfish);
  
  double sum = 0;
  int move_num = 0;
  Move move;
  auto [moves, pinned, pinning, checkers, kingSquare] = kingMoves(board);
  move.src = kingSquare;
  move.promoType = PieceTypeNone;
  //drawMoves(board, move.src, moves);
  while (moves) {
    move_num++;
    move.dst = lsBit(moves);
    ZobristHash tmp_hash = zh;
    auto start = std::chrono::high_resolution_clock::now();
    StateInfo state = {};
    auto [dp, dts] = accumulator_stack_push(ctx);
    const PieceType capturedType = do_move_dp(board, move, state, dp, dts);
    printf("move piece %s, src %s, dst %s, promo %c, move_type %s, captured type %s, prior hash %llx\n", piece[PC(static_cast<Color>(board.sideToMove ^ 1), King)], square[move.src], square[move.dst], uciPromoLetter[move.promoType], moveType[move.type], pieceType[capturedType], tmp_hash.hash);
    //writeDebug(board);
    if (reconcile(board)) return 1;
    updateHash(tmp_hash, board, move, capturedType, z);
    ZobristHash h = {};
    getHash(h, board, z);
    if (tmp_hash.hash != h.hash) {
      fprintf(stderr, "FAIL hash: updateHash %llx != getHash %llx\n", tmp_hash.hash, h.hash);
      return 1;
    }
    isCheckMateStaleMate(board);
    if (!board.isCheck && !board.isMate && !board.isStaleMate) {
      double eval1 = evaluate_nnue(board, ctx);
      accumulator_stack_reset(ctx2);
      double eval2 = evaluate_nnue(board, ctx2);
      char fen[MAX_FEN_STRING_LEN];
      strncpy(stockfish.position, board2fen(board, fen), MAX_FEN_STRING_LEN);
      float eval3 = 0;
      //Stockfish reports from White's perspective (engine.cpp:721); evaluate_nnue()
      //reports from the side to move's - so only negate when Black is to move
      if (position(stockfish)) eval3 = (board.sideToMove == ColorWhite) ? eval(stockfish) : -eval(stockfish);
      if (std::abs(eval1 - eval2) > INCREMENTAL_EPS) {
        failures++;
        fprintf(stderr, "FAIL accumulator: incremental %f != fresh %f  %s\n", eval1, eval2, fen);
      }
      if (std::abs(eval2 - eval3) > std::max((double)REF_ABS_TOL, REF_REL_TOL * std::abs(eval2))) {
        failures++;
        fprintf(stderr, "FAIL reference: libchess %f vs stockfish %f  %s\n", eval2, eval3, fen);
      }
    }
    undo_move(board, move, state);
    accumulator_stack_pop(ctx);
    double elapsed = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
    sum += elapsed;
    moves &= moves - 1;
  }
  if (bitCount(checkers) <= 1) {
    auto [check_mask, ep_mask] = checkers ? checkMask(board, kingSquare, checkers) : std::make_pair(0xffffffffffffffffULL, 0ULL);
    printf("check_mask %llx, ep_mask %llx\n", check_mask, ep_mask);
    for (PieceType pt = Queen; pt >= Pawn; --pt) {
    	uint64_t occupations = board.side[board.sideToMove] & board.pieceTypes[pt - 1]; 
    	while (occupations) {
    	  move.src = lsBit(occupations);
        //writeDebug(board);
        //printf("pt %s, move.src %s, kingSquare %s, pinned %llx, pinning %llx, checkers %llx\n", pieceType[pt], square[move.src], square[kingSquare], pinned, pinning, checkers);
        moves = piece_moves(board, pt, move.src, kingSquare, pinned, pinning, check_mask, ep_mask);
      	//drawMoves(board, move.src, moves);
  
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
            auto [dp, dts] = accumulator_stack_push(ctx);
            const PieceType capturedType = do_move_dp(board, move, state, dp, dts);
            printf("move piece %s, src %s, dst %s, promo %c, move_type %s, captured type %s, prior hash %llx\n", piece[PC(static_cast<Color>(board.sideToMove ^ 1), pt)], square[move.src], square[move.dst], uciPromoLetter[move.promoType], moveType[move.type], pieceType[capturedType], tmp_hash.hash);
            //writeDebug(board);
            if (reconcile(board)) return 1;
            updateHash(tmp_hash, board, move, capturedType, z);
            ZobristHash h = {};
            getHash(h, board, z);
            if (tmp_hash.hash != h.hash) {
              fprintf(stderr, "FAIL hash: updateHash %llx != getHash %llx\n", tmp_hash.hash, h.hash);
              return 1;
            }
            isCheckMateStaleMate(board);
            if (!board.isCheck && !board.isMate && !board.isStaleMate) {
              double eval1 = evaluate_nnue(board, ctx);
              accumulator_stack_reset(ctx2);
              double eval2 = evaluate_nnue(board, ctx2);
              char fen[MAX_FEN_STRING_LEN];
              strncpy(stockfish.position, board2fen(board, fen), MAX_FEN_STRING_LEN);
              float eval3 = 0;
              //Stockfish reports from White's perspective (engine.cpp:721); evaluate_nnue()
              //reports from the side to move's - so only negate when Black is to move
              if (position(stockfish)) eval3 = (board.sideToMove == ColorWhite) ? eval(stockfish) : -eval(stockfish);
              if (std::abs(eval1 - eval2) > INCREMENTAL_EPS) {
                failures++;
                fprintf(stderr, "FAIL accumulator: incremental %f != fresh %f  %s\n", eval1, eval2, fen);
              }
              if (std::abs(eval2 - eval3) > std::max((double)REF_ABS_TOL, REF_REL_TOL * std::abs(eval2))) {
                failures++;
                fprintf(stderr, "FAIL reference: libchess %f vs stockfish %f  %s\n", eval2, eval3, fen);
              }
            }
            move_num++;
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
  }
  if (move_num) printf("avg elapsed %f over %d moves\n", sum / move_num, move_num);
  return failures;
}

int main(int argc, char ** argv) {
  zobristHash(z);
  evaluations[0] = &evaluation;
  evaluations[0]->maxPlies = 1;
  char uciMove[6] = "";
  int total_failures = 0;
  bool file_source = false;
  std::string fenString(startPos);
  Stockfish::Bitboards::init();
  init_nnue();
	init_nnue_context(ctx);
	init_nnue_context(ctx2);
	initChessEngine(stockfish, STOCKFISH, MOVETIME, DEPTH, HASH, THREADS, SYZYGY_PATH, 1, LOGGING, LIMIT_STRENGTH, ELO); //MultiPV, logging, limitStrength, Elo
	if (argc == 2) {
	  std::string filename = argv[1];
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return 1;
    }
    file_source = true;
    std::string fenString;
    int positions = 0;
    while (std::getline(file, fenString)) {
      if (fenString.empty()) continue;
      positions++;
      //one call per FEN, and do not stop at the first failure - report them all
      total_failures += test(fenString);
    }
    std::cout << "Total FEN positions: " << positions << ", failures: " << total_failures << std::endl;
	}
	else if (argc >= 7) {
	  fenString = "";
	  for (int i = 1; i < 7; i++) {
	    fenString += argv[i];
	    if (i != 6) fenString += " ";
	   }
	}
	
	if (!file_source) total_failures += test(fenString);
	
  free_nnue_context(ctx);
  free_nnue_context(ctx2);
  cleanup_nnue();
  releaseChessEngine(stockfish);
  //the exit code is the verdict: non-zero if anything failed
  return total_failures ? 1 : 0;
}
