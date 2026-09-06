//For MacOS using clang
//c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -I /Users/ap/libchess  -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess chess_mcts_smp.cpp uci_smp.cpp tbcore.c tbprobe.c -o chess_mcts_smp

// For linux or Windows using mingw
// add -mpopcnt for X86_64
// might need to add -Wno-stringop-overflow to avoid some warnings in tbcore.h
//g++ -std=c++20 -mpopcnt -Wno-deprecated -Wno-write-strings -Wno-deprecated-declarations -Wno-stringop-overflow -O3 -I /home/ap/libchess -L /home/ap/libchess chess_mcts_smp.cpp uci_smp.cpp tbcore.c tbprobe.c -o chess_mcts_smp -lchess

//or with clang in MSYS2 MINGW64 or CLANG64
//clang++ -std=c++20 -mpopcnt -Wno-deprecated -Wno-write-strings -Wno-deprecated-declarations -O3 -I /home/ap/libchess -L /home/ap/libchess chess_mcts_smp.cpp uci_smp.cpp tbcore.c tbprobe.c -o chess_mcts_smp -lchess

#include "nnue/types.h"
#include "nnue/position.h"
#include "nnue/evaluate.h"
#include "nnue/nnue/nnue_common.h"
#include "nnue/nnue/network.h"
#include "nnue/nnue/nnue_accumulator.h"

#ifdef _MSC_VER
#include <mutex>
#endif

#ifdef __GNUC__ // g++ on Alpine Linux
#include <mutex>
#include <atomic>
#include <cstdarg>
#endif
#include <vector>
#include <thread>
#include <unordered_map>
#include <functional>
#include <random>
#include <chrono>
#include <math.h>
#include "tbprobe.h"
#include "libchess.h"

extern std::atomic<bool> stopFlag;
extern double timeAllocated; //ms
extern char best_move[6];

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
  void log_file(const char * message, ...);
  void print(const char * message, ...);
  
  #define MAX_DEPTH 50
  extern bool quitFlag;
  extern bool searchFlag;
  extern int logfile;
  extern struct Engine chessEngine;
  extern struct Board * board;
  std::mutex probe_mutex;
  
  struct Edge {
      int move;             // The move that leads to the child position
      double P;            // Prior probability - model move_probs for a given move in the node
      struct MCTSNode * child; // Pointer to the child node
  };

  struct MCTSNode {
    unsigned long long hash;
    unsigned long long N;        // Visit count - number of games that have reached this node
    double W;           // Total value W is a sum of loses (-1) and wins (1) for these N games
                        // Q = W / N
    struct Edge * children; //array of Edges to child nodes
    int num_children;
  };
  
  struct EdgeStats {
    int move;
    unsigned long long N;
    double W;
  };

  struct MCTSSearch {
    std::unordered_map<unsigned long long, MCTSNode> tree;
    struct MCTSNode * root = nullptr;
    double exploration_constant;
    double probability_mass;
    double dirichlet_alpha;
    std::mt19937 rng{std::random_device{}()};
  };

  struct ThreadParams {
      MCTSSearch search;
      int thread_id;
      int num_sims;  // Or time slice
      //struct Board * board;  // Shared read-only
      std::vector<EdgeStats> stats; //per thread stats
      unsigned long long currentNodes = 0;  // Per-thread
      int currentDepth = 0;                 // Per-thread max depth reached
      int seldepth = 0;                     // Per-thread selective depth
  };
  
  void cleanup(MCTSSearch * search) {
    for (auto& [h, node] : search->tree) {
        free(node.children);
    }
    search->tree.clear();
    search->root = nullptr;
  }
    
  void set_root(MCTSSearch * search) {
    unsigned long long hash = board->zh->hash ^ board->zh->hash2;
    auto& node = search->tree[hash];
    node.hash = hash;
    node.N = 0;
    node.W = 0;
    node.children = nullptr;
    node.num_children = 0;
    search->root = &node;
  }
    
  //Predictor + Upper Confidence Bound applied to Trees - used in select_best_child()
  double puct_score(struct MCTSNode * parent, int idx, double exploration_constant) {
    double Q = parent->children[idx].child->N ? parent->children[idx].child->W / parent->children[idx].child->N : 0.0;
    return -Q + exploration_constant * parent->children[idx].P * sqrt((double)parent->N) / (1.0 + parent->children[idx].child->N);
  }

  char * idx_to_move(struct Board * chess_board, int move_idx, char * uci_move) {
    uci_move[0] = '\0';
    if (!uci_move) {
      log_file("idx_to_move() error: invalid arg - uci_move is NULL\n");
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
  
  void expand_node(MCTSSearch * search, struct MCTSNode * parent, struct Board * chess_board, const std::vector<std::pair<double, int>>& top_moves) {
    int num_moves = top_moves.size();
    parent->children = (struct Edge *)calloc(num_moves, sizeof(struct Edge));
    parent->num_children = num_moves;

    // Dirichlet noise if root
    std::vector<double> priors(num_moves);
    for (int i = 0; i < num_moves; ++i) priors[i] = top_moves[i].first;  // Copy probs

    if (parent == search->root && search->dirichlet_alpha > 0.0) {
        std::gamma_distribution<double> gamma(search->dirichlet_alpha, 1.0);
        std::vector<double> noise(num_moves);
        double noise_sum = 0.0;
        for (auto& n : noise) {
            n = gamma(search->rng);
            noise_sum += n;
        }
        for (auto& n : noise) n /= noise_sum;  // Normalize
        for (int i = 0; i < num_moves; ++i) {
            priors[i] = (1.0 - (double)(chessEngine.optionSpin[DirichletEpsilon].value)) / 100.0 * priors[i] + (double)(chessEngine.optionSpin[DirichletEpsilon].value) / 100.0 * noise[i];
        }
    }
    for (int i = 0; i < num_moves; ++i) {
        struct Board * temp_board = cloneBoard(chess_board);
        if (!temp_board) {
          log_file("expand_node() error: cloneBoard() returned NULL\n");
  				exit(-1);
        }
        struct Move move;
        int move_idx = top_moves[i].second;
        init_move(&move, temp_board, move_idx / 64, move_idx % 64);
        makeMove(&move);
  			if (updateHash(temp_board, &move)) {
  				log_file("expand_node() error: updateHash() returned non-zero value\n");
  				exit(-1);
  			}
        unsigned long long hash = temp_board->zh->hash ^ temp_board->zh->hash2;
        auto it = search->tree.find(hash);
        struct MCTSNode * child = (it != search->tree.end()) ? &it->second : nullptr;
        if (!child) {
            auto& new_node = search->tree[hash];
            new_node.hash = hash;
            new_node.N = 0;
            new_node.W = 0;
            new_node.children = nullptr;
            new_node.num_children = 0;
            child = &new_node;
        }
        parent->children[i].P = priors[i];  // Use noised priors
        parent->children[i].move = move_idx;
        parent->children[i].child = child;
        freeBoard(temp_board);
    }
  }
  
  double most_visited_child(struct MCTSNode * parent, int * selected, unsigned long long * N) {
    double W = 0;
    for (int i = 0; i < parent->num_children; i++) {
      if (parent->children[i].child->N > *N) {
          *N = parent->children[i].child->N;
          *selected = i;
          W = parent->children[i].child->W;
      } 
    }
    return W;
  }
  
  void select_best_child(struct MCTSNode * parent, int * selected, struct Board * chess_board, double exploration_constant) {
      double best_score = -INFINITY;
      double score;
      for (int i = 0; i < parent->num_children; i++) {
        //debug - comment out later
        //char uci_move[6];
        //idx_to_move(chess_board, parent->children[i].move, uci_move);
        //log_file("select_best_child() debug: move %s, %.2f / %lld + sqrt(%lld) / %lld * %.4f * %.1f = %.5f + %.5f = %.5f\n", uci_move, -parent->children[i].child->W, parent->children[i].child->N, parent->N, parent->children[i].child->N + 1, parent->children[i].P, (double)(chessEngine.optionSpin[ExplorationConstant].value) / 100.0, parent->children[i].child->N ? -parent->children[i].child->W / parent->children[i].child->N : 0.0, sqrt(parent->N) / (parent->children[i].child->N + 1) * parent->children[i].P * (double)(chessEngine.optionSpin[ExplorationConstant].value) / 100.0, parent->children[i].child->N ? -parent->children[i].child->W / parent->children[i].child->N + sqrt(parent->N) / (parent->children[i].child->N + 1) * parent->children[i].P * (double)(chessEngine.optionSpin[ExplorationConstant].value) / 100.0 : sqrt(parent->N) / (parent->children[i].child->N + 1) * parent->children[i].P * (double)(chessEngine.optionSpin[ExplorationConstant].value) / 100.0);
        
        score = puct_score(parent, i, exploration_constant);
        if (score > best_score) {
            best_score = score;
            *selected = i;
        } 
     }
  }

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
  
  double process_check(struct Board * chess_board, struct Move * move, struct NNUEContext * ctx) {
    struct Board * temp_board = cloneBoard(chess_board);
    if (!temp_board) {
      log_file("process_check() error: cloneBoard() returned NULL\n");
			exit(-1);
    }
    move->chessBoard = temp_board;
    makeMove(move);
    /*
		if (updateHash(temp_board, &move)) {
			log_file("process_check() error: updateHash() returned non-zero value\n");
			exit(-1);
		}*/
    double best_value = temp_board->isMate ? -0.01 * MATE_SCORE : -INFINITY;
  	enum PieceName side = (enum PieceName)((temp_board->fen->sideToMove << 3) | PieceTypeAny);//either PieceNameWhite or PieceNameBlack
  	unsigned long long any = temp_board->occupations[side]; 
  	while (any) {
  	  int src = lsBit(any);
  	  unsigned long long moves = temp_board->sideToMoveMoves[src];
  	  while (moves) {
    	  int dst = lsBit(moves);
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

    Evaluation: Evaluate terminal positions (checkmate/stalemate) directly or use NNUE for non-terminal positions, mapping scores to [-1, 1]

    Backpropagation: Update visit counts (N) and total value (W) from the leaf back to the root, alternating the sign of the result to reflect perspective changes.
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

  void mcts_search(ThreadParams * params, struct NNUEContext * ctx) {
    char uci_move[6];
    struct Move move;
    params->currentNodes = 0;
    std::vector<struct MCTSNode *> path;  // Track the path from root to leaf
    path.reserve(chessEngine.depth);
    for (int i = 0; i < params->num_sims && !stopFlag.load(); i++) {
      bool repetition3x = false;
      params->currentDepth = 0;
      struct MCTSNode * node = params->search.root;
      //start from the same initial position given by board (at the root node, i.e. at the top of the tree)
      //clone the board to preserve it for subsequent iterations
      struct Board * sim_board = cloneBoard(board);
      if (!sim_board) {
        log_file("mcts_search() error: cloneBoard() returned NULL\n");
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
        select_best_child(node, &idx, sim_board, params->search.exploration_constant);
        //log_file("select_best_child() debug: selected move is %s%s, fen %s\n", squareName[node->children[idx].move / 64], squareName[node->children[idx].move % 64], sim_board->fen->fenString);
        //init edge's move that leads to the child node
    		init_move(&move, sim_board, node->children[idx].move / 64, node->children[idx].move % 64);
        //make the move
        makeMove(&move); //this updates sim_board
        //update Zobrist hash (it is needed so that we can call updateHash later instead of getHash)
  			if (updateHash(sim_board, &move)) {
  				log_file("mcts_search() error: updateHash() returned non-zero value\n");
  				exit(-1);
  			}
        //continue iterating down the tree until no more children or the end of the game is reached
        path.push_back(node);  // Add node to path
        node = node->children[idx].child;
        params->currentDepth++;
      } //end of while(node && node->num_children > 0 && !sim_board->isStaleMate && !sim_board->isMate) loop
      position_history.clear();
      path.push_back(node);  // Add leaf to path - sim_board and result correspond to this node!
      //Here we are at the bottom of the tree, i.e. at a leaf or at the terminal node (mate, stalemate)
      // Evaluation - evaluate a leaf, i.e. a node without children, then we expand it - add children, which will be evaluated at the subsequent iterations
 
      double result = 0, res;
      int src, dst, effective_branching = 1;
      std::vector<std::pair<double, int>> move_evals;
      if (bitCount(sim_board->occupations[PieceNameAny]) > TB_LARGEST) {
        if (sim_board->isCheck) {
            result = NNUE_CHECK; //NNUE cannot evaluate when in check - it will be resolved in expansion
        } else if (sim_board->isMate) { //sim_board->fen->sideToMove is mated
          result = -1.0;
  				//log_file("mcts_search() debug: checkmate for %s, fen %s\n", color[sim_board->fen->sideToMove], sim_board->fen->fenString);
        } else if (sim_board->isStaleMate || repetition3x) {
          result = 0.0;        
  				//log_file("mcts_search() debug: stalemate or 3x repetition for %s, fen %s\n", color[sim_board->fen->sideToMove], sim_board->fen->fenString);
        } else {
          //evaluate_nnue() returns result in pawns (not centipawns!) from sim_board->fen->sideToMove perspective
          result = evaluate_nnue(sim_board, NULL, ctx);
    			//log_file("mcts_search() debug: evaluate_nnue result %f, fen %s\n", result, sim_board->fen->fenString);
          result = tanh(result / 4.0);
        }
        // Expansion - add more children - increase the depth of the tree down using the model's predictions, NNUE evals or randomly
        if (params->currentDepth < chessEngine.depth && !sim_board->isMate && !sim_board->isStaleMate && !repetition3x) {
        	enum PieceName side = (enum PieceName)((sim_board->fen->sideToMove << 3) | PieceTypeAny);//either PieceNameWhite or PieceNameBlack
        	unsigned long long any = sim_board->occupations[side]; 
        	while (any) {
        	  src = lsBit(any);
        	  unsigned long long moves = sim_board->sideToMoveMoves[src];
        	  while (moves) {
          	    dst = lsBit(moves);
                init_move(&move, sim_board, src, dst);
                //evaluate_nnue() returns result in pawns (not centipawns!)
                //stockfish makes the move, so the res is from the perspective of sim_board->opponentColor
                res = evaluate_nnue(sim_board, &move, ctx);
                if (res == NNUE_CHECK) //we need to resolve the check to get NNUE score
                  res = process_check(sim_board, &move, ctx);
                move_evals.push_back({res, src * 64 + dst});
        		moves &= moves - 1;
        	  }
            any &= any - 1;
          }
          std::sort(move_evals.begin(), move_evals.end(), std::greater<>()); //sorted in descending order
          if (result == NNUE_CHECK) {
    			  //log_file("mcts_search() debug: check resolution result %f for move %s%s, fen %s\n", move_evals[0].first, squareName[move_evals[0].second / 64], squareName[move_evals[0].second % 64], sim_board->fen->fenString);
            result = tanh(move_evals[0].first / 4.0);
          }
          effective_branching = get_prob(move_evals, params->search.probability_mass);
          // Slice to top effective
          move_evals.resize(effective_branching);
          expand_node(&params->search, node, sim_board, move_evals);
          move_evals.clear();
        } //end of if (params->currentDepth < chessEngine.depth && !sim_board->isMate && !sim_board->isStaleMate) 
      } else {
        if (params->currentDepth < chessEngine.depth && !sim_board->isMate && !sim_board->isStaleMate && !repetition3x) {
          unsigned int ep = lsBit(sim_board->fen->enPassantLegalBit);
          unsigned int res = 0;
          {
            std::lock_guard<std::mutex> lock(probe_mutex);
            res = tb_probe_root(sim_board->occupations[PieceNameWhite], sim_board->occupations[PieceNameBlack], 
            sim_board->occupations[WhiteKing] | sim_board->occupations[BlackKing],
              sim_board->occupations[WhiteQueen] | sim_board->occupations[BlackQueen], sim_board->occupations[WhiteRook] | sim_board->occupations[BlackRook], sim_board->occupations[WhiteBishop] | sim_board->occupations[BlackBishop], sim_board->occupations[WhiteKnight] | sim_board->occupations[BlackKnight], sim_board->occupations[WhitePawn] | sim_board->occupations[BlackPawn],
              sim_board->fen->halfmoveClock, 0, ep == 64 ? 0 : ep, sim_board->opponentColor == ColorBlack ? 1 : 0, NULL);
          }
          if (res == TB_RESULT_FAILED) {
            log_file("error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, occupations %u, fen %s, ep %u, halfmoveClock %u, whiteToMove %u, whites %llu, blacks %llu, kings %llu, queens %llu, rooks %llu, bishops %llu, knights %llu, pawns %llu, err %s\n", TB_LARGEST, bitCount(sim_board->occupations[PieceNameAny]), sim_board->fen->fenString, ep, sim_board->fen->halfmoveClock, sim_board->opponentColor == ColorBlack ? 1 : 0, sim_board->occupations[PieceNameWhite], sim_board->occupations[PieceNameBlack], sim_board->occupations[WhiteKing] | sim_board->occupations[BlackKing],
              sim_board->occupations[WhiteQueen] | sim_board->occupations[BlackQueen], sim_board->occupations[WhiteRook] | sim_board->occupations[BlackRook], sim_board->occupations[WhiteBishop] | sim_board->occupations[BlackBishop], sim_board->occupations[WhiteKnight] | sim_board->occupations[BlackKnight], sim_board->occupations[WhitePawn] | sim_board->occupations[BlackPawn], strerror(errno));
            exit(-1);
          }
          unsigned int wdl = TB_GET_WDL(res); //0 - loss, 4 - win, 1..3 - draw
          if (wdl == 4) result = 1.0;
          else if (wdl == 0) result = -1.0;
          else result = 0.0;
          src = TB_GET_FROM(res);
          dst = TB_GET_TO(res);
          //unsigned int promotes = TB_GET_PROMOTES(res);
          move_evals.push_back({1.0, src * 64 + dst});
          move_evals.resize(1);
          expand_node(&params->search, node, sim_board, move_evals);
          move_evals.clear();
        }
      }     
      
      // Backpropagation: update node visits and results
      for (auto n = path.rbegin(); n != path.rend(); n++) {
        node = *n;
        node->N++;
        node->W += result;
        result = -result; 
      }
      params->currentNodes++;
      path.clear();
      freeBoard(sim_board);
      if (params->currentDepth > params->seldepth) params->seldepth = params->currentDepth;   
      if ((chessEngine.depth && params->currentDepth >= chessEngine.depth) || (chessEngine.nodes && params->currentNodes >= chessEngine.nodes)) break;
    } //end of the iterations loop
  }
    
  void thread_search(ThreadParams * params) {
      double elapsed = 0.0;
      unsigned long iterations = MIN_ITERATIONS;

      cleanup(&params->search);
      // Set params based on thread_id for diversity
      //params->search.exploration_constant = (double)(chessEngine.optionSpin[ExplorationConstant].value) / 100.0 + 0.1 * (params->thread_id);
      params->search.probability_mass = (double)(chessEngine.optionSpin[ProbabilityMass].value) / 100.0 + 0.07 * (params->thread_id);
      params->search.exploration_constant = (double)(chessEngine.optionSpin[ExplorationConstant].value) / 100.0 + 0.2 * (params->thread_id % 3 - 1);
      //params->search.probability_mass = (double)(chessEngine.optionSpin[ProbabilityMass].value) / 100.0 + 0.1 * (params->thread_id % 2);
      params->search.dirichlet_alpha = (double)(chessEngine.optionSpin[DirichletAlpha].value) / 100.0;
      
      set_root(&params->search);
  
      struct NNUEContext ctx;
      auto iter_start = std::chrono::steady_clock::now();
      init_nnue_context(&ctx);
      while (elapsed < (timeAllocated * 0.001) && !stopFlag.load()) {
        mcts_search(params, &ctx);
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
      }
      free_nnue_context(&ctx);
      if (params->search.root && params->search.root->num_children) {
        params->stats.reserve(params->search.root->num_children);
        for (int i = 0; i < params->search.root->num_children; i++) {
            params->stats.push_back({params->search.root->children[i].move, params->search.root->children[i].child->N, params->search.root->children[i].child->W});
        }        
      }
  }

  void runMCTS() {
    double elapsed = 0.0;
    int move_idx = 0;
    unsigned long long unique_nodes = 0;
    unsigned long long nodes = 0;
    std::vector<EdgeStats> merged_stats; //aggregated stats
    int move_number = bitCount(board->moves);
    if (move_number > 1) {
        unsigned long iterations = MIN_ITERATIONS;
        std::vector<ThreadParams> thread_params(chessEngine.optionSpin[Threads].value);
        for (int i = 0; i < chessEngine.optionSpin[Threads].value; ++i) {
            thread_params[i].thread_id = i;
            thread_params[i].num_sims = iterations;  // Or time-based loop inside thread
            //thread_params[i].board = board;  // Read-only
        }

        if (!chessEngine.depth) chessEngine.depth = MAX_DEPTH;
        std::vector<std::thread> threads;
        auto iter_start = std::chrono::steady_clock::now();
        for (int i = 0; i < chessEngine.optionSpin[Threads].value; ++i) {
            thread_params[i].stats.clear();
            threads.emplace_back(thread_search, &thread_params[i]);
        }
        for (auto& t : threads) t.join();
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();

        //chessEngine.currentNodes = 0;
        chessEngine.currentDepth = 0;
        chessEngine.seldepth = 0;
        for (const auto& tp : thread_params) {
            //chessEngine.currentNodes += tp.currentNodes;
            if (tp.currentDepth > chessEngine.currentDepth) chessEngine.currentDepth = tp.currentDepth;
            if (tp.seldepth > chessEngine.seldepth) chessEngine.seldepth = tp.seldepth;
        }

        std::unordered_map<int, EdgeStats> accum_map;
        for (int i = 0; i < chessEngine.optionSpin[Threads].value; ++i) {
            for (const auto& edge : thread_params[i].stats) {
                auto& acc = accum_map[edge.move];  // Creates if missing
                if (acc.move == 0) acc.move = edge.move;  // Set once
                acc.N += edge.N;
                acc.W += edge.W;
            }
            unique_nodes += thread_params[i].search.tree.size();
            cleanup(&thread_params[i].search);
        }
        // Transfer to merged_stats (optionally sort by N descending for easier selection)
        merged_stats.reserve(accum_map.size());
        for (const auto& [move, stats] : accum_map) {
            merged_stats.push_back(stats);
            nodes += stats.N;
        }
        std::sort(merged_stats.begin(), merged_stats.end(), [](const EdgeStats& a, const EdgeStats& b) {return a.N > b.N;});
        move_idx = merged_stats[0].move;
        merged_stats.clear();
    }
    else if (move_number == 1) {
        auto iter_start = std::chrono::steady_clock::now();
        enum PieceName side = (enum PieceName)((board->fen->sideToMove << 3) | PieceTypeAny);//either PieceNameWhite or PieceNameBlack
        unsigned long long any = board->occupations[side];
        int src = 0, dst = 0;
        while (any) {
            src = lsBit(any);
            unsigned long long moves = board->sideToMoveMoves[src];
            if (moves) {
                dst = lsBit(moves);
                break;
            }
            any &= any - 1;
        }
        move_idx = src * 64 + dst;
        nodes = 1;
        unique_nodes = 1;
        chessEngine.currentDepth = 1;
        chessEngine.seldepth = 1;
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
    }
    else {
        if (board->isMate) {
            log_file("info depth 0 score mate 0\n");
            log_file("bestmove (none)\n");
            print("info depth 0 score mate 0\n");
            print("bestmove (none)\n");
        }
        else if (board->isStaleMate) {
            log_file("info depth 0 score cp 0\n");
            log_file("bestmove (none)\n");
            print("info depth 0 score cp 0\n");
            print("bestmove (none)\n");
        }
        return;
    }

    idx_to_move(board, move_idx, best_move);
    struct Move move;
    init_move(&move, board, move_idx / 64, move_idx % 64);
    struct NNUEContext ctx;
    init_nnue_context(&ctx);
    double res = evaluate_nnue(board, &move, &ctx);
    if (res == NNUE_CHECK) //we need to resolve the check to get NNUE score
      res = process_check(board, &move, &ctx);
    free_nnue_context(&ctx);
    
    double nps = nodes / elapsed;

    // Calculate hashfull (in per-mille)
    size_t bytes_per_node = sizeof(MCTSNode) + 4 * sizeof(Edge) + 24;  // Avg 4 children?, 24B map overhead
    size_t total_memory = unique_nodes * bytes_per_node;
    size_t max_capacity = chessEngine.optionSpin[Hash].value * 1024 * 1024;  // MB to bytes
    int hashfull = max_capacity ? (total_memory * 1000) / max_capacity : 0;
    if (hashfull > 1000) hashfull = 1000;  // Cap at 1000 per UCI spec
    
    log_file("info depth %d seldepth %d multipv 1 score cp %ld nodes %llu nps %.0f hashfull %d tbhits 0 time %.0f pv %s timeAllocated %.2f\n", chessEngine.currentDepth, chessEngine.seldepth, (long)(res * 100), nodes, nps, hashfull, elapsed * 1000, best_move, timeAllocated * 0.001);
    log_file("bestmove %s\n", best_move);    

    print("info depth %d seldepth %d multipv 1 score cp %ld nodes %llu nps %.0f hashfull %d tbhits 0 time %.0f pv %s\n", chessEngine.currentDepth, chessEngine.seldepth, (long)(res * 100), nodes, nps, hashfull, elapsed * 1000, best_move);
    print("bestmove %s\n", best_move);
  }  
}
