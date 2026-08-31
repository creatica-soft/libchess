//For MacOS using clang
// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -I /Users/ap/libchess  -L /Users/ap/libchess -Wl,-lchess,-lcurl,-rpath,/Users/ap/libchess creatica-shared-root.cpp uci-shared-root.cpp tbcore.c tbprobe.c -o creatica-shared-root

// -O3 -g -fno-omit-frame-pointer - options for profiling
//

// For linux or Windows using mingw
// add -mpopcnt for X86_64
// might need to add -Wno-stringop-overflow to avoid some warnings in tbcore.h
//g++ -std=c++20 -mpopcnt -Wno-deprecated -Wno-write-strings -Wno-deprecated-declarations -Wno-stringop-overflow -O3 -I /home/ap/libchess -L /home/ap/libchess creatica.cpp uci.cpp tbcore.c tbprobe.c -o creatica -lchess

//or with clang in MSYS2 MINGW64 or CLANG64
//clang++ -std=c++20 -mpopcnt -Wno-deprecated -Wno-write-strings -Wno-deprecated-declarations -O3 -flto -I /home/ap/libchess -L /home/ap/libchess creatica.cpp uci.cpp tbcore.c tbprobe.c -o creatica -lchess

#include "creatica-shared-root.hpp"

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

FILE * logfile = nullptr;
char best_move[6] = "";
bool tb_init_done = false;
double timeAllocated = 0.0; //ms
double exploration_min;
double exploration_max;
double exploration_depth_decay;
//double probability_mass;
double virtual_loss;
double eval_scale;
double temperature;

std::string last_move;
std::unordered_set<uint64_t> position_history;
Board board = {};
ZobristHash zh = {};
Zobrist z = {};
Engine chessEngine = {};
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
              //if (child->generation == expected) child.generation = current_gen; else expected = child.generation; 
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
void set_root(NNUEContext& ctx) {
  auto it = search.tree.find(zh.hash);
  MCTSNode * root = (it != search.tree.end()) ? it->second : nullptr;
  if (!root) {
    root = new MCTSNode();
    root->hash.store(zh.hash, std::memory_order_relaxed);
    root->generation.store(generation.load(std::memory_order_relaxed), std::memory_order_relaxed);
    search.tree.emplace(zh.hash, root);
    std::unordered_set<uint64_t> pos_history;
    double result = eval_and_expand(root, board, zh, ctx, pos_history, 0);
    root->W.store(result, std::memory_order_relaxed);
    root->N.store(1, std::memory_order_relaxed);
  }
  search.root = root;    
}

//called from expand_node() and make_move()
//returns new or existing node
MCTSNode * make_child(const uint64_t hash, const int cp, const int terminal) {
  //first, try to find child_hash in the tree
  std::shared_lock search_lock(map_mutex);
  auto it = search.tree.find(hash);
  MCTSNode * child = (it != search.tree.end()) ? it->second : nullptr;
  search_lock.unlock();
  if (!child) { //if the child_hash is not found, create a child
    child = new MCTSNode();
    child->cp.store(cp, std::memory_order_relaxed);
    child->terminal.store(terminal, std::memory_order_relaxed);
    child->hash.store(hash, std::memory_order_relaxed);
    child->generation.store(generation.load(std::memory_order_relaxed));
    //we should probably update N and W as well. We're updating cp, the node is evaluated, meaning it's been visited
    if (cp != NO_MATE_SCORE) {
      child->N.store(1, std::memory_order_relaxed);
      child->W.store(tanh(cp * 0.01 / eval_scale), std::memory_order_relaxed);
    }
    std::unique_lock insert_lock(map_mutex);
    auto [it, inserted] = search.tree.emplace(hash, child);
    insert_lock.unlock();
    if (!inserted) {
      // Another thread inserted first; use the existing node and clean up ours.
      delete child;
      child = it->second; //it.second - is a pointer to the existing node (it.first is a hash)
    }
  }
  return child; //may not be nullptr
}
  
struct TempEdge {
    int move = 0;
    double P = 0.0; 
    MCTSNode * child = nullptr;
};
//called from mcts_search() and process_check()
//calls make_child()
void expand_node(MCTSNode * parent, const std::vector<std::tuple<double, int, int, int, uint64_t>>& top_moves) {
  //parent's expansion gate is held by the caller - mcts_search(); NOTE process_check() -> eval_and_expand() calls this WITHOUT holding the gate
  if (parent->num_children.load(std::memory_order_relaxed) > 0) return; //already expanded by other threads, perhaps
  int num_moves = top_moves.size();
  assert(num_moves > 0);
  Edge * children = new Edge[num_moves];
  for (int i = 0; i < num_moves; ++i) {
      auto [prior, move_idx, child_cp, terminal, child_hash] = top_moves[i];
      MCTSNode * child = make_child(child_hash, child_cp, terminal);
      children[i].P.store(prior, std::memory_order_relaxed);
      children[i].move.store(move_idx, std::memory_order_relaxed);
      children[i].child.store(child, std::memory_order_relaxed);        
  }
  total_children.fetch_add(num_moves, std::memory_order_relaxed); //update total_children counter
  //Publish with a CAS, not a bare store. process_check() -> eval_and_expand() reaches
  //here WITHOUT holding the expansion gate (see the note above), so two threads can both
  //pass the num_children guard and both allocate; a plain store let the loser's array
  //leak. Now exactly one array is ever published and the loser frees its own. The child
  //MCTSNodes themselves are shared via the tree map, so they must NOT be freed here.
  Edge * expected = nullptr;
  if (!parent->children.compare_exchange_strong(expected, children,
        std::memory_order_release, std::memory_order_relaxed)) {
    total_children.fetch_sub(num_moves, std::memory_order_relaxed);
    delete[] children;
    return;
  }
  parent->num_children.store(num_moves, std::memory_order_release);
}

//no locking, call only when search threads finished
//called from select_best_moves(), which in turn is called from runMCTS()
int most_visited_child(const MCTSNode * parent) {
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
    double Q = W / N;
    double U = exploration_max * prior * sqrt(parent_N) / (1 + N);
    std::string pv(uci_move);
    std::string pv2(uci_move);
    pv2 += " (" + std::to_string(N) + ", " + std::to_string(llround(W)) + ", " + std::to_string(cp) + ", " + std::to_string(Q) + " + " + std::to_string(U) + " = " + std::to_string(Q + U) + ")";
    // Build PV by following most visited children
    num_children = child->num_children.load(std::memory_order_relaxed);
    Edge * children2 = child->children.load(std::memory_order_relaxed);
    int depth = 0;
    while (num_children > 0 && pv.size() < maxLen) {
      int idx = most_visited_child(child); 
      if (idx < 0) break;
      depth++;
      parent_N = static_cast<double>(child->N.load(std::memory_order_relaxed));
      prior = children2[idx].P.load(std::memory_order_relaxed);
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
      U = std::max(exploration_min, exploration_max - (depth * exploration_depth_decay)) * prior * sqrt(parent_N) / (1 + N);
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

//called from mcts_search() in selection phase
//returns child index with the best PUCT value  
int select_best_child(MCTSNode * parent, const int depth) {
  int num_children = parent->num_children.load(std::memory_order_acquire);
  Edge * children = parent->children.load(std::memory_order_acquire);
  double best_score = -INFINITY;
  int selected; //it will be initialized in the for loop
  // Dynamic Exploration Constant - linear decay with depth
  // setting exploration_depth_decay to 0 will make exploration constant static = exploration_max
  // Decay: Start at exploration_constant, then for example drop by 0.05 - 0.1 per ply, floor at 0.25 - all tunable
  double C = std::max(exploration_min, exploration_max - (depth * exploration_depth_decay));
  //or square root decay with depth
  //double C = std::max(exploration_min, exploration_max - (sqrt(static_cast<double>(depth)) * exploration_depth_decay));
  
  // OPTIONAL: Bonus for Root Node (Depth 0) to ensure wide scanning
  //if (depth == 0) C = 2.0;
  const uint64_t parentN = parent->N.load(std::memory_order_acquire);
  for (int i = 0; i < num_children; i++) {
    double P = children[i].P.load(std::memory_order_relaxed);
    MCTSNode * child = children[i].child.load(std::memory_order_relaxed);
    uint64_t N = child->N.load(std::memory_order_relaxed);
    double W = -child->W.load(std::memory_order_relaxed); //parent perspective
    double Q = N ? W / N : 0.0;
    //PUCT formula
    double score = Q + C * P * sqrt(static_cast<double>(parentN)) / (1.0 + N);
    if (score > best_score) {
      best_score = score;
      selected = i;
    }
  }
  // Apply virtual loss to selected child to avoid contention among threads for the same node
  //children = parent.children.load(std::memory_order_acquire);
  MCTSNode * child = children[selected].child.load(std::memory_order_acquire);
  child->N.fetch_add(1, std::memory_order_release);
  child->W.fetch_sub(virtual_loss, std::memory_order_release);
  return selected;
}

void get_prob(std::vector<std::tuple<double, int, int, int, uint64_t>>& move_evals) { //, double prob_mass) {
    //size_t n = move_evals.size();
    //if (n == 0) return 0;
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
        double uniform = 1.0 / move_evals.size();
        for (auto& ev : move_evals) std::get<0>(ev) = uniform;
        return;
    }
    // Loop 3: Normalize to probs, accumulate cum_mass
    //double cum_mass = 0.0;
    //int effective = 0;
    for (auto& ev : move_evals) {
        std::get<0>(ev) = std::exp((std::get<0>(ev) - max_val)/temperature) / total;
        //cum_mass += std::get<0>(ev);
        //++effective;
        //if (cum_mass >= prob_mass) break;
    }
    //return effective;
}

//called from make_move() and run_MCTS()
//calls compute_move_evals(), make_child() and expand_node()
//returns the result in pawns from the temp_board.sideToMove perspective
double process_check(Board& temp_board, const ZobristHash& board_hash, NNUEContext& ctx, const std::unordered_set<uint64_t>& pos_history, int iter) {
  MCTSNode * node = make_child(board_hash.hash, NO_MATE_SCORE, -1);
  int stored_cp = node->cp.load(std::memory_order_relaxed);
  if (stored_cp == NO_MATE_SCORE) { //make_child() returned new node without a parent, let's update its cp and expand it
    /*if (iter >= 1) {
      char fen[MAX_FEN_STRING_LEN];
      printf("process_check() debug: iter %d, fen %s\n", iter, board2fen(temp_board, fen));
    }*/
    return eval_and_expand(node, temp_board, board_hash, ctx, pos_history, iter + 1); 
  } else return stored_cp * 0.01;
}

//called from make_move()
//calls isCheckMateStaleMate(), which checks for terminal state if any
//also calls evaluate_nnue() and process_check() if there is check
//returns a pair of position evaluation in pawns from chess_board.sideToMove perspective and terminal enum: -1 (check), 1 (mate), 2 (stalemate), 3 (repetition), 0 (non-terminal)
std::pair<double, int> position_eval(Board& chess_board, const ZobristHash& board_hash, NNUEContext& ctx, const std::unordered_set<uint64_t>& pos_history, int iter) {
  double res;
  int terminal;
  //char fen[MAX_FEN_STRING_LEN];
  //printf("position_eval() debug: iter %d, fen %s\n", iter, board2fen(chess_board, fen));
	isCheckMateStaleMate(chess_board);
  if (chess_board.isMate) {
    res = -MATE_SCORE * 0.01 + iter; //chess_board.sideToMove loses
    return std::make_pair(res, 1);
  }
  else if (chess_board.isStaleMate) {
    res = 0.0;
    return std::make_pair(res, 2);
  } else if (chess_board.isCheck) {
    terminal = -1;
  } else terminal = 0;
	const int pieceCount = bitCount(chess_board.side[ColorWhite] | chess_board.side[ColorBlack]);
	if (pieceCount > TB_LARGEST || chess_board.castlingRights) {
    //if (!terminal)
    //  res = evaluate_nnue(chess_board, ctx); //evaluate_nnue() returns result in pawns (not centipawns!)
	  //else if (terminal == -1) res = process_check(chess_board, board_hash, ctx, pos_history, iter);
	  if (terminal == -1) res = process_check(chess_board, board_hash, ctx, pos_history, iter);
    else res = evaluate_nnue(chess_board, ctx); //evaluate_nnue() returns result in pawns (not centipawns!)
  } else { //pieceCount <= TB_LARGEST, etc
    unsigned int ep = legalEnPassantMove(chess_board);
    const unsigned int wdl = tb_probe_wdl(chess_board.side[ColorWhite], chess_board.side[ColorBlack], chess_board.pieceTypes[King - 1], chess_board.pieceTypes[Queen - 1], chess_board.pieceTypes[Rook - 1], chess_board.pieceTypes[Bishop - 1], chess_board.pieceTypes[Knight - 1], chess_board.pieceTypes[Pawn - 1], 0, 0, ep == SquareNone ? 0 : ep, chess_board.sideToMove == ColorWhite ? 1 : 0);
    if (wdl == TB_RESULT_FAILED) {
      char fen[MAX_FEN_STRING_LEN];
      log_file("error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, occupations %u, ep %u, halfmoveClock %u, whiteToMove %u, whites %llu, blacks %llu, kings %llu, queens %llu, rooks %llu, bishops %llu, knights %llu, pawns %llu, fen %s, err %s\n", TB_LARGEST, pieceCount, ep, chess_board.halfmoveClock, chess_board.sideToMove == ColorWhite ? 1 : 0, chess_board.side[ColorWhite], chess_board.side[ColorBlack], chess_board.pieceTypes[King - 1], chess_board.pieceTypes[Queen - 1], chess_board.pieceTypes[Rook - 1], chess_board.pieceTypes[Bishop - 1], chess_board.pieceTypes[Knight - 1], chess_board.pieceTypes[Pawn - 1], board2fen(chess_board, fen), strerror(errno));
      //if (!terminal)
      //  res = evaluate_nnue(chess_board, ctx); //evaluate_nnue() returns result in pawns (not centipawns!)
  	  //else if (terminal == -1) res = process_check(chess_board, board_hash, ctx, pos_history, iter);
  	  if (terminal == -1) res = process_check(chess_board, board_hash, ctx, pos_history, iter);
      else res = evaluate_nnue(chess_board, ctx); //evaluate_nnue() returns result in pawns (not centipawns!)
    } else { //tb_probe_wdl() succeeded
      //0 - loss, 4 - win, 1..3 - draw
      if (wdl == 4) res = MATE_SCORE * 0.001;
      else if (wdl == 0) res = -MATE_SCORE * 0.001;
      else res = 0.0;
      tbhits.fetch_add(1, std::memory_order_relaxed);
    }
  } //end of else (pieceCount <= TB_LARGEST)
  return std::make_pair(res, terminal);
}

//called from compute_move_evals()
//makes a move and calls position_eval(), undo the move
//checks for repetition
//returns a pair of eval result in pawns from the perspective of chess_board.sideToMove and terminal state enum
std::pair<double, int> make_move(Board& chess_board, const ZobristHash& board_hash, Move& move, NNUEContext& ctx, uint64_t& child_hash, const std::unordered_set<uint64_t>& pos_history, int iter) {
  ZobristHash tmp_hash = board_hash;
  StateInfo state = {}; //keep track of num_moves and isCheck but not isMate or isStaleMate
  //Stockfish::DirtyPiece dp;
  auto [dp, dts] = accumulator_stack_push(ctx); //for incremental NNUE evaluation, which is one order faster than full evaluation; both dp and dts are references!
  updateHash(tmp_hash, chess_board, move, do_move_dp(chess_board, move, state, dp, dts), z); //do the move, update the hash
  child_hash = tmp_hash.hash;
  if (pos_history.count(child_hash) > 0) { //what if this position is terminal?? I suppose repetition cannot be terminal
    //int terminal = 3;
    //so in case of repetition, the leaf node may be in check because we do not expand it
    //if (getCheckers(chess_board, kingSquare(chess_board, static_cast<Color>(chess_board.sideToMove)))) terminal = -1;
    undo_move(chess_board, move, state);
    accumulator_stack_pop(ctx); //this early return used to leak one stack level per repetition
    return std::make_pair(0.0, 3); //repetition
  }
  auto [res, terminal] = position_eval(chess_board, tmp_hash, ctx, pos_history, iter);
  //if position_eval() returns terminal state setting chess_board.isMate or .isStaleMate, then undo_move() will not restore these fields until kingMoves() is called
  undo_move(chess_board, move, state);
  accumulator_stack_pop(ctx);
  return std::make_pair(-res, terminal);
}

//generate moves and calls make_move() for each move, which calls position_eval(), which calls isCheckMateStaleMate() and if check, calls process_check(), which calls this function again forming recursion for checks; otherwise, position_eval() calls evaluate_nnue(), which cannot be called in check
//computes and sorts move evaluations and calls expand_node()
//called from mcts_search(), set_root() and process_check()
//returns eval result in pawns from chess_board.sideToMove perspective
double eval_and_expand(MCTSNode * node, Board& chess_board, const ZobristHash& board_hash, NNUEContext& ctx, const std::unordered_set<uint64_t>& pos_history, int iter) {
	std::vector<std::tuple<double, int, int, int, uint64_t>> move_evals; //res, move_idx, cp, terminal, hash (res is converted to probabilities in get_prob(); hence, we need to preserve it in cp)      
  Move move;
  auto [king_moves, pinned, pinning, checkers, kingSquare] = kingMoves(chess_board);
  move.src = kingSquare;
  move.promoType = PieceTypeNone;
  while (king_moves) {
    move.dst = popLSB(king_moves);
    uint64_t child_hash = 0;
    auto [res, terminal] = make_move(chess_board, board_hash, move, ctx, child_hash, pos_history, iter);
    move_evals.push_back({res, (move.promoType << 12) | (move.src << 6) | move.dst, static_cast<int>(-res * 100), terminal, child_hash});
  }
  if (bitCount(checkers) <= 1) {
    auto [check_mask, ep_mask] = checkers ? checkMask(chess_board, kingSquare, checkers) : std::make_pair(0xffffffffffffffffULL, 0ULL);
    //printf("compute_move_evals() debug: checkers %llx, check_mask %llx\n", checkers, check_mask);
    for (PieceType pt = Queen; pt >= Pawn; --pt) {
    	uint64_t occupations = chess_board.side[chess_board.sideToMove] & chess_board.pieceTypes[pt - 1]; 
    	while (occupations) {
    	  move.src = popLSB(occupations);
	      uint64_t moves = piece_moves(chess_board, pt, move.src, kingSquare, pinned, pinning, check_mask, ep_mask);
        //printf("compute_move_evals() debug: pt %s, moves %llx\n", pieceType[pt], moves);
    	  while (moves) {
    	    move.dst = popLSB(moves);
        	PieceType startPiece = PieceTypeNone, endPiece = PieceTypeNone;
        	if (promoMove(chess_board, move)) {
        	  startPiece = Knight;
        	  endPiece = Queen;
        	}
      	  for (move.promoType = startPiece; move.promoType <= endPiece; ++move.promoType) { //loop over promotions if any
      	    uint64_t child_hash = 0;
      	    auto [res, terminal] = make_move(chess_board, board_hash, move, ctx, child_hash, pos_history, iter);
            move_evals.push_back({res, (move.promoType << 12) | (move.src << 6) | move.dst, static_cast<int>(-res * 100), terminal, child_hash});
      	  }
        }
      }
    }
  }
  if (chess_board.num_moves == 0) {
    if (checkers) {
      node->cp.store(-MATE_SCORE, std::memory_order_relaxed);
      node->terminal.store(1, std::memory_order_relaxed); //mate
      return -MATE_SCORE * 0.01 + iter;
    } else {
      node->cp.store(0, std::memory_order_relaxed);
      node->terminal.store(2, std::memory_order_relaxed); //stalemate
      return 0;      
    }
  }
  // Sort by res descending - still relies on sorting to select the best result in process_check() and mcts_search()
  std::sort(move_evals.begin(), move_evals.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b);});
  get_prob(move_evals);
  //updating node's cp with improve evaluation 
  int scorecp = -std::get<2>(move_evals[0]);
  node->cp.store(scorecp, std::memory_order_relaxed); //look-ahead update
  expand_node(node, move_evals);
  return scorecp * 0.01;
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
//calls compute_move_evals() and expand_node()
void mcts_search(ThreadParams& params, NNUEContext& ctx) {
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
  while (node->num_children.load(std::memory_order_relaxed) > 0) { //traversal stops at a leaf or at repetition (mate or stalemate node should not have children)
    //return child node index with the best score using PUCT (Predictor + Upper Confidence Bound)
    //it also adds virtual loss to the node to reduce contention for the same node in multi-threaded engine
    int idx = select_best_child(node, params.seldepth);
    Edge * children = node->children.load(std::memory_order_acquire);
    int move_idx = children[idx].move.load(std::memory_order_relaxed);
    path.push_back(node);  // Add parent node to path
    //continue iterating down the tree by getting next node until no more children
    node = children[idx].child.load(std::memory_order_acquire);
    //init and take edge's move that leads to the child node
    terminal = node->terminal.load(std::memory_order_acquire);
    if (terminal == 3) break; //repetition
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
      terminal = 3;
      node->terminal.store(3, std::memory_order_release);
      node->cp.store(0, std::memory_order_release);
      break;
    }
  } //end of while(node.num_children > 0) loop
  path.push_back(node);  // Add leaf node to path - sim_board corresponds to this node!
  //Here we are at the bottom of the tree, i.e. at a leaf or at the terminal node (mate, stalemate)
  // Evaluation - the node is already evaluated during previous expansion!
  // We could actually improve the eval by using move_evals calculated later in the code for the children nodes before expansion for evaluating its parent (this node), kind of look ahead eval
  int scorecp = 0;
  double result = 0.0;
  if (terminal <= 0) {
    if (node->expanding.exchange(1, std::memory_order_acquire) == 0) { //the leaf node is gated only for expansion
                                //nodes traversed in the selection phase are not leaf nodes, i.e. nodes without children
                                //if the gate is already taken, another thread is expanding this node right now
                                //(unlike try_lock(), an atomic exchange cannot fail spuriously or be blocked by readers)
      if (terminal == -1) { //node in check
        log_file("mcts_search() warning: leaf node in check!\n"); //leaf node should not be in check because of process_check()
      } else {
        evaluate_nnue(sim_board, ctx); //the leaf node is evaluated but we call it to make subsequent evals faster
      }
      result = eval_and_expand(node, sim_board, sim_zh, ctx, pos_history, 0);
      result = tanh(result / eval_scale);
      node->expanding.store(0, std::memory_order_release);
    } //end of if (node.expanding.exchange(1, acquire) == 0)
    else { //unable to lock the node, see if it's already evaluated
      scorecp = node->cp.load(std::memory_order_relaxed);
      if (scorecp == NO_MATE_SCORE) { //node has not been evaluated yet, return without a backprop (a bit of a waste)
        printf("mcts_search() warning: unevaluated child - skipping expansion\n");
        for (size_t j = 1; j < path.size(); ++j) {  // From first child to leaf reverse virtual loss
          MCTSNode * nd = path[j];
          nd->N.fetch_sub(1, std::memory_order_relaxed);
          nd->W.fetch_add(virtual_loss, std::memory_order_relaxed);
        }      
        return;
      }
      result = tanh(scorecp * 0.01 / eval_scale);
    }
  } //end of else if (terminal <= 0) - check or non-terminal
  else { //terminal (mate, stalemate, repetition)
    if (terminal == 1) { //mate
      scorecp = node->cp.load(std::memory_order_relaxed);
      result = scorecp == -MATE_SCORE ? -1 : 1;
    } //for stalemate the result is initiated to 0, so no need for this check    
  }
  // Backpropagation: update node visits and results regardless of whether we expand the node or not
  for (auto n = path.rbegin(); n != path.rend(); ++n) {
    node = *n;
    node->N.fetch_add(1, std::memory_order_relaxed);
    node->W.fetch_add(result, std::memory_order_relaxed);
    result = -result;
  }
  // Revert virtual loss for the selected path (skip root, as no loss was applied to it) regardless of expansion
  // because virtual loss was applied in select_best_child() which is called in the selection phase
  for (size_t j = 1; j < path.size(); ++j) {  // From first child to leaf
    MCTSNode * nd = path[j];
    nd->N.fetch_sub(1, std::memory_order_relaxed);
    nd->W.fetch_add(virtual_loss, std::memory_order_relaxed);
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
      log_file("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", d, seldepth.load(std::memory_order_relaxed), i + 1, -child->cp.load(std::memory_order_relaxed), nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, uci_move);
      print("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", d, seldepth.load(std::memory_order_relaxed), i + 1, -child->cp.load(std::memory_order_relaxed), nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, uci_move);
    }
  }
}

void runMCTS(NNUEContext& ctx) {
  double elapsed = 0.0;
  size_t unique_nodes = 0;
  uint64_t nodes = 0;
  int hashfull = 0;
  std::vector<std::pair<int, std::string>> pvs;
  int multiPV = 1;

	isCheckMateStaleMate(board); //it calculates board.num_moves as well as part of all legal moves generation
  if (board.num_moves > 1) { //run MCTS using multiple threads
    tbhits.store(0, std::memory_order_relaxed);
    //it seems there rarely is some kind of contamination or corruption of the tree
    //so let's try cleanup() instead of gc() if UCI Ponder option is false
    //avoid using Ponder option, sometimes called "permanent brain", i.e. thinking during opponent's time
    if (chessEngine.optionCheck[Ponder].value) {
      set_root(ctx);      
      gc();
    } else {
      cleanup(); 
      set_root(ctx);      
    }
    int num_threads = chessEngine.optionSpin[Threads].value;
    if (pool_threads.size() != num_threads) {
         log_file("Warning: Pool size mismatch: current threads %d != configured %d. Re-initializing...\n", pool_threads.size(), num_threads);
         init_thread_pool(num_threads);
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
        	  //This loop has two exit states, so the chosen promotion must be captured inside the
        	  //body - the pattern already used at line 508 and in test_pos.cpp:154. For a plain move
        	  //startPiece == endPiece == PieceTypeNone (7) and the final increment would leave
        	  //move.promoType == 8, making do_move() write board.pieceTypes[7] past the end of Board;
        	  //for a real promotion the move_idx == 0 guard would exit one piece past the one that
        	  //move_idx was actually built with (Bishop, while the GUI is told "n").
        	  for (PieceType promo = startPiece; promo <= endPiece && move_idx == 0; promo = (PieceType)(promo + 1)) { //loop over promotions if any
          	  move.promoType = promo;
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
      double res;
      if (board.isMate) res = MATE_SCORE * 0.01;
      else if (board.isStaleMate) res = 0.0;
      else if (board.isCheck) {
        std::unordered_set<uint64_t>pos_history;
        ZobristHash tmp_zh = zh;
        res = -process_check(board, tmp_zh, ctx, pos_history, 0);
      } else {
        res = -evaluate_nnue(board, ctx);
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
