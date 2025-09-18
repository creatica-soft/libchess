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
#include <random>
#include <algorithm>
#include <chrono>
#include <math.h>
#include "tbprobe.h"
#include "libchess.h"


#ifdef __cplusplus
extern "C" {
#endif

#define SYZYGY_PATH "/Users/ap/syzygy"
#define PROBABILITY_MASS 0.90
#define NOISE 0.15
#define EVAL_SCALE 6.0

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
double noise = NOISE;
std::mt19937 rng(std::random_device{}());

  int get_prob(std::vector<std::pair<double, int>>& move_evals, double probability_mass) {
    if (move_evals.empty()) return 0;
    double min_val = INFINITY;
    for (auto& ev : move_evals) {
      if (ev.first < min_val) min_val = ev.first;
    }
    min_val = (min_val < 0) ? -min_val + 1.0 : 1.0;
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
  //returns the evaluation from the perspective of chess_board->fen->sideToMove
  double process_check(struct Board * chess_board, struct Move * move, struct NNUEContext * ctx) {
    struct Board * temp_board;
    if (move) {
      temp_board = cloneBoard(chess_board);
      move->chessBoard = temp_board;
      makeMove(move); //if move is made, the perspective is changed to chess_board->opponentColor!
    } else temp_board = chess_board;
    double best_value = -INFINITY;
    if (!temp_board->isMate) {
    	enum PieceName side = (enum PieceName)((temp_board->fen->sideToMove << 3) | PieceTypeAny);//either PieceNameWhite or PieceNameBlack
    	unsigned long long any = temp_board->occupations[side]; 
    	struct Move m;
    	std::uniform_real_distribution<double> uniform(-noise, noise);
    	while (any) {
    	  int src = lsBit(any);
    	  unsigned long long moves = temp_board->sideToMoveMoves[src];
    	  while (moves) {
      	  int dst = lsBit(moves);
          init_move(&m, temp_board, src, dst);
          double res = evaluate_nnue(temp_board, &m, ctx);
          res += res * uniform(rng);
          if (res > best_value) best_value = res;
    		  moves &= moves - 1;
    		}
        any &= any - 1;
      }
    } else best_value = -0.01 * MATE_SCORE;
    if (move) {
      move->chessBoard = chess_board;
      freeBoard(temp_board);
      best_value = -best_value; //to preserve the perspective of chess_board->fen->sideToMove
    }
    return best_value;
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
  tb_init(SYZYGY_PATH);
  if (TB_LARGEST == 0) {
      fprintf(stderr, "info string error unable to initialize tablebase; no tablebase files found in %s\n", SYZYGY_PATH);
  } else {
    fprintf(stdout, "info string successfully initialized tablebases in %s. Max number of pieces %d\n", SYZYGY_PATH, TB_LARGEST);
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
	//std::mt19937 rng(std::random_device()());
	//rng.seed(static_cast<unsigned int>(std::random_device{}()));
  double result = 0;
  if (board.isCheck) {
      result = NNUE_CHECK; //NNUE cannot evaluate when in check - it will be resolved in expansion
  } else if (board.isMate) { //board.fen->sideToMove is mated
    result = -1.0;
  } else if (board.isStaleMate) {
    result = 0.0;        
  } else {
    //evaluate_nnue() returns result in pawns (not centipawns!) from board.fen->sideToMove perspective
    result = evaluate_nnue(&board, NULL, &ctx);
    result = tanh(result / EVAL_SCALE);
  }
  int effective_branching = 1;
  std::vector<std::pair<double, int>> move_evals;
  if (!board.isMate && !board.isStaleMate) {
    int src, dst;
    double res;
  	enum PieceName side = (enum PieceName)((board.fen->sideToMove << 3) | PieceTypeAny);//either PieceNameWhite or PieceNameBlack
  	unsigned long long any = board.occupations[side]; 
  	std::uniform_real_distribution<double> uniform(-noise, noise);
  	while (any) {
  	  src = lsBit(any);
  	  unsigned long long moves = board.sideToMoveMoves[src];
  	  while (moves) {
  	    dst = lsBit(moves);
      	struct Board * tmp_board = cloneBoard(&board);
        init_move(&move, tmp_board, src, dst);
        makeMove(&move); //perspective is changed to board.opponentColor - negate the eval to preserve perspective!
      	int pieceCount = bitCount(tmp_board->occupations[PieceNameAny]);
        if (pieceCount > TB_LARGEST || tmp_board->fen->halfmoveClock || tmp_board->fen->castlingRights) {
          //evaluate_nnue() returns result in pawns (not centipawns!)
          //stockfish makes the move, so the res is from the perspective of board.opponentColor
          if (tmp_board->isMate) res = MATE_SCORE * 0.01;
          else if (tmp_board->isCheck) res = -process_check(tmp_board, NULL, &ctx);
          else res = -evaluate_nnue(tmp_board, NULL, &ctx);
        } else { //pieceCount <= TB_LARGEST, etc
          unsigned int ep = lsBit(tmp_board->fen->enPassantLegalBit);
          unsigned int wdl = tb_probe_wdl(tmp_board->occupations[PieceNameWhite], tmp_board->occupations[PieceNameBlack], tmp_board->occupations[WhiteKing] | tmp_board->occupations[BlackKing],
            tmp_board->occupations[WhiteQueen] | tmp_board->occupations[BlackQueen], tmp_board->occupations[WhiteRook] | tmp_board->occupations[BlackRook], tmp_board->occupations[WhiteBishop] | tmp_board->occupations[BlackBishop], tmp_board->occupations[WhiteKnight] | tmp_board->occupations[BlackKnight], tmp_board->occupations[WhitePawn] | tmp_board->occupations[BlackPawn],
            0, 0, ep == 64 ? 0 : ep, tmp_board->opponentColor == ColorBlack ? 1 : 0);
          if (res == TB_RESULT_FAILED) {
            fprintf(stderr, "error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, occupations %u, fen %s, ep %u, halfmoveClock %u, whiteToMove %u, whites %llu, blacks %llu, kings %llu, queens %llu, rooks %llu, bishops %llu, knights %llu, pawns %llu, err %s\n", TB_LARGEST, pieceCount, tmp_board->fen->fenString, ep, tmp_board->fen->halfmoveClock, tmp_board->opponentColor == ColorBlack ? 1 : 0, tmp_board->occupations[PieceNameWhite], tmp_board->occupations[PieceNameBlack], tmp_board->occupations[WhiteKing] | tmp_board->occupations[BlackKing], tmp_board->occupations[WhiteQueen] | tmp_board->occupations[BlackQueen], tmp_board->occupations[WhiteRook] | tmp_board->occupations[BlackRook], tmp_board->occupations[WhiteBishop] | tmp_board->occupations[BlackBishop], tmp_board->occupations[WhiteKnight] | tmp_board->occupations[BlackKnight], tmp_board->occupations[WhitePawn] | tmp_board->occupations[BlackPawn], strerror(errno));
            if (tmp_board->isMate) res = MATE_SCORE * 0.01;
            else if (tmp_board->isCheck) res = -process_check(tmp_board, NULL, &ctx);
            else res = -evaluate_nnue(tmp_board, NULL, &ctx);
          } else { //tb_probe_wdl() succeeded
            //0 - loss, 4 - win, 1..3 - draw
            if (wdl == 4) res = -MATE_SCORE * 0.001;
            else if (wdl == 0) res = MATE_SCORE * 0.001;
            else res = 0.0;
          }
        } //end of else (pieceCount <= TB_LARGEST)
        printf("%s%s %.4f\n", squareName[src], squareName[dst], res);
        res += res * uniform(rng);
        move_evals.push_back({res, src * 64 + dst});
  		  moves &= moves - 1;
        freeBoard(tmp_board);
  	  } //end of while(moves)
      any &= any - 1;
    } //end of while(any)
    std::sort(move_evals.begin(), move_evals.end(), std::greater<>()); //sorted in descending order
    if (result == NNUE_CHECK)
      result = tanh(move_evals[0].first / EVAL_SCALE);
    effective_branching = get_prob(move_evals, PROBABILITY_MASS);
    std::cout << "Outcome " << result << " for " << color[board.fen->sideToMove] << std::endl;
    for (int i = 0; i < effective_branching; i++) {
      char uci_move[6] = "";
      idx_to_move(&board, move_evals[i].second, uci_move);
      std::cout << uci_move << " (" << move_evals[i].first * 100 << "%)" << std::endl;
    }
  } else if (board.isMate) 
    printf("%s is mated\n", color[board.fen->sideToMove]);
  else printf("stalemate, %s has no moves\n", color[board.fen->sideToMove]);

  move_evals.clear();	
  free_nnue_context(&ctx);
  cleanup_nnue();
  cleanup_magic_bitboards();
  return 0;
}
