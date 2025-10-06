//For MacOS using clang
//c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -I /Users/ap/libchess  -L /Users/ap/libchess -Wl,-lchess,-lcurl,-rpath,/Users/ap/libchess creatica.cpp uci.cpp tbcore.c tbprobe.c -o creatica

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
#include <unordered_set>
#include <functional>
#include <random>
#include <chrono>
#include <algorithm>
#include <math.h>
#include "tbprobe.h"
#include "libchess.h"

extern std::atomic<bool> stopFlag;
extern double timeAllocated; //ms
extern char best_move[6];
std::atomic<bool> search_done{false}; // Signals search completion
double temperature = 1.0; //used in calculating probabilities for moves in softmax exp((eval - max_eval)/temperature) / eval_sum
                        //can be tuned so that values < 1.0 sharpen the distribution and values > 1.0 flatten it
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
  bool sendGetRequest(const std::string& url, int& scorecp, std::string& uci_move);
  
  #define MAX_DEPTH 100
  //extern std::atomic<bool> quitFlag;
  //extern std::atomic<bool> searchFlag;
  extern struct Board * board;
  extern struct Engine chessEngine;
  extern std::unordered_map<unsigned long long, int> position_history;
  std::mutex probe_mutex;
  std::shared_mutex map_mutex;
  std::atomic<unsigned long long> total_children{0};
  std::atomic<int> hash_full{0};
  std::atomic<unsigned long long> tbhits{0};
  std::atomic<int> generation{0};
  extern std::atomic<bool> ponderHit;
  double exploration_constant = 1.0;
  double probability_mass = 0.9;
  double noise = 0.1;
  double virtual_loss = 3.0;  // Tune: UCI option, e.g., 1.0-3.0
  double eval_scale = 4.0; //Tune as well
  
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
      std::atomic<int> count{1}; //count the number of identical positions (nodes), i.e. for a given hash
      std::atomic<unsigned long long> N{0};  // Atomic for lock-free updates
      std::atomic<double> W{0};
      std::atomic<int> cp {NO_MATE_SCORE}; //position evaluation in centipawns 
      std::atomic<int> num_children{0};
      std::atomic<int> generation{0};
      std::shared_mutex mutex;  // For protecting children expansion
      struct Edge * children = nullptr;
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
      //int num_sims;
      unsigned long long time_alloc;
      unsigned long long currentNodes;
      int currentDepth;
      int seldepth;
      std::mt19937 rng;
  };
  
  
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
    //while (!stopFlag.load(std::memory_order_relaxed) && !search_done.load(std::memory_order_relaxed)) {
    //  if (hash_full.load(std::memory_order_relaxed) >= 750) {
        // Increment the global generation.
        int current_gen = generation.fetch_add(1, std::memory_order_relaxed) + 1;
        // BFS traversal to mark reachable nodes with the current generation.
        // Use queue to avoid recursion and potential stack overflow in deep trees.
        std::queue<MCTSNode *> q;
        // Mark root
        search.root->generation.store(current_gen, std::memory_order_relaxed);
        q.push(search.root);
    
        while (!q.empty()) {
            MCTSNode * node = q.front();
            q.pop();
    
            // Acquire shared lock to read children safely.
            //std::shared_lock<std::shared_mutex> lock(node->mutex);
            int num_child = node->num_children.load(std::memory_order_relaxed);
            if (num_child > 0 && node->children != nullptr) {
                for (int i = 0; i < num_child; ++i) {
                    MCTSNode * child = node->children[i].child;
                    if (child != nullptr) {
                        // Atomically update generation if it's outdated to avoid revisiting.
                        int expected = current_gen - 1;
                        if (child->generation.compare_exchange_strong(expected, current_gen, std::memory_order_relaxed)) {
                            q.push(child);
                        }
                    }
                }
            }
            //lock.unlock();
        }
        // Now iterate through the map and erase nodes with outdated generations.
        // Also clean up allocated children arrays.
        //std::unique_lock delete_lock(map_mutex);
        for (auto it = search.tree.begin(); it != search.tree.end();) {
          MCTSNode * node = it->second;
          if (node->generation.load(std::memory_order_relaxed) < current_gen) {
            // Clean up dynamically allocated children if any.
            if (node->children != nullptr) {
              total_children.fetch_sub(node->num_children, std::memory_order_relaxed);
              delete[] node->children;
            }
            it = search.tree.erase(it);
            delete node;
          } else ++it;
        }
        //fprintf(stderr, "gc() debug: delete %d nodes\n", deleted_nodes);
        //delete_lock.unlock();
     // } else std::this_thread::sleep_for(std::chrono::milliseconds(1000)); //hash_full < 750
    //} //end of while(!stopFlag.load(std::memory_order_relaxed) && !search_done.load(std::memory_order_relaxed) loop
      size_t total_memory = search.tree.size() * (sizeof(MCTSNode) + 24) + total_children.load(std::memory_order_relaxed) * sizeof(Edge);
      size_t max_capacity = chessEngine.optionSpin[Hash].value * 1024 * 1024;  // MB to bytes
      int hashfull = max_capacity ? (total_memory * 1000) / max_capacity : 0;
      if (hashfull > 1000) hashfull = 1000;  // Cap at 1000 per UCI spec
      //fprintf(stderr, "gc() debug: hashfull %d\n", hashfull);
      hash_full.store(hashfull, std::memory_order_relaxed);    
      //fprintf(stderr, "gc() debug: hash_full %d\n", hash_full.load(std::memory_order_relaxed));
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
  double puct_score(struct MCTSNode * parent, int idx) {
      parent->mutex.lock_shared();
      double P = parent->children[idx].P;
      unsigned long long n = parent->children[idx].child->N.load(std::memory_order_relaxed);
      double w = parent->children[idx].child->W.load(std::memory_order_relaxed);
      parent->mutex.unlock_shared();
      double Q = n ? w / n : 0.0;
      return -Q + exploration_constant * P * sqrt((double)parent->N.load(std::memory_order_relaxed)) / (1.0 + n);
  }
  
  char * idx_to_move(int move_idx, char * uci_move) {
    if (!uci_move) {
      log_file("idx_to_move() error: invalid arg - uci_move is NULL\n");
      return NULL;
    }
    memset(uci_move, 0, 6);
    enum SquareName source_square = (enum SquareName)(move_idx >> 9);
    enum SquareName destination_square = (enum SquareName)((move_idx >> 3) & 63);
    int promo = (move_idx & 7) + 1; //need to add 1 because we encode promo as 0 - no promo, 1 - knight, 2 - bishop, 3 - rook and 4 - queen
    strcat(uci_move, squareName[source_square]);
    strcat(uci_move, squareName[destination_square]);
    uci_move[4] = uciPromoLetter[promo];
    return uci_move;
  }
 /* 
  void expand_node(struct MCTSNode * parent, struct Board * chess_board, const std::vector<std::pair<double, int>>& top_moves) {//, std::mt19937 rng) {
    if (parent->num_children.load(std::memory_order_relaxed) > 0) return;  // Already expanded      
    int num_moves = top_moves.size();
    total_children.fetch_add(num_moves, std::memory_order_relaxed);
    //struct Edge * children = (struct Edge *)calloc(num_moves, sizeof(struct Edge));
    struct Edge * children = new Edge[num_moves];
    for (int i = 0; i < num_moves; ++i) {
        struct Board * temp_board = cloneBoard(chess_board);
        struct Move move;
        init_move(&move, temp_board, top_moves[i].second >> 9, (top_moves[i].second >> 3) & 63, (top_moves[i].second & 7) + 1);
        makeMove(&move);
  			if (updateHash(temp_board, &move)) {
  				log_file("expand_node() error: updateHash() returned non-zero value\n");
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
          //child->count.store(1, std::memory_order_relaxed);
          child->generation.store(generation.load(std::memory_order_relaxed));
        } else child->count.fetch_add(1, std::memory_order_relaxed);
        children[i].child = child;
        children[i].P = top_moves[i].first;  // Use noised priors for root
        children[i].move = top_moves[i].second;
    }
    parent->children = children;
    parent->num_children.store(num_moves, std::memory_order_relaxed);
  }*/
  
  void expand_node(struct MCTSNode * parent, const std::vector<std::tuple<double, int, unsigned long long, int>>& top_moves) {
    if (parent->num_children.load(std::memory_order_relaxed) > 0) return;
    int num_moves = top_moves.size();
    std::vector<Edge> valid_children;
    valid_children.reserve(num_moves);  // Optimistic reserve
    for (int i = 0; i < num_moves; ++i) {
        auto [prior, move_idx, child_hash, child_cp] = top_moves[i];
        struct MCTSNode * child = nullptr;
        int existing_count = 0;
        {
            std::shared_lock search_lock(map_mutex);
            auto it = search.tree.find(child_hash);
            child = (it != search.tree.end()) ? it->second : nullptr;
            if (child) existing_count = child->count.load(std::memory_order_relaxed);
            search_lock.unlock();
        }
        if (existing_count >= 1 && i < num_moves - 1 && child_cp > 100 && std::get<3>(top_moves[i + 1]) > 100) continue;
        if (!child) {
            child = new MCTSNode();
            std::unique_lock insert_lock(map_mutex);
            search.tree.emplace(child_hash, child);
            insert_lock.unlock();
            child->hash.store(child_hash, std::memory_order_relaxed);
            child->generation.store(generation.load(std::memory_order_relaxed));
            child->cp.store(child_cp, std::memory_order_relaxed);
        } else child->count.fetch_add(1, std::memory_order_relaxed);
        Edge e;
        e.P = prior;
        e.move = move_idx;
        e.child = child;
        valid_children.push_back(e);        
    }
    int actual_num = valid_children.size();
    if (actual_num == 0) return;  // No valid moves; leave unexpanded
    total_children.fetch_add(actual_num, std::memory_order_relaxed);
    parent->children = new Edge[actual_num];
    std::copy(valid_children.begin(), valid_children.end(), parent->children);
    parent->num_children.store(actual_num, std::memory_order_relaxed);    
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

  int select_best_children(int ** idx) {
    std::vector<std::pair<unsigned long long, int>> children;
    //std::lock_guard<std::mutex> lock(search.root->mutex); //perhaps, it is not needed here as root is only expanded once
    int num_children = search.root->num_children.load(std::memory_order_relaxed);
    
    for (int i = 0; i < num_children; i++) {
      search.root->mutex.lock_shared();
      children.push_back({search.root->children[i].child->N.load(std::memory_order_relaxed), i});
      search.root->mutex.unlock_shared();
    }      
    std::sort(children.begin(), children.end(), std::greater<>());
    int multiPV = std::min<int>(num_children, (int)chessEngine.optionSpin[MultiPV].value);
    if (multiPV > 0) {
      *idx = (int *)calloc(multiPV, sizeof(int));
      if (!*idx) {
        log_file("select_best_children() error: calloc() failed for idx\n");
        exit(-1);
      }      
    } else {
      log_file("select_best_children() error: root node has 0 children\n");
      exit(-1);      
    }
    for (int i = 0; i < multiPV; i++) {
      (*idx)[i] = children[i].second;
      //log_file("move_idx[%d] %d, pv[%d] %s\n", i, (*move_idx)[i], i, (*pv)[i]);
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
      std::sort(children.begin(), children.end(), std::greater<>()); //sort children desc by the number of visits N
    } else {
      log_file("select_best_moves() error: root node has 0 children\n");
      exit(-1);      
    }
    int pvLength = chessEngine.optionSpin[PVPlies].value * sizeof(uci_move);
    *pv = (char **)calloc(multiPV, sizeof(char *));
    if (!*pv) {
      log_file("select_best_moves() error: calloc() failed for pv\n");
      exit(-1);
    }
    for (int i = 0; i < multiPV; i++) {
      (*pv)[i] = (char *)calloc(1, pvLength);
      if (!(*pv)[i]) {
        log_file("select_best_moves() error: calloc() failed for pv[%d], pvLength %d\n", i, pvLength);
        exit(-1);
      }
    }
    *move_idx = (int *)calloc(multiPV, sizeof(int));
    if (!*move_idx) {
      log_file("select_best_moves() error: calloc() failed for move_idx, multiPV %d\n", multiPV);
      exit(-1);
    }

    int maxLen = pvLength - sizeof(uci_move);
    struct Move move;
    for (int i = 0; i < multiPV; i++) {
      int child_index = children[i].second;
      struct MCTSNode * current_node = search.root;
      (*move_idx)[i] = search.root->children[child_index].move;
      idx_to_move((*move_idx)[i], uci_move);
      strcat((*pv)[i], uci_move);
      current_node = search.root->children[child_index].child;      
      // Build PV by following most visited children
      int num_child = current_node->num_children.load(std::memory_order_relaxed);
      while (current_node && num_child > 0 && strlen((*pv)[i]) < maxLen) {
        int next_idx = most_visited_child(current_node);
        if (next_idx < 0) break;
        idx_to_move(current_node->children[next_idx].move, uci_move);
        strcat((*pv)[i], " ");
        strcat((*pv)[i], uci_move);
        current_node = current_node->children[next_idx].child;
        num_child = current_node->num_children.load(std::memory_order_relaxed);
      }
    }
    children.clear();
    return multiPV;
  }
  
  int select_best_child(struct MCTSNode * parent) {
    double best_score = -INFINITY;
    double score;
    int selected = -1;
    //parent->mutex.lock();  // Lock parent for read consistency (optional, but safe)
    int num_children = parent->num_children.load(std::memory_order_relaxed);
    //parent->mutex.unlock();
    for (int i = 0; i < num_children; i++) {
      score = puct_score(parent, i);
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

int get_prob(std::vector<std::tuple<double, int, unsigned long long, int>>& move_evals) {
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
          init_move(&m, temp_board, src, dst, Queen);
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
  //returns eval result in pawns
  double doMove(struct Board * chessBoard, int src, int dst, int promo, const int scorecp, struct NNUEContext * ctx, std::mt19937 rng, unsigned long long& child_hash, std::unordered_map<unsigned long long, int>& pos_history) {
    double res;
    struct Move move;
    struct Board * tmp_board = cloneBoard(chessBoard);
    init_move(&move, tmp_board, src, dst, promo);
    makeMove(&move);
    if (updateHash(tmp_board, &move)) {
      log_file("mcts_search() error: updateHash() returned non-zero value\n");
      exit(-1);
    }
    child_hash = tmp_board->zh->hash ^ tmp_board->zh->hash2;
    bool repetition3x = false;
    if (position_history.count(child_hash) + pos_history.count(child_hash) >= 2) repetition3x = true;
  	int pieceCount = bitCount(tmp_board->occupations[PieceNameAny]);
  	if (pieceCount > TB_LARGEST || tmp_board->fen->halfmoveClock || tmp_board->fen->castlingRights) {
      //evaluate_nnue() returns result in pawns (not centipawns!)
      //we made the move above, so the eval res is from the perspective of sim_board->opponentColor or tmp_board->fen->sideToMove
      //and must be negated to preserve the perspective of sim_board->fen->sideToMove
      if (tmp_board->isMate) res = MATE_SCORE * 0.01; //sim_board->fen->sideToMove wins
      else if (tmp_board->isStaleMate || repetition3x) {
        res = -evaluate_nnue(tmp_board, NULL, ctx);
        if (scorecp > 200 && res > 2.0) res = -2.0; //discourage stale mate or repetition in winning position
        else if (scorecp < -200 && res < -2.0) res = 2.0; //encourage stalemate or repetition in losing position
        else res = 0.0;
      }
      else if (tmp_board->isCheck) res = -process_check(tmp_board, NULL, ctx, rng);
      else res = -evaluate_nnue(tmp_board, NULL, ctx);
    } else { //pieceCount <= TB_LARGEST, etc
      unsigned int ep = lsBit(tmp_board->fen->enPassantLegalBit);
      unsigned int wdl = tb_probe_wdl(tmp_board->occupations[PieceNameWhite], tmp_board->occupations[PieceNameBlack], tmp_board->occupations[WhiteKing] | tmp_board->occupations[BlackKing],
        tmp_board->occupations[WhiteQueen] | tmp_board->occupations[BlackQueen], tmp_board->occupations[WhiteRook] | tmp_board->occupations[BlackRook], tmp_board->occupations[WhiteBishop] | tmp_board->occupations[BlackBishop], tmp_board->occupations[WhiteKnight] | tmp_board->occupations[BlackKnight], tmp_board->occupations[WhitePawn] | tmp_board->occupations[BlackPawn],
        0, 0, ep == 64 ? 0 : ep, tmp_board->opponentColor == ColorBlack ? 1 : 0);
      if (wdl == TB_RESULT_FAILED) {
        log_file("error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, occupations %u, fen %s, ep %u, halfmoveClock %u, whiteToMove %u, whites %llu, blacks %llu, kings %llu, queens %llu, rooks %llu, bishops %llu, knights %llu, pawns %llu, err %s\n", TB_LARGEST, pieceCount, tmp_board->fen->fenString, ep, tmp_board->fen->halfmoveClock, tmp_board->opponentColor == ColorBlack ? 1 : 0, tmp_board->occupations[PieceNameWhite], tmp_board->occupations[PieceNameBlack], tmp_board->occupations[WhiteKing] | tmp_board->occupations[BlackKing], tmp_board->occupations[WhiteQueen] | tmp_board->occupations[BlackQueen], tmp_board->occupations[WhiteRook] | tmp_board->occupations[BlackRook], tmp_board->occupations[WhiteBishop] | tmp_board->occupations[BlackBishop], tmp_board->occupations[WhiteKnight] | tmp_board->occupations[BlackKnight], tmp_board->occupations[WhitePawn] | tmp_board->occupations[BlackPawn], strerror(errno));
        if (tmp_board->isMate) res = MATE_SCORE * 0.01;
        else if (tmp_board->isStaleMate || repetition3x) {
          res = -evaluate_nnue(tmp_board, NULL, ctx);
          if (scorecp > 200 && res > 2.0) res = -2.0; //discourage stale mate in winning position
          else if (scorecp < -200 && res < -2.0) res = 2.0; //encourage stalemate in non-winning position
          else res = 0.0; 
        } else if (tmp_board->isCheck) res = -process_check(tmp_board, NULL, ctx, rng);
        else res = -evaluate_nnue(tmp_board, NULL, ctx);
      } else { //tb_probe_wdl() succeeded
        //0 - loss, 4 - win, 1..3 - draw
        if (wdl == 4) res = -MATE_SCORE * 0.001; //tmp_board->fen->sideToMove wins, sim_board->fen->sideToMove loses
        else if (wdl == 0) res = MATE_SCORE * 0.001;
        else res = 0.0;
        tbhits.fetch_add(1, std::memory_order_relaxed);
      }
    } //end of else (pieceCount <= TB_LARGEST)
    freeBoard(tmp_board);
    return res;
  }

/*
Overview of the MCTS Logic

MCTS implementation follows the four core phases:

    Selection: Starting from the root, traverse the tree using the PUCT (Predictor + Upper Confidence Bound applied to Trees) formula to select the most promising child node until reaching a leaf or terminal position.

    Expansion: At a leaf node, generate child nodes based on legal moves, using NNUE to assign prior probabilities.

    Evaluation: Evaluate terminal positions (checkmate/stalemate) directly or use NNUE for non-terminal positions, mapping scores to [-1, 1]

    Backpropagation: Update visit counts (N) and total value (W) from the leaf back to the root, alternating the sign of the result to reflect perspective changes.
*/

  void mcts_search(ThreadParams * params, struct NNUEContext * ctx) {
    char uci_move[6];
    struct Move move;
    std::vector<struct MCTSNode *> path;  // Track the path from root to leaf
    path.reserve(chessEngine.depth);
    bool repetition3x = false;
    params->currentDepth = 0;
    struct MCTSNode * node = search.root;
    //start from the same initial position given by board (at the root node, i.e. at the top of the tree)
    //clone the board to preserve it for subsequent iterations
    struct Board * sim_board = cloneBoard(board);
    // Selection
    //iterate down the tree updating sim_board by initiating and making moves
    std::unordered_map<unsigned long long, int> pos_history;
    while (node && node->num_children.load(std::memory_order_relaxed) > 0 && !sim_board->isStaleMate && !sim_board->isMate) { //traversal stops at a leaf or terminal node or at 3x repetition
      //unsigned long long hash = sim_board->zh->hash ^ sim_board->zh->hash2;
      unsigned long long hash = node->hash.load(std::memory_order_relaxed);
      if (position_history.count(hash) + pos_history.count(hash) >= 2) { //we need to count repetitions that have already occured + simulated ones
          repetition3x = true;
          break;
      }
      pos_history[hash]++;
      //return child node with the best score using PUCT (Predictor + Upper Confidence Bound)
      //log_file("mcts_search(%d) calling select_best_child()...\n", params->thread_id);
      int idx = select_best_child(node);
      //log_file("mcts_search(%d) returned from select_best_child()...\n", params->thread_id);
      if (idx < 0) {
        log_file("mcts_search() error: select_best_child() returned negative index\n");
        exit(-1);
      }
      //log_file("select_best_child() debug: selected move is %s%s, fen %s\n", squareName[node->children[idx].move / 64], squareName[node->children[idx].move % 64], sim_board->fen->fenString);
      //init edge's move that leads to the child node
      node->mutex.lock_shared();
      int move_idx = node->children[idx].move;
      node->mutex.unlock_shared();
  		init_move(&move, sim_board, move_idx >> 9, (move_idx >> 3) & 63, (move_idx & 7) + 1);
      //make the move
      makeMove(&move); //this updates sim_board
      //update Zobrist hash (it is needed so that we can call updateHash later instead of getHash)
			if (updateHash(sim_board, &move)) {
				log_file("mcts_search() error: updateHash() returned non-zero value\n");
				exit(-1);
			}
      //continue iterating down the tree until no more children or the end of the game is reached
      path.push_back(node);  // Add node to path
      std::shared_lock lock(node->mutex);
      node = node->children[idx].child;
      lock.unlock();
      params->currentDepth++;
    } //end of while(node && node->num_children > 0 && !sim_board->isStaleMate && !sim_board->isMate) loop
    //pos_history.clear(); //this should be done after expansion
    path.push_back(node);  // Add leaf to path - sim_board and result correspond to this node!

    //Here we are at the bottom of the tree, i.e. at a leaf or at the terminal node (mate, stalemate)
    // Evaluation - evaluate a leaf, i.e. a node without children, then we expand it - add children, which will be evaluated at the subsequent iterations - seems like redundant because the node is already evaluated during expansion!
    double result;
    int scorecp = node->cp.load(std::memory_order_relaxed); //cp is stored during expansion
    //log_file("mcts_search() debug: scorecp %d\n", scorecp);
    if (scorecp == NO_MATE_SCORE) {
      if (sim_board->isCheck) {
          result = NNUE_CHECK; //NNUE cannot evaluate when in check - it will be resolved in expansion
      } else if (sim_board->isMate) { //sim_board->fen->sideToMove is mated
        result = -1.0;
        scorecp = -MATE_SCORE;
        node->cp.store(scorecp);
  			//log_file("mcts_search() debug: checkmate for %s, fen %s\n", color[sim_board->fen->sideToMove], sim_board->fen->fenString);
      } else if (sim_board->isStaleMate || repetition3x) {
        result = evaluate_nnue(sim_board, NULL, ctx);
        scorecp = static_cast<int>(result * 100);
        node->cp.store(scorecp, std::memory_order_relaxed);
        if (result > 2.0) result = tanh(-1.5 / eval_scale); //try to discourage stalemate or 3x repetition if winning
        else if (result < -2.0) result = tanh(1.5 / eval_scale); //and encourage it if losing
        else result = 0.0;
  			//log_file("mcts_search() debug: stalemate or 3x repetition for %s, fen %s\n", color[sim_board->fen->sideToMove], sim_board->fen->fenString);
      } else {
        //evaluate_nnue() returns result in pawns (not centipawns!) from sim_board->fen->sideToMove perspective
        result = evaluate_nnue(sim_board, NULL, ctx);
        scorecp = static_cast<int>(result * 100);
        node->cp.store(scorecp, std::memory_order_relaxed);
  			//log_file("mcts_search() debug: evaluate_nnue result %f, fen %s\n", result, sim_board->fen->fenString);
        result = tanh(result / eval_scale);
      }
    } else result = tanh(scorecp * 0.01 / eval_scale);
    //log_file("mcts_search() debug: result %f\n", result);
    // Expansion - add more children - increase the depth of the tree down using the model's predictions, NNUE evals or randomly
    if (params->currentDepth < chessEngine.depth && !sim_board->isMate && !sim_board->isStaleMate && hash_full.load(std::memory_order_relaxed) < 1000) {
      if (node->mutex.try_lock()) {
        int src, dst, effective_branching = 1;
        double res;
      	std::vector<std::tuple<double, int, unsigned long long, int>> move_evals; //res, move_idx, hash
      	enum PieceName side = (enum PieceName)((sim_board->fen->sideToMove << 3) | PieceTypeAny);//either PieceNameWhite or PieceNameBlack
      	unsigned long long any = sim_board->occupations[side]; 
        std::uniform_real_distribution<double> uniform(-noise, noise);
      	while (any) {
      	  src = lsBit(any);
      	  unsigned long long moves = sim_board->sideToMoveMoves[src];
      	  while (moves) {
      	    dst = lsBit(moves);
      	    unsigned long long child_hash;
          	if (promoMove(sim_board, src, dst)) {
          	  for (int pt = Knight; pt <= Queen; pt++) {
          	    res = doMove(sim_board, src, dst, pt, scorecp, ctx, params->rng, child_hash, pos_history);
                res += res * uniform(params->rng);
                move_evals.push_back({res, (src << 9) | (dst << 3) | (pt - 1), child_hash, static_cast<int>(-res * 100)});
          	  }
          	} else {
          	  res = doMove(sim_board, src, dst, PieceTypeNone, scorecp, ctx, params->rng, child_hash, pos_history);          	  
              res += res * uniform(params->rng);
              move_evals.push_back({res, (src << 9) | (dst << 3), child_hash, static_cast<int>(-res * 100)});
            }
            moves &= moves - 1;
          }
          any &= any - 1;
        }
        pos_history.clear();
        // Sort by res descending (use lambda for tuple)
        std::sort(move_evals.begin(), move_evals.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b);});
        if (result == NNUE_CHECK) {
          if (scorecp == NO_MATE_SCORE) {
            scorecp = static_cast<int>(std::get<0>(move_evals[0]) * 100);
            node->cp.store(scorecp, std::memory_order_relaxed);
          }
          result = tanh(std::get<0>(move_evals[0]) / eval_scale);
        }
        effective_branching = get_prob(move_evals);
        move_evals.resize(effective_branching);  // Slice to top effective
        expand_node(node, move_evals);
        node->mutex.unlock();
        move_evals.clear();
      }
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
    path.clear();
    freeBoard(sim_board);
    if (params->currentDepth > params->seldepth) params->seldepth = params->currentDepth;   
  }
    
  void thread_search(ThreadParams * params) {
    struct NNUEContext ctx;
    init_nnue_context(&ctx);
    auto iter_start = std::chrono::steady_clock::now();
    double elapsed = 0.0;
    int depth = 0;
    params->currentNodes = 0;
    while (elapsed < (params->time_alloc * 0.001) && !stopFlag.load(std::memory_order_relaxed) && hash_full.load(std::memory_order_relaxed) < 1000) {
        mcts_search(params, &ctx);
        
        // Update depth
        int expected = engineDepth.currentDepth.load(std::memory_order_relaxed);
        while (params->currentDepth > expected && !engineDepth.currentDepth.compare_exchange_strong(expected, params->currentDepth, std::memory_order_relaxed)) {
            expected = engineDepth.currentDepth.load(std::memory_order_relaxed);
        }
        // Update seldepth
        expected = engineDepth.seldepth.load(std::memory_order_relaxed);
        while (params->seldepth > expected && !engineDepth.seldepth.compare_exchange_strong(expected, params->seldepth, std::memory_order_relaxed)) {
            expected = engineDepth.seldepth.load(std::memory_order_relaxed);
        }
        
        params->currentNodes++;
        if ((chessEngine.depth && params->currentDepth >= chessEngine.depth) || (chessEngine.nodes && params->currentNodes >= chessEngine.nodes)) break;        

        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
        //numberOfIterations++;
        if (elapsed > 1 && (int)elapsed % 10 == 0) {
          if (depth < engineDepth.currentDepth.load(std::memory_order_relaxed))
            depth = engineDepth.currentDepth.load(std::memory_order_relaxed);
          else {
            log_file("thread_search() debug: depth %d is not increasing after 10 sec\n", depth);
            break; //depth is not increasing after 5 sec
          }
        }
    }
    free_nnue_context(&ctx);
  }
  
  void uci_output_thread() {
    std::mt19937 rng(std::random_device{}());
    auto iter_start = std::chrono::steady_clock::now();

    while (!stopFlag.load(std::memory_order_relaxed) && !search_done.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      // Calculate nodes (total simulations)
      unsigned long long nodes = search.root->N.load(std::memory_order_relaxed);
      // Get elapsed time
      double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
      // Compute NPS
      double nps = elapsed > 0 ? nodes / elapsed : 0;
      int * idx = nullptr;
      int multiPV = select_best_children(&idx); //select best child indexes
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

      for (int i = 0; i < multiPV && idx; i++) {
        //to include promotions, we need to add 3 extra bits for promos 
        //(0 - no promo, 1 - knight, 2 - bishop, 3 - rook, 4 - queen) to 12-bit existing encoding like this:
        //move_idx = (source << 9) | (dest << 3) | promotion_type
        //decoding:
        //promotion_type = move_idx & 7 (lower 3 bits)
        //dest = (move_idx >> 3) & 63 (bits from 4 to 9)
        //source = move_idx >> 9 (bits from 10 to 15)
        int move_idx = search.root->children[idx[i]].move;
        char uci_move[6];
        idx_to_move(move_idx, uci_move);
        log_file("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", depth, seldepth, i + 1, search.root->children[idx[i]].child->cp.load(std::memory_order_relaxed), nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, uci_move);
        print("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", depth, seldepth, i + 1, search.root->children[idx[i]].child->cp.load(std::memory_order_relaxed), nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, uci_move);
      }
      if (idx) free(idx);
    }
  }
  
  void runMCTS() {
    double elapsed = 0.0;
    int * move_idx = nullptr;
    size_t unique_nodes = 0;
    unsigned long long nodes = 0;
    int hashfull = 0;
    int move_number = bitCount(board->moves);
    //log_file("runMCTS() debug: move_number %d\n", move_number);
    char ** pv = nullptr;
    int * idx = nullptr;
    int multiPV = 1;
    std::mt19937 rng(std::random_device{}());
    
    if (move_number > 1) {
      tbhits.store(0, std::memory_order_relaxed);
      exploration_constant = (double)chessEngine.optionSpin[ExplorationConstant].value * 0.01;
      probability_mass = (double)chessEngine.optionSpin[ProbabilityMass].value * 0.01;
      noise = (double)chessEngine.optionSpin[Noise].value * 0.01;
      virtual_loss = chessEngine.optionSpin[VirtualLoss].value;
      eval_scale = chessEngine.optionSpin[EvalScale].value;
      temperature = chessEngine.optionSpin[Temperature].value;
      unsigned long iterations = MIN_ITERATIONS;
      std::vector<ThreadParams> thread_params(chessEngine.optionSpin[Threads].value);
      for (int i = 0; i < chessEngine.optionSpin[Threads].value; ++i) {
        thread_params[i].thread_id = i;
        //thread_params[i].num_sims = iterations;  // Or time-based loop inside thread
        thread_params[i].time_alloc = timeAllocated;
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
      for (int i = 0; i < chessEngine.optionSpin[Threads].value && !stopFlag.load(std::memory_order_relaxed) && hash_full.load(std::memory_order_relaxed) < 1000; ++i) {
        threads.emplace_back(thread_search, &thread_params[i]);
      }
      if (!chessEngine.ponder && !stopFlag.load(std::memory_order_relaxed) && hash_full.load(std::memory_order_relaxed) < 1000) {
        search_done.store(false, std::memory_order_relaxed); // Reset
        std::thread output_thread(uci_output_thread);
        for (auto& t : threads) t.join();
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
        search_done.store(true, std::memory_order_relaxed); // Signal search complete
        output_thread.join();
      } else {
        for (auto& t : threads) t.join();
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();        
      }

      multiPV = select_best_children(&idx); //select best child indexes
      //log_file("runMCTS() debug: select_best_children() returned multiPV %d\n", multiPV);
      multiPV = select_best_moves(&pv, &move_idx);
      //log_file("runMCTS() debug: select_best_moves() returned multiPV %d\n", multiPV);
      nodes = search.root->N.load(std::memory_order_relaxed);
      unique_nodes = search.tree.size();
      // Calculate hashfull (in per-mille)
      // hashfull using unique_nodes
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
        move_idx = (int *)calloc(1, sizeof(int));
        if (!move_idx) {
          log_file("runMCTS() error: calloc() failed for move_idx\n");
          exit(-1);
        }
        while (any) {
            src = lsBit(any);
            unsigned long long moves = board->sideToMoveMoves[src];
            while (moves) {
                dst = lsBit(moves);
              	if (promoMove(board, src, dst)) {
              	  for (int promo = Queen; promo >= Knight; promo--) {
                    struct Move move;
                    struct Board * tmp_board = cloneBoard(board);
              	    init_move(&move, tmp_board, src, dst, promo);
                    makeMove(&move);
                    if (tmp_board->isStaleMate) continue;
                    else {
                      move_idx[0] = src << 9 | dst << 3 | promo - 1; //1 will be added to Rook to make it Queen (default promo)
                      break;
                    }
              	  }
              	} else {
                  move_idx[0] = src << 9 | dst << 3;
                  break;
                }            
                moves &= moves - 1;
            }
            any &= any - 1;
        }
        idx_to_move(move_idx[0], best_move);
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
    
    double nps = nodes / elapsed;
    int depth = engineDepth.currentDepth.load(std::memory_order_relaxed);
    int seldepth = engineDepth.seldepth.load(std::memory_order_relaxed);
    for (int i = 0; i < multiPV && idx; i++) {      
      log_file("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s timeAllocated %.2f\n", depth, seldepth, i + 1, search.root->children[idx[i]].child->cp.load(std::memory_order_relaxed), nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, pv[i], timeAllocated * 0.001);
      print("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", depth, seldepth, i + 1, search.root->children[idx[i]].child->cp.load(std::memory_order_relaxed), nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, pv[i]);
    }
    if (!ponderHit.load()) {
      char * bestmove = strtok(pv[0], " ");
      char * ponder = strtok(NULL, " ");
      if (ponder) {
        log_file("bestmove %s ponder %s\n", bestmove, ponder);
        print("bestmove %s ponder %s\n", bestmove, ponder);
      } else {
        log_file("bestmove %s\n", bestmove);
        print("bestmove %s\n", bestmove);      
      }
    }
    if (idx) free(idx);
    if (pv) {
      for (int i = 0; i < multiPV; i++) free(pv[i]);
      free(pv);
    }
    if (move_idx) free(move_idx);
  }
}
