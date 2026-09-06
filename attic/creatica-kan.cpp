//using libtorch
// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -O3 -flto -I /Users/ap/Downloads/libtorch2/include -I /Users/ap/Downloads/libtorch2/include/torch/csrc/api/include -L /Users/ap/Downloads/libtorch2/lib -L /Users/ap/libchess -L /Users/ap/pytorch/build/lib -Wl,-lcurl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/Users/ap/Downloads/libtorch2/lib,-rpath,/Users/ap/libchess creatica-kan.cpp uci-kan.cpp tbcore.c tbprobe.c  -o creatica-kan

//For MacOS using clang and CoreML
// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -O3 -flto -L /Users/ap/libchess -Wl,-lcurl,-lchess,-rpath,/Users/ap/libchess creatica-kan.cpp uci-kan.cpp tbcore.c tbprobe.c CoreMLEngine.mm -framework CoreML -framework Foundation -fobjc-arc -o creatica-kan

// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -O3 -flto -I /Users/ap/Downloads/libtorch2/include -I /Users/ap/Downloads/libtorch2/include/torch/csrc/api/include -L /Users/ap/libchess -L /Users/ap/pytorch/build/lib -Wl,-lcurl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/Users/ap/pytorch/build/lib,-rpath,/Users/ap/libchess,-force_load creatica-cnn.cpp uci-cnn.cpp tbcore.c tbprobe.c /Users/ap/pytorch/build/lib/libpytorch_qnnpack.a -o creatica-cnn

// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -O3 -flto -L /Users/ap/libchess -Wl,-lcurl,-lchess,-rpath,/Users/ap/libchess cnn_network.cpp creatica-cnn.cpp uci-cnn.cpp tbcore.c tbprobe.c -o creatica-cnn

#include "creatica-kan.hpp"

std::mutex mtx, log_mtx, print_mtx, pool_mutex, search_done_mtx, probe_mutex;
std::shared_mutex map_mutex;
std::condition_variable cv, pool_cv, pool_done_cv, cv_search_done;
std::atomic<bool> searchFlag {false};
std::atomic<bool> stopFlag {false};
std::atomic<bool> quitFlag {false};
std::atomic<bool> ponderHit {false};
std::atomic<bool> pool_quit{false};    // True when engine exits
std::atomic<int> pool_generation{0};   // Increments every new search
std::atomic<int> active_workers{0};    // Count of currently working threads
std::atomic<bool> search_done{false}; // Signals search completion
std::atomic<uint64_t> total_children{0};
std::atomic<uint64_t> tbhits{0};
std::atomic<int> generation{0};
std::atomic<int> hash_full{0};
std::atomic<int> depth{0};
std::atomic<int> seldepth{0};
//std::atomic<int> total_nodes{0};
std::atomic<uint64_t> total_evals_completed{0};
std::atomic<uint64_t> total_batches{0};
std::atomic<int> inflight_count{0};

FILE * logfile = nullptr;
char best_move[6] = "";
bool tb_init_done = false;
double timeAllocated = 0.0; //ms
double exploration_min;
double exploration_max;
double exploration_depth_decay;
double probability_mass;
double virtual_loss;
const double eval_scale = EVAL_SCALE;
double temperature;
extern ChessChebyKAN kan_network;
PositionDedup pos_dedup;
bool use_policy_priors = false;   //set from the UCI option

std::string last_move;
std::unordered_set<uint64_t> position_history;
Board board = {};
ZobristHash zh = {};
Zobrist z = {};
Engine chessEngine;
MCTSSearch search;
std::vector<std::thread> pool_threads;
std::vector<ThreadParams> pool_params;

void cleanup() {
  for (auto& [h, node] : search.tree) {
    Edge * children = node->children.load(std::memory_order_relaxed);
    delete[] children;
    delete node;
  }
  search.tree.clear();
  search.root = nullptr;
  total_children.store(0, std::memory_order_relaxed);
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
      Edge * children = node->children.load(std::memory_order_relaxed);
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
      Edge * children = node->children.load(std::memory_order_relaxed);
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
void set_root(EvalQueue<65536>& queue) {
  auto it = search.tree.find(zh.hash);
  MCTSNode * root = (it != search.tree.end()) ? it->second : nullptr;
  if (!root) {
    root = new MCTSNode();
    root->hash.store(zh.hash, std::memory_order_relaxed);
    root->generation.store(generation.load(std::memory_order_relaxed), std::memory_order_relaxed);
    search.tree.emplace(zh.hash, root);
    std::unordered_set<uint64_t> pos_history;
    std::vector<MCTSNode*> path;
    path.push_back(root);
    expand(root, board, zh, pos_history, queue, path);
  }
  search.root = root;    
}

//called from expand_node()
//returns new or existing node
MCTSNode * make_child(const unsigned long hash) {
  //first, try to find child_hash in the tree
  std::shared_lock search_lock(map_mutex);
  auto it = search.tree.find(hash);
  MCTSNode * child = (it != search.tree.end()) ? it->second : nullptr;
  search_lock.unlock();
  if (!child) { //if the child_hash is not found, create a child
    child = new MCTSNode();
    //child->cp.store(cp, std::memory_order_relaxed);
    child->hash.store(hash, std::memory_order_relaxed);
    child->generation.store(generation.load(std::memory_order_relaxed));
    //child->terminal.store(0, std::memory_order_relaxed);
    //child->parent.store(parent, std::memory_order_relaxed);
    //we should probably update N and W as well. We're updating cp, the node is evaluated, meaning it's been visited
    /*if (cp != NO_MATE_SCORE) {
      child->N.store(1, std::memory_order_relaxed);
      child->W.store(tanh(cp / eval_scale), std::memory_order_relaxed);
    }*/
    std::unique_lock insert_lock(map_mutex);
    auto [it, inserted] = search.tree.emplace(hash, child);
    //Read the existing pointer BEFORE releasing the lock. `it` is an iterator into a
    //shared unordered_map, and another thread's emplace can rehash the map the instant
    //this unlocks -- which invalidates iterators. Dereferencing it afterwards returned a
    //garbage MCTSNode*, and the next node->mutex.try_lock() on it died with
    //"recursive_mutex lock failed: Invalid argument". Rare with one expansion per ~29
    //network evaluations; common once the policy head makes an expansion cost ONE.
    MCTSNode * existing = inserted ? nullptr : it->second;
    insert_lock.unlock();
    if (!inserted) {
      // Another thread inserted first; use the existing node and clean up ours.
      delete child;
      child = existing;
    }
  }
  return child; //may not be nullptr
}
  
struct TempEdge {
    int move = 0;
    double P = 0.0; 
    MCTSNode * child = nullptr;
};

//no locking, call only when search threads finished
//called from select_best_moves(), which in turn is called from runMCTS()
int most_visited_child(MCTSNode * parent) {
  uint64_t N = 0;
  int idx = -1;
  std::vector<std::pair<double, int>> priors; //prior, child index
  int num_children = parent->num_children.load(std::memory_order_relaxed); // will be 0 for the last node
  Edge * children = parent->children.load(std::memory_order_relaxed); // will be nullptr for the last node
  for (int i = 0; i < num_children; i++) { //this loop will be skipped for the last node
    MCTSNode * child = children[i].child.load(std::memory_order_relaxed);
    uint64_t n = child->N.load(std::memory_order_relaxed);
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
int select_best_moves(std::vector<std::pair<int, std::string>>& pvs) { 
  int num_children = search.root->num_children.load(std::memory_order_relaxed);
  if (!num_children) {
    log_file("select_best_moves() warning: root node has no children!\n");
    return 0;
  }
  char uci_move[6];
  std::vector<std::tuple<uint64_t, int>> visits; //N, child_idx
  Edge * children = search.root->children.load(std::memory_order_relaxed);
  for (int i = 0; i < num_children; i++) {
    MCTSNode * child = children[i].child.load(std::memory_order_relaxed);
    visits.push_back({child->N.load(std::memory_order_relaxed), i});
  }
  std::sort(visits.begin(), visits.end(), std::greater()); //sort children desc by the number of visits N    
  while (visits.size() > 1) {
    int idx = std::get<1>(visits[0]); //index of the most visited child
    int next_idx = std::get<1>(visits[1]); //index of the next most visited child
    MCTSNode * child = children[idx].child.load(std::memory_order_relaxed);
    MCTSNode * next_child = children[next_idx].child.load(std::memory_order_relaxed);
    //NNUE static eval is not reliable for deciding whether the position is winning
    //Let's try to use W instead. If it is positive, the position is winning 
    int cp = -child->cp.load(std::memory_order_relaxed);
    int next_cp = -next_child->cp.load(std::memory_order_relaxed);
    double w = -child->W.load(std::memory_order_relaxed);
    double next_w = -next_child->W.load(std::memory_order_relaxed);
    if (w > 0 && next_w > 0) { //check for repetition in winning position
      int global_count = position_history.count(child->hash.load(std::memory_order_relaxed));
      if (global_count) {
          int move = children[idx].move.load(std::memory_order_relaxed);
          int promo = (move >> 12) & 7;
          char fen[MAX_FEN_STRING_LEN];
          log_file("select_best_moves() debug: skipping move %s%s%c (would cause repetition in winning position %s, W %f, nextW %f, cp %d, nextCP %d)\n", square[(move >> 6) & 63], square[move & 63], promo != PieceTypeNone ? uciPromoLetter[promo] : ' ', board2fen(board, fen), w, next_w, cp, next_cp);
          visits.erase(visits.begin());
          continue;
      } else break;
    } else break;
  } // end of while (visits.size() > 1)
  int num_visits = visits.size();
  int multiPV = std::min<int>(num_visits, (int)chessEngine.optionSpin[MultiPV].value);
  int pvLength = chessEngine.optionSpin[PVPlies].value * sizeof(uci_move);
  int maxLen = pvLength - sizeof(uci_move);
  for (int i = 0; i < multiPV; i++) {
    int index = std::get<1>(visits[i]);
    idx2uci(children[index].move.load(std::memory_order_relaxed), uci_move);
    MCTSNode * child = children[index].child.load(std::memory_order_relaxed);      
    int cp = -child->cp.load(std::memory_order_relaxed);
    double parent_N = static_cast<double>(search.root->N.load(std::memory_order_relaxed));
    double prior = children[index].P.load(std::memory_order_relaxed);
    uint64_t N = child->N.load(std::memory_order_relaxed);
    double W = -child->W.load(std::memory_order_relaxed);
    double Q = N > 0 ? W / N : 0;
    int score_cp = static_cast<int>(std::atanh(std::clamp(Q, -0.999, 0.999)) * eval_scale);
    double U = exploration_max * prior * sqrt(parent_N) / (1 + N);
    std::string pv(uci_move);
    std::string pv2(uci_move);
    pv2 += " (N=" + std::to_string(N) + ", W=" + std::to_string(llround(W)) + ", score=" + std::to_string(score_cp) + ", cp=" + std::to_string(cp) + ", Q(" + std::to_string(Q) + ") + U(" + std::to_string(U) + ") = " + std::to_string(Q + U) + ")";
    // Build PV by following most visited children
    num_children = child->num_children.load(std::memory_order_relaxed);
    Edge * children2 = child->children.load(std::memory_order_relaxed);
    int depth = 0;
    while (num_children > 0 && pv.size() < maxLen) {
      int idx = most_visited_child(child); 
      if (idx < 0) break;
      depth++;
      parent_N = static_cast<double>(child->N.load(std::memory_order_relaxed));
      prior = children2->P.load(std::memory_order_relaxed);
      idx2uci(children2[idx].move.load(std::memory_order_relaxed), uci_move);
      pv += ' ';
      pv.append(uci_move);  
      pv2 += ' ';
      pv2.append(uci_move);   
      child = children2[idx].child.load(std::memory_order_relaxed);
      int cp2 = -child->cp.load(std::memory_order_relaxed);
      N = child->N.load(std::memory_order_relaxed);
      W = -child->W.load(std::memory_order_relaxed);
      Q = W / N;
      U = std::max(exploration_min, exploration_max - (depth * exploration_depth_decay)) * exploration_max * prior * sqrt(parent_N) / (1 + N);
      pv2 += " (" + std::to_string(N) + ", " + std::to_string(llround(W)) + ", " + std::to_string(cp2) + ", " + std::to_string(Q) + " + " + std::to_string(U) + " = " + std::to_string(Q + U) + ")";
      children2 = child->children.load(std::memory_order_relaxed);
      num_children = child->num_children.load(std::memory_order_relaxed);
    } //end of while (num_children > 0 && pv.size() < maxLen)
    pvs.push_back({cp, pv});
    log_file("select_best_moves() debug: PV[%d] %s\n", i, pv2.c_str());
    //std::sort(pvs.begin(), pvs.end(), std::greater<>()); //this is incorrect because short pv have less accurate score
  } // end for (int i = 0; i < multiPV; i++)
  return multiPV;
}

int select_best_child(MCTSNode * parent, int depth, std::mt19937& mt) {
  // No locks needed: atomics handle visibility
  int num_children = parent->num_children.load(std::memory_order_acquire);
  Edge * children = parent->children.load(std::memory_order_acquire);
  if (num_children == 0) return -1;
  double best_score = -INFINITY;
  int selected = 0;
  double C = std::max(exploration_min, exploration_max - (depth * exploration_depth_decay));
  // Get parent Q to use as FPU
  //double parent_Q = tanh(parent->cp.load(std::memory_order_relaxed) / eval_scale);
  double parent_W = parent->W.load(std::memory_order_relaxed);
  uint64_t parent_N = parent->N.load(std::memory_order_acquire);
  for (int i = 0; i < num_children; i++) {
    //parent->mutex.lock_shared();
    double P = children[i].P.load(std::memory_order_relaxed);
    MCTSNode * child = children[i].child.load(std::memory_order_acquire);
    //parent->mutex.unlock_shared();
    uint64_t N = child->N.load(std::memory_order_relaxed);
    double W = -child->W.load(std::memory_order_relaxed);
    double Q = N ? (W / N) : parent_N ? parent_W / parent_N : 0.0;  // Q uses real N, not inflated
    double exploration = C * P * sqrt((double)parent_N) / (1.0 + N);  // exploration penalizes in-flight
    double score = Q + exploration;
    if (score > best_score) {
      best_score = score;
      selected = i;
    }
  }
  // Apply virtual loss to the winner
  //parent->mutex.lock_shared();
  MCTSNode * selected_child = children[selected].child.load(std::memory_order_acquire);
  //parent->mutex.unlock_shared();
  selected_child->W.fetch_sub(virtual_loss, std::memory_order_relaxed); 
  selected_child->N.fetch_add(1, std::memory_order_relaxed); 
  return selected;
}

//return the status of expansion (0 - pending eval, 1 - mate for chess_board.sideToMove, 2 - stalemate)
int expand(MCTSNode * node, Board& chess_board, const ZobristHash& board_hash, const std::unordered_set<uint64_t>& pos_history, EvalQueue<65536>& queue, const std::vector<MCTSNode*>& path) {
  std::vector<std::tuple<MCTSNode *, int, Board>> children; //node *, move_idx, board 
  Move move;
  auto [king_moves, pinned, pinning, checkers, kingSquare] = kingMoves(chess_board);
  move.src = kingSquare;
  move.promoType = PieceTypeNone;
  while (king_moves) {
    StateInfo state = {}; //keep track of num_moves and isCheck but not isMate or isStaleMate
    ZobristHash child_hash = board_hash;
    move.dst = popLSB(king_moves);
    updateHash(child_hash, chess_board, move, do_move(chess_board, move, state), z); //do the move, update the hash
    if (pos_history.count(child_hash.hash) > 0) {
      undo_move(chess_board, move, state);
      continue; //repetition - skip it
    }
    MCTSNode * child = make_child(child_hash.hash);
    //Detect a terminal CHILD here, as the NNUE engine does at creatica-shared-root.cpp:413.
    //Without it the network sees a mating position as ordinary and the mate is only found
    //if that child is itself expanded much later - a forced mate in one came back as
    //"score cp 589". One legal-move generation per child, no network evaluation.
    //Safe now that StateInfo carries isMate/isStaleMate, so undo_move() restores them.
    if (child->terminal.load(std::memory_order_acquire) == 0) {
      isCheckMateStaleMate(chess_board);
      const int want = chess_board.isMate ? 1 : (chess_board.isStaleMate ? 2 : 0);
      if (want) {
        //The load above is not atomic with these stores, and two threads can expand into
        //the same transposed child. The cp write is idempotent but the W/N seeding is not,
        //so claim the node with a CAS and let only the winner seed.
        int expected = 0;
        if (child->terminal.compare_exchange_strong(expected, want,
              std::memory_order_release, std::memory_order_relaxed)) {
          child->cp.store(want == 1 ? -MATE_SCORE : 0, std::memory_order_relaxed);
          //Mate reaches the search ONLY through cp -> finalize_priors today: e = -cp*0.01
          //is +200 pawns, so the mating edge takes P = 1.0 and every sibling underflows.
          //A policy head knows nothing about terminality, so seed the child's own W/N
          //instead. W is in the CHILD's frame and the parent reads -child->W, so a mated
          //child is W = -1 (it has lost) and the parent sees Q = +1.
          child->W.store(want == 1 ? -1.0 : 0.0, std::memory_order_relaxed);
          child->N.store(1, std::memory_order_relaxed);
        }
      }
    }
    children.push_back({child, (move.promoType << 12) | (move.src << 6) | move.dst, chess_board});
    undo_move(chess_board, move, state);
  }
  if (bitCount(checkers) <= 1) {
    auto [check_mask, ep_mask] = checkers ? checkMask(chess_board, kingSquare, checkers) : std::make_pair(0xffffffffffffffffULL, 0ULL);
    for (PieceType pt = Queen; pt >= Pawn; --pt) {
    	uint64_t occupations = chess_board.side[chess_board.sideToMove] & chess_board.pieceTypes[pt - 1]; 
    	while (occupations) {
    	  move.src = popLSB(occupations);
	      uint64_t moves = piece_moves(chess_board, pt, move.src, kingSquare, pinned, pinning, check_mask, ep_mask);
    	  while (moves) {
    	    move.dst = popLSB(moves);
        	PieceType startPiece = PieceTypeNone, endPiece = PieceTypeNone;
        	if (promoMove(chess_board, move)) {
        	  startPiece = Knight;
        	  endPiece = Queen;
        	}
      	  for (move.promoType = startPiece; move.promoType <= endPiece; ++move.promoType) { //loop over promotions if any
            StateInfo state = {}; //keep track of num_moves and isCheck but not isMate or isStaleMate
            ZobristHash child_hash = board_hash;
            updateHash(child_hash, chess_board, move, do_move(chess_board, move, state), z); //do the move, update the hash
            if (pos_history.count(child_hash.hash) > 0) {
              undo_move(chess_board, move, state);
              continue; //repetition - skip it
            }
            MCTSNode * child = make_child(child_hash.hash);
            //Detect a terminal CHILD here, as the NNUE engine does at creatica-shared-root.cpp:413.
            //Without it the network sees a mating position as ordinary and the mate is only found
            //if that child is itself expanded much later - a forced mate in one came back as
            //"score cp 589". One legal-move generation per child, no network evaluation.
            //Safe now that StateInfo carries isMate/isStaleMate, so undo_move() restores them.
            if (child->terminal.load(std::memory_order_acquire) == 0) {
              isCheckMateStaleMate(chess_board);
              const int want = chess_board.isMate ? 1 : (chess_board.isStaleMate ? 2 : 0);
              if (want) {
                //Not atomic with the load above, and two threads can expand into the same
                //transposed child. cp is idempotent, W/N seeding is not -- claim with a CAS.
                int expected = 0;
                if (child->terminal.compare_exchange_strong(expected, want,
                      std::memory_order_release, std::memory_order_relaxed)) {
                  child->cp.store(want == 1 ? -MATE_SCORE : 0, std::memory_order_relaxed);
                  //Mate reaches the search only via cp -> finalize_priors today; a policy head
                  //knows nothing about terminality. W is in the CHILD's frame and the parent
                  //reads -child->W, so a mated child is W = -1 and the parent sees Q = +1.
                  child->W.store(want == 1 ? -1.0 : 0.0, std::memory_order_relaxed);
                  child->N.store(1, std::memory_order_relaxed);
                }
              }
            }
            children.push_back({child, (move.promoType << 12) | (move.src << 6) | move.dst, chess_board});
            undo_move(chess_board, move, state);
      	  }
        }
      }
    }
  }
  if (chess_board.num_moves == 0) {
    if (checkers) {
      return 1; //mate
    } else {
      return 2; //stalemate
    }
  }
  size_t num_children = children.size();
  if (!num_children) return 0;
  
  //Build the whole Edge array first, so no reader can see a half-filled entry.
  Edge * edges = new Edge[num_children];
  for (int i = 0; i < num_children; ++i) {
    edges[i].child.store(std::get<0>(children[i]), std::memory_order_release); 
    edges[i].move.store(std::get<1>(children[i]), std::memory_order_release);
    //Seed a uniform prior. Edge::P defaults to 0.0, and between the publish below and
    //the priors arriving from finalize_priors there is a window in which every P is 0:
    //exploration = C*P*sqrt(parent_N)/(1+N) is then 0 for every child, every child has
    //N == 0 so every Q is the same FPU value, and select_best_child's strict
    //`score > best_score` against -INFINITY returns index 0 unconditionally -- a king
    //move, since king moves are generated first. One batch wide today; a full GPU round
    //trip once priors come from the policy head.
    edges[i].P.store(1.0 / (double)num_children, std::memory_order_relaxed);
  }

  //PUBLISH THE NODE BEFORE ANY EVAL CAN COMPLETE. The inference server calls
  //finalize_priors() the instant pending_evals reaches zero, and finalize_priors
  //returns immediately when num_children == 0 (creatica-kan.hpp:502-503). Enqueuing
  //inside the fill loop - as this did - let the server finish every child before
  //num_children was ever stored, so finalize_priors bailed out, EVERY edge prior
  //stayed at its default 0.0, PUCT's exploration term vanished and the search
  //degenerated into the logged 'seldepth 176, pv a1a1 a1a1 ...'.
  node->children.store(edges, std::memory_order_release);
  node->num_children.store(num_children, std::memory_order_release);
  total_children.fetch_add(num_children, std::memory_order_relaxed); //update total_children counter
  //Arm the counter only once the node is visible, then enqueue.
  if (use_policy_priors) {
    //One push for the PARENT. pending_evals stays 0 - there are no per-child completions,
    //and the server must not decrement it. chess_board is back at the parent position
    //after the last undo_move, and `path` already ends with `node` (mcts_search:516).
    while (inflight_count.load(std::memory_order_acquire) >= MAX_INFLIGHT_POLICY)
      std::this_thread::yield();
    inflight_count.fetch_add(1, std::memory_order_acq_rel);
    queue.push(node, chess_board, path, /*policy=*/true);
    return 0;
  }

  node->pending_evals.store(num_children, std::memory_order_release);

  for (int i = 0; i < num_children; ++i) {
    // Wait until we're allowed to push more work
    while (inflight_count.load(std::memory_order_acquire) >= MAX_INFLIGHT) {
        // This spin‑wait is okay because the server frees slots quickly.
        std::this_thread::yield();
    }
    inflight_count.fetch_add(1, std::memory_order_acq_rel);    
    queue.push(std::get<0>(children[i]), std::get<2>(children[i]), path);
  }
  return 0; //pending eval
}

void mcts_search(ThreadParams& params, EvalQueue<65536>& queue) {
  std::vector<MCTSNode *> path;  // Track the path from root to leaf
  path.reserve(chessEngine.depth);
  int terminal = 0;
  params.seldepth = 0;
  MCTSNode * node = search.root;
  //start from the same initial position given by board (at the root node, i.e. at the top of the tree - the up side down tree)
  //copy the board and the hash to preserve it for subsequent iterations
  Board sim_board = board;
  ZobristHash sim_zh = zh;
  // Selection
  //iterate down the tree updating sim_board by initiating and making moves
  //thread-local map to prevent repetition cycles - it should be global I think but thread-safety may be a problem
  std::unordered_set<uint64_t> pos_history;
  while (node->num_children.load(std::memory_order_acquire) > 0) { //traversal stops at a leaf or at repetition (mate or stalemate node should not have children but pending nodes may)
    //return child node index with the best score using PUCT (Predictor + Upper Confidence Bound)
    //it also adds virtual loss to the node to reduce contention for the same node in multi-threaded engine
    /*if (node->pending_evals.load(std::memory_order_acquire) > 0) {
      break; 
    }*/
    int idx = select_best_child(node, params.seldepth, params.mt);
    if (idx == -1) break;
    Edge * children = node->children.load(std::memory_order_acquire);
    std::shared_lock lock(node->mutex);
    int move_idx = children[idx].move.load(std::memory_order_relaxed);
    path.push_back(node);  // Add parent node to path
    //continue iterating down the tree by getting next node until no more children
    node = children[idx].child.load(std::memory_order_acquire);
    lock.unlock();
    //init and take edge's move that leads to the child node
    Move move;
    move.dst = (Square)(move_idx & 63);
    move.src = (Square)((move_idx >> 6) & 63);
    move.promoType = (PieceType)((move_idx >> 12) & 7);
    //update Zobrist hash (it is needed so that we can call updateHash() later instead of getHash()
    //char fen[MAX_FEN_STRING_LEN];
    //printf("mcts_search(%d) debug: depth %d, fen %s move %s%s%c\n", params.thread_id, params.seldepth, board2fen(sim_board, fen), square[move.src], square[move.dst], uciPromoLetter[move.promoType]);
		updateHash(sim_zh, sim_board, move, ff_move(sim_board, move), z);
    params.seldepth++;
    int global_count = position_history.count(sim_zh.hash); //actual positions that have occured in the game
    int path_count = pos_history.count(sim_zh.hash); //simulated positions ahead of the current one
    if (path_count == 0) pos_history.insert(sim_zh.hash);
    bool repetition = (global_count + path_count >= 1); 
    if (repetition) {
      //Repetition is a property of the PATH taken to reach this position, not of the
      //position itself, so it must stay local to this simulation. Stamping the shared
      //transposition node - as this used to - was a textbook graph-history-interaction
      //bug: every other path that later transposed into the node inherited a draw
      //verdict it had not earned. Worse, `terminal` doubles as the "priors finalized"
      //flag (finalize_priors stores 4, and :592/:632 test < 4), so writing 3 also
      //reset an already-expanded node to "not ready".
      terminal = 3;
      break;
    }
  } //end of while(node.num_children > 0) loop
  path.push_back(node);  // Add leaf node to path - sim_board corresponds to this node!
  //Here we are at the bottom of the tree, i.e. at a leaf or at the terminal node (mate, stalemate)
  // Evaluation - the node is already evaluated during previous expansion!
  // We could actually improve the eval by using move_evals calculated later in the code for the children nodes before expansion for evaluating its parent (this node), kind of look ahead eval
  int scorecp = 0;
  double result = 0.0;
  bool enqueued = false; //did this simulation actually hand work to the inference server?
  if ((terminal == 0) && pos_dedup.insert(sim_zh.hash)) {
    if (node->mutex.try_lock()) { //the leaf node in a tree is locked only for expansion
                                //nodes locked in selection phase are not leaf nodes, i.e. nodes without children
                                //if leaf node is already locked, it means that other thread is expanding it already
                                //or try_lock() spuriously fails!
      int res = expand(node, sim_board, sim_zh, pos_history, queue, path);
      node->mutex.unlock();
      //expand() publishes children/num_children BEFORE it enqueues, so a non-zero
      //num_children here means work really was handed to the server and a completion
      //will eventually backpropagate this path.
      if (res == 0) enqueued = (node->num_children.load(std::memory_order_acquire) > 0);
      if (res == 1) {
        result = -1;
        node->cp.store(-MATE_SCORE, std::memory_order_relaxed);
        node->terminal.store(1, std::memory_order_relaxed); //mate
        terminal = 1;
      } else if (res == 2) {
        node->cp.store(0, std::memory_order_relaxed);
        node->terminal.store(2, std::memory_order_relaxed); //stalemate
        terminal = 2;
      }
    } //end of if (node.mutex.try_lock())
  } //end of else if (terminal != 3) - not a repetition
  
  if (terminal > 0) { //terminal (mate, stalemate, repetition except pending)
    //scorecp = node->cp.load(std::memory_order_relaxed);
    //result = tanh(scorecp / eval_scale);
    if (terminal == 1) result = -1;
    else result = 0;
    // Backpropagation: revert virtual loss and add result
    for (auto n = path.rbegin(); n != path.rend(); ++n) {
      node = *n;
      node->W.fetch_add(result + virtual_loss, std::memory_order_relaxed);
      result = -result;
    }
    search.root->W.fetch_sub(virtual_loss, std::memory_order_relaxed); //virtual loss has not been applied to root!
    search.root->N.fetch_add(1, std::memory_order_relaxed); //only increment root visit count, other nodes already incremented in select_best_child() applicaiton of virtual loss
  } else if (!enqueued) {
    //Nothing was enqueued for this path - the position was already in pos_dedup, or
    //another thread held the expansion lock. No completion will ever arrive, so the
    //virtual loss select_best_child() applied on the way down would stay on every
    //node of this path FOREVER, permanently biasing W. Revert it exactly as the
    //terminal branch above does, with a result of zero.
    for (auto n = path.rbegin(); n != path.rend(); ++n) {
      (*n)->W.fetch_add(virtual_loss, std::memory_order_relaxed);
    }
    search.root->W.fetch_sub(virtual_loss, std::memory_order_relaxed);
    search.root->N.fetch_add(1, std::memory_order_relaxed);
  }  
}

void uci_output_thread() {
  auto iter_start = std::chrono::steady_clock::now();

  while (!stopFlag.load(std::memory_order_relaxed) && !search_done.load(std::memory_order_relaxed)) {
    std::unique_lock<std::mutex> lk(search_done_mtx);
    cv_search_done.wait_for(lk, std::chrono::milliseconds(1000), []{return search_done.load(std::memory_order_relaxed);});
    // Calculate nodes (total simulations)
    MCTSNode * current_node = search.root;
    uint64_t nodes = current_node->N.load(std::memory_order_relaxed);
    // Get elapsed time
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
    // Compute NPS
    double nps = elapsed > 0 ? nodes / elapsed : 0;
    //depth should be calculated by traversing the most visited nodes similar to select_best_moves()      
    int d = 0;
    std::unordered_set<uint64_t> visited; //hash
    std::vector<std::pair<uint64_t, int>> visits; //N, child_idx
    int num_root_children = current_node->num_children.load(std::memory_order_relaxed);
    while (current_node && d < MAX_DEPTH) {
      uint64_t current_hash = current_node->hash.load(std::memory_order_relaxed);
      if (visited.find(current_hash) != visited.end()) {
          log_file("uci_output_thread() debug: cycle detected at depth %d, breaking loop\n", d);
          break;
      }
      visited.insert(current_hash);        
      uint64_t N = 0;
      int next_idx = -1;
      int num_children = current_node->num_children.load(std::memory_order_acquire);
      Edge * children = current_node->children.load(std::memory_order_acquire);
      for (int i = 0; i < num_children; i++) {
        MCTSNode * child = children[i].child.load(std::memory_order_acquire);
        uint64_t n = child->N.load(std::memory_order_relaxed);
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
    log_file("uci_output_thread() debug: unique_nodes %lld, total_children %lld, max_capacity %lld\n", unique_nodes, total_children.load(std::memory_order_relaxed), max_capacity);
    int hashfull = max_capacity ? (total_memory * 1000) / max_capacity : 0;
    if (hashfull > 1000) hashfull = 1000;  // Cap at 1000 per UCI spec
    hash_full.store(hashfull, std::memory_order_relaxed);

    std::sort(visits.begin(), visits.end(), std::greater<>());
    int multiPV = std::min<int>(num_root_children, (int)chessEngine.optionSpin[MultiPV].value);
    Edge * children = search.root->children.load(std::memory_order_acquire);
    for (int i = 0; i < multiPV; i++) {
      const int move_idx = children[visits[i].second].move.load(std::memory_order_relaxed);
      char uci_move[6];
      idx2uci(move_idx, uci_move);
      MCTSNode * child = children[visits[i].second].child.load(std::memory_order_acquire);
      //Skip only nodes that are genuinely unevaluated. A terminal child (mate or
      //stalemate) carries a real score and MUST be reported - the old `terminal < 4`
      //test hid every mate from the PV, which is why a forced mate in one came back
      //as "score cp 456" and the mating move was never played.
      if (!child->evaluated.load(std::memory_order_acquire) &&
          child->terminal.load(std::memory_order_acquire) == 0) {
        continue; // skip unevaluated nodes
      }
      // Better score for UCI output:
      double W = -child->W.load(std::memory_order_relaxed);
      uint64_t N = child->N.load(std::memory_order_relaxed);
      double Q = N > 0 ? W / N : 0.0;
      int score_cp = static_cast<int>(std::atanh(std::clamp(Q, -0.999, 0.999)) * eval_scale);
      log_file("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f W %f N %lldpv %s\n", d, seldepth.load(std::memory_order_relaxed), i + 1, score_cp, nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, W, N, uci_move);
      print("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", d, seldepth.load(std::memory_order_relaxed), i + 1, score_cp, nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, uci_move);
      //printf("Debug: evals done %llu, root N %llu\n", total_evals_completed.load(), search.root->N.load());
    }
  }
}


void runMCTS(EvalQueue<65536>& queue) {
  double elapsed = 0.0;
  size_t unique_nodes = 0;
  uint64_t nodes = 0;
  int hashfull = 0;
  std::vector<std::pair<int, std::string>> pvs;
  int multiPV = 1;
  
  isCheckMateStaleMate(board);
  if (board.num_moves > 1) {
    tbhits.store(0, std::memory_order_relaxed);
    //it seems there rarely is some kind of contamination or corruption of the tree
    //so let's try cleanup() instead of gc() if UCI Ponder option is false
    //avoid using Ponder option, sometimes called "permanent brain", i.e. thinking during opponent's time
    //Drain work still in flight from the PREVIOUS search before freeing the tree.
    //cleanup() and gc() below delete every MCTSNode and every Edge array, while the
    //inference server runs continuously and never stops between searches. A late
    //write-back dereferences leaf->children and STORES into it, so a stale item writes
    //into freed memory -- that is the "contamination or corruption of the tree" noted
    //just below. Rare with child evaluations, where the write-back only touches edges
    //when pending_evals reaches zero; reliable with policy priors, where every item
    //rewrites the whole edge array. Bounded so a lost item cannot hang the engine.
    {
      const auto t0 = std::chrono::steady_clock::now();
      while (inflight_count.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(5)) {
          log_file("warning: %ld evaluations still in flight after 5s; freeing anyway\n",
                   (long)inflight_count.load(std::memory_order_relaxed));
          break;
        }
        std::this_thread::yield();
      }
    }
    pos_dedup.clear();
    if (chessEngine.optionCheck[Ponder].value) {
      set_root(queue);      
      gc();
    } else {
      cleanup(); 
      set_root(queue);      
    }
    // Inside runMCTS, after set_root(queue)
    //Also break out when the root is itself terminal, or this spins forever on a
    //position that is already mate or stalemate.
    while (!search.root->evaluated.load(std::memory_order_acquire) &&
           search.root->terminal.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    int num_threads = chessEngine.optionSpin[Threads].value;
    if (pool_threads.size() != num_threads) {
         log_file("Warning: Pool size mismatch: current threads %d != configured %d. Re-initializing...\n", pool_threads.size(), num_threads);
         init_thread_pool(num_threads, std::ref(queue));
    }
    if (!chessEngine.depth) chessEngine.depth = MAX_DEPTH;
    depth.store(0, std::memory_order_relaxed);
    seldepth.store(0, std::memory_order_relaxed);
    auto iter_start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        pool_params[i].thread_id = i;
        pool_params[i].time_alloc = timeAllocated;
        pool_params[i].seldepth = 0;
    }
    active_workers.store(num_threads); // Register how many we expect to run
    pool_generation.fetch_add(1);      // Increment generation ID
    pool_cv.notify_all();              // SIGNAL: "Start Engines!"
        
    if (chessEngine.optionCheck[IntermittentInfoLines].value && !chessEngine.ponder) {
      search_done.store(false);
      std::thread output_thread(uci_output_thread);
      // Wait until all workers are done
      while (active_workers.load()) {
        std::unique_lock<std::mutex> lock(pool_mutex);
        pool_done_cv.wait(lock, [] { return active_workers.load() == 0; });
        lock.unlock();
        if (active_workers.load()) continue;
      }
      // Signal output thread to stop
      {
        std::lock_guard<std::mutex> lk(search_done_mtx);
        search_done.store(true);
      }
      cv_search_done.notify_one();
      output_thread.join();
    } else {
      // Just wait for workers
      while (active_workers.load()) {
        std::unique_lock<std::mutex> lock(pool_mutex);
        pool_done_cv.wait(lock, []{ return active_workers.load() == 0; });
        lock.unlock();
        if (active_workers.load()) continue;
      }
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
  } else if (board.num_moves == 1) { //skip MCTS and just make the move
      auto iter_start = std::chrono::steady_clock::now();      
      int move_idx = 0;
      Move move;
      auto [moves, pinned, pinning, checkers, kingSquare] = kingMoves(board);
      move.src = kingSquare;
      move.promoType = PieceTypeNone;
  	  while (moves && move_idx == 0) {
  	    move.dst = lsBit(moves);
  	    move_idx = (move.promoType << 12) | (move.src << 6) | move.dst;
        moves &= moves - 1;
      }
      auto [check_mask, ep_mask] = checkers ? checkMask(board, kingSquare, checkers) : std::make_pair(0xffffffffffffffffULL, 0ULL);
      for (PieceType pt = Queen; pt >= Pawn && move_idx == 0; pt = (PieceType)(pt - 1)) {
      	uint64_t occupations = board.side[board.sideToMove] & board.pieceTypes[pt - 1]; 
      	while (occupations && move_idx == 0) {
      	  move.src = lsBit(occupations);
  	      moves = piece_moves(board, pt, move.src, kingSquare, pinned, pinning, check_mask, ep_mask);
      	  while (moves && move_idx == 0) {
      	    move.dst = lsBit(moves);
          	PieceType startPiece = PieceTypeNone, endPiece = PieceTypeNone;
          	if (promoMove(board, move)) {
          	  startPiece = Knight;
          	  endPiece = Queen;
          	}
        	  for (move.promoType = startPiece; move.promoType <= endPiece && move_idx == 0; move.promoType = (PieceType)(move.promoType + 1)) { //loop over promotions if any
          	  move_idx = (move.promoType << 12) | (move.src << 6) | move.dst;
        	  }
            moves &= moves - 1;
          }
          occupations &= occupations - 1;
        }
      }
      StateInfo state = {};
      do_move(board, move, state);
  		isCheckMateStaleMate(board);
      double res = 0;
      if (board.isMate) res = MATE_SCORE * 0.01;
      else if (board.isStaleMate) res = 0.0;
      else {
      	//std::vector<std::tuple<double, int, int, unsigned long long>> move_evals;
      	//ZobristHash tmp_zh = zh;
        //compute_move_evals(board, tmp_zh, move_evals, probability_mass);
        //res = -std::get<2>(move_evals[0]) * 0.01;
        //res = 0;
      }
      undo_move(board, move, state);      
      idx2uci(move_idx, best_move);
      std::string pv(best_move);
      pvs.push_back({static_cast<int>(100.0 * res), pv});
      multiPV = 1;
      if (chessEngine.optionCheck[FinalInfoLines].value) {    
        nodes = 1;
        depth.store(1, std::memory_order_relaxed);
        seldepth.store(1, std::memory_order_relaxed);
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
      }
  } //board.num_moves == 1
  else {
      if (board.isMate) {
          log_file("info depth 0 score mate 0\n");
          log_file("bestmove (none)\n");
          print("info depth 0 score mate 0\n");
          print("bestmove (none)\n");
      }
      else if (board.isStaleMate) {
          log_file("info depth 0 score cp 0\n");
          log_file("bestmove (none)\n");
          print("info depth 0 score cp 0\n");
          print("bestmove (none)\n");
      }
      return;
  }
  if (chessEngine.optionCheck[FinalInfoLines].value) {
    double nps = nodes / elapsed;
    for (int i = 0; i < multiPV; i++) {      
      log_file("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s timeAllocated %.2f\n", depth.load(std::memory_order_relaxed), seldepth.load(std::memory_order_relaxed), i + 1, pvs[i].first, nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, pvs[i].second.c_str(), timeAllocated * 0.001);
      print("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", depth.load(std::memory_order_relaxed), seldepth.load(std::memory_order_relaxed), i + 1, pvs[i].first, nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, pvs[i].second.c_str());
    }
  }
  if (!ponderHit.load(std::memory_order_relaxed)) {
    std::string bestmove;
    std::string ponder;
    if (!pvs.empty()) {
      int pos = pvs[0].second.find(" ");
      int pos2 = pvs[0].second.find(" ", pos + 5);
      bestmove = pvs[0].second.substr(0, pos);
      if (pos != std::string::npos) ponder = pvs[0].second.substr(pos + 1, pos2 - pos - 1);
      //here we need to make bestmove to update position_history
      if (!chessEngine.ponder) {
        Move move = {};
        uci2move_idx(bestmove.c_str(), move);
        updateHash(zh, board, move, ff_move(board, move), z);
        position_history.insert(zh.hash);
      }
    } else bestmove = "(none)"; //pvs is empty! 
    if (!ponder.empty()) {
      log_file("bestmove %s ponder %s\n", bestmove.c_str(), ponder.c_str());
      print("bestmove %s ponder %s\n", bestmove.c_str(), ponder.c_str());
    } else {
      log_file("bestmove %s\n", bestmove.c_str());
      print("bestmove %s\n", bestmove.c_str());      
    }
  }
}

