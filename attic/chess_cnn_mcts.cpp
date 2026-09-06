//c++ -std=c++17 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -O3 -I /Users/ap/Downloads/libtorch/include -I /Users/ap/Downloads/libtorch/include/torch/csrc/api/include -I /Users/ap/libchess -L /Users/ap/Downloads/libtorch/lib -L /Users/ap/libchess -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/Users/ap/Downloads/libtorch/lib,-rpath,/Users/ap/libchess chess_cnn_mcts.cpp uci.cpp chess_cnn9.cpp tbcore.c tbprobe.c -o chess_cnn_mcts

#include "nnue/types.h"
#include "nnue/position.h"
#include "nnue/evaluate.h"
#include "nnue/nnue/nnue_common.h"
#include "nnue/nnue/network.h"
#include "nnue/nnue/nnue_accumulator.h"

#include "uthash.h"
#include <torch/torch.h>
#include <vector>
#include <random>
//#include <time.h>
#include <chrono>
#include "libchess.h"
#include "chess_cnn9.h"

#include <math.h>

extern "C" {

struct NNUEContext {
    Stockfish::StateInfo * state;
    Stockfish::Position * pos;
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;
};
float evaluate_nnue(struct Board * board, struct Move * move, struct NNUEContext * ctx);
float evaluate_nnue_incremental(struct Board * board, struct Move * move, struct NNUEContext * ctx);

#define MAX_BATCH_SIZE 100
#define PATH_LEN 100000 //max depth
#define EXPLORATION_CONSTANT 1.0 //smaller value favor exploitation, i.e. deeper tree vs wider tree
#define BRANCHING 64 //smaller values increase depth

// Global stop flag for MCTS interruption
const int num_channels = 87;
extern volatile int stopFlag;
extern int logfile;
extern struct Engine chessEngine;
extern torch::Device device;
extern ChessCNN model;

  // MCTS Node
  struct MCTSNode {
    uint64_t zobrist;  // Zobrist hash
    uint32_t N;        // Visit count - number of games that have reached this node
    float W;           // Total value W is a sum of loses (-1) and wins (1) for these N games
                       // Q = W / N
    float P;           // Prior probability - model move_probs for a given move in the node
    int move; //index from 0 to 4095, uci move src_sq = move / 64, dst_sq = move % 64
    struct MCTSNode ** children; // Array of child pointers
    struct MCTSNode * parent; // Pointer to parent node
    int num_children;
    int sideToMove;
    UT_hash_handle hh; // Hash handle
  };
  
  struct MCTSNode * tree = NULL;
    
  //Predictor + Upper Confidence Bound applied to Trees - used in select_child()
  float puct_score(struct MCTSNode * parent, struct MCTSNode * child) {
    float Q = child->N ? child->W / child->N : 0.0;
    return Q + EXPLORATION_CONSTANT * child->P * sqrtf((float)parent->N) / (1.0 + child->N);
  }
  
  int legalMovesCount(struct Board * board) {
    unsigned long any = board->occupations[(board->fen->sideToMove << 3) | PieceTypeAny];
    int legal_move_count = 0;
    while (any) {
      enum SquareName sn = (enum SquareName)lsBit(any);
      legal_move_count += __builtin_popcountl(board->sideToMoveMoves[sn]);
      any &= any - 1;
    }
    return legal_move_count;
  }
  
  bool promoMove(struct Board * board, enum SquareName source_square, enum SquareName destination_square) {
    if ((board->piecesOnSquares[source_square] & 7) == Pawn) {
      int src_rank = source_square / 8;
      int dst_rank = destination_square / 8;
      int pre_promo_rank = board->fen->sideToMove == ColorWhite ? Rank7 : Rank2;
      int promo_rank = board->fen->sideToMove == ColorWhite ? Rank8 : Rank1;
      if (src_rank == pre_promo_rank && dst_rank == promo_rank) return true;
    }
    return false;
  }
  
  void idx_to_move(struct Board * board, int move_idx, char * uci_move) {
    if (!uci_move) {
      dprintf(logfile, "idx_to_move() error: invalid arg - uci_move is NULL\n");
      fprintf(stderr, "idx_to_move() error: invalid arg - uci_move is NULL\n");
      return;
    }
    uci_move[0] = '\0';
    div_t move = div(move_idx, 64);
    enum SquareName source_square = (enum SquareName)move.quot;
    enum SquareName destination_square = (enum SquareName)move.rem;
    strncat(uci_move, squareName[source_square], 2);
    strncat(uci_move, squareName[destination_square], 2);
    if (board) {
      bool promo_move = promoMove(board, source_square, destination_square);
      if (promo_move) {
        uci_move[4] = 'q';
        uci_move[5] = '\0';
      }
    }
  }
  
  void expand_node(struct MCTSNode * node, struct Board * board, float * move_probs, int * moves, int num_moves) {
      char uci_move[6];
      node->children = (struct MCTSNode **)malloc(num_moves * sizeof(struct MCTSNode *));
      node->num_children = num_moves;
  
      for (int i = 0; i < num_moves; i++) {
          struct MCTSNode * child = (struct MCTSNode *)malloc(sizeof(struct MCTSNode));
          child->move = moves[i]; // Store the move (e.g., 796)
          child->N = 0;
          child->W = 0.0;
          child->P = move_probs[i];
          child->children = NULL;
          child->num_children = 0;
          child->parent = node;
  
          // Compute child’s board state
          struct Board * temp_board = cloneBoard(board);
          if (!temp_board) {
            dprintf(logfile, "expand_node() error: cloneBoard() returned NULL\n");
            fprintf(stderr, "expand_node() error: cloneBoard() returned NULL\n");
    				exit(-1);
          }
          idx_to_move(temp_board, child->move, uci_move);
          struct Move move;
          //fprintf(stderr, "expand_node(): calling initMove(%s)\n", uci_move);
      		if (initMove(&move, temp_board, uci_move)) {
      			dprintf(logfile, "expand_node() error: invalid move %u%s%s (%s); FEN %s\n", move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
      			fprintf(stderr, "expand_node() error: invalid move %u%s%s (%s); FEN %s\n", move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
      			exit(-1);
      		}

          makeMove(&move);
    			if (updateHash(temp_board->zh, temp_board, &move)) {
    				dprintf(logfile, "expand_node() error: updateHash() returned non-zero value\n");
    				fprintf(stderr, "expand_node() error: updateHash() returned non-zero value\n");
    				exit(-1);
    			}
          // Set child's zobrist hash before adding to tree
          child->zobrist = temp_board->hash;
          child->sideToMove = temp_board->fen->sideToMove;
          
          // Add to tree
          HASH_ADD(hh, tree, zobrist, sizeof(uint64_t), child);
          node->children[i] = child;
  				//dprintf(logfile, "expand_node(): added hash for %s: move %s, parent %s\n", color[child->sideToMove], uci_move, color[child->parent->sideToMove]);
  				//fprintf(stderr, "expand_node(): added hash for %s: move %s, parent %s\n", color[child->sideToMove], uci_move, color[child->parent->sideToMove]);
            
          // Free temp_board for next child
          freeBoard(temp_board);
      }
  }
  
  float select_child(struct MCTSNode * node, struct MCTSNode ** selected) {
      if (node->num_children == 0) {
          *selected = NULL;
          dprintf(logfile, "select_child(): parent %p has 0 children\n", node);
          fprintf(stderr, "select_child(): parent %p has 0 children\n", node);
          return 0;
      }
  
      float best_score = -INFINITY;
      int best_idx = 0;
      int ties = 0;
      float scores[node->num_children];
  
      // Compute scores
      for (int i = 0; i < node->num_children; i++) {
          scores[i] = puct_score(node, node->children[i]);
          if (scores[i] > best_score) {
              best_score = scores[i];
              best_idx = i;
              ties = 1;
          } else if (scores[i] >= best_score * 0.99) { // Near-tie (within 1%)
              ties++;
          }
      }
  
      // Randomly select among near-ties
      if (ties > 1) {
          //dprintf(logfile, "select_child(): randomly selecting among %d near ties\n", ties);
          //fprintf(stderr, "select_child(): randomly selecting among %d near ties\n", ties);
          int choice = rand() % ties;
          int count = 0;
          for (int i = 0; i < node->num_children; i++) {
              if (scores[i] >= best_score * 0.99) {
                  if (count == choice) {
                      best_idx = i;
                      best_score = scores[i];
                      break;
                  }
                  count++;
              }
          }
      }
  
      *selected = node->children[best_idx];
      return best_score;
  }
  //returns an array of legal moves - indeces from 0 to 4095 (src_sq * 64 + dst_sq)
  int * legal_moves(struct Board * board, int * num_moves) {
    if (!board || !(board->fen)) {
      dprintf(logfile, "legal_moves() error: either arg board is NULL or board->fen is NULL\n");
      fprintf(stderr, "legal_moves() error: either arg board is NULL or board->fen is NULL\n");
      return NULL;
    }
    int * moves = NULL;
    moves = (int *)calloc(4096, sizeof(int));
    if (!moves) {
      dprintf(logfile, "legal_moves() error: calloc returned NULL: %s\n", strerror(errno));
      fprintf(stderr, "legal_moves() error: calloc returned NULL: %s\n", strerror(errno));
      return NULL;
    }	
  	* num_moves = 0;
  	enum PieceName color = (enum PieceName)((board->fen->sideToMove << 3) | PieceTypeAny);//either PieceNameWhite or PieceNameBlack
  	unsigned long any = board->occupations[color]; 
  	while (any) {
  	  enum SquareName src_sq = (enum SquareName)lsBit(any);
  	  unsigned long moves_from_sq = board->sideToMoveMoves[src_sq];
  	  while (moves_from_sq) {
    	  enum SquareName dst_sq = (enum SquareName)lsBit(moves_from_sq);
  		  moves[(*num_moves)++] = src_sq * 64 + dst_sq;
  		  moves_from_sq &= moves_from_sq - 1;
  		}
      any &= any - 1;
    }
    return moves;
  }
}

int run_inference(struct Board ** board, int samples, int * top_moves, float * top_probs, int * outcome, int branching) {
    float * board_moves = NULL;
    size_t input_size = samples * num_channels * 64; // samples, num_channels, 8x8 bitboards
    board_moves = (float *)calloc(input_size, sizeof(float));
    if (!board_moves) {
      dprintf(logfile, "run_inference() error: calloc failed to allocate board_moves: %s\n", strerror(errno));
      fprintf(stderr, "run_inference() error: calloc failed to allocate board_moves: %s\n", strerror(errno));
      exit(-1);
    }
    int res;
    for (int i = 0; i < samples; i++) {
      if ((res = boardLegalMoves(board_moves, i, num_channels, board[i]))) {
        dprintf(logfile, "run_inference() error: boardLegalMoves() return non-zero code %d\n", res);
        fprintf(stderr, "run_inference() error: boardLegalMoves() return non-zero code %d\n", res);
        exit(-1);
      }
    }
    auto boardMoves = torch::from_blob(board_moves, {samples, num_channels, 8, 8}, torch::kFloat32).to(device, false);
    auto [policy_logits, value_logits, x_legal] = model->forward(boardMoves);    
    //policy_logits.shape [samples, 4096]
    //value_logits.shape [samples, 3]
    //x_legal.shape [samples, 10, 8, 8]
    
    auto move_mask = x_legal.view({samples, -1}); // [samples, 4096]
    policy_logits.masked_fill_(move_mask == 0, -std::numeric_limits<float>::infinity());
    auto policy_probs = torch::softmax(policy_logits, 1); // [samples, 4096]
    auto top_k = policy_probs.topk(branching, 1); //std::tuple<at::Tensor, at:Tensor>
    auto top_prob = std::get<0>(top_k);
    auto top_idx = std::get<1>(top_k);

    auto outcome_prob = torch::softmax(value_logits, 1); // [samples, 3]
    auto outcome_pred = torch::argmax(outcome_prob, 1); // [samples]
    for (int i = 0; i < samples; i++) {
        int legal_moves_count = legalMovesCount(board[i]);
        //std::cerr << "Legal moves count " << legal_moves_count << std::endl;
        for (int j = 0; j < std::min(branching, legal_moves_count); j++) {
            //std::cerr << "move idx for sample " << i << ": " << top_idx[i][j].item<int64_t>() << std::endl;
            //std::cerr << "move prob for sample " << i << ": " << top_prob[i][j].item<float>() << std::endl;
            top_moves[i * branching + j] = (int)top_idx[i][j].item<int64_t>();
            top_probs[i * branching + j] = top_prob[i][j].item<float>();
        }
        outcome[i] = (int)outcome_pred[i].item<int64_t>();
        //std::cerr << "outcome prediction for sample " << i << ": " << outcome_pred[i].item<int64_t>() << std::endl;
    }
    free(board_moves);
    return 0;
}

std::vector<int> generateUniqueRandomNumbers(int n, int k) {
    // Initialize array [0, n-1]
    std::vector<int> nums(n);
    for (int i = 0; i < n; ++i) nums[i] = i;

    // Random number generator
    std::random_device rd;
    std::mt19937 gen(rd());

    // Shuffle the array
    std::shuffle(nums.begin(), nums.end(), gen);

    // Return first k elements
    nums.resize(k);
    return nums;
}

  
extern "C" {
  
  // Function to create a new Probability struct
  void get_prob(float * results, size_t size) {  
      // Find minimum value for shifting
      float min_val = results[0];
      float weights[size];
      for (size_t i = 1; i < size; ++i) {
          if (results[i] < min_val) {
              min_val = results[i];
          }
      }
  
      // Calculate weights
      float shift = (min_val < 0) ? -min_val : 0.0; // Shift by |min| if negative
      float total_weight = 0.0;
      for (size_t i = 0; i < size; ++i) {
          weights[i] = results[i] + shift;
          total_weight += weights[i];
      }
        
      // Calculate probabilities
      for (size_t i = 0; i < size; ++i) {
          results[i] = weights[i] / total_weight;
      }
  }

  void mcts_search(struct Board * board, int num_simulations, int branching, struct NNUEContext * ctx) {
    //int64_t total = 0;
    //int maxPathLen = 0;
    char uci_move[6];
    struct MCTSNode * path[PATH_LEN]; // used in backpropagation to update all the nodes 
    int path_len;
    int min_depth = PATH_LEN;
    
    for (int i = 0; i < num_simulations; i++) {
      //start from the same initial position given by board (at the root node, i.e. at the top of the tree)
      //clone the board to preserve it for subsequent iterations
      //IMPORTANT: all evaluations should be done from the perspetive of this board->fen->sideToMove color
      struct Board * sim_board = cloneBoard(board);
      if (!sim_board) {
        dprintf(logfile, "mcts_search() error: cloneBoard() returned NULL\n");
        fprintf(stderr, "mcts_search() error: cloneBoard() returned NULL\n");
        exit(-1);
      }
      
      // Selection
      //try to find the position in the hash table
      struct MCTSNode * node = NULL;
      path_len = 0;
      HASH_FIND(hh, tree, &sim_board->hash, sizeof(uint64_t), node);
      //create it if not found, this likely happens at the beginning of a game, i.e. the hash table is empty
      //so here we are creating the root node, i.e. the node at the top of the tree
      if (!node) {
        node = (struct MCTSNode *)malloc(sizeof(struct MCTSNode));
        if (!node) {
          dprintf(logfile, "mcts_search() error: malloc(MCTSNode) returned NULL\n");
          fprintf(stderr, "mcts_search() error: malloc(MCTSNode) returned NULL\n");
          exit(-1);
        }
        node->zobrist = sim_board->hash;
        node->N = 0;
        node->W = 0.0;
        node->P = 0.0;
        node->children = NULL;
        node->num_children = 0;
        node->parent = NULL;
        node->move = 0;
        node->sideToMove = sim_board->fen->sideToMove;
        HASH_ADD(hh, tree, zobrist, sizeof(uint64_t), node);
        //add new node to the path for backpropagation
        if (path_len >= PATH_LEN) {
          dprintf(logfile, "mcts_search() error: path buffer is full, increase PATH_LEN (%d), branching (%d) or exploration constant (%.2f)\n", PATH_LEN, BRANCHING, EXPLORATION_CONSTANT);
          fprintf(stderr, "mcts_search() error: path buffer is full, increase PATH_LEN (%d), branching (%d) or exploration constant (%.2f)\n", PATH_LEN, BRANCHING, EXPLORATION_CONSTANT);
          exit(1);
        } 
        path[path_len++] = node;
      }
      //iterate down the tree
      while (node && node->num_children > 0 && !sim_board->isStaleMate && !sim_board->isMate) {
        //save current node as a parent
        struct MCTSNode * parent = node;
        //return child node with the best score using PUCT (Predictor + Upper Confidence Bound)
        select_child(parent, &node);
        if (node) {
          //fprintf(stderr, "Loop over childred: selected child node %p, zobrist %llu, visits %d, score %.1f, prob %.1f\n", node, node->zobrist, node->N, node->W, node->P);
          //decode child's move (index from 0 to 4096) to uci (from sq = move // 64) (to sq = move % 64) 
          idx_to_move(sim_board, node->move, uci_move);
          //fprintf(stderr, "Loop over childred: uci_move %s, Fen %s, move_idx %d, promo %c\n", uci_move, sim_board->fen->fenString, node->move, node->promo > 0 ? pieceLetter[node->promo + 1] : '0'); 
          //init child's move
          struct Move move;
          //fprintf(stderr, "mcts_search(): calling initMove(%s)\n", uci_move);
      		if (initMove(&move, sim_board, uci_move)) {
      			dprintf(logfile, "mcts_search() error: invalid move %u%s%s (%s); FEN %s\n", move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
      			fprintf(stderr, "mcts_search() error: invalid move %u%s%s (%s); FEN %s\n", move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
      			exit(-1);
      		}
          //make the move
          makeMove(&move);
          //update Zobrist hash (it is needed so that we can call updateHash later instead of getHash)
    			if (updateHash(sim_board->zh, sim_board, &move)) {
    				dprintf(logfile, "mcts_search() error: updateHash() returned non-zero value\n");
    				fprintf(stderr, "mcts_search() error: updateHash() returned non-zero value\n");
    				exit(-1);
    			}
          //add this child node to the path for backpropagation (i.e. to update the score and visit number)
          if (path_len >= PATH_LEN) {
            dprintf(logfile, "mcts_search() error: path buffer is full, increase PATH_LEN (%d), branching (%d) or exploration constant (%.2f)\n", PATH_LEN, BRANCHING, EXPLORATION_CONSTANT);
            fprintf(stderr, "mcts_search() error: path buffer is full, increase PATH_LEN (%d), branching (%d) or exploration constant (%.2f)\n", PATH_LEN, BRANCHING, EXPLORATION_CONSTANT);
            exit(1);
          }
          path[path_len++] = node;
        } else {
          //fprintf(stderr, "mcts_search() error in loop over childred: found unexpanded child node, i.e. node without children\n");
          break;
        }
        //continue iterating down the tree until no more children or the end of the game is reached
      }
      //Here we are at the bottom of the tree or at the terminal node
      // Expansion and Evaluation - add more children - increase the length of the tree down using the model's predictions or randomly
      float result = 0;
      //int outcome = 0;
      if (!sim_board->isStaleMate && !sim_board->isMate) {
        int num_moves, * moves = legal_moves(sim_board, &num_moves);
  
        //allocate the memory for model's output
        int top_move_pred[branching];
        float top_move_probs[branching];
        float results[branching];
        int idxSet[branching];
        memset((int *)idxSet, -1, branching);
        //run the model
        //run_inference(&sim_board, 1, top_move_pred, top_move_probs, &outcome, branching);
        //fprintf(stderr, "mcts_search(): top move prediction: %d(%.1f), promo_pred %c, predicted outcome %d\n", top_move_pred[0], top_move_probs[0], promoLetter[promo_pred + 1], outcome);

        //instead of running the model, we try random moves from moves[] to see how good is the model vs random moves
        if (branching > num_moves) branching = num_moves;
        std::vector<int> idx = generateUniqueRandomNumbers(num_moves - 1, branching);
        result = evaluate_nnue(sim_board, NULL, ctx); //to reset accumulators and get eval for this node
        if (result > 2) result = board->fen->sideToMove == sim_board->fen->sideToMove ? 1 : -1;
        else if (result < -2) result = board->fen->sideToMove == sim_board->fen->sideToMove ? -1 : 1;
        else result = 0;
        for (int i = 0; i < branching; i++) {
          top_move_pred[i] = moves[idx[i]];
          struct Move move;
          char uci_move[6];
          idx_to_move(sim_board, top_move_pred[i], uci_move);
      		if (initMove(&move, sim_board, uci_move)) {
      			dprintf(logfile, "mcts_search() error: invalid move %u%s%s (%s); FEN %s\n", move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
      			fprintf(stderr, "mcts_search() error: invalid move %u%s%s (%s); FEN %s\n", move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
      			exit(-1);
      		}          
          float res = evaluate_nnue(sim_board, &move, ctx); //hopefully, NNUE updates are incremental
          results[i] = board->fen->sideToMove == sim_board->fen->sideToMove ? res : -res;
        }
        get_prob(results, branching); //scaled down to [0..1] and sum to 1

        //result = (float)outcome;
        //total += outcome;
          
        if (!node || node->num_children == 0) {
          //if we found unexpanded child, i.e. select_child(parent, &node) returned NULL in node, i.e. a node without children
          //basically we reached the bottom of the tree, let's expand it further down
          if (!node) {
            node = (struct MCTSNode *)malloc(sizeof(struct MCTSNode));
            node->zobrist = sim_board->hash;
            node->N = 0;
            node->W = 0.0;
            node->P = 0.0; 
            node->move = 0;
            node->children = NULL;
            node->num_children = 0;
            node->sideToMove = sim_board->fen->sideToMove;
            node->parent = path[path_len - 1]; // Set parent
            HASH_ADD(hh, tree, zobrist, sizeof(uint64_t), node);
            if (path_len >= PATH_LEN) {
              dprintf(logfile, "mcts_search() error: path buffer is full, increase PATH_LEN (%d), branching (%d) or exploration constant (%.2f)\n", PATH_LEN, BRANCHING, EXPLORATION_CONSTANT);
              fprintf(stderr, "mcts_search() error: path buffer is full, increase PATH_LEN (%d), branching (%d) or exploration constant (%.2f)\n", PATH_LEN, BRANCHING, EXPLORATION_CONSTANT);
              exit(-1);
            }
            path[path_len++] = node;
          }
          //fprintf(stderr, "mcts_search() warning in tree expansion: added hash %llu because select_child(parent, &node) returned NULL\n", node->zobrist);
          //int legal_moves_count = legalMovesCount(sim_board);
          int legal_moves_count = num_moves;
          if (!legal_moves_count) {
            dprintf(logfile, "mcts_search() error: legal_moves_count is 0 but not end game!\n");
            fprintf(stderr, "mcts_search() error: legal_moves_count is 0 but not end game!\n");
            exit(-1);
          }
          if (legal_moves_count > branching) legal_moves_count = branching;
          //add children to a new or empty node
          expand_node(node, sim_board, top_move_probs, top_move_pred, legal_moves_count > branching ? branching : legal_moves_count);
          //fprintf(stderr, "Expansion: expanded node %p, zobrist %llu with %d children\n", node, node->zobrist, legal_moves_count > branching ? branching : legal_moves_count);
        }                 
      } else { //end of game
        result = 0.0;
        if (sim_board->isMate) //sideToMove is mated
          result = -1;
        //fprintf(stderr, "result %.1f\n", result);
        //total++;
      }
      // Backpropagation: update node visits and results
      //if (path_len > maxPathLen) maxPathLen = path_len;
      for (int j = path_len - 1; j >= 0; j--) { //starting from the current node moving back all the way to the root node
        path[j]->N++;
        //we need to alternate results because they are looked at from the node's perspective: 
        //positive - winning, negative - losing regardless of the color (white or black)
        //and nodes alternate such as if parent node is white, then its children are black and vice versa        
        //path[j]->W += path[j]->sideToMove == sim_board->fen->sideToMove ? result : -result;
        path[j]->W += result; //we only look at evals from a single perspective of the board->fen->sideToMove
        //fprintf(stderr, "Backpropagation: updated node %p, zobrist %llu: visits %d, results %.1f\n", path[j], path[j]->zobrist, path[j]->N, path[j]->W);         
      }
      freeBoard(sim_board);
      if (chessEngine.seldepth < path_len) chessEngine.seldepth = path_len;   
      if (min_depth > path_len) min_depth = path_len;   
    } //end of the iterations loop
    if (chessEngine.depth && min_depth >= chessEngine.depth) {
        stopFlag = 1;
    }
    //fprintf(stderr, "total terminal positions %lld, maxPathLen %d\n", total, maxPathLen);
  }
  
  void mcts_search_batched(struct Board * board, int num_simulations, int branching, struct NNUEContext * ctx) {
      struct MCTSNode * path[MAX_BATCH_SIZE * PATH_LEN];
      char uci_move[6];
      int path_len;
      struct Board * sim_boards[MAX_BATCH_SIZE];
      int top_move_pred[MAX_BATCH_SIZE * branching];
      float top_move_probs[MAX_BATCH_SIZE * branching];
      //int promo_pred[MAX_BATCH_SIZE];
      int outcomes[MAX_BATCH_SIZE];
      int batch_positions = 0;
      struct MCTSNode * batch_nodes[MAX_BATCH_SIZE];
      int batch_path_lens[MAX_BATCH_SIZE];
      unsigned int * visited_leaves = (unsigned int *)malloc(num_simulations * sizeof(unsigned int)); // Track leaves
      int visited_count = 0;
  
      for (int i = 0; i < num_simulations; i++) {
          struct Board * sim_board = cloneBoard(board);
          if (!sim_board) {
              dprintf(logfile, "mcts_search_batched() error: cloneBoard() returned NULL\n");
              fprintf(stderr, "mcts_search_batched() error: cloneBoard() returned NULL\n");
              exit(-1);
          }
  
          // Selection
          struct MCTSNode * node = NULL;
          path_len = 0;
          HASH_FIND(hh, tree, &sim_board->hash, sizeof(uint64_t), node);
          if (!node) {
              node = (struct MCTSNode *)malloc(sizeof(struct MCTSNode));
              if (!node) {
                  dprintf(logfile, "mcts_search_batched() error: malloc(MCTSNode) returned NULL\n");
                  fprintf(stderr, "mcts_search_batched() error: malloc(MCTSNode) returned NULL\n");
                  exit(-1);
              }
              node->zobrist = sim_board->hash;
              node->N = 0;
              node->W = 0.0;
              node->P = 0.0;
              node->children = NULL;
              node->num_children = 0;
              node->parent = NULL;
              node->move = 0;
              HASH_ADD(hh, tree, zobrist, sizeof(uint64_t), node);
              if (path_len >= PATH_LEN) {
                dprintf(logfile, "mcts_search_batched() error: path buffer is full, increase PATH_LEN (%d), branching (%d) or exploration constant (%.2f)\n", PATH_LEN, BRANCHING, EXPLORATION_CONSTANT);
                fprintf(stderr, "mcts_search_batched() error: path buffer is full, increase PATH_LEN (%d), branching (%d) or exploration constant (%.2f)\n", PATH_LEN, BRANCHING, EXPLORATION_CONSTANT);
                exit(-1);
              }
              path[path_len++] = node;
          }
  
          // Traverse until end of branch (no more children) or terminal state
          while (node && node->num_children > 0 && !sim_board->isStaleMate && !sim_board->isMate) {
              struct MCTSNode * parent = node;
              select_child(parent, &node);
              if (node) {
                  idx_to_move(sim_board, node->move, uci_move);
                  struct Move move;
                  //fprintf(stderr, "mcts_search_batched(): tree traversal, calling initMove(%s)\n", uci_move);
              		if (initMove(&move, sim_board, uci_move)) {
              			dprintf(logfile, "mcts_search_batched() error: invalid move %u%s%s (%s); FEN %s\n", move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
              			fprintf(stderr, "mcts_search_batched() error: invalid move %u%s%s (%s); FEN %s\n", move.chessBoard->fen->moveNumber, move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ", move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
              			exit(-1);
              		}
                  makeMove(&move);
                  if (updateHash(sim_board->zh, sim_board, &move)) {
                      dprintf(logfile, "mcts_search_batched() error: updateHash() returned non-zero value\n");
                      fprintf(stderr, "mcts_search_batched() error: updateHash() returned non-zero value\n");
                      exit(-1);
                  }
                  if (path_len >= PATH_LEN) {
                      dprintf(logfile, "mcts_search_batched() error: path buffer is full, increase PATH_LEN (%d), branching (%d) or exploration constant (%.2f)\n", PATH_LEN, BRANCHING, EXPLORATION_CONSTANT);
                      fprintf(stderr, "mcts_search_batched() error: path buffer is full, increase PATH_LEN (%d), branching (%d) or exploration constant (%.2f)\n", PATH_LEN, BRANCHING, EXPLORATION_CONSTANT);
                      exit(-1);
                  }
                  path[path_len++] = node;
              } else { //no more children
                  break; //while (node && node->num_children > 0 && !sim_board->isStaleMate && !sim_board->isMate)
              }
          } //end of while (node && node->num_children > 0 && !sim_board->isStaleMate && !sim_board->isMate)
  
          // Check if leaf is unique
          int is_unique = 1;
          for (int j = 0; j < visited_count; j++) {
              if (node && node->zobrist == visited_leaves[j]) {
                  is_unique = 0;
                  break;
              }
          }
  
          // Queue for batch if leaf and unique
          if (!sim_board->isStaleMate && !sim_board->isMate && is_unique) {
  
              sim_boards[batch_positions] = sim_board;
              batch_nodes[batch_positions] = node;
              batch_path_lens[batch_positions] = path_len;
              memcpy(path + PATH_LEN * batch_positions, path, path_len * sizeof(struct MCTSNode *));
              visited_leaves[visited_count++] = node ? node->zobrist : sim_board->hash;
              batch_positions++;
  
              // Run inference if batch is full
              if (batch_positions == MAX_BATCH_SIZE) {
  
                  if (run_inference(sim_boards, batch_positions, top_move_pred, top_move_probs, outcomes, branching)) {
                      dprintf(logfile, "mcts_search_batched() error: run_inference() failed\n");
                      fprintf(stderr, "mcts_search_batched() error: run_inference() failed\n");
                      exit(-1);
                  }
                  //fprintf(stderr, "mcts_search_batch(): top move prediction: %d(%.1f), promo %c, predicted outcome %d\n", top_move_pred[0], top_move_probs[0], promoLetter[promo_pred[0] + 1], outcomes[0]);
  
                  // Process batch
                  for (int b = 0; b < batch_positions; b++) {
                      node = batch_nodes[b];
                      sim_board = sim_boards[b];
                      path_len = batch_path_lens[b];
                      memcpy(path, path + PATH_LEN * b, path_len * sizeof(struct MCTSNode *));
  
                      if (!node || node->num_children == 0) {
                          if (!node) {
                              node = (struct MCTSNode *)malloc(sizeof(struct MCTSNode));
                              node->zobrist = sim_board->hash;
                              node->N = 0;
                              node->W = 0.0;
                              node->P = 0.0;
                              //node->promo = sim_board->promoPiece;
                              node->children = NULL;
                              node->num_children = 0;
                              node->parent = path[path_len - 1];
                              node->move = 0;
                              HASH_ADD(hh, tree, zobrist, sizeof(uint64_t), node);
                              path[path_len++] = node;
                          }
                          int legal_moves_count = legalMovesCount(sim_board);
                          if (!legal_moves_count) {
                              dprintf(logfile, "mcts_search_batched() error: legal_moves_count is 0 but not end game!\n");
                              fprintf(stderr, "mcts_search_batched() error: legal_moves_count is 0 but not end game!\n");
                              exit(-1);
                          }
                          if (legal_moves_count > branching) legal_moves_count = branching;
                          expand_node(node, sim_board, &top_move_probs[b * branching], &top_move_pred[b * branching], legal_moves_count > branching ? branching : legal_moves_count);
                          //fprintf(stderr, "Expansion: expanded node %p, zobrist %llu with %d children\n", node, node->zobrist, legal_moves_count > branching ? branching : legal_moves_count);
                      }
                      
                      //float result = (float)outcomes[b] - 1.0;
                      float result = evaluate_nnue(sim_board, NULL, ctx);
                      if (result > 2) result = board->fen->sideToMove == sim_board->fen->sideToMove ? 1 : -1;
                      else if (result < -2) result = board->fen->sideToMove == sim_board->fen->sideToMove ? -1 : 1;
                      else result = 0;        
                      for (int j = path_len - 1; j >= 0; j--) {
                          path[j]->N++;
                          //path[j]->W += path[j]->sideToMove == sim_board->fen->sideToMove ? result : -result;
                          path[j]->W += result;
                      }
                      freeBoard(sim_board);
                  }
                  batch_positions = 0;
                  visited_count = 0; // Reset for next batch
              } //end of if (batch_positions == MAX_BATCH_SIZE)
          } else { //if !(!sim_board->isStaleMate && !sim_board->isMate && is_unique)
              // Terminal node or duplicate leaf
              if (sim_board->isStaleMate || sim_board->isMate) {
                  float result = sim_board->isMate ? (board->fen->sideToMove == sim_board->fen->sideToMove ? -1 : 1) : 0;
                  for (int j = path_len - 1; j >= 0; j--) {
                      path[j]->N++;
                      //path[j]->W += path[j]->sideToMove == sim_board->fen->sideToMove ? result : -result;
                      path[j]->W += result;
                  }
              }
              freeBoard(sim_board);
          }
      } //end of for (int i = 0; i < num_simulations; i++)
  
      // Process remaining incomplete batch
      if (batch_positions > 0) {
  
          if (run_inference(sim_boards, batch_positions, top_move_pred, top_move_probs, outcomes, branching)) {
              dprintf(logfile, "mcts_search_batched() error: run_inference() failed\n");
              fprintf(stderr, "mcts_search_batched() error: run_inference() failed\n");
              exit(-1);
          }
          //fprintf(stderr, "mcts_search_batch(): top move prediction: %d(%.1f), predicted outcome %d\n", top_move_pred[0], top_move_probs[0], outcomes[0]);
 
          for (int b = 0; b < batch_positions; b++) {
              struct MCTSNode * node = batch_nodes[b];
              struct Board * sim_board = sim_boards[b];
              path_len = batch_path_lens[b];
              memcpy(path, path + PATH_LEN * b, path_len * sizeof(struct MCTSNode *));
  
              if (!node || node->num_children == 0) {
                  if (!node) {
                      node = (struct MCTSNode *)malloc(sizeof(struct MCTSNode));
                      node->zobrist = sim_board->hash;
                      node->N = 0;
                      node->W = 0.0;
                      node->P = 0.0;
                      //node->promo = sim_board->promoPiece;
                      node->children = NULL;
                      node->num_children = 0;
                      node->parent = path[path_len - 1];
                      node->move = -1;
                      HASH_ADD(hh, tree, zobrist, sizeof(uint64_t), node);
                      if (path_len >= PATH_LEN) {
                        dprintf(logfile, "mcts_search_batched() error: path buffer is full, increase PATH_LEN (%d), branching (%d) or exploration constant (%.2f)\n", PATH_LEN, BRANCHING, EXPLORATION_CONSTANT);
                        fprintf(stderr, "mcts_search_batched() error: path buffer is full, increase PATH_LEN (%d), branching (%d) or exploration constant (%.2f)\n", PATH_LEN, BRANCHING, EXPLORATION_CONSTANT);
                        exit(-1);
                      }
                      path[path_len++] = node;
                  }
                  int legal_moves_count = legalMovesCount(sim_board);
                  if (!legal_moves_count) {
                      dprintf(logfile, "mcts_search_batched() error: legal_moves_count is 0 but not end game!\n");
                      fprintf(stderr, "mcts_search_batched() error: legal_moves_count is 0 but not end game!\n");
                      exit(-1);
                  }
                  if (legal_moves_count > branching) legal_moves_count = branching;
                  expand_node(node, sim_board, &top_move_probs[b * branching], &top_move_pred[b * branching], legal_moves_count > branching ? branching : legal_moves_count);
                  //fprintf(stderr, "Expansion: expanded node %p, zobrist %llu with %d children\n", node, node->zobrist, legal_moves_count > branching ? branching : legal_moves_count);
             }
              
              //float result = (float)outcomes[b] - 1.0;
              float result = evaluate_nnue(sim_board, NULL, ctx);
              if (result > 2) result = board->fen->sideToMove == sim_board->fen->sideToMove ? 1 : -1;
              else if (result < -2) result = board->fen->sideToMove == sim_board->fen->sideToMove ? -1 : 1;
              else result = 0;
              
              for (int j = path_len - 1; j >= 0; j--) {
                  path[j]->N++;
                  //path[j]->W += path[j]->sideToMove == sim_board->fen->sideToMove ? result : -result;
                  path[j]->W += result;
              }
              freeBoard(sim_board);
          }
      }
      free(visited_leaves);
  }
  
  float select_best_move(struct Board * board, char * uciMove) {
    struct MCTSNode * node = NULL;
    HASH_FIND(hh, tree, &board->hash, sizeof(uint64_t), node);
    if (!node) {
      dprintf(logfile, "select_best_move() error: HASH_FIND returned NULL\n");
      fprintf(stderr, "select_best_move() error: HASH_FIND returned NULL\n");
      exit(-1);
    } 
    struct MCTSNode * parent = node;
    node = NULL;
    float best_score = select_child(parent, &node);
    if (!node) {
      //I think to avoid NULL node condition, the number of iterations in MCTSrun should be odd, not even
      dprintf(logfile, "select_best_move() warning: select_child() returned NULL\n");
      fprintf(stderr, "select_best_move() warning: select_child() returned NULL\n");
      //return NULL;
      if (parent->parent->parent) { //parent->parent is the opponent's move
        dprintf(logfile, "select_best_move() notice: select_child() returned NULL but parent->parent is not NULL\n");
        fprintf(stderr, "select_best_move() notice: select_child() returned NULL but parent->parent is not NULL\n");
        parent = parent->parent;
        idx_to_move(board, parent->parent->move, uciMove);
      } else {
        dprintf(logfile, "select_best_move() error: select_child() returned NULL and parent->parent is also NULL\n");
        fprintf(stderr, "select_best_move() error: select_child() returned NULL and parent->parent is also NULL\n");
        exit(-1);      
      }
    } else idx_to_move(board, node->move, uciMove);
    return best_score;
  }
  
  void free_mcts_node(struct MCTSNode ** table, struct MCTSNode * node) {
    if (!node) return;
    char uci_move[6];

    // Recursively free children
    if (node->children) {
        for (int i = 0; i < node->num_children; i++) {
            if (node->children[i]) {
                if (node->children[i]->move > 0) idx_to_move(NULL, node->children[i]->move, uci_move);
                else uci_move[0] = '\0';
                //dprintf(logfile, "hash %llu, visits %d, result %.1f, Q %.5f, probs %.5f, move %s, sideToMove %s\n", node->children[i]->zobrist, node->children[i]->N, node->children[i]->W, node->children[i]->N > 0 ? node->children[i]->W / node->children[i]->N : 0, node->children[i]->P, uci_move, color[node->sideToMove]);

                free_mcts_node(table, node->children[i]);
                node->children[i] = NULL;
            }
        }
        free(node->children);
        node->children = NULL;
    }

    // Remove from hash table and free node
    HASH_DEL(*table, node);
    free(node);
  }
  
  void free_hash_table_top(struct MCTSNode ** table, struct Board * board) {
    struct MCTSNode * node = NULL, * parent;

    // Find the node corresponding to the board's Zobrist hash
    HASH_FIND(hh, * table, &board->hash, sizeof(uint64_t), node);
    if (!node || !node->parent) return; // No parent to free

    parent = node->parent;

    // Free all sibling nodes (children of parent except node)
    if (parent->children) {
        for (int i = 0; i < parent->num_children; i++) {
            if (parent->children[i] && parent->children[i] != node) {
                free_mcts_node(table, parent->children[i]);
                parent->children[i] = NULL;
            }
        }
    }

    // Free the parent node
    free_mcts_node(table, parent);
    node->parent = NULL; // Make current node the root
  }
  
  void free_hash_table(struct MCTSNode **table) {
    struct MCTSNode *node = NULL, *tmp;
    char uci_move[6];

    // Iterate over all nodes
    HASH_ITER(hh, *table, node, tmp) {
        // Log node information
        if (node->move > 0) idx_to_move(NULL, node->move, uci_move);
        else uci_move[0] = '\0';
        //dprintf(logfile, "hash %llu, visits %d, result %.1f, Q %.5f, probs %.5f, move %s, sideToMove %s\n", node->zobrist, node->N, node->W, node->N > 0 ? node->W / node->N : 0, node->P, uci_move, color[node->sideToMove]);

        // Remove and free the node
        tmp = (struct MCTSNode *)node->hh.next; // Save next node before deletion
        free_mcts_node(table, node);
        node = NULL;
    }

    *table = NULL;
  }
    
  int neuralEvaluate(struct Board * board, char * uciMove, struct NNUEContext * ctx) {
    int res = 0;
    //number of moves to try at each node - the greater branching
    //the shorter tree or ply depth; the formular for number of simulation is depth ^ branching;
    //for example, for if depth is 10 and branching is 3, the number of simulation is 1000.
    //but if branching is 2, 900 simulations will reach depth of 30 plies
    //another words, it's a tradeoff between exploration and depth
    int branching = 2;
    const int max_pieces = 8;
  
    if (__builtin_popcountl(board->occupations[PieceNameAny]) > max_pieces) {
      int result = 0;
      int top_move_pred[branching];
      float top_move_probs[branching];
      run_inference(&board, 1, top_move_pred, top_move_probs, &result, branching);
      idx_to_move(board, top_move_pred[0], uciMove);
    } else {
      // Main MCTS loop
      const int NUM_SIMULATIONS = 1001;
      //while (!board->isMate && !board->isStaleMate) {
        // Run MCTS simulations. Results are stored in uthash tree
        mcts_search(board, NUM_SIMULATIONS, branching, ctx);
        
        // Select best move from uthash tree
        select_best_move(board, uciMove);
    }
    return res;
  }
  
  void neuralEvaluateDirect(struct Board * board, char * uciMove) {
      int result = 0;
      int top_move_pred = 0;
      //int promo_pred = 0;
      float top_move_probs = 0.0;
      run_inference(&board, 1, &top_move_pred, &top_move_probs, &result, 1);
      idx_to_move(board, top_move_pred, uciMove);
  }

  void cleanup() {
      if (tree) {
        free_hash_table(&tree);
        tree = NULL;
      }
  }

  int runMCTS(struct Board * board, double maxTime, char * uciMove, struct NNUEContext * ctx) {
      int res = 0;
      time_t start = time(NULL); //sec
      double elapsed = 0.0;
      int iterations = MIN_ITERATIONS;
      int branching = BRANCHING;
      
      stopFlag = 0;
      while (elapsed < maxTime * 0.001 && !stopFlag && iterations < MAX_ITERATIONS) {
          auto iter_start = std::chrono::steady_clock::now();
          //mcts_search_batched(board, iterations, branching, ctx);
          mcts_search(board, iterations, branching, ctx);
          float score = select_best_move(board, uciMove); 
          elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
          dprintf(logfile, "info multipv 1 score cp %d depth %.0f seldepth %d nodes %d nps %.0f pv %s\n", (int)(score * 100), round(sqrtf(iterations)), chessEngine.seldepth, iterations, round((double)iterations / elapsed), uciMove);
          printf("info multipv 1 score cp %d depth %.0f nodes %d nps %.0f pv %s\n", (int)(score * 100), round(sqrtf(iterations)), iterations, round((double)iterations / elapsed), uciMove);
          fflush(stdout);
          int iter = (maxTime * 0.001 / elapsed - 1) * iterations / 10;
          if (iter % 2 == 0)
            iterations += iter;
          else iterations += iter + 1;
      }
      
      free_hash_table_top(&tree, board);
  
      return res;
  }
  
}