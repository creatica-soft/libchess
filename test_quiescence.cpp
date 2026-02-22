// compile with c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -Wl,-lchess,-rpath,/Users/ap/libchess -L /Users/ap/libchess -o test_quiescence tbcore.c tbprobe.c test_quiescence.cpp
#include "nnue/types.h"
#include "nnue/position.h"
#include "nnue/evaluate.h"
#include "nnue/nnue/nnue_common.h"
#include "nnue/nnue/network.h"
#include "nnue/nnue/nnue_accumulator.h"
#include "nnue/nnue/nnue_architecture.h"
#include "nnue/nnue/features/half_ka_v2_hm.h"
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <math.h>
#include "tbprobe.h"
#include "libchess.h"

#define SYZYGY_PATH "/Users/ap/syzygy"
//#define PROBABILITY_MASS 100
#define EVAL_SCALE 62
#define TEMPERATURE 58
const int negamax_depth = 3;

struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;    
};
void init_nnue(const char * nnue_file_big = EvalFileDefaultNameBig, const char * nnue_file_small = EvalFileDefaultNameSmall);
void cleanup_nnue();
void init_nnue_context(NNUEContext& ctx);
void free_nnue_context(NNUEContext& ctx);
double evaluate_nnue(Board& board, NNUEContext& ctx);
void accumulator_stack_push(NNUEContext& ctx, DirtyPiece& dp);
void accumulator_stack_pop(NNUEContext& ctx);
void accumulator_stack_reset(NNUEContext& ctx);
void compute_move_evals(Board& chess_board, ZobristHash& board_hash, NNUEContext& ctx, std::vector<std::tuple<double, int, int, unsigned long long>>& move_evals);
std::tuple<double, int, uint64_t> position_eval(Board& chess_board, ZobristHash& board_hash, NNUEContext& ctx);

double temperature = TEMPERATURE * 0.01;
//double probability_mass = PROBABILITY_MASS * 0.01;
double eval_scale = EVAL_SCALE * 0.1;
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
        return static_cast<int>(n);
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
 
std::tuple<double, int, uint64_t> process_check(Board& chess_board, ZobristHash& board_hash, NNUEContext& ctx) {
  std::vector<std::tuple<double, int, uint64_t>> move_evals;// eval, move, hash
  //use 1.0 for probability mass to try all moves - when in check, there shouldn't be too many moves
  //compute_move_evals(temp_board, board_hash, ctx, move_evals, 1.0, movesFromSquares); 
  uint64_t any = chess_board.side[chess_board.sideToMove];
  Move move = {};
  while (any) {
      move.src = lsBit(any);
      uint64_t moves = movesFromSquares[move.src];
      while (moves) {
          move.dst = lsBit(moves);
          PieceType startP = PieceTypeNone, endP = PieceTypeNone;
          if (promoMove(chess_board, move)) { startP = Knight; endP = Queen; }
          for (move.promoType = startP; move.promoType <= endP; move.promoType = (PieceType)(move.promoType + 1)) {
              ZobristHash tmp_hash = board_hash;
              StateInfo state = {};
              DirtyPiece dp;
              updateHash(tmp_hash, chess_board, move, do_move_dp(chess_board, move, state, dp), z);
              accumulator_stack_push(ctx, dp);
              MovesContext movesContext;
              //uint64_t movesFromSquares2[64] = {0}; //can't reuse movesFromSquares
          		//generateMoves(chess_board, movesContext, getAttackedSquares(chess_board, movesContext), movesFromSquares2); //fills 
              auto [eval, move_idx, hash] = position_eval(chess_board, tmp_hash, ctx);
              move_evals.push_back({eval, move_idx, hash});
              undo_move(chess_board, move, state);
              accumulator_stack_pop(ctx);
              moves &= moves - 1;
          }
      } //end of while(moves)
      any &= any - 1;
  } //end of while(any)
  std::sort(move_evals.begin(), move_evals.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b);});
  return move_evals[0];
}

//called from do_move() and set_root()
//calls evaluate_nnue() and process_check()
//returns position evaluation in pawns from board_fen.sideToMove perspective
void position_eval(Board& chess_board, ZobristHash& board_hash, NNUEContext& ctx, std::vector<std::tuple<double, int, int, unsigned long long>>& move_evals) {
  move_evals.emplace_back();
  auto& move_eval = move_evals.back();
  std::get<1>(move_eval) = 0; //move_idx init
  std::get<3>(move_eval) = board_hash.hash;

	const int pieceCount = bitCount(chess_board.side[ColorWhite] | chess_board.side[ColorBlack]);
	if (pieceCount > TB_LARGEST || ((unsigned int *)chess_board.castlingRook)[0] != 0x08080808) {
    //evaluate_nnue() returns result in pawns (not centipawns!)
    //we made the move above, so the eval res is from the perspective of opponent color or board_fen.sideToMove
    //and must be negated to preserve the perspective of board_fen.sideToMove
		isCheckMateStaleMate(chess_board);
    if (chess_board.isMate) std::get<0>(move_eval) = -MATE_SCORE * 0.01; //chess_board.sideToMove loses
    else if (chess_board.isStaleMate) {
      std::get<0>(move_eval) = 0.0;
    } else if (chess_board.isCheck) {
      std::tie(std::get<0>(move_eval), std::get<1>(move_eval), std::get<3>(move_eval)) = process_check(chess_board, board_hash, ctx);
    } else {
      std::get<0>(move_eval) = evaluate_nnue(chess_board, ctx); //evaluate_nnue() returns result in pawns (not centipawns!)
    }
  } else { //pieceCount <= TB_LARGEST, etc
    const unsigned int ep = enPassantLegal(chess_board);
    const unsigned int wdl = tb_probe_wdl(chess_board.side[ColorWhite], chess_board.side[ColorBlack], chess_board.pieceTypes[King - 1], chess_board.pieceTypes[Queen - 1], chess_board.pieceTypes[Rook - 1], chess_board.pieceTypes[Bishop - 1], chess_board.pieceTypes[Knight - 1], chess_board.pieceTypes[Pawn - 1],
      0, 0, ep == SquareNone ? 0 : ep, OPP_COLOR(chess_board.sideToMove) == ColorBlack ? 1 : 0);
    if (wdl == TB_RESULT_FAILED) {
      fprintf(stderr, "error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, occupations %u, ep %u, halfmoveClock %u, whiteToMove %u, whites %llu, blacks %llu, kings %llu, queens %llu, rooks %llu, bishops %llu, knights %llu, pawns %llu, err %s\n", TB_LARGEST, pieceCount, ep, chess_board.halfmoveClock, OPP_COLOR(chess_board.sideToMove) == ColorBlack ? 1 : 0, chess_board.side[ColorWhite], chess_board.side[ColorBlack], chess_board.pieceTypes[King - 1], chess_board.pieceTypes[Queen - 1], chess_board.pieceTypes[Rook - 1], chess_board.pieceTypes[Bishop - 1], chess_board.pieceTypes[Knight - 1], chess_board.pieceTypes[Pawn - 1], strerror(errno));
  		isCheckMateStaleMate(chess_board);
      if (chess_board.isMate) std::get<0>(move_eval) = -MATE_SCORE * 0.01; //chess_board.sideToMove loses
      else if (chess_board.isStaleMate) {
        std::get<0>(move_eval) = 0.0;
      } else if (chess_board.isCheck) {
        std::tie(std::get<0>(move_eval), std::get<1>(move_eval), std::get<3>(move_eval)) = process_check(chess_board, board_hash, ctx);
      } else {
        std::get<0>(move_eval) = evaluate_nnue(chess_board, ctx); //evaluate_nnue() returns result in pawns (not centipawns!)
      }
    } else { //tb_probe_wdl() succeeded
      //0 - loss, 4 - win, 1..3 - draw
      if (wdl == 4) std::get<0>(move_eval) = MATE_SCORE * 0.001;
      else if (wdl == 0) std::get<0>(move_eval) = -MATE_SCORE * 0.001;
      else std::get<0>(move_eval) = 0.0;
    }
  } //end of else (pieceCount <= TB_LARGEST)
  std::get<2>(move_eval) = static_cast<int>(std::get<0>(move_eval) * 100);  
}

//similar to move_evals vector but as a tree with children
struct MiniNode {
    uint64_t hash = 0;     
    double score = 0;
    int move = 0;
    int scorecp = 0;
    std::vector<MiniNode> children;
};

//recursive negamax
std::tuple<double, int, int, uint64_t> resolve_negamax(const MiniNode& node) {
    if (node.children.empty()) return std::make_tuple(node.score, node.move, node.scorecp, node.hash);
    double search_val = -std::numeric_limits<double>::infinity();
    int search_move = 0, search_cp = 0;
    uint64_t search_hash = 0;
    if (!node.children.empty()) {
      for (const auto& child : node.children) {
          auto [child_val, child_move, child_cp, child_hash] = resolve_negamax(child);
          if (-child_val > search_val) {
            search_val = -child_val;
            search_move = child_move;
            search_cp = -child_cp;
            search_hash = child_hash;
          }
      }
    }
    return std::make_tuple(search_val, search_move, search_cp, search_hash);
}

void eval_recursive(Board& chess_board, ZobristHash& board_hash, NNUEContext& ctx, MiniNode& node, int depth) {
    bool only_captures = (depth >= 2); //also includes checks
    bool hard_stop = (depth >= negamax_depth); // Prevent explosion
    std::vector<std::tuple<double, int, int, unsigned long long>> move_evals;
    if (hard_stop) {
        //std::tie(node.score, node.move, node.hash) = position_eval(chess_board, board_hash, ctx, move_evals);
        position_eval(chess_board, board_hash, ctx, move_evals);
        node.score = std::get<0>(move_evals)
        node.scorecp =  static_cast<int>(node.score * 1000);
        node.hash = board_hash.hash;
        return;
    }
    //Generating just capture moves is pointless because we won't know if position is terminal or not
    //we must generate all legal moves to find it out
    //later we can filter capture moves by masking legal moves with opponent's occupations and adding legal en passant captures if any
    if (chess_board.isMate) {
      node.score = -1.0; // Current side lost
      node.scorecp = -MATE_SCORE; // Current side lost
      node.hash = board_hash.hash;
      return;
    } else if (chess_board.isStaleMate) {
      node.score = 0.0; // Current side lost
      node.scorecp = 0; // Current side lost
      node.hash = board_hash.hash;
      return;
    }
    uint64_t any = chess_board.side[chess_board.sideToMove];
    Move move = {};
    while (any) {
        move.src = lsBit(any);
        uint64_t moves = movesFromSquares[move.src];
        if (only_captures) { //mask 'moves' with opponent pieces and add en passant if any plus checks
          moves &= chess_board.side[chess_board.sideToMove ^ 1]; 
      		//plus legal en passant pawn captures if any
    			if (PC_TYPE(chess_board.piecesOnSquares[move.src]) == Pawn) {
		  			const Square ep = enPassantMoveLegal(chess_board, move.src);
      			if (ep != SquareNone) moves |= (1ULL << ep);
          }
        } //end of if (captures_only)
        while (moves) {
            move.dst = lsBit(moves);
            PieceType startP = PieceTypeNone, endP = PieceTypeNone;
            if (promoMove(chess_board, move)) { startP = Knight; endP = Queen; }
            for (move.promoType = startP; move.promoType <= endP; move.promoType = (PieceType)(move.promoType + 1)) {
                ZobristHash tmp_hash = board_hash;
                StateInfo state = {};
                DirtyPiece dp;
                updateHash(tmp_hash, chess_board, move, do_move_dp(chess_board, move, state, dp), z);
                accumulator_stack_push(ctx, dp);
                //MovesContext mctx;
                //uint64_t movesFromSquares2[64] = {0}; //can't reuse movesFromSquares!
                //generateMoves(chess_board, mctx, getAttackedSquares(chess_board, mctx), movesFromSquares2);    
                auto [eval, move_idx, hash] = position_eval(chess_board, tmp_hash, ctx);
                node.children.emplace_back();
                MiniNode& child = node.children.back();
                child.move = move_idx > 0 ? move_idx : (move.promoType << 12) | (move.src << 6) | move.dst;
                child.hash = hash != 0 ? hash : tmp_hash.hash;
                eval_recursive(chess_board, tmp_hash, ctx, child, depth + 1);
                undo_move(chess_board, move, state);
                accumulator_stack_pop(ctx);
                moves &= moves - 1;
            }
        } //end of while(moves)
        any &= any - 1;
    } //end of while(any)
    auto [eval, move_idx, hash] = position_eval(chess_board, board_hash, ctx);
    node.score = eval;
    node.move = move_idx;
    node.scorecp =  static_cast<int>(eval * 1000);
    node.hash = hash != 0 ? hash : board_hash.hash;
}

//called from mcts_search() and process_check()
//calls make_move()
//computes and returns move_evals tuple given chess_board and movesFromSquares
void compute_move_evals(Board& chess_board, ZobristHash& board_hash, NNUEContext& ctx, 
                        std::vector<std::tuple<double, int, int, uint64_t>>& move_evals) {
    MiniNode node = {};
    eval_recursive(chess_board, board_hash, ctx, node, 0);
    if (!node.children.empty()) {
      for (auto& child : node.children) {
        move_evals.emplace_back();
        auto& move_eval = move_evals.back();
        std::tie(std::get<0>(move_eval), std::get<1>(move_eval), std::get<2>(move_eval), std::get<3>(move_eval)) = resolve_negamax(child);
        //auto [eval, move, cp, hash] = resolve_negamax(child);
        //move_evals.push_back({eval, move, cp, hash});
      }
    }
    get_prob(move_evals);
}

int main(int argc, char ** argv) {
  Board board = {};
  ZobristHash zh = {};
  NNUEContext ctx;
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
	init_magic_bitboards();
  zobristHash(z);
	if (fen2board(board, fen)) {
		printf("test_nnue error: fen2board() failed; FEN %s\n", fen);
		return 1;
	}
  getHash(zh, board, z);
  //init_nnue("nn-1111cefa1111.nnue", "nn-37f18f62d772.nnue");
  init_nnue("nn-1c0000000000.nnue", "nn-37f18f62d772.nnue");
	init_nnue_context(ctx);
  accumulator_stack_reset(ctx);
  std::vector<std::tuple<double, int, int, unsigned long long>> move_evals; //res, move_idx, scorecp, hash
  position_eval(board, zh, ctx, move_evals); //we need to evaluate the position once for incremental eval to work
  if (!board.isMate && !board.isStaleMate) {
    compute_move_evals(board, zh, ctx, move_evals);
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
  cleanup_nnue();
  cleanup_magic_bitboards();
  return 0;
}
