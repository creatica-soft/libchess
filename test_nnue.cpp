// compile with c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -Wl,-lchess,-rpath,/Users/ap/libchess -L /Users/ap/libchess -o test_nnue tbcore.c tbprobe.c test_nnue.cpp
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
//#include <random>
#include <algorithm>
#include <chrono>
#include <math.h>
#include "tbprobe.h"
#include "libchess.h"


#ifdef __cplusplus
extern "C" {
#endif

#define SYZYGY_PATH "/Users/ap/syzygy"
#define PROBABILITY_MASS 1.0
//#define NOISE 0.03
#define EVAL_SCALE 6.0
#define TEMPERATURE 0.6


struct NNUEContext {
    Stockfish::StateInfo * state;
    Stockfish::Position * pos;
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;    
};

void init_nnue(const char * nnue_file_big = EvalFileDefaultNameBig, const char * nnue_file_small = EvalFileDefaultNameSmall);
void cleanup_nnue();
void init_nnue_context(struct NNUEContext * ctx);
void free_nnue_context(struct NNUEContext * ctx);
double evaluate_nnue(struct Board * board, struct Move * move, struct NNUEContext * ctx);
void compute_move_evals(struct Board * chess_board, struct NNUEContext * ctx, std::vector<std::tuple<double, int, int, unsigned long long/*, unsigned long long*/>>& move_evals, double prob_mass);
//double noise = NOISE;
double temperature = TEMPERATURE;
double probability_mass = PROBABILITY_MASS;
double eval_scale = EVAL_SCALE;
//std::mt19937 rng(std::random_device{}());

int get_prob(std::vector<std::tuple<double, int, int, unsigned long long/*, unsigned long long*/>>& move_evals, double prob_mass) {
    size_t n = move_evals.size();
    if (n == 0) return 0;
    // Loop 1: Find max for stability
    double max_val = -std::numeric_limits<double>::infinity();
    for (const auto& ev : move_evals) {
        if (std::get<0>(ev) > max_val) max_val = std::get<0>(ev);
    }
    // Loop 2: Compute total sum of exp(shifted)
    double total = 0.0;
    for (const auto& ev : move_evals) {
        total += std::exp((std::get<0>(ev) - max_val)/temperature);
    }
    if (total == 0.0) {  // Rare case: all -inf or underflow
        double uniform = 1.0 / n;
        for (auto& ev : move_evals) std::get<0>(ev) = uniform;
        return static_cast<int>(n);
    }
    // Loop 3: Normalize to probs, accumulate cum_mass
    double cum_mass = 0.0;
    int effective = 0;
    for (auto& ev : move_evals) {
        std::get<0>(ev) = std::exp((std::get<0>(ev) - max_val)/temperature) / total;
        cum_mass += std::get<0>(ev);
        ++effective;
        if (cum_mass >= prob_mass) break;
    }
    return effective;
}
 
  double process_check(struct Board * temp_board, struct NNUEContext * ctx) {
    std::vector<std::tuple<double, int, int, unsigned long long/*, unsigned long long*/>> move_evals; 
    //use 1.0 for probability mass to try all moves - when in check, there shouldn't be too many moves
    compute_move_evals(temp_board, ctx, move_evals, 1.0); 
    return -std::get<2>(move_evals[0]) * 0.01;
  }

  //called from do_move() and set_root()
  //calls evaluate_nnue() and process_check()
  //returns position evaluation in pawns from chess_board->fen->sideToMove perspective
  double position_eval(struct Board * chess_board, struct NNUEContext * ctx) {
    double res;
  	const int pieceCount = bitCount(chess_board->occupations[PieceNameAny]);
  	if (pieceCount > TB_LARGEST || chess_board->fen->halfmoveClock || chess_board->fen->castlingRights) {
      //evaluate_nnue() returns result in pawns (not centipawns!)
      //we made the move above, so the eval res is from the perspective of opponent color or chess_board->fen->sideToMove
      //and must be negated to preserve the perspective of sim_board->fen->sideToMove
      generateMoves(chess_board);
      if (chess_board->isMate) res = -MATE_SCORE * 0.01; //chess_board->fen->sideToMove wins
      else if (chess_board->isStaleMate) {
        res = 0.0;
      } else if (chess_board->isCheck) {
        res = process_check(chess_board, ctx);
      } else {
        updateFen(chess_board);
        res = evaluate_nnue(chess_board, NULL, ctx);
      }
    } else { //pieceCount <= TB_LARGEST, etc
      const unsigned int ep = lsBit(enPassantLegalBit(chess_board));
      const unsigned int wdl = tb_probe_wdl(chess_board->occupations[PieceNameWhite], chess_board->occupations[PieceNameBlack], chess_board->occupations[WhiteKing] | chess_board->occupations[BlackKing],
        chess_board->occupations[WhiteQueen] | chess_board->occupations[BlackQueen], chess_board->occupations[WhiteRook] | chess_board->occupations[BlackRook], chess_board->occupations[WhiteBishop] | chess_board->occupations[BlackBishop], chess_board->occupations[WhiteKnight] | chess_board->occupations[BlackKnight], chess_board->occupations[WhitePawn] | chess_board->occupations[BlackPawn],
        0, 0, ep == 64 ? 0 : ep, OPP_COLOR(chess_board->fen->sideToMove) == ColorBlack ? 1 : 0);
      if (wdl == TB_RESULT_FAILED) {
        fprintf(stderr, "error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, occupations %u, ep %u, halfmoveClock %u, whiteToMove %u, whites %llu, blacks %llu, kings %llu, queens %llu, rooks %llu, bishops %llu, knights %llu, pawns %llu, err %s\n", TB_LARGEST, pieceCount, ep, chess_board->fen->halfmoveClock, OPP_COLOR(chess_board->fen->sideToMove) == ColorBlack ? 1 : 0, chess_board->occupations[PieceNameWhite], chess_board->occupations[PieceNameBlack], chess_board->occupations[WhiteKing] | chess_board->occupations[BlackKing], chess_board->occupations[WhiteQueen] | chess_board->occupations[BlackQueen], chess_board->occupations[WhiteRook] | chess_board->occupations[BlackRook], chess_board->occupations[WhiteBishop] | chess_board->occupations[BlackBishop], chess_board->occupations[WhiteKnight] | chess_board->occupations[BlackKnight], chess_board->occupations[WhitePawn] | chess_board->occupations[BlackPawn], strerror(errno));
        generateMoves(chess_board);
        if (chess_board->isMate) res = -MATE_SCORE * 0.01;
        else if (chess_board->isStaleMate) {
          res = 0.0; 
        } else if (chess_board->isCheck) {
          res = process_check(chess_board, ctx);
        } else {
          updateFen(chess_board);
          res = evaluate_nnue(chess_board, NULL, ctx);
        }
      } else { //tb_probe_wdl() succeeded
        //0 - loss, 4 - win, 1..3 - draw
        if (wdl == 4) res = MATE_SCORE * 0.001; //chess_board->fen->sideToMove wins, sim_board->fen->sideToMove loses
        else if (wdl == 0) res = -MATE_SCORE * 0.001;
        else res = 0.0;
      }
    } //end of else (pieceCount <= TB_LARGEST)
    return res;
  }

  double do_move(struct Board * chess_board, const int src, const int dst, const int promo, struct NNUEContext * ctx, unsigned long long& child_hash/*, unsigned long long& child_hash2*/) {
    struct Board * tmp_board = cloneBoard(chess_board);
    struct Move move;
    //init_move(&move, tmp_board, src, dst, promo);
    //make_move(&move);
    ff_move(tmp_board, &move, src, dst, promo);
    updateHash(tmp_board, &move);
    child_hash = tmp_board->zh->hash;
    //child_hash2 = tmp_board->zh->hash2;
    double res = position_eval(tmp_board, ctx);
    freeBoard(tmp_board);    
    return -res;
  }

  void compute_move_evals(struct Board * chess_board, struct NNUEContext * ctx, std::vector<std::tuple<double, int, int, unsigned long long/*, unsigned long long*/>>& move_evals, double prob_mass) {
        int src, dst;
        double res;
        //std::uniform_real_distribution<double> uniform(-noise, noise);
      	int side = PC(chess_board->fen->sideToMove, PieceTypeAny);//either PieceNameWhite or PieceNameBlack
      	unsigned long long any = chess_board->occupations[side]; 
      	while (any) {
      	  src = lsBit(any);
      	  unsigned long long moves = chess_board->movesFromSquares[src];
      	  while (moves) {
      	    dst = lsBit(moves);
      	    unsigned long long child_hash = 0; //, child_hash2 = 0;
          	int startPiece = PieceTypeNone, endPiece = PieceTypeNone;
          	if (promoMove(chess_board, src, dst)) {
          	  startPiece = Knight;
          	  endPiece = Queen;
          	}
        	  for (int pt = startPiece; pt <= endPiece; pt++) { //loop over promotions if any, Pawn means no promo
        	    res = do_move(chess_board, src, dst, pt, ctx, child_hash/*, child_hash2*/);
        	    printf("%s%s%c %.4f %llu\n", squareName[src], squareName[dst], uciPromoLetter[pt], res, child_hash);
              //res += res * uniform(rng);
              move_evals.push_back({res, (src << 9) | (dst << 3) | pt, static_cast<int>(-res * 100), child_hash/*, child_hash2*/});
        	  }
            moves &= moves - 1;
          }
          any &= any - 1;
        }
        // Sort by res descending
        std::sort(move_evals.begin(), move_evals.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b);});
        int effective_branching = get_prob(move_evals, prob_mass);
        move_evals.resize(effective_branching);
  }

#ifdef __cplusplus
}
#endif

int main(int argc, char ** argv) {
  struct Board board;
  struct Fen fen;
  struct ZobristHash zh;
	struct Move move;
  struct NNUEContext ctx;
  char fenString[MAX_FEN_STRING_LEN] = "";
  char uciMove[6] = "";
	if (argc == 1) strncpy(fenString, startPos, MAX_FEN_STRING_LEN);
	else if (argc >= 7) {
	  for (int i = 1; i < 7; i++) {
	    strcat(fenString, argv[i]);
	    strcat(fenString, " ");
	   }
	} 
  tb_init(SYZYGY_PATH);
  if (TB_LARGEST == 0) {
      fprintf(stderr, "info string error unable to initialize tablebase; no tablebase files found in %s\n", SYZYGY_PATH);
  } else {
    fprintf(stdout, "info string successfully initialized tablebases in %s. Max number of pieces %d\n", SYZYGY_PATH, TB_LARGEST);
  }
	init_magic_bitboards();
  zobristHash(&zh);
  board.zh = &zh;
	if (strtofen(&fen, fenString)) {
		printf("test_nnue error: strtofen() failed; FEN %s\n", fenString);
		return 1;
	}
	if (fentoboard(&fen, &board)) {
		printf("test_nnue error: fentoboard() failed; FEN %s\n", fen.fenString);
		return 1;
	}
  getHash(board.zh, &board);
  //init_nnue("nn-1111cefa1111.nnue", "nn-37f18f62d772.nnue");
  init_nnue("nn-1c0000000000.nnue", "nn-37f18f62d772.nnue");
	init_nnue_context(&ctx);

  generateMoves(&board); //needed for checks such as isMate or isStaleMate as well as for sim_board->movesFromSquares
  if (!board.isMate && !board.isStaleMate) {
    std::vector<std::tuple<double, int, int, unsigned long long/*, unsigned long long*/>> move_evals; //res, move_idx, scorecp, hash, hash2
    compute_move_evals(&board, &ctx, move_evals, probability_mass);
    std::cout << "Outcome " << -std::get<2>(move_evals[0]) << " for " << color[board.fen->sideToMove] << std::endl;
    int effective_branching = move_evals.size();
    for (int i = 0; i < effective_branching; i++) {
      char uci_move[6] = "";
      idx_to_move(std::get<1>(move_evals[i]), uci_move);
      std::cout << uci_move << " (" << std::get<0>(move_evals[i]) * 100 << "%)" << std::endl;
    }
  } else if (board.isMate) 
    printf("%s is mated\n", color[board.fen->sideToMove]);
  else printf("stalemate, %s has no moves\n", color[board.fen->sideToMove]);
  free_nnue_context(&ctx);
  cleanup_nnue();
  cleanup_magic_bitboards();
  return 0;
}
