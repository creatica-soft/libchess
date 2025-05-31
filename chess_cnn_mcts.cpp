//c++ -Wno-deprecated-declarations -Wno-deprecated -O3 -I /Users/ap/Downloads/libtorch/include -I /Users/ap/Downloads/libtorch/include/torch/csrc/api/include -I /Users/ap/libchess -L /Users/ap/Downloads/libtorch/lib -L /Users/ap/libchess -std=c++17 -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/Users/ap/Downloads/libtorch/lib,-rpath,/Users/ap/libchess chess_cnn_mcts.cpp uci.cpp chess_cnn6.cpp tbcore.c tbprobe.c -o chess_cnn_mcts
/*
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
*/
#include "uthash.h"
#include "libchess.h"
#include "chess_cnn6.h"
#include <torch/torch.h>
#include <vector>
#include <random>

#define MAX_BATCH_SIZE 1024
#define PATH_LEN 1000
#define UCB_C 1.4 // Exploration constant

// Global stop flag for MCTS interruption
const int num_channels = 128;
extern volatile int stopFlag;
extern int logfile;
extern torch::Device device;
extern ChessCNN model;

extern "C" {
  // MCTS Node
  struct MCTSNode {
    uint64_t zobrist;  // Zobrist hash
    uint32_t N;        // Visit count - number of games that have reached this node
    float W;           // Total value W is a sum of loses (-1) and wins (1) for these N games
                       // Q = W / N
    float P;           // Prior probability - model move_probs for a given move in the node
    //unsigned char promo; //promotion: 0 - none, 1 - knight, 2 - bishop, 3 - rook, 4 - queen
    int move; //index from 0 to 4095, uci move srcsqr = move / 64, dstsqr = move % 64
    struct MCTSNode ** children; // Array of child pointers
    struct MCTSNode * parent; // Pointer to parent node
    int num_children;
    UT_hash_handle hh; // Hash handle
  };
  
  struct MCTSNode * tree = NULL;
  
  
  //Predictor + Upper Confidence Bound applied to Trees - used in select_child()
  
  float puct_score(struct MCTSNode * parent, struct MCTSNode * child) {
    const float c = 1.0; // Exploration constant, smaller values favor exploitation
    float Q = child->N ? child->W / child->N : 0.5;
    return Q + c * child->P * sqrtf((float)parent->N) / (1.0 + child->N);
  }
  /*
  float puct_score(struct MCTSNode * parent, struct MCTSNode * child) {
      if (child->N == 0) return INFINITY; // Prioritize unvisited
      float exploit = child->W / child->N;
      float explore = UCB_C * sqrtf(logf(parent->N + 1) / child->N);
      return exploit + explore * child->P;
  }*/
  
  int legalMovesCount(struct Board * board) {
    unsigned long any = board->occupations[(board->fen->sideToMove << 3) | PieceTypeAny];
    int legal_move_count = 0;
    while (any) {
      enum SquareName sn = (enum SquareName)__builtin_ctzl(any);
      legal_move_count += __builtin_popcountl(board->sideToMoveMoves[sn]);
      any &= any - 1;
    }
    return legal_move_count;
  }
  
  bool promoMove(struct Board * board, enum SquareName source_square, enum SquareName destination_square) {
    if ((board->piecesOnSquares[source_square] & 7) == Pawn) {
      if ((source_square >= SquareA7 && source_square <= SquareH7 && destination_square >= SquareA8 && destination_square <= SquareH8) || (source_square >= SquareA2 && source_square <= SquareH2 && destination_square >= SquareA1 && destination_square <= SquareH1))
      return true;
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
        //if (promo) uci_move[4] = tolower(promoLetter[promo]); 
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
          /*bool promo_move = promoMove(board, (enum SquareName)(moves[i] / 64), (enum SquareName)(moves[i] % 64));
          if (promo_move) child->promo = promo;
          else child->promo = 0;*/
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
          initMove(&move, temp_board, uci_move);
          makeMove(&move);
    			if (updateHash(temp_board->zh, temp_board, &move)) {
    				dprintf(logfile, "expand_node() error: updateHash() returned non-zero value\n");
    				fprintf(stderr, "expand_node() error: updateHash() returned non-zero value\n");
    				exit(-1);
    			}
          // Set child's zobrist hash before adding to tree
          child->zobrist = temp_board->hash;
          
          // Add to tree
          HASH_ADD(hh, tree, zobrist, sizeof(uint64_t), child);
          node->children[i] = child;
  
          // Free temp_board for next child
          freeBoard(temp_board);
      }
  }
  
  // Select child using UCB1 with randomization for ties
  /*
  void select_child(struct MCTSNode * parent, struct MCTSNode ** node) {
    if (node->num_children == 0) {
      *selected = NULL;
      dprintf(logfile, "select_child(): parent %p has 0 children\n", node);
      fprintf(stderr, "select_child(): parent %p has 0 children\n", node);
      return 0;
    }
    float max_ucb = -INFINITY;
    struct MCTSNode * best_child = NULL;
    int ties = 0;
    float ucb_values[parent->num_children];
    
    for (int i = 0; i < parent->num_children; i++) {
      struct MCTSNode * child = &parent->children[i];
      float ucb;
      if (child->N == 0) {
        ucb = INFINITY;
      } else {
        ucb = (child->W / child->N) + UCB_C * sqrtf(logf(parent->N + 1) / child->N);
      }
      ucb_values[i] = ucb;
      if (ucb > max_ucb) {
        max_ucb = ucb;
        best_child = child;
        ties = 1;
      } else if (ucb == max_ucb) {
        ties++;
      }
    }
    
    // Random tie-breaking
    if (ties > 1) {
      int choice = rand() % ties;
      int count = 0;
      for (int i = 0; i < parent->num_children; i++) {
        if (ucb_values[i] == max_ucb) {
          if (count == choice) {
            best_child = &parent->children[i];
            break;
          }
          count++;
        }
      }
    }
    *node = best_child;
  }
  */
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
          if (node->children[i]->zobrist == 0) { // Unexpanded child
              *selected = NULL;
              dprintf(logfile, "select_child(): found unexpanded child node %p, index %d\n", node, i);
              fprintf(stderr, "select_child(): found unexpanded child node %p, index %d\n", node, i);
              return 0;
          }
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
  //returns an array of legal moves - indeces from 0 to 4095 (srcsqr * 64 + dstsqr)
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
  	*num_moves = 0;
  	enum PieceName color = (enum PieceName)((board->fen->sideToMove << 3) | PieceTypeAny);//either PieceNameWhite or PieceNameBlack
  	unsigned long any = board->occupations[color]; 
  	while (any) {
  	  enum SquareName src_sq = (enum SquareName)__builtin_ctzl(any);
  	  unsigned long moves_from_sq = board->sideToMoveMoves[src_sq];
  	  while (moves_from_sq) {
    	  enum SquareName dst_sq = (enum SquareName)__builtin_ctzl(moves_from_sq);
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
    //enum GameStage * stages = NULL;
    size_t input_size = samples * num_channels * 8 * 8; // samples, num_channels, 8x8 bitboards
    board_moves = (float *)calloc(input_size, sizeof(float));
    if (!board_moves) {
      dprintf(logfile, "run_inference() error: calloc failed to allocate board_moves: %s\n", strerror(errno));
      fprintf(stderr, "run_inference() error: calloc failed to allocate board_moves: %s\n", strerror(errno));
      exit(-1);
    }
    /*
    stages = (enum GameStage *)calloc(samples, sizeof(enum GameStage));
    if (!stages) {
      dprintf(logfile, "run_inference() error: calloc failed to allocate stages: %s\n", strerror(errno));
      fprintf(stderr, "run_inference() error: calloc failed to allocate stages: %s\n", strerror(errno));
      exit(-1);
    }*/

    int res;
    for (int i = 0; i < samples; i++) {
      if ((res = boardLegalMoves(board_moves, i, num_channels, board[i]))) {
        dprintf(logfile, "run_inference() error: boardLegalMoves() return non-zero code %d\n", res);
        fprintf(stderr, "run_inference() error: boardLegalMoves() return non-zero code %d\n", res);
        exit(-1);
      }
      //stages[i] = getStage(board[i]);
    }

    //auto stage = torch::from_blob(stages, {samples}, torch::kInt32).to(device, false);
    auto boardMoves = torch::from_blob(board_moves, {samples, num_channels, 8, 8}, torch::kFloat32).to(device, false);

    //fprintf(stderr, "run_inference(): calling model->forward()...\n");    
    auto [moves_logits, value_logits, x_legal] = model->forward(boardMoves);
    //fprintf(stderr, "run_inference(): calling model->forward()...done\n");    
    //std::cerr << "moves_logits_endgame:\n" << moves_logits_endgame << std::endl;
    
    auto legal_moves = x_legal.view({boardMoves.size(0), -1});
    //std::cerr << "legal_moves:\n" << legal_moves << std::endl;

    auto top_move_prob = torch::empty({samples, branching}).to(device, torch::kFloat32);
    //auto top_move_prob_middlegame = torch::empty({samples, branching}).to(device, torch::kFloat32);
    //auto top_move_prob_endgame = torch::empty({samples, branching}).to(device, torch::kFloat32);
    auto move_prob = torch::empty({samples, 4096}).to(device, torch::kFloat32);
    //auto move_prob_middlegame = torch::empty({samples, 4096}).to(device, torch::kFloat32);
    //auto move_prob_endgame = torch::empty({samples, 4096}).to(device, torch::kFloat32);
    auto move_pred = torch::empty({samples, branching}).to(device, torch::kInt32);
    //auto move_pred_middlegame = torch::empty({samples, branching}).to(device, torch::kInt32);
    //auto move_pred_endgame = torch::empty({samples, branching}).to(device, torch::kInt32);
    auto outcome_pred = torch::empty({samples}).to(device, torch::kInt32);
    //auto outcome_pred_middlegame = torch::empty({samples}).to(device, torch::kInt32);
    //auto outcome_pred_endgame = torch::empty({samples}).to(device, torch::kInt32);
    /*
    std::vector<torch::Tensor> moves_outputs = {moves_logits_opening, moves_logits_middlegame, moves_logits_endgame};
    std::vector<torch::Tensor> value_outputs = {value_logits_opening, value_logits_middlegame, value_logits_endgame};
    std::vector<torch::Tensor> top_move_probs = {top_move_prob_opening, top_move_prob_middlegame, top_move_prob_endgame};
    std::vector<torch::Tensor> move_probs = {move_prob_opening, move_prob_middlegame, move_prob_endgame};
    std::vector<torch::Tensor> move_pred = {move_pred_opening, move_pred_middlegame, move_pred_endgame};
    std::vector<torch::Tensor> outcome_pred = {outcome_pred_opening, outcome_pred_middlegame, outcome_pred_endgame};
    */
    //fprintf(stderr, "run_inference(): processing stages...\n");
    //for (int s = 0; s < 3; ++s) {
    //    auto mask_int = (stage == s);
        //std::cerr << "mask_int:\n" << mask_int << std::endl;
    //    auto mask = mask_int.to(torch::kFloat32);
        //std::cerr << "mask:\n" << mask << std::endl;
    //    auto mask_sum = mask.sum().item<float>(); //number of samples for stage s
    //    if (mask_sum > 0) {
            //Completely suppress illegal moves
            moves_logits.index_put_({legal_moves == 0}, -std::numeric_limits<float>::infinity());
            //std::cerr << "moves_logits:\n" << moves_logits << std::endl;
            move_prob = torch::softmax(moves_logits, 1);
            //std::cerr << "move_prob:\n" << move_prob << std::endl;
            auto topk_result = torch::topk(move_prob, branching, 1);
            //std::cerr << "topk_result:\n" << topk_result << std::endl;
            top_move_prob = std::get<0>(topk_result);
            //std::cerr << "top_move_prob:\n" << top_move_prob << std::endl;
            move_pred = std::get<1>(topk_result);
            //std::cerr << "move_pred:\n" << move_pred << std::endl;
            outcome_pred = value_logits.argmax(1);
            //std::cerr << "outcome_pred:\n" << outcome_pred << std::endl;
    //    }
    //}
    //fprintf(stderr, "run_inference(): processing stages...done\n");
    //fprintf(stderr, "run_inference(): copying the results...\n");
    for (int i = 0; i < samples; i++) {
        for (int j = 0; j < branching; j++) {
          top_moves[i * branching + j] = move_pred.index({i, j}).item<int>();
          top_probs[i * branching + j] = top_move_prob.index({i, j}).item<float>();
        }
        outcome[i] = outcome_pred.index({i}).item<int>();
    }
    //fprintf(stderr, "run_inference(): copying the results...done\n");
    
    free(board_moves);
    //free(stages);
    return 0;
}

extern "C" {
  void mcts_search(struct Board * board, int num_simulations, int branching) {
    //int64_t total = 0;
    //int maxPathLen = 0;
    char uci_move[6];
    struct MCTSNode * path[PATH_LEN]; // used in backpropagation to update all the nodes 
    int path_len;
    
    for (int i = 0; i < num_simulations; i++) {
      //start from the same initial position given by board
      //clone the board to preserve it for subsequent iterations
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
      //create it if not found
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
        //node->promo = sim_board->promoPiece;
        node->children = NULL;
        node->num_children = 0;
        node->parent = NULL;
        node->move = 0;
        HASH_ADD(hh, tree, zobrist, sizeof(uint64_t), node);
        //add existing or new node to the path for backpropagation
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
          initMove(&move, sim_board, uci_move);
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
            dprintf(logfile, "mcts_search() error: path buffer is full, pathLen %d\n", PATH_LEN);
            fprintf(stderr, "mcts_search() error: path buffer is full, pathLen %d\n", PATH_LEN);
            exit(1);
          }
          path[path_len++] = node;
        } else {
          //fprintf(stderr, "mcts_search() error in loop over childred: found unexpanded child node\n");
          break;
        }
        //continue iterating down the tree until no more children or the end of the game is reached
      }
      
      // Expansion and Evaluation - add more children - increase the length of the tree down using the model's predictions
      float result = 0;
      int outcome = 0;
      if (!sim_board->isStaleMate && !sim_board->isMate) {
        //int num_moves, *moves = legal_moves(sim_board, &num_moves);
  
        //allocate the memory for model's output
        int top_move_pred[branching];
        float top_move_probs[branching];
        //int promo_pred = 0;
        //run the model
        run_inference(&sim_board, 1, top_move_pred, top_move_probs, &outcome, branching);
        //fprintf(stderr, "mcts_search(): top move prediction: %d(%.1f), promo_pred %c, predicted outcome %d\n", top_move_pred[0], top_move_probs[0], promoLetter[promo_pred + 1], outcome);
        //result = (float)outcome;
        //total += outcome;
        //if (sim_board->fen->sideToMove == ColorBlack) {
          //result = -result; // Flip for Black’s perspective
        //}
          
        if (!node || node->num_children == 0) {
          //if we found unexpanded child, i.e. select_child(parent, &node) returned NULL in node
          if (!node) {
            node = (struct MCTSNode *)malloc(sizeof(struct MCTSNode));
            node->zobrist = sim_board->hash;
            node->N = 0;
            node->W = 0.0;
            node->P = 0.0; 
            //node->promo = sim_board->promoPiece;
            node->move = 0;
            node->children = NULL;
            node->num_children = 0;
            node->parent = path[path_len - 1]; // Set parent
            HASH_ADD(hh, tree, zobrist, sizeof(uint64_t), node);
            if (path_len >= PATH_LEN) {
              dprintf(logfile, "mcts_search() error: path buffer is full, pathLen %d\n", PATH_LEN);
              fprintf(stderr, "mcts_search() error: path buffer is full, pathLen %d\n", PATH_LEN);
              exit(-1);
            }
            path[path_len++] = node;
          }
          //fprintf(stderr, "mcts_search() warning in tree expansion: added hash %llu because select_child(parent, &node) returned NULL\n", node->zobrist);
          //instead of using all legal moves, we only take top 3 from the model prediction or less 
          //if legal moves less than 3
          int legal_moves_count = legalMovesCount(sim_board);
          if (!legal_moves_count) {            
            dprintf(logfile, "mcts_search() error: legal_moves_count is 0 but not end game!\n");
            fprintf(stderr, "mcts_search() error: legal_moves_count is 0 but not end game!\n");
            exit(-1);
          }
          if (legal_moves_count > branching) legal_moves_count = branching;
          //add children to a new or empty node
          expand_node(node, sim_board, top_move_probs, top_move_pred, legal_moves_count > branching ? branching : legal_moves_count);
          //fprintf(stderr, "Expansion: expanded node %p, zobrist %llu with %d children\n", node, node->zobrist, legal_moves_count > 3 ? 3 : legal_moves_count);
        }                 
      } else { //end of game
        result = 0.0;
        if (sim_board->isMate)
          result = 1.0; //Always win from the perspective of the player for which this is a child node!
        //fprintf(stderr, "result %.1f\n", result);
        //total++;
      }
      // Backpropagation: update node visits and results
      //if (path_len > maxPathLen) maxPathLen = path_len;
      for (int j = path_len - 1; j >= 0; j--) {
        path[j]->N++;
        //we need to alternate results because they are looked at from the node's perspective: 
        //positive - winning, negative - losing regardless of the color (white or black)
        //and nodes alternate such as if parent node is white, then its children are black and vice versa
        float node_result = (path_len - 1 - j) % 2 == 0 ? result : -result;
        path[j]->W += node_result;
        //fprintf(stderr, "Backpropagation: updated node %p, zobrist %llu: visits %d, results %.1f\n", path[j], path[j]->zobrist, path[j]->N, path[j]->W);         
      }
      freeBoard(sim_board);
    }
    //fprintf(stderr, "total terminal positions %lld, maxPathLen %d\n", total, maxPathLen);
  }
  
  void mcts_search_batched(struct Board * board, int num_simulations, int branching) {
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
              //node->promo = sim_board->promoPiece;
              node->children = NULL;
              node->num_children = 0;
              node->parent = NULL;
              node->move = 0;
              HASH_ADD(hh, tree, zobrist, sizeof(uint64_t), node);
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
                  initMove(&move, sim_board, uci_move);
                  makeMove(&move);
                  if (updateHash(sim_board->zh, sim_board, &move)) {
                      dprintf(logfile, "mcts_search_batched() error: updateHash() returned non-zero value\n");
                      fprintf(stderr, "mcts_search_batched() error: updateHash() returned non-zero value\n");
                      exit(-1);
                  }
                  if (path_len >= PATH_LEN) {
                      dprintf(logfile, "mcts_search_batched() error: path buffer is full, PATH_LEN %d\n", PATH_LEN);
                      fprintf(stderr, "mcts_search_batched() error: path buffer is full, PATH_LEN %d\n", PATH_LEN);
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
                      /*
                      float result = (float)outcomes[b] - 1.0;
                      for (int j = path_len - 1; j >= 0; j--) {
                          path[j]->N++;
                          float node_result = (path_len - 1 - j) % 2 == 0 ? result : -result;
                          path[j]->W += node_result;
                      }*/
                      freeBoard(sim_board);
                  }
                  batch_positions = 0;
                  visited_count = 0; // Reset for next batch
              } //end of if (batch_positions == MAX_BATCH_SIZE)
          } else { //if !(!sim_board->isStaleMate && !sim_board->isMate && is_unique)
              // Terminal node or duplicate leaf
              if (sim_board->isStaleMate || sim_board->isMate) {
                  float result = sim_board->isMate ? 1.0 : 0.0;
                  for (int j = path_len - 1; j >= 0; j--) {
                      path[j]->N++;
                      float node_result = (path_len - 1 - j) % 2 == 0 ? result : -result;
                      path[j]->W += node_result;
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
          //fprintf(stderr, "mcts_search_batch(): top move prediction: %d(%.1f), promo %c, predicted outcome %d\n", top_move_pred[0], top_move_probs[0], promoLetter[promo_pred[0] + 1], outcomes[0]);
 
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
              /*
              float result = (float)outcomes[b] - 1.0;
              for (int j = path_len - 1; j >= 0; j--) {
                  path[j]->N++;
                  float node_result = (path_len - 1 - j) % 2 == 0 ? result : -result;
                  path[j]->W += node_result;
              }*/
              freeBoard(sim_board);
          }
      }
      free(visited_leaves);
  }
  
  float select_best_move(struct Board * board, char * uciMove) {
    /*
    int num_moves, * moves = legal_moves(board, &num_moves);
    if (!moves || num_moves == 0) return NULL;
    float best_score = -1.0;
    float epsilon = 1.0; //exploration-exploitation constant; smaller value favor exploitation
    
    for (int i = 0; i < num_moves; i++) {
      struct Board * next_board = cloneBoard(board);
      if (!next_board) {
        dprintf(logfile, "select_best_move() error: cloneBoard() returned NULL\n");
        fprintf(stderr, "select_best_move() error: cloneBoard() returned NULL\n");
        exit(-1);
      }
      struct Move move;
      idx_to_move(next_board, moves[i], uciMove);
      initMove(&move, next_board, uciMove);
      makeMove(&move);
  		if (!updateHash(next_board->zh, next_board, &move)) next_board->hash = next_board->zh->hash;
  		else {
  			dprintf(logfile, "select_best_move() error: updateHash() returned non-zero value\n");
  			fprintf(stderr, "select_best_move() error: updateHash() returned non-zero value\n");
        freeBoard(next_board);
  			exit(-1);
  		}    
      struct MCTSNode * node = NULL;
      HASH_FIND(hh, tree, &next_board->hash, sizeof(uint64_t), node);
      if (node) {
        float score = (float)(node->N); //Simply choose the node with the most number of visits
        if (score > best_score) best_score = score;
      }
      freeBoard(next_board);
    }
    free(moves);
    */
    //it appears to be better to select best_move from the hash table using PUCT, i.e. select_child()  
    struct MCTSNode * node = NULL;
    HASH_FIND(hh, tree, &board->hash, sizeof(uint64_t), node);
    if (!node) {
      dprintf(logfile, "select_best_move() error: HASH_FIND returned NULL\n");
      fprintf(stderr, "select_best_move() error: HASH_FIND returned NULL\n");
      exit(-1);
    } 
    struct MCTSNode * parent = node;
    float best_score = select_child(parent, &node);
    if (!node) {
      //I think to avoid NULL node condition, the number of iterations in MCTSrun should be odd, not even
      dprintf(logfile, "select_best_move() warning: select_child() returned NULL\n");
      fprintf(stderr, "select_best_move() warning: select_child() returned NULL\n");
      //return NULL;
      if (parent->parent) {
        dprintf(logfile, "select_best_move() notice: select_child() returned NULL but parent->parent is not NULL\n");
        fprintf(stderr, "select_best_move() notice: select_child() returned NULL but parent->parent is not NULL\n");
        parent = parent->parent;
        idx_to_move(board, parent->move, uciMove);
      } else {
        dprintf(logfile, "select_best_move() error: select_child() returned NULL and parent->parent is also NULL\n");
        fprintf(stderr, "select_best_move() error: select_child() returned NULL and parent->parent is also NULL\n");
        exit(-1);      
      }
    } else idx_to_move(board, node->move, uciMove);
    return best_score;
  }
  
  void free_hash_table(struct MCTSNode **table) {
      struct MCTSNode * node, * tmp;
      char uci_move[6];
      HASH_ITER(hh, *table, node, tmp) {
          if (node->move > 0) idx_to_move(NULL, node->move, uci_move);
          else uci_move[0] = '\0';
          dprintf(logfile, "hash %llu, visits %d, result %.1f, Q %.5f, probs %.5f, move %s\n", node->zobrist, node->N, node->W, node->N > 0 ? node->W / node->N : 0, node->P, uci_move);
          if (node->children) free(node->children);
          HASH_DEL(*table, node);
          free(node);
      }
      *table = NULL;
  }
  
  int neuralEvaluate(struct Board * board, char * uciMove) {
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
      //int promo_pred = 0;
      run_inference(&board, 1, top_move_pred, top_move_probs, &result, branching);
      //fprintf(stderr, "Evaluation: top 3 move predictions: %lld(%.1f) %lld(%.1f) %lld(%.1f), predicted outcome %lld\n", top_move_pred[0], top_move_probs[0], top_move_pred[1], top_move_probs[1], top_move_pred[2], top_move_probs[2], result);
      idx_to_move(board, top_move_pred[0], uciMove);
    } else {
      // Main MCTS loop
      const int NUM_SIMULATIONS = 1001;
      //while (!board->isMate && !board->isStaleMate) {
        // Run MCTS simulations. Results are stored in uthash tree
        mcts_search(board, NUM_SIMULATIONS, branching);
        
        // Select best move from uthash tree
        select_best_move(board, uciMove);
        //struct Move move;
        //initMove(&move, board, best_move);
        //makeMove(&move);
  			//if (!updateHash(board->zh, board, &move)) board->hash = board->zh->hash;
  			//else {
  			//	fprintf(stderr, "neuralEvaluate() error: updateHash() returned non-zero value\n");
  			//	exit(-1);
  			//}
        //fprintf(stderr, "Played move: %s\n", best_move);
        
        // Clear tree for next iteration (optional)
        //free_hash_table(&tree);
      //}
    }
  exit:
    /*
    if (tree) {
      free_hash_table(&tree);
      tree = NULL;
    }*/
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
  
  int runMCTS(struct Board * board, double maxTime, char * uciMove) {
      int res = 0;
      clock_t start = clock();
      double elapsed = 0.0;
      int iterations = 10 * MIN_ITERATIONS + 1;
      int branching = 8;
      
      stopFlag = 0;
      while (elapsed < maxTime && !stopFlag && iterations < MAX_ITERATIONS) {
          clock_t iter_start = clock();
          //fprintf(stderr, "runMCTS(): calling mcts_search_batched(board, %d, %d)...\n", iterations, branching);
          mcts_search_batched(board, iterations, branching);
          //mcts_search(board, iterations, branching);
          //fprintf(stderr, "runMCTS(): calling mcts_search_batched(board, %d, %d)...done\n", iterations, branching);
          //score must be from the engine point of view
          //fprintf(stderr, "runMCTS(): calling select_best_move()...\n");
          float score = select_best_move(board, uciMove); 
          //fprintf(stderr, "runMCTS(): calling select_best_move()...done. Bestmove %s\n", uciMove);
          double iter_elapsed_sec = (double)(clock() - iter_start) / CLOCKS_PER_SEC; //sec
          elapsed = ((double)(clock() - start)) / CLOCKS_PER_SEC * 1000; //ms
          dprintf(logfile, "info multipv 1 score cp %d depth %.0f nodes %d nps %.0f pv %s\n", (int)(score * 10000), round(sqrtf(iterations)), iterations, round((double)iterations / iter_elapsed_sec), uciMove);
          printf("info multipv 1 score cp %d depth %.0f nodes %d nps %.0f pv %s\n", (int)(score * 10000), round(sqrtf(iterations)), iterations, round((double)iterations / iter_elapsed_sec), uciMove);
          fflush(stdout);
          int iter = (maxTime / elapsed - 1) * iterations;
          if (iter % 2 == 0)
            iterations += iter;
          else iterations += iter + 1;
      }
      
  exit:
      /*
      if (tree) {
        free_hash_table(&tree);
        tree = NULL;
      }*/
      return res;
  }
  
  void cleanup() {
      if (tree) {
        free_hash_table(&tree);
        tree = NULL;
      }
  }
}