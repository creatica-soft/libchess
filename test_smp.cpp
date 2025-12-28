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
#include <condition_variable>
#include <queue>
#include <cstdarg>
#endif
#include <vector>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <random>
#include <chrono>
#include <algorithm>
#include <math.h>
//#include <fstream>
#include <iostream>
#include "tbprobe.h"
#include "libchess.h"

//#define MAX_NOISE 3
#define TEMPERATURE 60
#define MAX_DEPTH 100
#define THREADS 8
#define MULTI_PV 10
#define SYZYGY_PATH_DEFAULT "<empty>"
#define SYZYGY_PATH "/Users/ap/syzygy"
#define HASH 1024 //default, GUI may set it via Hash option (once full, expansion won't happen!)
#define EXPLORATION_MIN 40 // used in formular for exploration constant decay with depth
#define EXPLORATION_MAX 120 //smaller value favor exploitation, i.e. deeper tree vs wider tree
#define EXPLORATION_DEPTH_DECAY 6 //linear decay of EXPLORATION CONSTANT with depth using formula:
                      // C * 100 = max(EXPLORATION_MIN, (EXPLORATION_MAX - seldepth * EXPLORATION_DEPTH_DECAY))
#define PROBABILITY_MASS 100 //% - cumulative probability - how many moves we consider
#define VIRTUAL_LOSS 4
#define PV_PLIES 16
#define EVAL_SCALE 6.0 //used in conversion to result = tanh(cp * 0.01 / eval_scale), i.e.
                       //how pessimistic evaluation in pawns (cp * 0.01) is, higher values -> lower result
                       //another words, what is considered a definite win or loss in pawns
                       //for example, for an extra queen, result = tanh(9 / 6) = 0.9 - almost a win, it is close to 1
                       //but if eval_scale = 4, then tanh(9 / 4) = ~0.98 - even closer to 1

double timeAllocated = 10000; //ms
char best_move[6];
std::atomic<bool> search_done{false}; // Signals search completion
std::condition_variable cv_search_done;
std::mutex search_done_mtx;
double exploration_max, exploration_min, exploration_depth_decay;
double probability_mass;
//double noise;
double virtual_loss;
double eval_scale;
double temperature;

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
  void compute_move_evals(struct Board * chess_board, struct NNUEContext * ctx, const std::unordered_set<unsigned long long>& pos_history, std::vector<std::tuple<double, int, int, unsigned long long/*, unsigned long long*/>>& move_evals, double prob_mass);//, bool in_playout = false);
  double position_eval(struct Board * chess_board, struct NNUEContext * ctx, const std::unordered_set<unsigned long long>& pos_history);
  
  extern struct Board * board;
  extern struct Engine chessEngine;
  std::mutex probe_mutex, log_mtx, print_mtx;
  std::shared_mutex map_mutex;
  std::atomic<unsigned long long> total_children{0};
  std::atomic<int> hash_full{0};
  std::atomic<unsigned long long> tbhits{0};
  std::atomic<int> generation{0};
  
  std::atomic<int> depth{0};
  std::atomic<int> seldepth{0};
    
  struct Edge {
      std::atomic<int> move {0};             // The move that leads to the child position
      std::atomic<double> P {0.0};            // Prior probability - model move_probs for a given move in the node
      std::atomic<struct MCTSNode *> child {nullptr}; // Pointer to the child node
  };

  struct MCTSNode {
      std::atomic<unsigned long long> hash{0};
      //std::atomic<unsigned long long> hash2{0};
      std::atomic<unsigned long long> N{0};  // Atomic for lock-free updates
      std::atomic<double> W{0};
      std::atomic<int> cp {NO_MATE_SCORE}; //position evaluation in centipawns 
      std::atomic<int> num_children{0};
      std::atomic<int> generation{0};
      std::shared_mutex mutex;  // For protecting children expansion
      std::atomic<struct Edge *> children {nullptr};
  };
  
  // Custom hasher that uses the key directly
  struct NoOpHash {
      std::size_t operator()(unsigned long long key) const noexcept {
          return key; // Directly use the key as the hash
      }
  };
  
  struct MCTSSearch {
      struct MCTSNode * root = nullptr;
      std::unordered_map<unsigned long long, MCTSNode *, NoOpHash> tree;
  };

  static MCTSSearch search;
  
  // Remove MCTSSearch from ThreadParams (shared now)
  struct ThreadParams {
      int thread_id;
      unsigned long long time_alloc;
      int seldepth;
      //std::mt19937 rng;
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
        struct Edge * children = node->children.load(std::memory_order_relaxed);
        delete[] children;
        delete node;
    }
    search.tree.clear();
    search.root = nullptr;
  }
  
  //we run gc() in runMCTS() before starting search threads, so no locking
  void gc() {
    assert(search.root);
    //fetch_add() updates generation but returns original value before addition; hence, we add 1
    int current_gen = generation.fetch_add(1, std::memory_order_relaxed) + 1;
    // BFS traversal to mark reachable nodes with the current generation.
    // Use queue to avoid recursion and potential stack overflow in deep trees.
    std::queue<MCTSNode *> q;
    // Update generation for root and push it to the queue
    search.root->generation.store(current_gen, std::memory_order_relaxed);
    q.push(search.root);

    while (!q.empty()) {
        MCTSNode * node = q.front();
        q.pop();  
        int num_children = node->num_children.load(std::memory_order_relaxed);
        struct Edge * children = node->children.load(std::memory_order_relaxed);
        for (int i = 0; i < num_children; ++i) {
            MCTSNode * child = children[i].child.load(std::memory_order_relaxed);
            if (child) {
                // Atomically update generation if it's outdated to avoid revisiting.
                int expected = current_gen - 1;
                //compare_exchange_strong<weak> logic: 
                //if (child->generation == expected) child->generation = current_gen; else expected = child->generation; 
                //weak form allows spurious failures and works faster in loops
                if (child->generation.compare_exchange_strong(expected, current_gen, std::memory_order_relaxed)) {
                    q.push(child); //only push to the queue those nodes (that are reachable) that we updated
                }
            }
        }
    }
    // Now iterate through the map and erase nodes with outdated generations, i.e. nodes that are not reachable
    // Also clean up allocated children arrays.
    for (auto it = search.tree.begin(); it != search.tree.end();) {
      MCTSNode * node = it->second;
      if (node->generation.load(std::memory_order_relaxed) < current_gen) {
        // Clean up dynamically allocated children if any.
        struct Edge * children = node->children.load(std::memory_order_relaxed);
        int num_children = node->num_children.load(std::memory_order_relaxed);
        if (num_children > 0) {
          total_children.fetch_sub(num_children, std::memory_order_relaxed); //update total_children count
          delete[] children;
        }
        it = search.tree.erase(it);
        delete node;
      } else ++it;
    }
    //update hash_full
    size_t total_memory = search.tree.size() * (sizeof(MCTSNode) + 24) + total_children.load(std::memory_order_relaxed) * sizeof(Edge);
    size_t max_capacity = chessEngine.optionSpin[Hash].value * 1024 * 1024;  // MB to bytes
    int hashfull = max_capacity ? (total_memory * 1000) / max_capacity : 0;
    if (hashfull > 1000) hashfull = 1000;  // Cap at 1000 per UCI spec
    hash_full.store(hashfull, std::memory_order_relaxed);    
  }
  
  //no locking, call it before starting other threads (search, etc)
  void set_root() {
    auto it = search.tree.find(board->zh->hash);
    struct MCTSNode * root = (it != search.tree.end()) ? it->second : nullptr;
    if (!root) {
      root = new MCTSNode();
      root->hash.store(board->zh->hash, std::memory_order_relaxed);
      //root->hash2.store(board->zh->hash2, std::memory_order_relaxed);
      root->generation.store(generation.load(std::memory_order_relaxed), std::memory_order_relaxed);
      struct NNUEContext ctx;
      init_nnue_context(&ctx);
      std::unordered_set<unsigned long long> pos_history;
      double res = position_eval(board, &ctx, pos_history); //calls generateMoves()
      free_nnue_context(&ctx);
      root->cp.store(static_cast<int>(res * 100), std::memory_order_relaxed);
      //we should probably update N and W as well. We're storing cp, the node is evaluated, meaning it's been visited
      root->N.store(1, std::memory_order_relaxed);
      root->W.store(tanh(res / eval_scale), std::memory_order_relaxed);      
      search.tree.emplace(board->zh->hash, root);
    } /*else {
      if (root->hash2 != board->zh->hash2) {
        updateFen(board);
        print("set_root() error: Zobrist hash collision detected, hash %llu hash2 %llu fen %s\n", board->zh->hash, board->zh->hash2, board->fen->fenString);
        exit(1);
      }
    }*/
    search.root = root;    
  }
  
  //called from expand_node() and next_moves()
  //returns new or existing node
  struct MCTSNode * make_child(const unsigned long hash, /*const unsigned long hash2,*/ const int cp) {
    struct MCTSNode * child = nullptr;
    //first, try to find hash in the tree
    std::shared_lock search_lock(map_mutex);
    auto it = search.tree.find(hash);
    child = (it != search.tree.end()) ? it->second : nullptr;
    search_lock.unlock();
    //if (child) {
    if (!child) { //if the hash is not found, create a child
      /*if (child->hash2.load(std::memory_order_relaxed) != hash2) {
        print("make_child() error: hash collision detected: hash %llu, hash2 %llu != hash2 %llu\n", hash, child->hash2.load(std::memory_order_relaxed), hash2);
        exit(-1); //need to decide how to better handle it later (perhaps, use both hash and hash2 for indexing)
      }*/
      //int child_cp = child->cp.load(std::memory_order_relaxed);
      //if (cp != NO_MATE_SCORE && child_cp != cp) print("make_child() warning: child exists but its cp %d is not equal to recent cp %d. Child N %lld, W %f\n", child_cp, cp, child->N.load(std::memory_order_relaxed), child->W.load(std::memory_order_relaxed));
    //} else { //then, if the hash is not found, create a child
      child = new MCTSNode();
      child->hash.store(hash, std::memory_order_relaxed);
      //child->hash2.store(hash2, std::memory_order_relaxed);
      child->generation.store(generation.load(std::memory_order_relaxed));
      child->cp.store(cp, std::memory_order_relaxed);
      //we should probably update N and W as well. We're updating cp, the node is evaluated, meaning it's been visited
      if (cp != NO_MATE_SCORE) {
        child->N.store(1, std::memory_order_relaxed);
        child->W.store(tanh(cp * 0.01 / eval_scale), std::memory_order_relaxed);
      }
      std::unique_lock insert_lock(map_mutex);
      auto [it, inserted] = search.tree.emplace(hash, child);
      insert_lock.unlock();
      if (!inserted) {
        // Another thread inserted first; use the existing node and clean up our child.
        delete child;
        child = it->second; //it->second - is a pointer to the existing node (it->first is a hash)
        // Verify no collision on the existing node.
        /*if (child->hash2.load(std::memory_order_relaxed) != hash2) {
          print("make_child() error: hash collision detected in insert: hash %llu, hash2 %llu != hash2 %llu\n", hash, child->hash2.load(std::memory_order_relaxed), hash2);
          exit(-1); // Or handle as needed.
        }*/
      }
    }
    return child; //may not be nullptr
  }
    
  struct TempEdge {
      int move = 0;
      double P = 0.0; 
      struct MCTSNode * child = nullptr;
  };
  //called from mcts_search() and next_moves()
  //calls make_child()
  void expand_node(struct MCTSNode * parent, const std::vector<std::tuple<double, int, int, unsigned long long/*, unsigned long long*/>>& top_moves, const std::unordered_set<unsigned long long>& pos_history) {
    //parent is locked for the expansion with unique_lock in caller - mcts_search() or next_moves()
    if (parent->num_children.load(std::memory_order_relaxed) > 0) return; //already expanded by other threads, perhaps
    int num_moves = top_moves.size();
    assert(num_moves > 0);
    struct Edge * children = new Edge[num_moves];
    for (int i = 0; i < num_moves; ++i) {
        auto [prior, move_idx, child_cp, child_hash/*, child_hash2*/] = top_moves[i];
        struct MCTSNode * child = make_child(child_hash, /*child_hash2,*/ child_cp);
        children[i].P.store(prior, std::memory_order_relaxed);
        children[i].move.store(move_idx, std::memory_order_relaxed);
        children[i].child.store(child, std::memory_order_relaxed);        
    }
    total_children.fetch_add(num_moves, std::memory_order_relaxed); //update total_children counter
    //below are two separate atomic operations: first - for the children pointer, second - for num_children
    //we need to remember this when processing children in other threads!
    //for instance, children might be a valid pointer but num_children might be 0!
    //so checking for num_children > 0 means that children is not null, right?
    parent->children.store(children, std::memory_order_release);
    parent->num_children.store(num_moves, std::memory_order_release);    
  }
  
  //no locking, call only when search threads finished
  //called from select_best_moves(), which in turn is called from runMCTS()
  int most_visited_child(struct MCTSNode * parent) {
    unsigned long long N = 0;
    int idx = -1;
    std::vector<std::pair<double, int>> priors; //prior, child index
    int num_children = parent->num_children.load(std::memory_order_relaxed); // will be 0 for the last node
    struct Edge * children = parent->children.load(std::memory_order_relaxed); // will be nullptr for the last node
    for (int i = 0; i < num_children; i++) { //this loop will be skipped for the last node
      struct MCTSNode * child = children[i].child.load(std::memory_order_relaxed);
      unsigned long long n = child->N.load(std::memory_order_relaxed);
      priors.push_back({children[i].P.load(std::memory_order_relaxed), i});
      if (n > N) {
        N = n;
        idx = i;
      } 
    }
    //in case of unexpanded child (node before last, last has no children), return idx of best prior and hence, the best scorecp
    if (idx == -1 && num_children > 0) {
      std::sort(priors.begin(), priors.end(), [](const auto& a, const auto& b) { return a.first > b.first;});
      return priors[0].second;
    }
    return idx; //this will be negative for the last node
  }

  //no locking, call only when search threads finished
  //called from runMCTS()
  //returns multiPV and PVs
  int select_best_moves(std::vector<std::pair<int, std::string>>& pvs) { 
    char uci_move[6];
    std::vector<std::tuple<unsigned long long, int>> visits; //N, child_idx
    int num_children = search.root->num_children.load(std::memory_order_relaxed);
    if (!num_children) return 0;
    struct Edge * children = search.root->children.load(std::memory_order_relaxed);
    for (int i = 0; i < num_children; i++) {
      struct MCTSNode * child = children[i].child.load(std::memory_order_relaxed);
      visits.push_back({child->N.load(std::memory_order_relaxed), i});
    }
    std::sort(visits.begin(), visits.end(), std::greater()); //sort children desc by the number of visits N    
    int num_visits = visits.size();
    int multiPV = std::min<int>(num_visits, (int)chessEngine.optionSpin[MultiPV].value);
    int pvLength = chessEngine.optionSpin[PVPlies].value * sizeof(uci_move);
    int maxLen = pvLength - sizeof(uci_move);
    for (int i = 0; i < multiPV; i++) {
      int index = std::get<1>(visits[i]);
      idx_to_move(children[index].move.load(std::memory_order_relaxed), uci_move);
      struct MCTSNode * child = children[index].child.load(std::memory_order_relaxed);      
      int cp = -child->cp.load(std::memory_order_relaxed);
      double parent_N = static_cast<double>(search.root->N.load(std::memory_order_relaxed));
      double prior = children[index].P.load(std::memory_order_relaxed);
      unsigned long long N = child->N.load(std::memory_order_relaxed);
      double W = -child->W.load(std::memory_order_relaxed);
      double Q = W / N;
      double U = exploration_max * prior * sqrt(parent_N) / (1 + N);
      std::string pv(uci_move);
      std::string pv2(uci_move);
      pv2 += " (" + std::to_string(N) + ", " + std::to_string(llround(W)) + ", " + std::to_string(cp) + ", " + std::to_string(Q) + " + " + std::to_string(U) + " = " + std::to_string(Q + U) + ")";
      // Build PV by following most visited children
      num_children = child->num_children.load(std::memory_order_relaxed);
      struct Edge * children2 = child->children.load(std::memory_order_relaxed);
      int depth = 0;
      while (num_children > 0 && pv.size() < maxLen) {
        int idx = most_visited_child(child); 
        if (idx < 0) break;
        depth++;
        parent_N = child->N.load(std::memory_order_relaxed);
        prior = children2->P.load(std::memory_order_relaxed);
        idx_to_move(children2[idx].move.load(std::memory_order_relaxed), uci_move);
        pv += ' ';
        pv.append(uci_move);  
        pv2 += ' ';
        pv2.append(uci_move);   
        child = children2[idx].child.load(std::memory_order_relaxed);
        cp = -child->cp.load(std::memory_order_relaxed);
        N = child->N.load(std::memory_order_relaxed);
        W = -child->W.load(std::memory_order_relaxed);
        Q = W / N;
        U = std::max(exploration_min, exploration_max - (depth * exploration_depth_decay)) * exploration_max * prior * sqrt(parent_N) / (1 + N);
        pv2 += " (" + std::to_string(N) + ", " + std::to_string(llround(W)) + ", " + std::to_string(cp) + ", " + std::to_string(Q) + " + " + std::to_string(U) + " = " + std::to_string(Q + U) + ")";
        children2 = child->children.load(std::memory_order_relaxed);
        num_children = child->num_children.load(std::memory_order_relaxed);
      }
      pvs.push_back({cp, pv});
      print("select_best_moves() debug: PV[%d] %s\n", i, pv2.c_str());
      //std::sort(pvs.begin(), pvs.end(), std::greater<>()); //this is incorrect because short pv have less accurate score
    }
    return multiPV;
  }
  
  //called from mcts_search() in selection phase
  //returns child index with the best PUCT value  
  int select_best_child(struct MCTSNode * parent, int depth) {
    // 1. Dynamic Exploration Constant - linear decay with depth
    // setting exploration_depth_decay to 0 will make exploration constant static = exploration_max
    // Decay: Start at exploration_constant, then for example drop by 0.05 - 0.1 per ply, floor at 0.25 - all tunable
    double C = std::max(exploration_min, exploration_max - (depth * exploration_depth_decay));
    //or square root decay with depth
    //double C = std::max(exploration_min, exploration_max - (sqrt(static_cast<double>(depth)) * exploration_depth_decay));
    
    // OPTIONAL: Bonus for Root Node (Depth 0) to ensure wide scanning
    //if (depth == 0) C = 2.0;

    int num_children = parent->num_children.load(std::memory_order_acquire);
    struct Edge * children = parent->children.load(std::memory_order_acquire);    
    
    double best_score = -INFINITY;
    int selected = -1;
    for (int i = 0; i < num_children; i++) {
      parent->mutex.lock_shared();
      double P = children[i].P.load(std::memory_order_relaxed);
      struct MCTSNode * child = children[i].child.load(std::memory_order_acquire);
      parent->mutex.unlock_shared();
      unsigned long long N = child->N.load(std::memory_order_relaxed);
      double W = -child->W.load(std::memory_order_relaxed); //parent perspective
      double Q = N ? W / N : 0.0;
      double score = Q + C * P * sqrt(static_cast<double>(parent->N.load(std::memory_order_acquire))) / (1.0 + N);
      if (score > best_score) {
        best_score = score;
        selected = i;
      }
    }
    // Apply virtual loss to selected child to avoid contention among threads for the same node
    children = parent->children.load(std::memory_order_acquire);
    parent->mutex.lock_shared();
    struct MCTSNode * child = children[selected].child.load(std::memory_order_acquire);
    parent->mutex.unlock_shared();
    child->N.fetch_add(1, std::memory_order_release);
    child->W.fetch_sub(virtual_loss, std::memory_order_release);
    return selected;
  }

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
  
  //called from do_move() and run_MCTS()
  //calls compute_move_evals(), make_child() and expand_node()
  //returns the result in pawns from the temp_board->fen->sideToMove perspective
  double process_check(struct Board * temp_board, struct NNUEContext * ctx, const std::unordered_set<unsigned long long>& pos_history) {
    struct MCTSNode * node = make_child(temp_board->zh->hash, /*temp_board->zh->hash2,*/ NO_MATE_SCORE);
    int stored_cp = node->cp.load(std::memory_order_relaxed);
    if (stored_cp == NO_MATE_SCORE) { //make_child() returned new node without a parent, let's update its cp and expand it
      //we will link this node to the parent during a call expand_node() made later from mcts_search() 
      std::vector<std::tuple<double, int, int, unsigned long long/*, unsigned long long*/>> move_evals; 
      //use 1.0 for probability mass to try all moves - when in check, there shouldn't be too many moves
      compute_move_evals(temp_board, ctx, pos_history, move_evals, 1.0); 
      int cp = -std::get<2>(move_evals[0]); //select the best cp for check evasion, may not be the best one though
                                            //for example, capture moves would always have higher priors
      node->cp.store(cp, std::memory_order_relaxed);
      node->N.store(1, std::memory_order_relaxed);
      node->W.store(tanh(cp * 0.01 / eval_scale), std::memory_order_relaxed);
      if (node->mutex.try_lock()) { //this should always return true because the node is new
        expand_node(node, move_evals, pos_history); //preserve move_evals in the tree to avoid costly repeat of evaluate_nnue()
        node->mutex.unlock();
      }
      return cp * 0.01;
    } else return -stored_cp * 0.01;
  }
  
  //called from do_move() and set_root()
  //calls evaluate_nnue() and process_check()
  //returns position evaluation in pawns from chess_board->fen->sideToMove perspective
  double position_eval(struct Board * chess_board, struct NNUEContext * ctx, const std::unordered_set<unsigned long long>& pos_history) {
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
        res = process_check(chess_board, ctx, pos_history);
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
        print("error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, occupations %u, ep %u, halfmoveClock %u, whiteToMove %u, whites %llu, blacks %llu, kings %llu, queens %llu, rooks %llu, bishops %llu, knights %llu, pawns %llu, err %s\n", TB_LARGEST, pieceCount, ep, chess_board->fen->halfmoveClock, OPP_COLOR(chess_board->fen->sideToMove) == ColorBlack ? 1 : 0, chess_board->occupations[PieceNameWhite], chess_board->occupations[PieceNameBlack], chess_board->occupations[WhiteKing] | chess_board->occupations[BlackKing], chess_board->occupations[WhiteQueen] | chess_board->occupations[BlackQueen], chess_board->occupations[WhiteRook] | chess_board->occupations[BlackRook], chess_board->occupations[WhiteBishop] | chess_board->occupations[BlackBishop], chess_board->occupations[WhiteKnight] | chess_board->occupations[BlackKnight], chess_board->occupations[WhitePawn] | chess_board->occupations[BlackPawn], strerror(errno));
        generateMoves(chess_board);
        if (chess_board->isMate) res = -MATE_SCORE * 0.01;
        else if (chess_board->isStaleMate) {
          res = 0.0; 
        } else if (chess_board->isCheck) {
          res = process_check(chess_board, ctx, pos_history);
        } else {
          updateFen(chess_board);
          res = evaluate_nnue(chess_board, NULL, ctx);
        }
      } else { //tb_probe_wdl() succeeded
        //0 - loss, 4 - win, 1..3 - draw
        if (wdl == 4) res = MATE_SCORE * 0.001; //chess_board->fen->sideToMove wins, sim_board->fen->sideToMove loses
        else if (wdl == 0) res = -MATE_SCORE * 0.001;
        else res = 0.0;
        tbhits.fetch_add(1, std::memory_order_relaxed);
      }
    } //end of else (pieceCount <= TB_LARGEST)
    return res;
  }
  
  //called from compute_move_evals()
  //calls process_check() and evaluate_nnue()
  //returns eval result in pawns from the perspective of chess_board->fen->sideToMove
  double do_move(struct Board * chess_board, const int src, const int dst, const int promo, struct NNUEContext * ctx, unsigned long long& child_hash, /*unsigned long long& child_hash2,*/ const std::unordered_set<unsigned long long>& pos_history) {
    struct Board * tmp_board = cloneBoard(chess_board);
    struct Move move;
    //init_move(&move, tmp_board, src, dst, promo);
    //make_move(&move);
    ff_move(tmp_board, &move, src, dst, promo);
    updateHash(tmp_board, &move);
    child_hash = tmp_board->zh->hash;
    //child_hash2 = tmp_board->zh->hash2;
    if (pos_history.count(tmp_board->zh->hash) > 0) {
        // This specific move causes a repetition relative to the current search path.
        // Return a draw score immediately.
        freeBoard(tmp_board);
        return 0.0; 
    }
    double res = position_eval(tmp_board, ctx, pos_history);        
    freeBoard(tmp_board);    
    return -res;
  }

  //called from mcts_search() and process_check()
  //calls do_move()
  //computes and returns move_evals tuple given chess_board, prob_mass and pos_history
  void compute_move_evals(struct Board * chess_board, struct NNUEContext * ctx, const std::unordered_set<unsigned long long>& pos_history, std::vector<std::tuple<double, int, int, unsigned long long/*, unsigned long long*/>>& move_evals, double prob_mass) {
        int src, dst;
        double res;
        //std::uniform_real_distribution<double> uniform(-noise, noise);
      	int side = PC(chess_board->fen->sideToMove, PieceTypeAny); //either PieceNameWhite or PieceNameBlack
      	unsigned long long any = chess_board->occupations[side]; 
      	while (any) { //loop for all pieces of the side to move
      	  src = lsBit(any);
      	  unsigned long long moves = chess_board->movesFromSquares[src];
      	  while (moves) { //loop for all piece moves
      	    dst = lsBit(moves);
      	    unsigned long long child_hash = 0;//, child_hash2 = 0;
          	int startPiece = PieceTypeNone, endPiece = PieceTypeNone;
          	if (promoMove(chess_board, src, dst)) {
          	  startPiece = Knight;
          	  endPiece = Queen;
          	}
        	  for (int pt = startPiece; pt <= endPiece; pt++) { //loop over promotions if any, Pawn means no promo
        	    res = do_move(chess_board, src, dst, pt, ctx, child_hash, /*child_hash2,*/ pos_history);
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
      
/*
Overview of the MCTS Logic

MCTS implementation follows the four core phases:

    Selection: Starting from the root, traverse the tree using the PUCT (Predictor + Upper Confidence Bound applied to Trees) formula to select the most promising child node until reaching a leaf or terminal position.

    Expansion: At a leaf node, generate child nodes based on legal moves, using NNUE to assign prior probabilities.

    Evaluation: Evaluate terminal positions (checkmate/stalemate) directly or use NNUE for non-terminal positions, mapping scores to [-1, 1]

    Backpropagation: Update visit counts (N) and total value (W) from the leaf back to the root, alternating the sign of the result to reflect perspective changes.
*/

  //called from thread_search()
  //calls expand()
  void mcts_search(ThreadParams * params, struct NNUEContext * ctx) {
    std::vector<struct MCTSNode *> path;  // Track the path from root to leaf
    path.reserve(chessEngine.depth);
    bool repetition = false;
    params->seldepth = 0;
    struct MCTSNode * node = search.root;
    //start from the same initial position given by board (at the root node, i.e. at the top of the tree - the up side down tree)
    //clone the board to preserve it for subsequent iterations
    struct Board * sim_board = cloneBoard(board);
    
    // Selection
    //iterate down the tree updating sim_board by initiating and making moves
    //thread-local map to prevent repetition cycles - it should be global I think but thread-safety may be a problem
    std::unordered_set<unsigned long long> pos_history;
    int idx = -1;
    struct Edge * children = nullptr;
    while (node->num_children.load(std::memory_order_relaxed) > 0) { //traversal stops at a leaf or at repetition (mate or stalemate node should not have children)
      //return child node index with the best score using PUCT (Predictor + Upper Confidence Bound)
      //it also adds virtual loss to the node to reduce contention for the same node in multi-threaded engine
      idx = select_best_child(node, params->seldepth);
      assert(idx >= 0);
      children = node->children.load(std::memory_order_acquire);
      std::shared_lock lock(node->mutex);
      int move_idx = children[idx].move.load(std::memory_order_relaxed);
      path.push_back(node);  // Add node to path for backprop
      //continue iterating down the tree by getting next node until no more children
      node = children[idx].child.load(std::memory_order_acquire);
      lock.unlock();
      //init edge's move that leads to the child node to update sim_board by making the move
      struct Move move;
  		//init_move(&move, sim_board, move_idx >> 9, (move_idx >> 3) & 63, move_idx & 7);
      //make_move(&move); //this updates sim_board
      ff_move(sim_board, &move, move_idx >> 9, (move_idx >> 3) & 63, move_idx & 7);
      //update Zobrist hash (it is needed so that we can call updateHash later instead of getHash)
			updateHash(sim_board, &move);
      params->seldepth++;
      int path_count = pos_history.count(sim_board->zh->hash); //simulated positions ahead of the current one
      if (path_count == 0) pos_history.insert(sim_board->zh->hash);
      repetition = (path_count >= 1); 
      if (repetition) break;
    } //end of while(node && node->num_children > 0) loop
    path.push_back(node);  // Add leaf to path - sim_board corresponds to this node!
    //Here we are at the bottom of the tree, i.e. at a leaf or at the terminal node (mate, stalemate)
    // Evaluation - the node is already evaluated during previous expansion!
    // We could actually improve the eval by using move_evals calculated later in the code for the children nodes before expansion for evaluating its parent (this node), kind of look ahead eval
    int scorecp = 0;
    double result = 0.0;
    if (!repetition) {
      scorecp = node->cp.load(std::memory_order_relaxed);
      result = tanh(scorecp * 0.01 / eval_scale);
    }
    
    // Expansion - add more children - increase the depth of the tree using the model's predictions, NNUE evals or randomly
    // in theory, if children evaluation is noticably different from its parent, 
    // then we need to continue selectively expanding until position is quiet
    // otherwise, this difference gets propagated to the root and may affect selection, leading to suboptimal play
    if (node->mutex.try_lock()) { //the leaf node in a tree is locked only for expansion
                                  //nodes locked in selection phase are not leaf nodes, i.e. nodes without children
                                  //if leaf node is already locked, it means that other thread is expanding it already
      generateMoves(sim_board); //needed for checks such as isMate or isStaleMate as well as for sim_board->movesFromSquares
      if (!sim_board->isMate && !sim_board->isStaleMate && hash_full.load(std::memory_order_relaxed) < 1000) {
        	std::vector<std::tuple<double, int, int, unsigned long long/*, unsigned long long*/>> move_evals; //res, move_idx, cp, hash, hash2 (res is converted to probabilities in get_prob(), hence we need to preserve it in cp)
          compute_move_evals(sim_board, ctx, pos_history, move_evals, probability_mass);
          //updating node's cp with improve evaluation 
          scorecp = -std::get<2>(move_evals[0]);
          result = tanh(scorecp * 0.01 / eval_scale);
          node->cp.store(scorecp, std::memory_order_relaxed); //look-ahead update
          //before expanding, it would be nice to insure that position is quiet for correct cp, i.e. evals are correct!
          //we could try to iteratively play moves that are different in evals from scorecp by at least 1 pawn
          expand_node(node, move_evals, pos_history);
      } //end of if (!sim_board->isMate && !sim_board->isStaleMate && hash_full.load(std::memory_order_relaxed) < 1000)
      node->mutex.unlock();
    } //if node->mutex.try_lock() 
    
    // Backpropagation: update node visits and results regardless of whether the expansion happened or not
    for (auto n = path.rbegin(); n != path.rend(); ++n) {
      node = *n;
      node->N.fetch_add(1, std::memory_order_relaxed);
      node->W.fetch_add(result, std::memory_order_relaxed);
      result = -result;
    }
    // Revert virtual loss for the selected path (skip root, as no loss was applied to it) regardless of expansion
    // because virtual loss was applied in select_best_child() which is called in the selection phase
    for (size_t j = 1; j < path.size(); ++j) {  // From first child to leaf
      struct MCTSNode * nd = path[j];
      nd->N.fetch_sub(1, std::memory_order_relaxed);
      nd->W.fetch_add(virtual_loss, std::memory_order_relaxed);
    }      
    freeBoard(sim_board);
  }
    
  void thread_search(ThreadParams * params) {
    struct NNUEContext ctx;
    init_nnue_context(&ctx);
    auto iter_start = std::chrono::steady_clock::now();
    double elapsed = 0.0;
    while (depth.load(std::memory_order_relaxed) < chessEngine.depth && elapsed < (params->time_alloc * 0.001) && hash_full.load(std::memory_order_relaxed) < 1000) {
        mcts_search(params, &ctx); //single sim

        // Update seldepth
        int expected = seldepth.load(std::memory_order_relaxed);
        while (params->seldepth > expected && !seldepth.compare_exchange_strong(expected, params->seldepth, std::memory_order_relaxed)) {
            expected = seldepth.load(std::memory_order_relaxed);
        }
        
        if ((chessEngine.depth && depth.load(std::memory_order_relaxed) >= chessEngine.depth) || (chessEngine.nodes && search.root->N.load(std::memory_order_relaxed) >= chessEngine.nodes)) break;        

        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
    }
    free_nnue_context(&ctx);
  }
  
  void uci_output_thread() {
    //std::mt19937 rng(std::random_device{}());
    auto iter_start = std::chrono::steady_clock::now();

    while (!search_done.load(std::memory_order_relaxed)) {
      std::unique_lock<std::mutex> lk(search_done_mtx);
      cv_search_done.wait_for(lk, std::chrono::milliseconds(1000), []{return search_done.load(std::memory_order_relaxed);});
      // Calculate nodes (total simulations)
      struct MCTSNode * current_node = search.root;
      unsigned long long nodes = current_node->N.load(std::memory_order_relaxed);
      // Get elapsed time
      double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
      // Compute NPS
      double nps = elapsed > 0 ? nodes / elapsed : 0;
      //depth should be calculated by traversing the most visited nodes similar to select_best_moves()      
      int d = 0;
      std::unordered_set<unsigned long long> visited;
      std::vector<std::pair<unsigned long long, int>> visits; //N, child_idx
      int num_root_children = current_node->num_children.load(std::memory_order_relaxed);
      while (current_node && d < MAX_DEPTH) {
        unsigned long long current_hash = current_node->hash.load(std::memory_order_relaxed);
        if (visited.find(current_hash) != visited.end()) {
            print("uci_output_thread() debug: cycle detected at depth %d, breaking loop\n", d);
            break;
        }
        visited.insert(current_hash);        
        unsigned long long N = 0;
        int next_idx = -1;
        int num_children = current_node->num_children.load(std::memory_order_acquire);
        struct Edge * children = current_node->children.load(std::memory_order_acquire);
        for (int i = 0; i < num_children; i++) {
          struct MCTSNode * child = children[i].child.load(std::memory_order_acquire);
          unsigned long long n = child->N.load(std::memory_order_relaxed);
          if (current_node == search.root) visits.push_back({n, i});
          if (n > N) {
            N = n;
            next_idx = i;
          } 
        }
        if (next_idx < 0) break; //meaning current_node is a leaf node, i.e. no children
        current_node = children[next_idx].child.load(std::memory_order_acquire);
        d++;
      }
      depth.store(d, std::memory_order_relaxed);
      std::shared_lock lock(map_mutex);
      size_t unique_nodes = search.tree.size();
      lock.unlock();
      size_t total_memory = unique_nodes * (sizeof(MCTSNode) + 24) + total_children.load(std::memory_order_relaxed) * sizeof(Edge);
      size_t max_capacity = chessEngine.optionSpin[Hash].value * 1024 * 1024;  // MB to bytes
      int hashfull = max_capacity ? (total_memory * 1000) / max_capacity : 0;
      if (hashfull > 1000) hashfull = 1000;  // Cap at 1000 per UCI spec
      hash_full.store(hashfull, std::memory_order_relaxed);

      std::sort(visits.begin(), visits.end(), std::greater<>());
      int multiPV = std::min<int>(num_root_children, (int)chessEngine.optionSpin[MultiPV].value);
      struct Edge * children = search.root->children.load(std::memory_order_acquire);
      for (int i = 0; i < multiPV; i++) {
        const int move_idx = children[visits[i].second].move.load(std::memory_order_relaxed);
        char uci_move[6];
        idx_to_move(move_idx, uci_move);
        struct MCTSNode * child = children[visits[i].second].child.load(std::memory_order_acquire);
        print("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", d, seldepth.load(std::memory_order_relaxed), i + 1, -child->cp.load(std::memory_order_relaxed), nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, uci_move);
      }
    }
  }
  
  void runMCTS() {
    double elapsed = 0.0;
    size_t unique_nodes = 0;
    unsigned long long nodes = 0;
    int hashfull = 0;
    int move_number = bitCount(board->moves);
    std::vector<std::pair<int, std::string>> pvs;
    int multiPV = 1;
    //std::mt19937 rng(std::random_device{}());
    
    if (move_number > 1) {
      tbhits.store(0, std::memory_order_relaxed);
      std::vector<ThreadParams> thread_params(chessEngine.optionSpin[Threads].value);
      for (int i = 0; i < chessEngine.optionSpin[Threads].value; ++i) {
        thread_params[i].thread_id = i;
        thread_params[i].time_alloc = timeAllocated;
        thread_params[i].seldepth = 0;
        //thread_params[i].rng.seed(static_cast<unsigned int>(std::random_device{}() ^ std::hash<int>{}(i)));
      }
      set_root();
      gc();
      
      if (!chessEngine.depth) chessEngine.depth = MAX_DEPTH;
      depth.store(0, std::memory_order_relaxed);
      seldepth.store(0, std::memory_order_relaxed);
      std::vector<std::thread> threads;
      auto iter_start = std::chrono::steady_clock::now();
      for (int i = 0; i < chessEngine.optionSpin[Threads].value && hash_full.load(std::memory_order_relaxed) < 1000; ++i) {
        threads.emplace_back(thread_search, &thread_params[i]);
      }
      if (chessEngine.optionCheck[IntermittentInfoLines].value && !chessEngine.ponder && hash_full.load(std::memory_order_relaxed) < 1000) {
        search_done.store(false, std::memory_order_relaxed); // Reset
        std::thread output_thread(uci_output_thread);
        for (auto& t : threads) t.join();
        //print("runMCTS() debug: all search threads terminated\n");
        {
          std::lock_guard<std::mutex> lk(search_done_mtx);
          search_done.store(true, std::memory_order_relaxed); // Signal search complete
        }
        cv_search_done.notify_one();
        output_thread.join();
        //print("runMCTS() debug: output thread terminated\n");
      } else {
        for (auto& t : threads) t.join();
        //print("runMCTS() debug: all search threads terminated\n");
      }
      multiPV = select_best_moves(pvs);
      if (chessEngine.optionCheck[FinalInfoLines].value) {    
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
        nodes = search.root->N.load(std::memory_order_relaxed);
        unique_nodes = search.tree.size();
        // Calculate hashfull (in per-mille) using unique_nodes
        size_t total_memory = unique_nodes * (sizeof(MCTSNode) + 24) + total_children.load(std::memory_order_relaxed) * sizeof(Edge);
        size_t max_capacity = chessEngine.optionSpin[Hash].value * 1024 * 1024;  // MB to bytes
        hashfull = max_capacity ? (total_memory * 1000) / max_capacity : 0;
        if (hashfull > 1000) hashfull = 1000;  // Cap at 1000 per UCI spec
        hash_full.store(hashfull, std::memory_order_relaxed);
      }
    } else if (move_number == 1) {
        auto iter_start = std::chrono::steady_clock::now();
        int side = PC(board->fen->sideToMove, PieceTypeAny); //either PieceNameWhite or PieceNameBlack
        unsigned long long any = board->occupations[side];
        int src = 0, dst = 0;
        int move_idx = 0;
        int promoPiece = PieceNameNone;
        //generateMoves(board);
        while (any) {
            src = lsBit(any);
            unsigned long long moves = board->movesFromSquares[src];
            while (moves) {
                dst = lsBit(moves);
              	if (promoMove(board, src, dst)) {
              	  for (int promo = Queen; promo >= Knight; promo--) {
                    struct Move move;
                    struct Board * tmp_board = cloneBoard(board);
              	    //init_move(&move, tmp_board, src, dst, promo);
                    //make_move(&move);
                    ff_move(tmp_board, &move, src, dst, promo);
                    generateMoves(tmp_board);
                    if (tmp_board->isStaleMate) {
                      freeBoard(tmp_board);
                      continue;
                    } else {
                      promoPiece = promo;
                      move_idx = (src << 9) | (dst << 3) | promo;
                      freeBoard(tmp_board);
                      break;
                    }
              	  }
              	} else {
                  move_idx = (src << 9) | (dst << 3);
                  break;
                }            
                moves &= moves - 1;
            }
            if (move_idx) break;
            any &= any - 1;
        }
        struct NNUEContext ctx;
        init_nnue_context(&ctx);        
        struct Board * tmp_board = cloneBoard(board);
        struct Move move;
  	    //init_move(&move, tmp_board, src, dst, promoPiece);
        //make_move(&move);
        ff_move(tmp_board, &move, src, dst, promoPiece);
        //updateHash(tmp_board, &move);
        //updateFen(tmp_board);
        generateMoves(tmp_board);
        double res;
        if (tmp_board->isMate) res = MATE_SCORE * 0.01;
        else if (tmp_board->isStaleMate) res = 0.0;
        else if (tmp_board->isCheck) {
          std::unordered_set<unsigned long long>pos_history;
          res = -process_check(tmp_board, &ctx, pos_history);
        }
        else res = -evaluate_nnue(tmp_board, NULL, &ctx);
        freeBoard(tmp_board);
        free_nnue_context(&ctx);
        idx_to_move(move_idx, best_move);
        std::string pv(best_move);
        pvs.push_back({static_cast<int>(100.0 * res), pv});
        multiPV = 1;
        if (chessEngine.optionCheck[FinalInfoLines].value) {    
          nodes = 1;
          depth.store(1, std::memory_order_relaxed);
          seldepth.store(1, std::memory_order_relaxed);
          elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
        }
    } //end of if (move_number == 1)
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
    if (chessEngine.optionCheck[FinalInfoLines].value) {
      double nps = nodes / elapsed;
      for (int i = 0; i < multiPV; i++) {      
        print("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s timeAllocated %.2f\n", depth.load(std::memory_order_relaxed), seldepth.load(std::memory_order_relaxed), i + 1, pvs[i].first, nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, pvs[i].second.c_str(), timeAllocated * 0.001);
      }
    }
    std::string bestmove;
    std::string ponder;
    if (!pvs.empty()) {
      int pos = pvs[0].second.find(" ");
      int pos2 = pvs[0].second.find(" ", pos + 5);
      bestmove = pvs[0].second.substr(0, pos);
      if (pos != std::string::npos) ponder = pvs[0].second.substr(pos + 1, pos2 - pos - 1);
    } else bestmove = "(none)";
    if (!ponder.empty()) {
      print("bestmove %s ponder %s\n", bestmove.c_str(), ponder.c_str());
    } else {
      print("bestmove %s\n", bestmove.c_str());
    }
  }
}

void setEngineOptions() {
    strcpy(chessEngine.id, "Creatica Chess Engine 1.0");
    strcpy(chessEngine.authors, "Creatica");
    chessEngine.numberOfCheckOptions = 3;
	  chessEngine.numberOfComboOptions = 0;
	  chessEngine.numberOfSpinOptions = 11;
		chessEngine.numberOfStringOptions = 1;
		chessEngine.numberOfButtonOptions = 0;
	  strcpy(chessEngine.optionCheck[Ponder].name, "Ponder");
	  chessEngine.optionCheck[Ponder].defaultValue = false;
	  chessEngine.optionCheck[Ponder].value = chessEngine.optionCheck[Ponder].defaultValue;
	  strcpy(chessEngine.optionCheck[FinalInfoLines].name, "FinalInfoLines");
	  chessEngine.optionCheck[FinalInfoLines].defaultValue = true;
	  chessEngine.optionCheck[FinalInfoLines].value = chessEngine.optionCheck[FinalInfoLines].defaultValue;
	  strcpy(chessEngine.optionCheck[IntermittentInfoLines].name, "IntermittentInfoLines");
	  chessEngine.optionCheck[IntermittentInfoLines].defaultValue = true;
	  chessEngine.optionCheck[IntermittentInfoLines].value = chessEngine.optionCheck[IntermittentInfoLines].defaultValue;
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
	  }
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
	  strcpy(chessEngine.optionSpin[ExplorationMax].name, "ExplorationMax");
	  chessEngine.optionSpin[ExplorationMax].defaultValue = EXPLORATION_MAX;
	  chessEngine.optionSpin[ExplorationMax].value = chessEngine.optionSpin[ExplorationMax].defaultValue;
	  chessEngine.optionSpin[ExplorationMax].min = 0;
	  chessEngine.optionSpin[ExplorationMax].max = 200;
	  strcpy(chessEngine.optionSpin[ExplorationMin].name, "ExplorationMin");
	  chessEngine.optionSpin[ExplorationMin].defaultValue = EXPLORATION_MIN;
	  chessEngine.optionSpin[ExplorationMin].value = chessEngine.optionSpin[ExplorationMin].defaultValue;
	  chessEngine.optionSpin[ExplorationMin].min = 0;
	  chessEngine.optionSpin[ExplorationMin].max = 200;
	  strcpy(chessEngine.optionSpin[ExplorationDepthDecay].name, "ExplorationDepthDecay");
	  chessEngine.optionSpin[ExplorationDepthDecay].defaultValue = EXPLORATION_DEPTH_DECAY;
	  chessEngine.optionSpin[ExplorationDepthDecay].value = chessEngine.optionSpin[ExplorationDepthDecay].defaultValue;
	  chessEngine.optionSpin[ExplorationDepthDecay].min = 0;
	  chessEngine.optionSpin[ExplorationDepthDecay].max = 200;
	  //strcpy(chessEngine.optionSpin[Noise].name, "Noise");
	  //chessEngine.optionSpin[Noise].defaultValue = MAX_NOISE;
	  //chessEngine.optionSpin[Noise].value = chessEngine.optionSpin[Noise].defaultValue;
	  //chessEngine.optionSpin[Noise].min = 1;
	  //chessEngine.optionSpin[Noise].max = 30;
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
	  chessEngine.optionSpin[EvalScale].min = 1;
	  chessEngine.optionSpin[EvalScale].max = 16;
	  strcpy(chessEngine.optionSpin[Temperature].name, "Temperature");
	  chessEngine.optionSpin[Temperature].defaultValue = TEMPERATURE;
	  chessEngine.optionSpin[Temperature].value = chessEngine.optionSpin[Temperature].defaultValue;
	  chessEngine.optionSpin[Temperature].min = 1;
	  chessEngine.optionSpin[Temperature].max = 200;
    exploration_min = static_cast<double>(chessEngine.optionSpin[ExplorationMin].value) * 0.01;
    exploration_max = static_cast<double>(chessEngine.optionSpin[ExplorationMax].value) * 0.01;
    exploration_depth_decay = static_cast<double>(chessEngine.optionSpin[ExplorationDepthDecay].value) * 0.01;
    probability_mass = static_cast<double>(chessEngine.optionSpin[ProbabilityMass].value) * 0.01;
    //noise = static_cast<double>(chessEngine.optionSpin[Noise].value) * 0.01;
    virtual_loss = static_cast<double>(chessEngine.optionSpin[VirtualLoss].value);
    eval_scale = static_cast<double>(chessEngine.optionSpin[EvalScale].value);
    temperature = static_cast<double>(chessEngine.optionSpin[Temperature].value); //used in calculating probabilities for moves in softmax exp((eval - max_eval)/temperature) / eval_sum
                          //can be tuned so that values < 1.0 sharpen the distribution and values > 1.0 flatten it	  
    chessEngine.wtime = 1e9;
    chessEngine.btime = 1e9;
    chessEngine.winc = 0;
    chessEngine.binc = 0;
    chessEngine.movestogo = 0;
    chessEngine.movetime = 0;
    chessEngine.depth = 0;
    chessEngine.nodes = 0;
    chessEngine.infinite = false;
    chessEngine.ponder = false;
}

int main(int argc, char **argv) {
    TB_LARGEST = 0;
    struct Fen fen;
    struct ZobristHash zh;
    struct Board chess_board;
    chess_board.fen = &fen;
    zobristHash(&zh);
    chess_board.zh = &zh;
    board = &chess_board;
    init_magic_bitboards();
    init_nnue("nn-1c0000000000.nnue", "nn-37f18f62d772.nnue");
    setEngineOptions();
    char fenString[MAX_FEN_STRING_LEN] = "";
    char uciMove[6] = "";
    unsigned long long hash = 0;
  	if (argc == 1) strncpy(fenString, startPos, MAX_FEN_STRING_LEN);
  	else if (argc == 7) {
  	  for (int i = 1; i < 7; i++) {
  	    strcat(fenString, argv[i]);
  	    strcat(fenString, " ");
  	   }
  	} else if (argc == 8) {
  	  for (int i = 1; i < 7; i++) {
  	    strcat(fenString, argv[i]);
  	    strcat(fenString, " ");
  	   }
       strcat(uciMove, argv[7]);	  
  	} else if (argc == 9) {
  	  for (int i = 1; i < 7; i++) {
  	    strcat(fenString, argv[i]);
  	    strcat(fenString, " ");
  	   }
       strcat(uciMove, argv[7]);
       hash = strtoull(argv[8], nullptr, 10);
  	}
  	else {
  	  print("usage: test_smp [fen] [ucimove] [hash]\n");
  	  exit(1);
  	}
  	if (strtofen(&fen, fenString)) {
		  printf("test_nnue error: strtofen() failed; FEN %s\n", fenString);
		  return 1;
	  }
	  if (fentoboard(&fen, board)) {
		  printf("test_nnue error: fentoboard() failed; FEN %s\n", fen.fenString);
		  return 1;
	  }
	  getHash(board->zh, board);
    srand(time(NULL)); 
    
    runMCTS();
    bool move_given = (strlen(uciMove) > 0);
    int src = 0, dst = 0, promo = 0, move_idx;
    if (move_given) {
      move_idx = move_to_idx(uciMove, &src, &dst, &promo);
      printf("searching for move %s (from %s to %s (promo %c), idx %d) in root's children...\n", uciMove, squareName[src], squareName[dst], uciPromoLetter[promo], move_idx);
    }
    int num_children = search.root->num_children.load(std::memory_order_relaxed);
    struct Edge * children = search.root->children.load(std::memory_order_relaxed);
    std::vector<std::tuple<unsigned long long, double, double, int, int>> move_evals; //N, W, P, move_idx, cp    
    
    for (int i = 0; i < num_children; i++) {
      int idx = children[i].move.load(std::memory_order_relaxed);
      double P = children[i].P.load(std::memory_order_relaxed);
      struct MCTSNode * child = children[i].child.load(std::memory_order_relaxed);
      double W = child->W.load(std::memory_order_relaxed);
      unsigned long long N = child->N.load(std::memory_order_relaxed);
      int cp = child->cp.load(std::memory_order_relaxed);
      move_evals.push_back({N, W, P, idx, cp});
    }
    std::sort(move_evals.begin(), move_evals.end(), std::greater<>());
    for (int i = 0; i < num_children; i++) {
      char uci_move[6];
      if (move_given && move_idx == std::get<3>(move_evals[i]))
        printf("found child %d: %s, P %f, W %f, N %llu, Q %f, cp %d\n", i, idx_to_move(std::get<3>(move_evals[i]), uci_move), std::get<2>(move_evals[i]), -std::get<1>(move_evals[i]), std::get<0>(move_evals[i]), -std::get<1>(move_evals[i]) / std::get<0>(move_evals[i]), -std::get<4>(move_evals[i]));
      else 
        printf("move %d: %s, P %f, W %f, N %llu, Q %f, cp %d\n", i, idx_to_move(std::get<3>(move_evals[i]), uci_move), std::get<2>(move_evals[i]), -std::get<1>(move_evals[i]), std::get<0>(move_evals[i]), -std::get<1>(move_evals[i]) / std::get<0>(move_evals[i]), -std::get<4>(move_evals[i]));
    }
    if (move_given) {
      printf("searching for position from move %s (from %s to %s (promo %c), idx %d) in MCT...\n", uciMove, squareName[src], squareName[dst], uciPromoLetter[promo], move_idx);
      struct Move move;
      //init_move(&move, board, src, dst, promo);
      //make_move(&move);
      ff_move(board, &move, src, dst, promo);
      updateHash(board, &move);
      auto it = search.tree.find(board->zh->hash);
      struct MCTSNode * node = (it != search.tree.end()) ? it->second : nullptr;
      if (node) {
          double W = node->W.load(std::memory_order_relaxed);
          unsigned long long N = node->N.load(std::memory_order_relaxed);
          int cp = node->cp.load(std::memory_order_relaxed);
          printf("found node with W = %f, N = %llu, W/N = %f, cp = %d\n", W, N, N > 0 ? W / N : 0, cp);        
      } else printf("node not found\n");
    }
    if (hash) {
      printf("searching for position %llu in MCT...\n", hash);
      auto it = search.tree.find(hash);
      struct MCTSNode * node = (it != search.tree.end()) ? it->second : nullptr;
      if (node) {
          double W = node->W.load(std::memory_order_relaxed);
          unsigned long long N = node->N.load(std::memory_order_relaxed);
          int cp = node->cp.load(std::memory_order_relaxed);
          printf("found node with W = %f, N = %llu, W/N = %f, cp = %d\n", W, N, N > 0 ? W / N : 0, cp);        
      } else printf("node not found\n");      
    }
    cleanup();
    cleanup_nnue();
    cleanup_magic_bitboards();
    return 0;
}
