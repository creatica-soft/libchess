// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -Wl,-lchess,-rpath,/Users/ap/libchess -L /Users/ap/libchess -o test_nnue tbcore.c tbprobe.c test_nnue.cpp
//#include "nnue/types.h"
//#include "nnue/position.h"
//#include "nnue/evaluate.h"
//#include "nnue/nnue/nnue_common.h"
//#include "nnue/nnue/network.h"
#include "nnue/nnue/nnue_accumulator.h"
//#include "nnue/nnue/nnue_architecture.h"
//#include "nnue/nnue/features/half_ka_v2_hm.h"
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <math.h>
#include "tbprobe.h"
#include "libchess.h"

#define SYZYGY_PATH "/Users/ap/syzygy"
#define PROBABILITY_MASS 100
#define EVAL_SCALE 6.0
#define TEMPERATURE 60

struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;    
};
//void init_nnue(const char * nnue_file_big = EvalFileDefaultNameBig, const char * nnue_file_small = EvalFileDefaultNameSmall);
void init_nnue();
void cleanup_nnue();
void init_nnue_context(NNUEContext& ctx);
void free_nnue_context(NNUEContext& ctx);
double evaluate_nnue(const Board& board, NNUEContext& ctx);
std::pair<Stockfish::DirtyPiece&, Stockfish::DirtyThreats&> accumulator_stack_push(NNUEContext& ctx);
void accumulator_stack_pop(NNUEContext& ctx);
void accumulator_stack_reset(NNUEContext& ctx);
void compute_move_evals(Board& chess_board, ZobristHash& board_hash, NNUEContext& ctx, NNUEContext& ctx2, std::vector<std::tuple<double, int, int, unsigned long long>>& move_evals);
double temperature = TEMPERATURE * 0.01;
//double probability_mass = PROBABILITY_MASS * 0.01;
double eval_scale = EVAL_SCALE;
Zobrist z = {};

void get_prob(std::vector<std::tuple<double, int, int, unsigned long long>>& move_evals) {
    //size_t n = move_evals.size();
    //if (n == 0) return 0;
    double max_val = -std::numeric_limits<double>::infinity();
    for (const auto& ev : move_evals) {
        if (std::get<0>(ev) > max_val) max_val = std::get<0>(ev);
    }
    double total = 0.0;
    for (const auto& ev : move_evals) {
        total += std::exp((std::get<0>(ev) - max_val)/temperature);
    }
    if (total == 0.0) {
        double uniform = 1.0 / move_evals.size();
        for (auto& ev : move_evals) std::get<0>(ev) = uniform;
        return;
    }
    //double cum_mass = 0.0;
    //int effective = 0;
    for (auto& ev : move_evals) {
        std::get<0>(ev) = std::exp((std::get<0>(ev) - max_val)/temperature) / total;
        //cum_mass += std::get<0>(ev);
        //++effective;
        //if (cum_mass >= prob_mass) break;
    }
    //return effective;
}
 
double process_check(Board& temp_board, ZobristHash& board_hash, NNUEContext& ctx, NNUEContext& ctx2) {
  std::vector<std::tuple<double, int, int, unsigned long long>> move_evals; 
  //use 1.0 for probability mass to try all moves - when in check, there shouldn't be too many moves
  printf("check! ");
  compute_move_evals(temp_board, board_hash, ctx, ctx2, move_evals); 
  return -std::get<2>(move_evals[0]) * 0.01;
}

//called from do_move() and set_root()
//calls evaluate_nnue() and process_check()
//returns position evaluation in pawns from board_fen.sideToMove perspective
double position_eval(Board& chess_board, ZobristHash& board_hash, NNUEContext& ctx, NNUEContext& ctx2) {
  double res;
	const int pieceCount = bitCount(chess_board.side[ColorWhite] | chess_board.side[ColorBlack]);
	isCheckMateStaleMate(chess_board);
	if (pieceCount > TB_LARGEST || chess_board.castlingRights) {
    //evaluate_nnue() returns result in pawns (not centipawns!)
    //we made the move above, so the eval res is from the perspective of opponent color or board_fen.sideToMove
    //and must be negated to preserve the perspective of board_fen.sideToMove
    if (chess_board.isMate) res = -MATE_SCORE * 0.01; //chess_board.sideToMove loses
    else if (chess_board.isStaleMate) {
      res = 0.0;
    } else if (chess_board.isCheck) {
      res = process_check(chess_board, board_hash, ctx, ctx2);
    } else {
      res = evaluate_nnue(chess_board, ctx); //evaluate_nnue() returns result in pawns (not centipawns!)
      accumulator_stack_reset(ctx2);
      double res2 = evaluate_nnue(chess_board, ctx2);
      if (std::abs(res - res2) > 0.1) printf("inc eval res %f, full eval res %f", res, res2);
    }
  } else { //pieceCount <= TB_LARGEST, etc
    const unsigned int ep = legalEnPassantMove(chess_board);
    const unsigned int wdl = tb_probe_wdl(chess_board.side[ColorWhite], chess_board.side[ColorBlack], chess_board.pieceTypes[King - 1], chess_board.pieceTypes[Queen - 1], chess_board.pieceTypes[Rook - 1], chess_board.pieceTypes[Bishop - 1], chess_board.pieceTypes[Knight - 1], chess_board.pieceTypes[Pawn - 1],
      0, 0, ep == SquareNone ? 0 : ep, (chess_board.sideToMove ^ 1) == ColorBlack ? 1 : 0);
    if (wdl == TB_RESULT_FAILED) {
      fprintf(stderr, "error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, occupations %u, ep %u, halfmoveClock %u, whiteToMove %u, whites %llu, blacks %llu, kings %llu, queens %llu, rooks %llu, bishops %llu, knights %llu, pawns %llu, err %s\n", TB_LARGEST, pieceCount, ep, chess_board.halfmoveClock, (chess_board.sideToMove ^ 1) == ColorBlack ? 1 : 0, chess_board.side[ColorWhite], chess_board.side[ColorBlack], chess_board.pieceTypes[King - 1], chess_board.pieceTypes[Queen - 1], chess_board.pieceTypes[Rook - 1], chess_board.pieceTypes[Bishop - 1], chess_board.pieceTypes[Knight - 1], chess_board.pieceTypes[Pawn - 1], strerror(errno));
      if (chess_board.isMate) res = -MATE_SCORE * 0.01;
      else if (chess_board.isStaleMate) {
        res = 0.0; 
      } else if (chess_board.isCheck) {
        res = process_check(chess_board, board_hash, ctx, ctx2);
      } else {
        res = evaluate_nnue(chess_board, ctx);
        accumulator_stack_reset(ctx2);
        double res2 = evaluate_nnue(chess_board, ctx2);
        if (std::abs(res - res2) > 0.1) printf("inc eval res %f, full eval res2 %f", res, res2);
      }
    } else { //tb_probe_wdl() succeeded
      //0 - loss, 4 - win, 1..3 - draw
      if (wdl == 4) res = MATE_SCORE * 0.001;
      else if (wdl == 0) res = -MATE_SCORE * 0.001;
      else res = 0.0;
    }
  } //end of else (pieceCount <= TB_LARGEST)
  return res;
}

double make_move(Board& chess_board, const ZobristHash& board_hash, Move& move, NNUEContext& ctx, NNUEContext& ctx2, unsigned long long& child_hash) {
  ZobristHash tmp_hash = board_hash;
  StateInfo state = {};
  auto [dp, dts] = accumulator_stack_push(ctx);
  updateHash(tmp_hash, chess_board, move, do_move_dp(chess_board, move, state, dp, dts), z); //do the move, update the hash
  child_hash = tmp_hash.hash;
  printf("move %s%s%c: ", square[move.src], square[move.dst], uciPromoLetter[move.promoType]);
  double res = position_eval(chess_board, tmp_hash, ctx, ctx2); //evaluate the position
  printf("\n");
  undo_move(chess_board, move, state); //undo the move
  accumulator_stack_pop(ctx);
  return -res;
}

void compute_move_evals(Board& chess_board, ZobristHash& board_hash, NNUEContext& ctx, NNUEContext& ctx2,  std::vector<std::tuple<double, int, int, unsigned long long>>& move_evals) {
  double res;
  Move move;
  auto [moves, pinned, pinning, checkers, kingSquare] = kingMoves(chess_board);
  move.src = kingSquare;
  move.promoType = PieceTypeNone;
  while (moves) {
    move.dst = lsBit(moves);
    uint64_t child_hash = 0;
    res = make_move(chess_board, board_hash, move, ctx, ctx2, child_hash);
    move_evals.push_back({res, (move.promoType << 12) | (move.src << 6) | move.dst, static_cast<int>(-res * 100), child_hash});
    moves &= moves - 1;
  }
  if (bitCount(checkers) <= 1) {
    auto [check_mask, ep_mask] = checkers ? checkMask(chess_board, kingSquare, checkers) : std::make_pair(0xffffffffffffffffULL, 0ULL);
    for (PieceType pt = Queen; pt >= Pawn; pt = (PieceType)(pt - 1)) {
    	uint64_t occupations = chess_board.side[chess_board.sideToMove] & chess_board.pieceTypes[pt - 1]; 
    	while (occupations) {
    	  move.src = lsBit(occupations);
	      moves = piece_moves(chess_board, pt, move.src, kingSquare, pinned, pinning, check_mask, ep_mask);
    	  while (moves) {
    	    move.dst = lsBit(moves);
        	PieceType startPiece = PieceTypeNone, endPiece = PieceTypeNone;
        	if (promoMove(chess_board, move)) {
        	  startPiece = Knight;
        	  endPiece = Queen;
        	}
      	  for (move.promoType = startPiece; move.promoType <= endPiece; move.promoType = (PieceType)(move.promoType + 1)) { //loop over promotions if any
      	    uint64_t child_hash = 0;
      	    res = make_move(chess_board, board_hash, move, ctx, ctx2, child_hash);
            move_evals.push_back({res, (move.promoType << 12) | (move.src << 6) | move.dst, static_cast<int>(-res * 100), child_hash});
      	  }
          moves &= moves - 1;
        }
        occupations &= occupations - 1;
      }
    }
  }
  // Sort by res descending
  std::sort(move_evals.begin(), move_evals.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b);});
  get_prob(move_evals);
  //move_evals.resize(effective_branching);
}

int main(int argc, char ** argv) {
  Board board;
  ZobristHash zh;
	Move move;
  NNUEContext ctx, ctx2;
  char fen[MAX_FEN_STRING_LEN] = "";
  char uciMove[6] = "";
	if (argc == 1) strncpy(fen, startPos, MAX_FEN_STRING_LEN);
	else if (argc >= 7) {
	  for (int i = 1; i < 7; i++) {
	    strcat(fen, argv[i]);
	    strcat(fen, " ");
	   }
	} 
  tb_init(SYZYGY_PATH);
  if (TB_LARGEST == 0) {
      fprintf(stderr, "info string error unable to initialize tablebase; no tablebase files found in %s\n", SYZYGY_PATH);
  } else {
    fprintf(stdout, "info string successfully initialized tablebases in %s. Max number of pieces %d\n", SYZYGY_PATH, TB_LARGEST);
  }
	//init_magic_bitboards();
  zobristHash(z);
	if (fen2board(board, fen)) {
		printf("test_nnue error: fen2board() failed; FEN %s\n", fen);
		return 1;
	}
  getHash(zh, board, z);
  //init_nnue("nn-1111cefa1111.nnue", "nn-37f18f62d772.nnue");
  //init_nnue("nn-1c0000000000.nnue", "nn-37f18f62d772.nnue");
  Stockfish::Bitboards::init();
  init_nnue();
	init_nnue_context(ctx);
	init_nnue_context(ctx2);
  accumulator_stack_reset(ctx);
  
  position_eval(board, zh, ctx, ctx2);
  
	isCheckMateStaleMate(board);
  if (!board.isMate && !board.isStaleMate) {
    std::vector<std::tuple<double, int, int, unsigned long long>> move_evals; //res, move_idx, scorecp, hash
    compute_move_evals(board, zh, ctx, ctx2, move_evals);
    std::cout << "Outcome " << -std::get<2>(move_evals[0]) << " for " << color[board.sideToMove] << std::endl;
    int effective_branching = move_evals.size();
    for (int i = 0; i < effective_branching; i++) {
      char uci_move[6] = "";
      idx2uci(std::get<1>(move_evals[i]), uci_move);
      std::cout << uci_move << " (" << std::get<0>(move_evals[i]) * 100 << "%)" << ", cp " << -std::get<2>(move_evals[i]) << std::endl;
    }
  } else if (board.isMate) 
    printf("%s is mated\n", color[board.sideToMove]);
  else printf("stalemate, %s has no moves\n", color[board.sideToMove]);
  free_nnue_context(ctx);
  free_nnue_context(ctx2);
  cleanup_nnue();
  //cleanup_magic_bitboards();
  return 0;
}
