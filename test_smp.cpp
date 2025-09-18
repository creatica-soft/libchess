//c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -I /Users/ap/libchess  -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess tbcore.c tbprobe.c test_smp.cpp -o test_smp

//For MacOS using clang
//c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -I /Users/ap/libchess  -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess creatica.cpp uci.cpp tbcore.c tbprobe.c -o creatica

// For linux or Windows using mingw
// add -mpopcnt for X86_64
// might need to add -Wno-stringop-overflow to avoid some warnings in tbcore.h
//g++ -std=c++20 -mpopcnt -Wno-deprecated -Wno-write-strings -Wno-deprecated-declarations -Wno-stringop-overflow -O3 -I /home/ap/libchess -L /home/ap/libchess creatica.cpp uci.cpp tbcore.c tbprobe.c -o creatica -lchess

//or with clang in MSYS2 MINGW64 or CLANG64
//clang++ -std=c++20 -mpopcnt -Wno-deprecated -Wno-write-strings -Wno-deprecated-declarations -O3 -flto -I /home/ap/libchess -L /home/ap/libchess creatica.cpp uci.cpp tbcore.c tbprobe.c -o creatica -lchess

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
#include <shared_mutex>
#include <atomic>
#include <cstdarg>
#endif
#include <vector>
#include <thread>
#include <unordered_map>
#include <functional>
#include <random>
#include <chrono>
#include <algorithm>
#include <math.h>
//#include <fstream>
#include <iostream>
#include "tbprobe.h"
#include "libchess.h"

double timeAllocated = 10000; //ms
char best_move[6];
std::atomic<bool> search_done{false}; // Signals search completion
double exploration_constant = 1.0;
double probability_mass = 0.9;
double noise = 0.15;
double virtual_loss = 3.0;  // Tune: UCI option, e.g., 1.0-3.0
double eval_scale = 6.0; //Tune as well

extern "C" {
  struct Board * board = nullptr;
  struct Engine chessEngine;

  struct NNUEContext {
      Stockfish::StateInfo * state = nullptr;
      Stockfish::Position * pos = nullptr;
      Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack = nullptr;
      Stockfish::Eval::NNUE::AccumulatorCaches * caches = nullptr;    
  };
  void init_nnue(const char * nnue_file_big, const char * nnue_file_small);
  void cleanup_nnue();
  void init_nnue_context(struct NNUEContext * ctx);
  void free_nnue_context(struct NNUEContext * ctx);
  double evaluate_nnue(struct Board * chess_board, struct Move * move, struct NNUEContext * ctx);
  void print(const char * message, ...);
  
  #define MAX_DEPTH 100
  #define THREADS 8
  #define MULTI_PV 5
  #define SYZYGY_PATH_DEFAULT "<empty>"
  #define SYZYGY_PATH "/Users/ap/syzygy"
  #define HASH 1024 //default, GUI may set it via Hash option (once full, expansion won't happen!)
  #define EXPLORATION_CONSTANT 100 //smaller value favor exploitation, i.e. deeper tree vs wider tree - varies per thread
  #define PROBABILITY_MASS 90 //% - cumulative probability - how many moves we consider - varies per thread [0.5..0.99]
  #define NOISE 15 //% - default noise applied to move NNUE evaluations relative to their values, ie eval += eval * noise * 0.01
  #define VIRTUAL_LOSS 3
  #define PV_PLIES 16
  #define EVAL_SCALE 6.0

  //extern std::atomic<bool> quitFlag;
  //extern std::atomic<bool> searchFlag;
  extern struct Board * board;
  extern struct Engine chessEngine;
  std::mutex probe_mutex, log_mtx, print_mtx;
  std::shared_mutex map_mutex;
  std::atomic<unsigned long long> total_children{0};
  std::atomic<int> hash_full{0};
  std::atomic<unsigned long long> tbhits{0};
  std::atomic<int> generation{0};
  
  struct EngineDepth {
    std::atomic<int> currentDepth{0};
    std::atomic<int> seldepth{0};
  };
  
  struct EngineDepth engineDepth;
  
  struct Edge {
      int move = 0;             // The move that leads to the child position
      double P = 0.0;            // Prior probability - model move_probs for a given move in the node
      struct MCTSNode * child = nullptr; // Pointer to the child node
  };

  struct MCTSNode {
      std::atomic<unsigned long long> hash{0};
      std::atomic<unsigned long long> N{0};  // Atomic for lock-free updates
      std::atomic<double> W{0};
      struct Edge * children = nullptr;
      std::atomic<int> num_children{0};
      std::atomic<int> generation{0};
      std::shared_mutex mutex;  // For protecting children expansion
  };
  
  struct MCTSSearch {
      std::unordered_map<unsigned long long, MCTSNode *> tree;
      struct MCTSNode * root = nullptr;
  };

  static MCTSSearch search;
  
  // Remove MCTSSearch from ThreadParams (shared now)
  struct ThreadParams {
      int thread_id;
      int num_sims;
      unsigned long long currentNodes;
      int currentDepth;
      int seldepth;
      std::mt19937 rng;
  };
    
  void print(const char * message, ...) {
    std::lock_guard<std::mutex> lock(print_mtx);
    va_list args;
    va_start(args, message);
    vprintf(message, args);
    va_end(args);
    fflush(stdout);
  }

  void cleanup() {
    for (auto& [h, node] : search.tree) {
        free(node->children);
        delete node;
    }
    search.tree.clear();
    search.root = nullptr;
  }
  
  //ideally garbage collection should be done by a separate thread
  //so locking is important here
  void gc() {
    if (!search.root) return;
      int current_gen = generation.fetch_add(1, std::memory_order_relaxed) + 1;
      std::queue<MCTSNode *> q;
      search.root->generation.store(current_gen, std::memory_order_relaxed);
      q.push(search.root);
      while (!q.empty()) {
        MCTSNode * node = q.front();
        q.pop();
        int num_child = node->num_children.load(std::memory_order_relaxed);
        if (num_child > 0 && node->children != nullptr) {
          for (int i = 0; i < num_child; ++i) {
            MCTSNode * child = node->children[i].child;
            if (child != nullptr) {
              int expected = current_gen - 1;
              if (child->generation.compare_exchange_strong(expected, current_gen, std::memory_order_relaxed)) 
                q.push(child);
            }
          }
        }
      }
      for (auto it = search.tree.begin(); it != search.tree.end();) {
        MCTSNode * node = it->second;
        if (node->generation.load(std::memory_order_relaxed) < current_gen) {
          if (node->children != nullptr) {
            total_children.fetch_sub(node->num_children, std::memory_order_relaxed);
            delete[] node->children;
          }
          it = search.tree.erase(it);
          delete node;
        } else ++it;
      }
      size_t total_memory = search.tree.size() * (sizeof(MCTSNode) + 24) + total_children.load(std::memory_order_relaxed) * sizeof(Edge);
      size_t max_capacity = chessEngine.optionSpin[Hash].value * 1024 * 1024;  // MB to bytes
      int hashfull = max_capacity ? (total_memory * 1000) / max_capacity : 0;
      if (hashfull > 1000) hashfull = 1000;  // Cap at 1000 per UCI spec
      hash_full.store(hashfull, std::memory_order_relaxed);    
  }
  
  //no locking, call it before starting other threads (search, etc)
  void set_root() {
    unsigned long long hash = board->zh->hash ^ board->zh->hash2;
    auto it = search.tree.find(hash);
    struct MCTSNode * root = (it != search.tree.end()) ? it->second : nullptr;
    if (!root) {
      //root = search.tree[hash];
      root = new MCTSNode();
      search.tree.emplace(hash, root);
      root->hash.store(hash, std::memory_order_relaxed);
      root->generation.store(generation.load(std::memory_order_relaxed));
    }
    search.root = root;    
  }
  
  //Predictor + Upper Confidence Bound applied to Trees - used in select_best_child()
  double puct_score(struct MCTSNode * parent, int idx, double exploration_constant) {
      parent->mutex.lock_shared();
      double P = parent->children[idx].P;
      unsigned long long n = parent->children[idx].child->N.load(std::memory_order_relaxed);
      double w = parent->children[idx].child->W.load(std::memory_order_relaxed);
      parent->mutex.unlock_shared();
      double Q = n ? w / n : 0.0;
      return -Q + exploration_constant * P * sqrt((double)parent->N.load(std::memory_order_relaxed)) / (1.0 + n);
  }
  
  char * idx_to_move(struct Board * chess_board, int move_idx, char * uci_move) {
    uci_move[0] = '\0';
    if (!uci_move) {
      print("idx_to_move() error: invalid arg - uci_move is NULL\n");
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
  
  void expand_node(struct MCTSNode * parent, struct Board * chess_board, const std::vector<std::pair<double, int>>& top_moves) {
    if (parent->num_children.load(std::memory_order_relaxed) > 0) return;  // Already expanded      
    int num_moves = top_moves.size();
    total_children.fetch_add(num_moves, std::memory_order_relaxed);
    struct Edge * children = new Edge[num_moves];
    for (int i = 0; i < num_moves; ++i) {
        struct Board * temp_board = cloneBoard(chess_board);
        struct Move move;
        init_move(&move, temp_board, top_moves[i].second / 64, top_moves[i].second % 64);
        makeMove(&move);
  			if (updateHash(temp_board, &move)) {
  				print("expand_node() error: updateHash() returned non-zero value\n");
  				exit(-1);
  			}
        unsigned long long hash = temp_board->zh->hash ^ temp_board->zh->hash2;
        freeBoard(temp_board);
        std::shared_lock search_lock(map_mutex);
        auto it = search.tree.find(hash);
        struct MCTSNode * child = (it != search.tree.end()) ? it->second : nullptr;
        search_lock.unlock();
        if (!child) {
          child = new MCTSNode();
          std::unique_lock insert_lock(map_mutex);
          search.tree.emplace(hash, child);
          insert_lock.unlock();
          child->hash.store(hash, std::memory_order_relaxed);
          child->generation.store(generation.load(std::memory_order_relaxed));
        }
        children[i].child = child;
        children[i].P = top_moves[i].first;
        children[i].move = top_moves[i].second;
    }
    parent->children = children;
    parent->num_children.store(num_moves, std::memory_order_relaxed);
  }
  
  //no locking, call only when search threads finished
  int most_visited_child(struct MCTSNode * parent) {
    unsigned long long N = 0;
    int idx = -1;
    std::vector<std::pair<double, int>> children;
    int num_children = parent->num_children.load(std::memory_order_relaxed);
    for (int i = 0; i < num_children; i++) {
      unsigned long long n = parent->children[i].child->N.load(std::memory_order_relaxed);
      children.push_back({parent->children[i].P, i});
      if (n > N) {
        N = n;
        idx = i;
      } 
    }
    //in case of unexpanded child, return idx of best prior
    if (idx == -1 && num_children > 0) {
      std::sort(children.begin(), children.end(), std::greater<>());
      idx = children[0].second;
    }
    return idx;
  }
  
  int select_best_move(char *** pv, int ** move_idx) {
    std::vector<std::pair<unsigned long long, int>> children;
    //std::lock_guard<std::mutex> lock(search.root->mutex); //perhaps, it is not needed here as root is only expanded once
    int num_children = 0;
    num_children = search.root->num_children.load(std::memory_order_relaxed);
    
    for (int i = 0; i < num_children; i++) {
      search.root->mutex.lock_shared();
      children.push_back({search.root->children[i].child->N.load(std::memory_order_relaxed), i});
      search.root->mutex.unlock_shared();
    }      
    std::sort(children.begin(), children.end(), std::greater<>());
    int multiPV = std::min<int>(num_children, (int)chessEngine.optionSpin[MultiPV].value);
    if (multiPV > 0) {
      *pv = (char **)calloc(multiPV, sizeof(char *));
      if (!*pv) {
        print("select_best_move() error: calloc() failed for pv\n");
        exit(-1);
      }
      for (int i = 0; i < multiPV; i++) {
        (*pv)[i] = (char *)calloc(1, sizeof(best_move));
        if (!(*pv)[i]) {
          print("select_best_move() error: calloc() failed for pv[%d], pv length %d\n", i, sizeof(best_move));
          exit(-1);
        }
      }
      *move_idx = (int *)calloc(multiPV, sizeof(int));
      if (!*move_idx) {
        print("select_best_move() error: calloc() failed for move_idx\n");
        exit(-1);
      }      
    } else {
      print("select_best_move() error: root node has 0 children\n");
      exit(-1);      
    }
    for (int i = 0; i < multiPV; i++) {
      (*move_idx)[i] = search.root->children[children[i].second].move;
      idx_to_move(board, (*move_idx)[i], (*pv)[i]);
      //print("move_idx[%d] %d, pv[%d] %s\n", i, (*move_idx)[i], i, (*pv)[i]);
    }
    children.clear();
    return multiPV;
  }

  //no locking, call only when search threads finished
  int select_best_moves(char *** pv, int ** move_idx) {
    char uci_move[6];
    std::vector<std::pair<unsigned long long, int>> children;
    int num_children = search.root->num_children.load(std::memory_order_relaxed);
    int multiPV = std::min<int>(num_children, (int)chessEngine.optionSpin[MultiPV].value);
    if (num_children > 0) {
      for (int i = 0; i < num_children; i++) {
        children.push_back({search.root->children[i].child->N.load(std::memory_order_relaxed), i});
      }
      std::sort(children.begin(), children.end(), std::greater<>());
    } else {
      print("select_best_moves() error: root node has 0 children\n");
      exit(-1);      
    }
    int pvLength = chessEngine.optionSpin[PVPlies].value * sizeof(uci_move);
    *pv = (char **)calloc(multiPV, sizeof(char *));
    if (!*pv) {
      print("select_best_moves() error: calloc() failed for pv\n");
      exit(-1);
    }
    for (int i = 0; i < multiPV; i++) {
      (*pv)[i] = (char *)calloc(1, pvLength);
      if (!(*pv)[i]) {
        print("select_best_moves() error: calloc() failed for pv[%d], pvLength %d\n", i, pvLength);
        exit(-1);
      }
    }
    *move_idx = (int *)calloc(multiPV, sizeof(int));
    if (!*move_idx) {
      print("select_best_moves() error: calloc() failed for move_idx, multiPV %d\n", multiPV);
      exit(-1);
    }

    int maxLen = pvLength - sizeof(uci_move);
    struct Move move;
    for (int i = 0; i < multiPV; i++) {
      int child_index = children[i].second;
      struct MCTSNode * current_node = search.root;
      struct Board * temp_board = cloneBoard(board);
      (*move_idx)[i] = search.root->children[child_index].move;
      idx_to_move(temp_board, (*move_idx)[i], uci_move);
      strcat((*pv)[i], uci_move);
      init_move(&move, temp_board, (*move_idx)[i] / 64, (*move_idx)[i] % 64);
      makeMove(&move);
      current_node = search.root->children[child_index].child;      
      // Build PV by following most visited children
      int num_child = current_node->num_children.load(std::memory_order_relaxed);
      while (current_node && num_child > 0 && strlen((*pv)[i]) < maxLen) {
        int next_idx = most_visited_child(current_node);
        if (next_idx < 0) break;
        idx_to_move(temp_board, current_node->children[next_idx].move, uci_move);
        strcat((*pv)[i], " ");
        strcat((*pv)[i], uci_move);
        init_move(&move, temp_board, current_node->children[next_idx].move / 64, current_node->children[next_idx].move % 64);
        makeMove(&move);
        current_node = current_node->children[next_idx].child;
        num_child = current_node->num_children.load(std::memory_order_relaxed);
      }
      freeBoard(temp_board);
    }
    children.clear();
    return multiPV;
  }
  
  int select_best_child(struct MCTSNode * parent, struct Board * chess_board, double exploration_constant) {
    double best_score = -INFINITY;
    double score;
    int selected = -1;
    //parent->mutex.lock();  // Lock parent for read consistency (optional, but safe)
    int num_children = parent->num_children.load(std::memory_order_relaxed);
    //parent->mutex.unlock();
    for (int i = 0; i < num_children; i++) {
      //debug - comment out later
      //char uci_move[6];
      //idx_to_move(chess_board, parent->children[i].move, uci_move);
      //print("select_best_child() debug: move %s, %.2f / %lld + sqrt(%lld) / %lld * %.4f * %.1f = %.5f + %.5f = %.5f\n", uci_move, -parent->children[i].child->W, parent->children[i].child->N, parent->N, parent->children[i].child->N + 1, parent->children[i].P, (double)(chessEngine.optionSpin[ExplorationConstant].value) / 100.0, parent->children[i].child->N ? -parent->children[i].child->W / parent->children[i].child->N : 0.0, sqrt(parent->N) / (parent->children[i].child->N + 1) * parent->children[i].P * (double)(chessEngine.optionSpin[ExplorationConstant].value) / 100.0, parent->children[i].child->N ? -parent->children[i].child->W / parent->children[i].child->N + sqrt(parent->N) / (parent->children[i].child->N + 1) * parent->children[i].P * (double)(chessEngine.optionSpin[ExplorationConstant].value) / 100.0 : sqrt(parent->N) / (parent->children[i].child->N + 1) * parent->children[i].P * (double)(chessEngine.optionSpin[ExplorationConstant].value) / 100.0);      
      score = puct_score(parent, i, exploration_constant);
      if (score > best_score) {
        best_score = score;
        selected = i;
      }
    }

    // Apply virtual loss to selected child
    parent->mutex.lock_shared();
    parent->children[selected].child->N.fetch_add(1, std::memory_order_relaxed);
    parent->children[selected].child->W.fetch_sub(virtual_loss, std::memory_order_relaxed);
    parent->mutex.unlock_shared();
    return selected;
  }


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
  
  //return the eval result from the perspective of chess_board->fen->sideToMove
  double process_check(struct Board * chess_board, struct Move * move, struct NNUEContext * ctx, std::mt19937 rng) {
    struct Board * temp_board;
    if (move) {
      temp_board = cloneBoard(chess_board);
      move->chessBoard = temp_board;
      makeMove(move); //to preserve the perspective, negate the result of eval!
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
      best_value = -best_value; //negate the result of nnue eval to preserve the perspective - move was made!
    }
    return best_value;
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
    //print("mcts_search(%d) entered...\n", params->thread_id);
    char uci_move[6];
    struct Move move;
    params->currentNodes = 0;
    std::vector<struct MCTSNode *> path;  // Track the path from root to leaf
    path.reserve(chessEngine.depth);
    for (int i = 0; i < params->num_sims && hash_full.load(std::memory_order_relaxed) < 1000; i++) {
      bool repetition3x = false;
      params->currentDepth = 0;
      struct MCTSNode * node = search.root;
      //start from the same initial position given by board (at the root node, i.e. at the top of the tree)
      //clone the board to preserve it for subsequent iterations
      struct Board * sim_board = cloneBoard(board);
      // Selection
      //iterate down the tree updating sim_board by initiating and making moves
      std::unordered_map<std::pair<uint64_t, uint64_t>, int, PairHash> position_history;
      while (node && node->num_children.load(std::memory_order_relaxed) > 0 && !sim_board->isStaleMate && !sim_board->isMate) { //traversal stops at a leaf or terminal node or at 3x repetition
        std::pair<uint64_t, uint64_t> pos_key(sim_board->zh->hash, sim_board->zh->hash2);
        if (position_history[pos_key] >= 2) { // 3rd occurrence
            repetition3x = true;
            break;
        }
        position_history[pos_key]++;
        //return child node with the best score using PUCT (Predictor + Upper Confidence Bound)
        //print("mcts_search(%d) calling select_best_child()...\n", params->thread_id);
        int idx = select_best_child(node, sim_board, exploration_constant);
        //print("mcts_search(%d) returned from select_best_child()...\n", params->thread_id);
        if (idx < 0) {
          print("mcts_search() error: select_best_child() returned negative index\n");
          exit(-1);
        }
        //print("select_best_child() debug: selected move is %s%s, fen %s\n", squareName[node->children[idx].move / 64], squareName[node->children[idx].move % 64], sim_board->fen->fenString);
        //init edge's move that leads to the child node
        node->mutex.lock_shared();
        int move_idx = node->children[idx].move;
        node->mutex.unlock_shared();
    		init_move(&move, sim_board, move_idx / 64, move_idx % 64);
        //make the move
        makeMove(&move); //this updates sim_board
        //update Zobrist hash (it is needed so that we can call updateHash later instead of getHash)
  			if (updateHash(sim_board, &move)) {
  				print("mcts_search() error: updateHash() returned non-zero value\n");
  				exit(-1);
  			}
        //continue iterating down the tree until no more children or the end of the game is reached
        path.push_back(node);  // Add node to path
        std::shared_lock lock(node->mutex);
        node = node->children[idx].child;
        lock.unlock();
        params->currentDepth++;
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
				//print("mcts_search() debug: checkmate for %s, fen %s\n", color[sim_board->fen->sideToMove], sim_board->fen->fenString);
      } else if (sim_board->isStaleMate || repetition3x) {
        result = evaluate_nnue(sim_board, NULL, ctx);
        if (result > 1) result = -0.8;
        else if (result < -1) result = 0.8;
        else result = 0.0;        
				//print("mcts_search() debug: stalemate or 3x repetition for %s, fen %s\n", color[sim_board->fen->sideToMove], sim_board->fen->fenString);
      } else {
        //evaluate_nnue() returns result in pawns (not centipawns!) from sim_board->fen->sideToMove perspective
        result = evaluate_nnue(sim_board, NULL, ctx);
  			//print("mcts_search() debug: evaluate_nnue result %f, fen %s\n", result, sim_board->fen->fenString);
        result = tanh(result / EVAL_SCALE);
      }
      // Expansion - add more children - increase the depth of the tree down using the model's predictions, NNUE evals or randomly
      if (params->currentDepth < chessEngine.depth && !sim_board->isMate && !sim_board->isStaleMate && !repetition3x && node->mutex.try_lock() && hash_full.load(std::memory_order_relaxed) < 1000) {
        int src, dst, effective_branching = 1;
        double res;
        std::vector<std::pair<double, int>> move_evals;
      	enum PieceName side = (enum PieceName)((sim_board->fen->sideToMove << 3) | PieceTypeAny);//either PieceNameWhite or PieceNameBlack
      	unsigned long long any = sim_board->occupations[side];
      	std::uniform_real_distribution<double> uniform(-noise, noise);
      	while (any) {
      	  src = lsBit(any);
      	  unsigned long long moves = sim_board->sideToMoveMoves[src];
      	  while (moves) {
      	    dst = lsBit(moves);
          	struct Board * tmp_board = cloneBoard(sim_board);
            init_move(&move, tmp_board, src, dst);
            makeMove(&move); //the move is made - negate the result of eval to preserve sim_board perspective!
          	int pieceCount = bitCount(tmp_board->occupations[PieceNameAny]);
            if (pieceCount > TB_LARGEST || tmp_board->fen->halfmoveClock || tmp_board->fen->castlingRights) {
              //evaluate_nnue() returns result in pawns (not centipawns!)
              //we made the move, so the eval res is from the perspective of tmp_board->fen->sideToMove
              //or sim_board->opponentColor and must be negated to preserve the perspective of sim_board->fen->sideToMove!
              if (tmp_board->isMate) res = MATE_SCORE * 0.01; //sim_board->fen->sideToMove wins
              else if (tmp_board->isCheck) res = -process_check(tmp_board, NULL, ctx, params->rng);
              else res = -evaluate_nnue(tmp_board, NULL, ctx);
            } else { //pieceCount <= TB_LARGEST, etc
              unsigned int ep = lsBit(tmp_board->fen->enPassantLegalBit);
              unsigned int wdl = tb_probe_wdl(tmp_board->occupations[PieceNameWhite], tmp_board->occupations[PieceNameBlack], tmp_board->occupations[WhiteKing] | tmp_board->occupations[BlackKing],
                tmp_board->occupations[WhiteQueen] | tmp_board->occupations[BlackQueen], tmp_board->occupations[WhiteRook] | tmp_board->occupations[BlackRook], tmp_board->occupations[WhiteBishop] | tmp_board->occupations[BlackBishop], tmp_board->occupations[WhiteKnight] | tmp_board->occupations[BlackKnight], tmp_board->occupations[WhitePawn] | tmp_board->occupations[BlackPawn],
                0, 0, ep == 64 ? 0 : ep, tmp_board->opponentColor == ColorBlack ? 1 : 0);
              if (res == TB_RESULT_FAILED) {
                print("error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, occupations %u, fen %s, ep %u, halfmoveClock %u, whiteToMove %u, whites %llu, blacks %llu, kings %llu, queens %llu, rooks %llu, bishops %llu, knights %llu, pawns %llu, err %s\n", TB_LARGEST, pieceCount, tmp_board->fen->fenString, ep, tmp_board->fen->halfmoveClock, tmp_board->opponentColor == ColorBlack ? 1 : 0, tmp_board->occupations[PieceNameWhite], tmp_board->occupations[PieceNameBlack], tmp_board->occupations[WhiteKing] | tmp_board->occupations[BlackKing], tmp_board->occupations[WhiteQueen] | tmp_board->occupations[BlackQueen], tmp_board->occupations[WhiteRook] | tmp_board->occupations[BlackRook], tmp_board->occupations[WhiteBishop] | tmp_board->occupations[BlackBishop], tmp_board->occupations[WhiteKnight] | tmp_board->occupations[BlackKnight], tmp_board->occupations[WhitePawn] | tmp_board->occupations[BlackPawn], strerror(errno));
                if (tmp_board->isMate) res = MATE_SCORE * 0.01;
                else if (tmp_board->isCheck) res = -process_check(tmp_board, NULL, ctx, params->rng);
                else res = -evaluate_nnue(tmp_board, NULL, ctx);
              } else { //tb_probe_wdl() succeeded
                //0 - loss, 4 - win, 1..3 - draw
                if (wdl == 4) res = -MATE_SCORE * 0.001;
                else if (wdl == 0) res = MATE_SCORE * 0.001;
                else res = 0.0;
                tbhits.fetch_add(1, std::memory_order_relaxed);
              }
            } //end of else (pieceCount <= TB_LARGEST)
            res += res * uniform(params->rng);
            move_evals.push_back({res, src * 64 + dst});
      		  moves &= moves - 1;
            freeBoard(tmp_board);
      	  } //end of while(moves)
          any &= any - 1;
        } //end of while(any)
        std::sort(move_evals.begin(), move_evals.end(), std::greater<>()); //sorted in descending order
        if (result == NNUE_CHECK) {
          result = tanh(move_evals[0].first / EVAL_SCALE);
        }
        effective_branching = get_prob(move_evals, probability_mass);
        // Slice to top effective
        move_evals.resize(effective_branching);
        //print("mcts_search(%d) calling expand_node()...\n", params->thread_id);
        expand_node(node, sim_board, move_evals);
        node->mutex.unlock();
        //print("mcts_search(%d) returned from expand_node()...\n", params->thread_id);          
        move_evals.clear();
      } //end of if (params->currentDepth < chessEngine.depth && !sim_board->isMate && !sim_board->isStaleMate && node->mutex.try_lock() && hash_full.load(std::memory_order_relaxed) < 1000) 
      // Backpropagation: update node visits and results
      for (auto n = path.rbegin(); n != path.rend(); ++n) {
        node = *n;
        node->N.fetch_add(1, std::memory_order_relaxed);
        node->W.fetch_add(result, std::memory_order_relaxed);
        result = -result;
      }
      // Revert virtual loss for the selected path (skip root, as no loss was applied to it)
      for (size_t j = 1; j < path.size(); ++j) {  // From first child to leaf
        struct MCTSNode * nd = path[j];
        nd->N.fetch_sub(1, std::memory_order_relaxed);
        nd->W.fetch_add(virtual_loss, std::memory_order_relaxed);
      }      
      params->currentNodes++;      
      path.clear();
      freeBoard(sim_board);
      if (params->currentDepth > params->seldepth) params->seldepth = params->currentDepth;   
      if ((chessEngine.depth && params->currentDepth >= chessEngine.depth) || (chessEngine.nodes && params->currentNodes >= chessEngine.nodes)) break;
    } //end of the iterations loop
    //print("mcts_search(%d) exiting...\n", params->thread_id);
  }
    
  void thread_search(ThreadParams * params) {
    struct NNUEContext ctx;
    init_nnue_context(&ctx);
    auto iter_start = std::chrono::steady_clock::now();
    double elapsed = 0.0;
    while (elapsed < (timeAllocated * 0.001) && hash_full.load(std::memory_order_relaxed) < 1000) {
        mcts_search(params, &ctx);
        
        // Update max depth
        int expected = engineDepth.currentDepth.load(std::memory_order_relaxed);
        while (params->currentDepth > expected && !engineDepth.currentDepth.compare_exchange_strong(expected, params->currentDepth, std::memory_order_relaxed)) {
            expected = engineDepth.currentDepth.load(std::memory_order_relaxed);
        }
        // Update max seldepth
        expected = engineDepth.seldepth.load(std::memory_order_relaxed);
        while (params->seldepth > expected && !engineDepth.seldepth.compare_exchange_strong(expected, params->seldepth, std::memory_order_relaxed)) {
            expected = engineDepth.seldepth.load(std::memory_order_relaxed);
        }
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
    }
    free_nnue_context(&ctx);
  }
  
  void uci_output_thread() {
    std::mt19937 rng(std::random_device{}());
    auto iter_start = std::chrono::steady_clock::now();

    struct NNUEContext ctx;
    init_nnue_context(&ctx);
    while (!search_done.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      
      // Calculate nodes (total simulations)
      unsigned long long nodes = search.root->N.load(std::memory_order_relaxed);
            
      // Get elapsed time
      double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();

      // Compute NPS
      double nps = elapsed > 0 ? nodes / elapsed : 0;
      
      int * move_idx = nullptr;
      char ** pv = nullptr;
      int multiPV = select_best_move(&pv, &move_idx);
      //Get NNUE eval for the best_move (move_idx)
      int depth = engineDepth.currentDepth.load(std::memory_order_relaxed);
      int seldepth = engineDepth.seldepth.load(std::memory_order_relaxed);
      std::shared_lock lock(map_mutex);
      size_t unique_nodes = search.tree.size();
      lock.unlock();
      size_t total_memory = unique_nodes * (sizeof(MCTSNode) + 24) + total_children.load(std::memory_order_relaxed) * sizeof(Edge);
      size_t max_capacity = chessEngine.optionSpin[Hash].value * 1024 * 1024;  // MB to bytes
      int hashfull = max_capacity ? (total_memory * 1000) / max_capacity : 0;
      if (hashfull > 1000) hashfull = 1000;  // Cap at 1000 per UCI spec
      hash_full.store(hashfull, std::memory_order_relaxed);

      for (int i = 0; i < multiPV && move_idx && pv; i++) {
        struct Move move;
        struct Board * temp_board = cloneBoard(board);
        init_move(&move, temp_board, move_idx[i] / 64, move_idx[i] % 64);
        double res = evaluate_nnue(temp_board, &move, &ctx);
        if (res == NNUE_CHECK) //we need to resolve the check to get NNUE score
          res = process_check(temp_board, &move, &ctx, rng); 
        freeBoard(temp_board); 
        print("info depth %d seldepth %d multipv %d score cp %ld nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", depth, seldepth, i + 1, (long)(res * 100), nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, pv[i]);
      }
      if (pv) {
        for (int i = 0; i < multiPV; i++) {
          free(pv[i]);
        }
        free(pv);
      }
      if (move_idx) {
        free(move_idx);
      }
    }
    free_nnue_context(&ctx);
  }
  
  void runMCTS() {
    double elapsed = 0.0;
    int * move_idx = nullptr;
    size_t unique_nodes = 0;
    unsigned long long nodes = 0;
    int hashfull = 0;
    int move_number = bitCount(board->moves);
    char ** pv = nullptr;
    int multiPV = 1;
    std::mt19937 rng(std::random_device{}());

    if (move_number > 1) {
      //total_children.store(0, std::memory_order_relaxed);
      tbhits.store(0, std::memory_order_relaxed);
      exploration_constant = (double)chessEngine.optionSpin[ExplorationConstant].value * 0.01;
      probability_mass = (double)chessEngine.optionSpin[ProbabilityMass].value * 0.01;
      noise = (double)chessEngine.optionSpin[Noise].value * 0.01;
      virtual_loss = chessEngine.optionSpin[VirtualLoss].value;
      std::vector<ThreadParams> thread_params(chessEngine.optionSpin[Threads].value);
      for (int i = 0; i < chessEngine.optionSpin[Threads].value; ++i) {
        thread_params[i].thread_id = i;
        thread_params[i].num_sims = MIN_ITERATIONS;  // Or time-based loop inside thread
        thread_params[i].currentNodes = 0;
        thread_params[i].currentDepth = 0;
        thread_params[i].seldepth = 0;
        thread_params[i].rng.seed(static_cast<unsigned int>(std::random_device{}() ^ std::hash<int>{}(i)));
      }
      
      set_root();
      gc();
      
      if (!chessEngine.depth) chessEngine.depth = MAX_DEPTH;
      engineDepth.currentDepth.store(0, std::memory_order_relaxed);
      engineDepth.seldepth.store(0, std::memory_order_relaxed);
      std::vector<std::thread> threads;
      auto iter_start = std::chrono::steady_clock::now();
      for (int i = 0; i < chessEngine.optionSpin[Threads].value; ++i) {
        threads.emplace_back(thread_search, &thread_params[i]);
      }
      search_done.store(false, std::memory_order_relaxed); // Reset
      std::thread output_thread(uci_output_thread);
      
      for (auto& t : threads) t.join();
      elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();

      search_done.store(true, std::memory_order_relaxed); // Signal search complete
      output_thread.join();
              
      multiPV = select_best_moves(&pv, &move_idx);
      if (multiPV > 0) {
        unsigned long long max_visits = 0;
        int best_index = 0;
        int num_children = search.root->num_children.load(std::memory_order_relaxed);
        for (int i = 0; i < num_children; i++) {
          unsigned long long visits = search.root->children[i].child->N.load(std::memory_order_relaxed);
          if (visits > max_visits) {
            max_visits = visits;
            best_index = i;
          }
        }
        idx_to_move(board, search.root->children[best_index].move, best_move);
      }      
      nodes = search.root->N.load(std::memory_order_relaxed);
      unique_nodes = search.tree.size();
      //cleanup(); //should be called on exit
      // Calculate hashfull (in per-mille)
      // hashfull using unique_nodes
      //size_t bytes_per_node = sizeof(MCTSNode) + 4 * sizeof(Edge) + 24;  // Avg 4 children? - depends on probability mass, 24B map overhead
      size_t total_memory = unique_nodes * (sizeof(MCTSNode) + 24) + total_children.load(std::memory_order_relaxed) * sizeof(Edge);
      size_t max_capacity = chessEngine.optionSpin[Hash].value * 1024 * 1024;  // MB to bytes
      hashfull = max_capacity ? (total_memory * 1000) / max_capacity : 0;
      if (hashfull > 1000) hashfull = 1000;  // Cap at 1000 per UCI spec
      hash_full.store(hashfull, std::memory_order_relaxed);
    } else if (move_number == 1) {
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
        move_idx = (int *)calloc(1, sizeof(int));
        if (!move_idx) {
          print("runMCTS() error: calloc() failed for move_idx\n");
          exit(-1);
        }
        move_idx[0] = src * 64 + dst;
        idx_to_move(board, move_idx[0], best_move);
        pv = (char **)calloc(1, sizeof(char *));
        pv[0] = (char *)calloc(1, 6);
        strcpy(pv[0], best_move);
        multiPV = 1;
        nodes = 1;
        engineDepth.currentDepth.store(1, std::memory_order_relaxed);
        engineDepth.seldepth.store(1, std::memory_order_relaxed);
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
    }
    else {
        if (board->isMate) {
            print("info depth 0 score mate 0\n");
            print("bestmove (none)\n");
        }
        else if (board->isStaleMate) {
            print("info depth 0 score cp 0\n");
            print("bestmove (none)\n");
        }
        return;
    }
    
    double nps = nodes / elapsed;
    int depth = engineDepth.currentDepth.load(std::memory_order_relaxed);
    int seldepth = engineDepth.seldepth.load(std::memory_order_relaxed);
    struct Move move;
    struct NNUEContext ctx;
    init_nnue_context(&ctx);
    for (int i = 0; i < multiPV && move_idx && pv; i++) {
      struct Board * temp_board = cloneBoard(board);
      //Get NNUE eval for the best_move (move_idx)
      init_move(&move, temp_board, move_idx[i] / 64, move_idx[i] % 64);
      double res = evaluate_nnue(temp_board, &move, &ctx);
      if (res == NNUE_CHECK) //we need to resolve the check to get NNUE score
        res = process_check(temp_board, &move, &ctx, rng);
      freeBoard(temp_board);
      print("info depth %d seldepth %d multipv %d score cp %ld nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s timeAllocated %.2f\n", depth, seldepth, i + 1, (long)(res * 100), nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, pv[i], timeAllocated * 0.001);
    }
    free_nnue_context(&ctx);
    print("bestmove %s\n", best_move);    
    if (pv) {
      for (int i = 0; i < multiPV; i++) free(pv[i]);
      free(pv);
    }
    if (move_idx) free(move_idx);
  }
}

void setEngineOptions() {
    strcpy(chessEngine.id, "Creatica Chess Engine 1.0");
    strcpy(chessEngine.authors, "Creatica");
    chessEngine.numberOfCheckOptions = 0;
	  chessEngine.numberOfComboOptions = 0;
	  chessEngine.numberOfSpinOptions = 8;
		chessEngine.numberOfStringOptions = 1;
		chessEngine.numberOfButtonOptions = 0;
	  strcpy(chessEngine.optionString[SyzygyPath].name, "SyzygyPath");
	  strcpy(chessEngine.optionString[SyzygyPath].defaultValue, SYZYGY_PATH_DEFAULT);
	  strcpy(chessEngine.optionString[SyzygyPath].value, SYZYGY_PATH);
	  if (chessEngine.optionString[SyzygyPath].value[0]) {
      tb_init(chessEngine.optionString[SyzygyPath].value);
      if (TB_LARGEST == 0) {
          print("info string error unable to initialize tablebase; no tablebase files found in %s\n", chessEngine.optionString[SyzygyPath].value);
      } else {
        print("info string successfully initialized tablebases in %s. Max number of pieces %d\n", chessEngine.optionString[SyzygyPath].value, TB_LARGEST);
      }
	  } else print("setEngineOptions() error: syzygy path is 0\n");
	  strcpy(chessEngine.optionSpin[Hash].name, "Hash");
	  chessEngine.optionSpin[Hash].defaultValue = HASH;
	  chessEngine.optionSpin[Hash].value = chessEngine.optionSpin[Hash].defaultValue;
	  chessEngine.optionSpin[Hash].min = 128;
	  chessEngine.optionSpin[Hash].max = 4096;
	  strcpy(chessEngine.optionSpin[Threads].name, "Threads");
	  chessEngine.optionSpin[Threads].defaultValue = THREADS;
	  chessEngine.optionSpin[Threads].value = chessEngine.optionSpin[Threads].defaultValue;
	  chessEngine.optionSpin[Threads].min = 1;
	  chessEngine.optionSpin[Threads].max = 8;
	  strcpy(chessEngine.optionSpin[MultiPV].name, "MultiPV");
	  chessEngine.optionSpin[MultiPV].defaultValue = MULTI_PV;
	  chessEngine.optionSpin[MultiPV].value = chessEngine.optionSpin[MultiPV].defaultValue;
	  chessEngine.optionSpin[MultiPV].min = 1;
	  chessEngine.optionSpin[MultiPV].max = 8;
	  strcpy(chessEngine.optionSpin[ProbabilityMass].name, "ProbabilityMass");
	  chessEngine.optionSpin[ProbabilityMass].defaultValue = PROBABILITY_MASS;
	  chessEngine.optionSpin[ProbabilityMass].value = chessEngine.optionSpin[ProbabilityMass].defaultValue;
	  chessEngine.optionSpin[ProbabilityMass].min = 1;
	  chessEngine.optionSpin[ProbabilityMass].max = 100;
	  strcpy(chessEngine.optionSpin[ExplorationConstant].name, "ExplorationConstant");
	  chessEngine.optionSpin[ExplorationConstant].defaultValue = EXPLORATION_CONSTANT;
	  chessEngine.optionSpin[ExplorationConstant].value = chessEngine.optionSpin[ExplorationConstant].defaultValue;
	  chessEngine.optionSpin[ExplorationConstant].min = 0;
	  chessEngine.optionSpin[ExplorationConstant].max = 200;
	  strcpy(chessEngine.optionSpin[Noise].name, "Noise");
	  chessEngine.optionSpin[Noise].defaultValue = NOISE;
	  chessEngine.optionSpin[Noise].value = chessEngine.optionSpin[Noise].defaultValue;
	  chessEngine.optionSpin[Noise].min = 1;
	  chessEngine.optionSpin[Noise].max = 30;
	  strcpy(chessEngine.optionSpin[VirtualLoss].name, "VirtualLoss");
	  chessEngine.optionSpin[VirtualLoss].defaultValue = VIRTUAL_LOSS;
	  chessEngine.optionSpin[VirtualLoss].value = chessEngine.optionSpin[VirtualLoss].defaultValue;
	  chessEngine.optionSpin[VirtualLoss].min = 0;
	  chessEngine.optionSpin[VirtualLoss].max = 10;
	  strcpy(chessEngine.optionSpin[PVPlies].name, "PVPlies");
	  chessEngine.optionSpin[PVPlies].defaultValue = PV_PLIES;
	  chessEngine.optionSpin[PVPlies].value = chessEngine.optionSpin[PVPlies].defaultValue;
	  chessEngine.optionSpin[PVPlies].min = 1;
	  chessEngine.optionSpin[PVPlies].max = 32;
	  strcpy(chessEngine.optionSpin[EvalScale].name, "EvalScale");
	  chessEngine.optionSpin[EvalScale].defaultValue = EVAL_SCALE;
	  chessEngine.optionSpin[EvalScale].value = chessEngine.optionSpin[EvalScale].defaultValue;
	  chessEngine.optionSpin[EvalScale].min = 2;
	  chessEngine.optionSpin[EvalScale].max = 8;
    chessEngine.wtime = 1e9;
    chessEngine.btime = 1e9;
    chessEngine.winc = 0;
    chessEngine.binc = 0;
    chessEngine.movestogo = 0;
    chessEngine.movetime = 0;
    chessEngine.depth = 0;
    chessEngine.nodes = 0;
    chessEngine.infinite = false;
}

int main(int argc, char ** argv) {
  TB_LARGEST = 0;
  struct Fen fen;
  struct ZobristHash zh;
  struct Board chess_board;
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
	if (fentoboard(&fen, &chess_board)) {
		printf("test_nnue error: fentoboard() failed; FEN %s\n", fen.fenString);
		return 1;
	}
  zobristHash(&zh);
  chess_board.zh = &zh;
  board = &chess_board;
  //init_nnue("nn-1111cefa1111.nnue", "nn-37f18f62d772.nnue");
  init_nnue("nn-1c0000000000.nnue", "nn-37f18f62d772.nnue");
  setEngineOptions();
	runMCTS();
  cleanup_nnue();
  cleanup_magic_bitboards();
  return 0;
}









