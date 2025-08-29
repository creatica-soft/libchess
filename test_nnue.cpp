// compile with c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -Wl,-lchess,-rpath,/Users/ap/libchess -L /Users/ap/libchess -o test_nnue test_nnue.cpp
#include "nnue/types.h"
#include "nnue/position.h"
#include "nnue/evaluate.h"
#include "nnue/nnue/nnue_common.h"
#include "nnue/nnue/network.h"
#include "nnue/nnue/nnue_accumulator.h"
#include "nnue/nnue/nnue_architecture.h"
#include "nnue/nnue/features/half_ka_v2_hm.h"
#include <vector>
#include <random>
#include <chrono>
#include <math.h>
#include "libchess.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROBABILITY_MASS 0.50

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
  
  int get_prob(std::vector<std::pair<double, int>>& move_evals, double probability_mass) {
    if (move_evals.empty()) return 0;
    double min_val = move_evals[0].first;
    for (const auto& ev : move_evals) {
        if (ev.first < min_val) min_val = ev.first;
    }
    min_val = (min_val < 0) ? -min_val : 0.0;
    double total = 0.0;
    for (auto& ev : move_evals) {
        ev.first += min_val;
        total += ev.first;
    }
    if (total == 0.0) {
        double uniform = 1.0 / move_evals.size();
        for (auto& ev : move_evals) ev.first = uniform;
        return move_evals.size();
    }
    double cum_mass = 0.0;
    int effective = 0;
    for (auto& ev : move_evals) {
        ev.first /= total;  // Normalize to prob
        cum_mass += ev.first;
        ++effective;
        if (cum_mass >= probability_mass) break;
    }
    return effective;
  }
  
  char * idx_to_move(struct Board * chess_board, int move_idx, char * uci_move) {
    uci_move[0] = 0;
    if (!uci_move) {
      fprintf(stderr, "idx_to_move() error: invalid arg - uci_move is NULL\n");
      return NULL;
    }
    div_t move = div(move_idx, 64);
    enum SquareName source_square = (enum SquareName)move.quot;
    enum SquareName destination_square = (enum SquareName)move.rem;
    strncat(uci_move, squareName[source_square], 2);
    strncat(uci_move, squareName[destination_square], 2);
    if (chess_board) {
      bool promo_move = promoMove(chess_board, source_square, destination_square);
      if (promo_move) {
        uci_move[4] = 'q';
        uci_move[5] = 0;
      }
    }
    return uci_move;
  }
  
  double process_check(struct Board * chess_board, struct Move * move, struct NNUEContext * ctx) {
    struct Board * temp_board = cloneBoard(chess_board);
    move->chessBoard = temp_board;
    makeMove(move);
    double best_value = temp_board->isMate ? -0.01 * MATE_SCORE : -INFINITY;
  	enum PieceName side = (enum PieceName)((temp_board->fen->sideToMove << 3) | PieceTypeAny);//either PieceNameWhite or PieceNameBlack
  	unsigned long long any = temp_board->occupations[side]; 
  	while (any) {
  	  int src = __builtin_ctzl(any);
  	  unsigned long long moves = temp_board->sideToMoveMoves[src];
  	  while (moves) {
    	  int dst = __builtin_ctzl(moves);
        init_move(move, temp_board, src, dst);
        double res = evaluate_nnue(temp_board, move, ctx);
        if (res > best_value) best_value = res;
  		  moves &= moves - 1;
  		}
      any &= any - 1;
    }
    move->chessBoard = chess_board;
    freeBoard(temp_board);
    return -best_value;
  }
#ifdef __cplusplus
}
#endif

int main(int argc, char ** argv) {
  struct Board board;
  struct Fen fen;
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
	init_magic_bitboards();
	if (strtofen(&fen, fenString)) {
		printf("test_nnue error: strtofen() failed; FEN %s\n", fenString);
		return 1;
	}
	if (fentoboard(&fen, &board)) {
		printf("test_nnue error: fentoboard() failed; FEN %s\n", fen.fenString);
		return 1;
	}
  //init_nnue("nn-1111cefa1111.nnue", "nn-37f18f62d772.nnue");
  init_nnue("nn-1c0000000000.nnue", "nn-37f18f62d772.nnue");
	init_nnue_context(&ctx);
	
  int num_moves = 0;
  double result = 0;
  if (board.isCheck) {
      result = NNUE_CHECK; //NNUE cannot evaluate when in check
  } else if (board.isMate) { //sim_board->fen->sideToMove is mated
      result = -1;
			printf("checkmate for %s, fen %s\n", color[board.fen->sideToMove], board.fen->fenString);
  } else if (board.isStaleMate) {
      result = 0.0;        
			printf("stalemate for %s, fen %s\n", color[board.fen->sideToMove], board.fen->fenString);
  } else {
    result = evaluate_nnue(&board, NULL, &ctx); //to reset accumulators and get eval for this node
  }
  std::vector<std::pair<double, int>> move_evals;
  enum PieceName side = (enum PieceName)((board.fen->sideToMove << 3) | PieceTypeAny);//either PieceNameWhite or PieceNameBlack
	unsigned long long any = board.occupations[side]; 
	while (any) {
	  int src = __builtin_ctzl(any);
	  unsigned long long moves = board.sideToMoveMoves[src];
	  while (moves) {
  	  int dst = __builtin_ctzl(moves);
      init_move(&move, &board, src, dst);
      //evaluate_nnue() returns result in pawns (not centipawns!)
      double res = evaluate_nnue(&board, &move, &ctx);
      if (res == NNUE_CHECK) //we need to resolve the check to get NNUE score
        res = process_check(&board, &move, &ctx);
      std::cout << "res of move " << squareName[src] << squareName[dst] << " in NNUE eval " << res << std::endl; 
      move_evals.push_back({res, src * 64 + dst});
		  moves &= moves - 1;
		  num_moves++;
		}
    any &= any - 1;
  }
  std::sort(move_evals.begin(), move_evals.end(), std::greater<>());
  if (result == NNUE_CHECK) result = move_evals[0].first;
  
  int effective_branching = get_prob(move_evals, PROBABILITY_MASS);

  std::cout << "Outcome " << result << " (" << tanh(result / 4.0) << ") for " << color[board.fen->sideToMove] << std::endl;
  for (int i = 0; i < effective_branching; i++) {
    char uci_move[6] = "";
    idx_to_move(&board, move_evals[i].second, uci_move);
    std::cout << uci_move << " (" << move_evals[i].first * 100 << "%)" << std::endl;
  }
  
  move_evals.clear();	
  free_nnue_context(&ctx);
  cleanup_nnue();
  cleanup_magic_bitboards();
  return 0;
}









