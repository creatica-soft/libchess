//c++ -std=c++17 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -I /Users/ap/libchess  -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess chess_mcts.cpp uci.cpp tbcore.c tbprobe.c -o chess_mcts

#include "nnue/types.h"
#include "nnue/position.h"
#include "nnue/evaluate.h"
#include "nnue/nnue/nnue_common.h"
#include "nnue/nnue/network.h"
#include "nnue/nnue/nnue_accumulator.h"

#include "uthash.h"
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <random>
#include <chrono>
#include <math.h>
#include "libchess.h"

extern "C" {

  struct NNUEContext {
      Stockfish::StateInfo * state;
      Stockfish::Position * pos;
      Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
      Stockfish::Eval::NNUE::AccumulatorCaches * caches;    
  };
  void init_nnue_context(struct NNUEContext * ctx);
  void free_nnue_context(struct NNUEContext * ctx);
  double evaluate_nnue(struct Board * chess_board, struct Move * move, struct NNUEContext * ctx);
  
  #define EXPLORATION_CONSTANT 1.0 //smaller value favor exploitation, i.e. deeper tree vs wider tree
  #define PROBABILITY_MASS 1.0 //cumulative probability - how many moves we consider
  #define MAX_DEPTH 100
  
  // Global stop flag for MCTS interruption
  //extern volatile int stopFlag;
  extern bool stopFlag;
  extern bool quitFlag;
  extern bool searchFlag;
  extern int logfile;
  extern double timeAllocated; //ms
  extern struct Engine chessEngine;
  extern char best_move[6];
  extern struct Board * board;
  // Global generation counter
  //int current_generation = 0;

  struct Edge {
      int move;             // The move that leads to the child position
      double P;            // Prior probability - model move_probs for a given move in the node
      struct MCTSNode * child; // Pointer to the child node
  };
  // MCTS Node
  struct MCTSNode {
    unsigned long long hash;
    unsigned long long N;        // Visit count - number of games that have reached this node
    double W;           // Total value W is a sum of loses (-1) and wins (1) for these N games
                        // Q = W / N
    struct Edge * children; //array of Edges to child nodes
    int num_children;
    //int generation; //use for clearing the tree top
    UT_hash_handle hh; // Hash handle
  };
  
  struct MCTSNode * tree = NULL;
  struct MCTSNode * root = NULL;  // Points to the current root of the MCTS tree

  void free_hash_table(struct MCTSNode **table) {
    struct MCTSNode * node = NULL, * tmp;
    HASH_ITER(hh, *table, node, tmp) {
        HASH_DEL(*table, node);
        free(node->children);
        free(node);
    }
    *table = NULL;
  }
  
  void cleanup() {
      if (tree) {
        free_hash_table(&tree);
        tree = NULL;
      }
  }
    
  void set_root() {
      cleanup();
      struct MCTSNode * node = (struct MCTSNode *)calloc(1, sizeof(struct MCTSNode));
      if (!node) {
          dprintf(logfile, "get_root() error: calloc failed\n");
          exit(-1);
      }
      node->hash = board->zh->hash ^ board->zh->hash2;
      node->N = 0;
      node->W = 0;
      node->children = NULL;
      node->num_children = 0;
      //node->generation = 0;
      HASH_ADD(hh, tree, hash, sizeof(unsigned long long), node);
      root = node;
  }

  //Predictor + Upper Confidence Bound applied to Trees - used in select_best_child()
  double puct_score(struct MCTSNode * parent, int idx ) {
    double Q = parent->children[idx].child->N ? parent->children[idx].child->W / parent->children[idx].child->N : 0.0;
    return -Q + EXPLORATION_CONSTANT * parent->children[idx].P * sqrt((double)parent->N) / (1.0 + parent->children[idx].child->N);
  }

  char * idx_to_move(struct Board * chess_board, int move_idx, char * uci_move) {
    uci_move[0] = '\0';
    if (!uci_move) {
      dprintf(logfile, "idx_to_move() error: invalid arg - uci_move is NULL\n");
      fprintf(stderr, "idx_to_move() error: invalid arg - uci_move is NULL\n");
      return NULL;
    }
    div_t move = div(move_idx, 64);
    enum SquareName source_square = (enum SquareName)move.quot;
    enum SquareName destination_square = (enum SquareName)move.rem;
    strcat(uci_move, squareName[source_square]);
    strcat(uci_move, squareName[destination_square]);
    if (chess_board) {
      bool promo_move = promoMove(chess_board, source_square, destination_square);
      if (promo_move) {
        uci_move[4] = 'q';
        uci_move[5] = '\0';
      }
    }
    return uci_move;
  }
  
  void expand_node(struct MCTSNode * parent, struct Board * chess_board, double * move_probs, int * moves, int num_moves) {
      struct MCTSNode * temp_node = NULL;
      parent->children = (struct Edge *)calloc(num_moves, sizeof(struct Edge));
      parent->num_children = num_moves;

      for (int i = 0; i < num_moves; i++) {
          struct Board * temp_board = cloneBoard(chess_board);
          if (!temp_board) {
            dprintf(logfile, "expand_node() error: cloneBoard() returned NULL\n");
            fprintf(stderr, "expand_node() error: cloneBoard() returned NULL\n");
    				exit(-1);
          }
          struct Move move;
      		init_move(&move, temp_board, moves[i] / 64, moves[i] % 64);
          makeMove(&move);
    			if (updateHash(temp_board, &move)) {
    				dprintf(logfile, "expand_node() error: updateHash() returned non-zero value\n");
    				fprintf(stderr, "expand_node() error: updateHash() returned non-zero value\n");
    				exit(-1);
    			}
    			struct MCTSNode * child = NULL;
    			unsigned long long hash = temp_board->zh->hash ^ temp_board->zh->hash2;
          HASH_FIND(hh, tree, &hash, sizeof(unsigned long long), child);
          if (!child) {
            child = (struct MCTSNode *)calloc(1, sizeof(struct MCTSNode));
            if (!child) {
              dprintf(logfile, "expand_node() error: calloc returned NULL for child\n");
              fprintf(stderr, "expand_node() error: calloc returned NULL for child\n");
              free(parent->children);
      				exit(-1);
            }          
            child->hash = hash;
            child->N = 0;
            child->W = 0;
            child->children = NULL;
            child->num_children = 0;
            HASH_ADD(hh, tree, hash, sizeof(unsigned long long), child);
          } 
          parent->children[i].P = move_probs[i];
          parent->children[i].move = moves[i];
          parent->children[i].child = child;
          freeBoard(temp_board);
      }
  }
  
  void most_visited_child(struct MCTSNode * parent, int * selected, struct Board * chess_board) {
    unsigned long long maxN = 0;
    for (int i = 0; i < parent->num_children; i++) {
      //char uci_move[6];
      //idx_to_move(chess_board, parent->children[i].move, uci_move);
      //dprintf(logfile, "most_visited_child() debug: move %s, %.2f / %lld + sqrt(%lld) / %lld * %.4f * %.1f = %.5f + %.5f = %.5f\n", uci_move, -parent->children[i].child->W, parent->children[i].child->N, parent->N, parent->children[i].child->N + 1, parent->children[i].P, EXPLORATION_CONSTANT, parent->children[i].child->N > 0 ? -parent->children[i].child->W / parent->children[i].child->N : 0.0, sqrt(parent->N) / (parent->children[i].child->N + 1) * parent->children[i].P * EXPLORATION_CONSTANT, parent->children[i].child->N > 0 ? -parent->children[i].child->W / parent->children[i].child->N + sqrt(parent->N) / (parent->children[i].child->N + 1) * parent->children[i].P * EXPLORATION_CONSTANT : sqrt(parent->N) / (parent->children[i].child->N + 1) * parent->children[i].P * EXPLORATION_CONSTANT);
      if (parent->children[i].child->N > maxN) {
          maxN = parent->children[i].child->N;
          *selected = i;
      } 
    }
  }
  
  void select_best_child(struct MCTSNode * parent, int * selected, struct Board * chess_board) {
      double best_score = -INFINITY;
      double score;
      for (int i = 0; i < parent->num_children; i++) {
        //debug - comment out later
        //char uci_move[6];
        //idx_to_move(chess_board, parent->children[i].move, uci_move);
        //dprintf(logfile, "select_best_child() debug: move %s, %.2f / %lld + sqrt(%lld) / %lld * %.4f * %.1f = %.5f + %.5f = %.5f\n", uci_move, -parent->children[i].child->W, parent->children[i].child->N, parent->N, parent->children[i].child->N + 1, parent->children[i].P, EXPLORATION_CONSTANT, parent->children[i].child->N ? -parent->children[i].child->W / parent->children[i].child->N : 0.0, sqrt(parent->N) / (parent->children[i].child->N + 1) * parent->children[i].P * EXPLORATION_CONSTANT, parent->children[i].child->N ? -parent->children[i].child->W / parent->children[i].child->N + sqrt(parent->N) / (parent->children[i].child->N + 1) * parent->children[i].P * EXPLORATION_CONSTANT : sqrt(parent->N) / (parent->children[i].child->N + 1) * parent->children[i].P * EXPLORATION_CONSTANT);
        
        score = puct_score(parent, i);
        if (score > best_score) {
            best_score = score;
            *selected = i;
        } 
     }
  }
  
  int get_prob(double * values, size_t size) {
      double min = values[0];
      double total = 0.0;
      double mass = 0.0;
      for (size_t i = 1; i < size; i++) {
          if (values[i] < min) {
              min = values[i];
          }
      }
      min = (min < 0) ? -min : 0.0; // Shift by min if negative
      for (size_t i = 0; i < size; i++) {
          values[i] += min;
          total += values[i];
      }
      for (size_t i = 0; i < size; i++) {
          if (total) {
            values[i] /= total;
            mass += values[i];
            if (mass >= PROBABILITY_MASS) return i + 1;
          } else values[i] = 1.0 / size;
      }
      return size;
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
  
/*
Overview of the MCTS Logic

MCTS implementation follows the four core phases:

    Selection: Starting from the root, traverse the tree using the PUCT (Predictor + Upper Confidence Bound applied to Trees) formula to select the most promising child node until reaching a leaf or terminal position.

    Expansion: At a leaf node, generate child nodes based on legal moves, using NNUE to assign prior probabilities.

    Evaluation: Evaluate terminal positions (checkmate/stalemate) directly or use NNUE for non-terminal positions, mapping scores to [-1, 1]. Check is handled specially as 0 result because NNUE does not like checks

    Backpropagation: Update visit counts (N) and total value (W) from the leaf back to the root, alternating the sign of the result to reflect perspective changes.

The tree is stored using UTHash with a composite key (zobrist, move), where zobrist is the position’s Zobrist hash, and move is the move leading to that position (an index from 0 to 4095, encoding source and destination squares, promos are not encoded and treated always as promotion to queen).
*/

  // Hash function for std::pair<uint64_t, uint64_t>
  struct PairHash {
      std::size_t operator()(const std::pair<uint64_t, uint64_t>& p) const {
          // Combine the two 64-bit hashes using XOR
          // You can also use other mixing functions for better distribution
          //return std::hash<uint64_t>{}(p.first) ^ std::hash<uint64_t>{}(p.second);
          return p.first ^ p.second;
      }
  };

  void mcts_search(int num_simulations) {
    char uci_move[6], temp_move[6];
    struct Move move;
    struct NNUEContext ctx;
    init_nnue_context(&ctx);
    chessEngine.currentNodes = 0;
    std::vector<struct MCTSNode *> path;  // Track the path from root to leaf
    path.reserve(MAX_DEPTH);
    for (int i = 0; i < num_simulations; i++) {
      bool repetition3x = false;
      chessEngine.currentDepth = 0;
      struct MCTSNode * node = root;
      //start from the same initial position given by board (at the root node, i.e. at the top of the tree)
      //clone the board to preserve it for subsequent iterations
      struct Board * sim_board = cloneBoard(board);
      if (!sim_board) {
        dprintf(logfile, "mcts_search() error: cloneBoard() returned NULL\n");
        fprintf(stderr, "mcts_search() error: cloneBoard() returned NULL\n");
        exit(-1);
      }
      // Selection
      //iterate down the tree updating sim_board by initiating and making moves
      std::unordered_map<std::pair<uint64_t, uint64_t>, int, PairHash> position_history;
      while (node && node->num_children > 0 && !sim_board->isStaleMate && !sim_board->isMate) { //traversal stops at a leaf or terminal node or at 3x repetition
        std::pair<uint64_t, uint64_t> pos_key(sim_board->zh->hash, sim_board->zh->hash2);
        if (position_history[pos_key] >= 2) { // 3rd occurrence
            repetition3x = true;
            break;
        }
        position_history[pos_key]++;
        //return child node with the best score using PUCT (Predictor + Upper Confidence Bound)
        int idx = -1;
        select_best_child(node, &idx, sim_board);
        //dprintf(logfile, "select_best_child() debug: selected move is %s%s, fen %s\n", squareName[node->children[idx].move / 64], squareName[node->children[idx].move % 64], sim_board->fen->fenString);
        //init edge's move that leads to the child node
    		init_move(&move, sim_board, node->children[idx].move / 64, node->children[idx].move % 64);
        //make the move
        makeMove(&move); //this updates sim_board
        //update Zobrist hash (it is needed so that we can call updateHash later instead of getHash)
  			if (updateHash(sim_board, &move)) {
  				dprintf(logfile, "mcts_search() error: updateHash() returned non-zero value\n");
  				fprintf(stderr, "mcts_search() error: updateHash() returned non-zero value\n");
  				exit(-1);
  			}
        //continue iterating down the tree until no more children or the end of the game is reached
        path.push_back(node);  // Add node to path
        node = node->children[idx].child;
        chessEngine.currentDepth++;
      } //end of while(node && node->num_children > 0 && !sim_board->isStaleMate && !sim_board->isMate) loop
      position_history.clear();
      path.push_back(node);  // Add leaf to path - sim_board and result correspond to this node!
      //Here we are at the bottom of the tree, i.e. at a leaf or at the terminal node (mate, stalemate)
      // Evaluation - evaluate a leaf, i.e. a node without children, then we expand it - add children, which will be evaluated at the subsequent iterations
      double result = 0;
      if (sim_board->isCheck) {
          result = NNUE_CHECK; //NNUE cannot evaluate when in check - it will be resolved in expansion
      } else if (sim_board->isMate) { //sim_board->fen->sideToMove is mated
        result = -1.0;
				//dprintf(logfile, "mcts_search() debug: checkmate for %s, fen %s\n", color[sim_board->fen->sideToMove], sim_board->fen->fenString);
      } else if (sim_board->isStaleMate || repetition3x) {
        result = 0.0;        
				//dprintf(logfile, "mcts_search() debug: stalemate or 3x repetition for %s, fen %s\n", color[sim_board->fen->sideToMove], sim_board->fen->fenString);
      } else {
        //evaluate_nnue() returns result in pawns (not centipawns!) from sim_board->fen->sideToMove perspective
        result = evaluate_nnue(sim_board, NULL, &ctx);
  			//dprintf(logfile, "mcts_search() debug: evaluate_nnue result %f, fen %s\n", result, sim_board->fen->fenString);
        result = tanh(result / 4.0);
      }
      // Expansion - add more children - increase the depth of the tree down using the model's predictions, NNUE evals or randomly
      if (chessEngine.currentDepth < MAX_DEPTH && !sim_board->isMate && !sim_board->isStaleMate && !repetition3x) {
        int num_moves = 0;
        std::vector<std::pair<double, int>> move_evals;
      	enum PieceName side = (enum PieceName)((sim_board->fen->sideToMove << 3) | PieceTypeAny);//either PieceNameWhite or PieceNameBlack
      	unsigned long long any = sim_board->occupations[side]; 
      	while (any) {
      	  int src = __builtin_ctzl(any);
      	  unsigned long long moves = sim_board->sideToMoveMoves[src];
      	  while (moves) {
        	  int dst = __builtin_ctzl(moves);
            init_move(&move, sim_board, src, dst);
            //evaluate_nnue() returns result in pawns (not centipawns!)
            //stockfish makes the move, so the res is from the perspective of sim_board->opponentColor
            double res = evaluate_nnue(sim_board, &move, &ctx);
            if (res == NNUE_CHECK) //we need to resolve the check to get NNUE score
              res = process_check(sim_board, &move, &ctx);
            move_evals.push_back({res, src * 64 + dst});
      		  moves &= moves - 1;
      		  num_moves++;
      		}
          any &= any - 1;
        }
        std::sort(move_evals.begin(), move_evals.end(), std::greater<>()); //sorted in descending order
        if (result == NNUE_CHECK) {
  			  //dprintf(logfile, "mcts_search() debug: check resolution result %f for move %s%s, fen %s\n", move_evals[0].first, squareName[move_evals[0].second / 64], squareName[move_evals[0].second % 64], sim_board->fen->fenString);
          result = tanh(move_evals[0].first / 4.0);
        }
        double top_move_probs[num_moves];
        int top_move_pred[num_moves];
        for (int j = 0; j < num_moves; j++) {
            top_move_probs[j] = move_evals[j].first;
            top_move_pred[j] = move_evals[j].second;
        }
        move_evals.clear();     
        int effective_branching = get_prob(top_move_probs, num_moves); //scaled to [0..1] and sum to 1
        /*
        for (int j = 0; j < effective_branching; j++) {
    			  dprintf(logfile, "mcts_search() debug: expansion moves %s%s (%f%%), fen %s\n", squareName[top_move_pred[j] / 64], squareName[top_move_pred[j] % 64], top_move_probs[j] * 100, sim_board->fen->fenString);
        }*/
        
        //Dirichlet Noise (optional)
        /*
        double alpha = 0.03;  // Tune this value
        for (int i = 0; i < effective_branching; i++) {
            top_move_probs[i] = 0.75 * top_move_probs[i] + 0.25 * (alpha / effective_branching);  // Simplified noise
        }
        get_prob(top_move_probs, effective_branching);  // Re-normalize
        */
        expand_node(node, sim_board, top_move_probs, top_move_pred, effective_branching);
      } //end of if (path_len < MAX_DEPTH && !sim_board->isMate && !sim_board->isStaleMate)
      // Backpropagation: update node visits and results
      for (auto n = path.rbegin(); n != path.rend(); n++) {
        node = *n;
        node->N++;
        node->W += result;
        result = -result; 
      }
      chessEngine.currentNodes++;
      path.clear();
      freeBoard(sim_board);
      if (chessEngine.seldepth < chessEngine.currentDepth) chessEngine.seldepth = chessEngine.currentDepth;   
    } //end of the iterations loop
    if ((chessEngine.depth && chessEngine.currentDepth >= chessEngine.depth) || (chessEngine.nodes && chessEngine.currentNodes >= chessEngine.nodes)) stopFlag = 1;
    free_nnue_context(&ctx);
  }
    
  int select_best_move(char * pv) {
    struct MCTSNode * parent = root;
    int best_idx = -1;
    //MCTS implementations choose the most visited child (N) rather than PUCT
    most_visited_child(parent, &best_idx, board);
    if (best_idx < 0) {
      dprintf(logfile, "select_best_move() error: best_idx < 0\n");
      fprintf(stderr, "select_best_move() error: best_idx < 0\n");
      exit(-1);      
    }
    idx_to_move(board, parent->children[best_idx].move, best_move);
    strcat(pv, best_move);
    strcat(pv, " ");
    
    //continue the best pv line
    char uci_move[6];
    struct Board * temp_board = cloneBoard(board);
    struct Move move;
    int idx = best_idx;
    init_move(&move, temp_board, parent->children[idx].move / 64, parent->children[idx].move % 64);
    makeMove(&move);
    parent = parent->children[idx].child;
    while (parent->num_children > 0 && strlen(pv) < 84) {
      most_visited_child(parent, &idx, temp_board);
      idx_to_move(temp_board, parent->children[idx].move, uci_move);
      strcat(pv, uci_move);
      strcat(pv, " ");
      init_move(&move, temp_board, parent->children[idx].move / 64, parent->children[idx].move % 64);
      makeMove(&move);
      parent = parent->children[idx].child;
    }
    freeBoard(temp_board);
    return best_idx;
  }

/*
  //Breadth-First Search (BFS) to find accessible nodes, then delete all others
  void free_hash_table_top() {
    std::queue<struct MCTSNode *> queue;
    current_generation++; // Start a new generation
    
    // Start traversal from the new root
    root->generation = current_generation;
    queue.push(root);

    // BFS to find all reachable nodes
    while (!queue.empty()) {
        struct MCTSNode * current = queue.front();
        queue.pop();
        for (int i = 0; i < current->num_children; i++) {
            struct MCTSNode * child = current->children[i].child;
            if (child->generation != current_generation) { // Not yet visited
                child->generation = current_generation;    // Mark as reachable
                queue.push(child);
            }        
        }
    }

    // Remove all nodes not in the reachable set
    struct MCTSNode * node, * tmp;
    HASH_ITER(hh, tree, node, tmp) {
        if (node->generation < current_generation) { // Unreachable node
            HASH_DEL(tree, node);
            free(node->children);
            node->children = NULL;
            free(node); // Free memory for the node
            node = NULL;
        }
    }
}
*/

  void runMCTS() {
      double elapsed = 0.0, elapsed2 = 0.0;
      unsigned long iterations = MIN_ITERATIONS;
      auto iter_start = std::chrono::steady_clock::now();
    
      set_root();
      //cut the tree above us to make this board's node the root
      //free_hash_table_top();
      
      //elapsed2 = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
      //dprintf(logfile, "runMCTS() debug: time spent for freeing hash table top %.2f sec\n", elapsed2);
      while (elapsed < (timeAllocated * 0.001) && !stopFlag && iterations < MAX_ITERATIONS) {
          mcts_search(iterations);
          char pv[96] = "";
          int idx = select_best_move(pv);
          struct Move move;
          init_move(&move, board, root->children[idx].move / 64, root->children[idx].move % 64);
          struct NNUEContext ctx;
          init_nnue_context(&ctx);
          double res = evaluate_nnue(board, &move, &ctx);
          if (res == NNUE_CHECK) //we need to resolve the check to get NNUE score
            res = process_check(board, &move, &ctx);
          free_nnue_context(&ctx);
          unsigned long total_nodes = HASH_COUNT(tree);
          unsigned long hash_overhead_bytes = HASH_OVERHEAD(hh, tree);
          elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
          dprintf(logfile, "info depth %d seldepth %d multipv 1 score cp %ld nodes %lu nps %.0f hashfull %lu tbhits 0 time %.0f pv %s timeAllocated %.2f stopFlag %d\n", chessEngine.currentDepth, chessEngine.seldepth, (long)(res * 100), chessEngine.currentNodes, chessEngine.currentNodes / elapsed, (sizeof(struct MCTSNode) * total_nodes + hash_overhead_bytes) / 1000 / chessEngine.optionSpin[0].value, elapsed * 1000, pv, timeAllocated * 0.001, stopFlag);
          printf("info depth %d seldepth %d multipv 1 score cp %ld nodes %lu nps %.0f hashfull %lu tbhits 0 time %.0f pv %s\n", chessEngine.currentDepth, chessEngine.seldepth, (long)(res * 100), chessEngine.currentNodes, chessEngine.currentNodes / elapsed, (sizeof(struct MCTSNode) * total_nodes + hash_overhead_bytes) / 1000 / chessEngine.optionSpin[0].value, elapsed * 1000, pv);
          fflush(stdout);
      }
  }  
}
