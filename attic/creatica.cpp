//For MacOS using clang
//c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -I /Users/ap/libchess  -L /Users/ap/libchess -Wl,-lchess,-lcurl,-rpath,/Users/ap/libchess creatica.cpp uci.cpp tbcore.c tbprobe.c -o creatica

//for samply
//c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -g -fno-omit-frame-pointer -I /Users/ap/libchess  -L /Users/ap/libchess -Wl,-lchess,-lcurl,-rpath,/Users/ap/libchess creatica.cpp uci.cpp tbcore.c tbprobe.c -o creatica

// -O3 -g -fno-omit-frame-pointer - options for profiling

#include "creatica.hpp"

std::mutex mtx, log_mtx, print_mtx, pool_mutex, search_done_mtx;//, nnue_cache_mutex;
//std::shared_mutex nnue_mutex;
std::condition_variable cv, pool_cv, pool_done_cv, cv_search_done;
std::atomic<bool> searchFlag {false};
std::atomic<bool> stopFlag {false};
std::atomic<bool> quitFlag {false};
std::atomic<bool> ponderHit {false};
std::atomic<bool> pool_quit{false};    // True when engine exits
std::atomic<int> pool_generation{0};   // Increments every new search
std::atomic<int> active_workers{0};    // Count of currently working threads
std::atomic<bool> search_done{false}; // Signals search completion
std::atomic<uint64_t> tbhits{0};
std::atomic<int> generation{0};
std::atomic<int> hash_full{0};
std::atomic<int> depth{0};
std::atomic<int> seldepth{0};
std::atomic<uint32_t> root_N[128] = {0};
std::atomic<double> root_W[128] = {0};

FILE * logfile = nullptr;
char best_move[6] = "";
bool tb_init_done = false;
double timeAllocated = 0.0; //ms
double exploration_min;
double exploration_max;
double exploration_depth_decay;
double eval_scale;
double temperature;
const int qs_max_depth = 3;

std::string last_move;
std::unordered_set<uint64_t> position_history;
Board board = {};
ZobristHash zh = {};
Zobrist z = {};
Engine chessEngine = {};
std::vector<std::thread> pool_threads;
std::vector<ThreadParams *> pool_params;

void cleanup(const int thread_id) {
  auto& params = *pool_params[thread_id];
  params.nodes.clear();
  params.total_children.store(0, std::memory_order_relaxed);
  params.next_idx = 0;
}

uint32_t allocate_node(ThreadParams& params) {
    uint32_t idx = params.next_idx.fetch_add(1, std::memory_order_relaxed);
    if (idx >= params.nodes.size()) {
        // Grow if needed (but pre-reserve to avoid)
        params.nodes.resize(idx + 1000000);  // batch grow
    }
    return idx;
}

void set_root(const int thread_id) {
  auto& params = *pool_params[thread_id];
  const uint32_t child_idx = allocate_node(params);
  auto& child = params.nodes[child_idx];
  child.hash = zh.hash;
  child.cp = NO_MATE_SCORE;
  child.N = 0;
  child.W = 0;
}
  
//called from eval_and_expand()
void expand_node(uint32_t parent_idx, const std::vector<std::tuple<double, int, int, uint64_t>>& top_moves, const std::unordered_set<uint64_t>& pos_history, const int thread_id) {
  auto& params = *pool_params[thread_id];
  int num_moves = top_moves.size();
  assert(num_moves > 0);
  params.nodes[parent_idx].children.reserve(num_moves);
  for (int i = 0; i < num_moves; ++i) {
      auto [prior, move_idx, cp, hash] = top_moves[i]; 
      const uint32_t child_idx = allocate_node(params);
      auto& child = params.nodes[child_idx];
      child.hash = hash;
      child.cp = cp;
      //we should probably update N and W as well. We're updating cp, the node is evaluated, meaning it's been visited
      if (cp != NO_MATE_SCORE) {
        child.N = 1;
        child.W = tanh(cp * 0.01 / eval_scale);
      } else {
        child.N = 0;
        child.W = 0;
      }
      params.nodes[parent_idx].children.push_back({move_idx, prior, child_idx});
  }
  params.total_children.fetch_add(num_moves, std::memory_order_relaxed); //update total_children counter
}

//called from select_best_moves(), which in turn is called from runMCTS()
int most_visited_child(const uint32_t parent) {
  uint32_t N = 0;
  int idx = -1;
  std::vector<std::pair<double, int>> priors; //prior, child index
  uint32_t max_N = 0;
  int best_thread = 0;
  for (auto params : pool_params) {
    if (params->nodes[parent].N > max_N) {
      max_N = params->nodes[parent].N;
      best_thread = params->thread_id;
    }
  }
  auto& params = *pool_params[best_thread];
  auto num_children = params.nodes[parent].children.size();
  for (int i = 0; i < num_children; i++) { //this loop will be skipped for the last node
    priors.push_back({params.nodes[parent].children[i].P, i});
    auto idx = params.nodes[parent].children[i].child_idx;
    auto& child = params.nodes[idx];
    if (child.N > N) {
      N = child.N;
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

//called from runMCTS()
int select_best_moves(std::vector<std::pair<int, std::string>>& pvs) {
  //find the best thread by accumulating the visits (finding best move) and finding the thread with the most visits for the best move
  std::vector<std::tuple<uint32_t, int>> visits; //N, child_idx
  for (int i = 0; i < board.num_moves; ++i) {
    uint32_t N = root_N[i].load(std::memory_order_relaxed);
    visits.push_back({N, i});
  }
  std::sort(visits.begin(), visits.end(), std::greater()); //sort children desc by the number of visits N    

  char uci_move[6];
  while (visits.size() > 1) {
    auto idx = std::get<1>(visits[0]); //index of the most visited child
    auto next_idx = std::get<1>(visits[1]); //index of the next most visited child
    //here we'd better use the thread with the highest N for a particular move (child_idx) instead of thread 0
    //this thread should have the most accurate W
    uint32_t N = 0;
    int thread_id = 0;
    for (int i = 0; i < chessEngine.optionSpin[Threads].value; i++) {
      auto& params = *pool_params[i];
      auto& root = params.nodes[0];
      auto& child = params.nodes[root.children[idx].child_idx];
      if (child.N > N) {
        N = child.N;
        thread_id = i;
      }
    }
    auto& params = *pool_params[thread_id];
    auto& root = params.nodes[0];
    auto& child = params.nodes[root.children[idx].child_idx];
    auto& next_child = params.nodes[root.children[next_idx].child_idx];
    //NNUE static eval is not reliable for deciding whether the position is winning
    //Let's try to use W instead. If it is positive, the position is winning 
    if (-child.W > 0 && -next_child.W > 0) { //check for repetition in winning position
      int global_count = position_history.count(child.hash);
      if (global_count) {
          int move = root.children[idx].move;
          int promo = (move >> 12) & 7;
          char fen[MAX_FEN_STRING_LEN];
          log_file("select_best_moves() debug: skipping move %s%s%c (would cause repetition in winning position %s, W %f, nextW %f, cp %d, nextCP %d)\n", square[(move >> 6) & 63], square[move & 63], promo != PieceTypeNone ? uciPromoLetter[promo] : ' ', board2fen(board, fen), -child.W, -next_child.W, -child.cp, -next_child.cp);
          visits.erase(visits.begin());
          continue;
      } else break;
    } else break;
  } // end of while (visits.size() > 1)
  int num_moves = visits.size();
  int multiPV = std::min<int>(num_moves, (int)chessEngine.optionSpin[MultiPV].value);
  int pvLength = chessEngine.optionSpin[PVPlies].value * sizeof(uci_move);
  int maxLen = pvLength - sizeof(uci_move);
  for (int i = 0; i < multiPV; i++) {
    int index = std::get<1>(visits[i]);
    //here we'd better use the thread with the highest N for a particular move instead of using thread 0,
    //and then just follow this thread moves
    uint32_t N = 0;
    int thread_id = 0;
    for (int i = 0; i < chessEngine.optionSpin[Threads].value; i++) {
      auto& params = *pool_params[i];
      auto& root = params.nodes[0];
      auto& child = params.nodes[root.children[index].child_idx];
      if (child.N > N) {
        N = child.N;
        thread_id = i;
      }
    }
    auto& params = *pool_params[thread_id];
    auto& root = params.nodes[0];
    idx2uci(root.children[index].move, uci_move);
    auto& child = params.nodes[root.children[index].child_idx];
    double parent_N = static_cast<double>(root.N);
    double Q = -child.W / child.N;
    double U = exploration_max * root.children[index].P * sqrt(parent_N) / (1 + child.N);
    std::string pv(uci_move);
    std::string pv2(uci_move);
    pv2 += " (" + std::to_string(child.N) + ", " + std::to_string(llround(-child.W)) + ", " + std::to_string(-child.cp) + ", " + std::to_string(Q) + " + " + std::to_string(U) + " = " + std::to_string(Q + U) + ")";
    // Build PV by following most visited children
    size_t num_children = child.children.size();
    int depth = 0;
    while (num_children > 0 && pv.size() < maxLen) {
      int idx = most_visited_child(root.children[index].child_idx); 
      if (idx < 0) break;
      depth++;
      parent_N = static_cast<double>(child.N);
      double prior = child.children[idx].P;
      idx2uci(child.children[idx].move, uci_move);
      pv += ' ';
      pv.append(uci_move);  
      pv2 += ' ';
      pv2.append(uci_move);   
      child = params.nodes[child.children[idx].child_idx];
      Q = -child.W / child.N;
      U = std::max(exploration_min, exploration_max - (depth * exploration_depth_decay)) * prior * sqrt(parent_N) / (1 + child.N);
      pv2 += " (" + std::to_string(child.N) + ", " + std::to_string(llround(-child.W)) + ", " + std::to_string(-child.cp) + ", " + std::to_string(Q) + " + " + std::to_string(U) + " = " + std::to_string(Q + U) + ")";
      num_children = child.children.size();
    } //end of while (num_children > 0 && pv.size() < maxLen)
    pvs.push_back({-child.cp, pv});
    log_file("select_best_moves() debug: PV[%d] %s\n", i, pv2.c_str());
  } // end for (int i = 0; i < multiPV; i++)
  return multiPV;
}

//called from mcts_search() in selection phase
//returns child index with the best PUCT value  
int select_best_child(MCTSNode * parent, const int depth, const int thread_id) {
  double best_score = -INFINITY;
  int selected; //it will be initialized in the for loop
  // Dynamic Exploration Constant - linear decay with depth
  // setting exploration_depth_decay to 0 will make exploration constant static = exploration_max
  // Decay: Start at exploration_constant, then for example drop by 0.05 - 0.1 per ply, floor at 0.25 - all tunable
  double C = std::max(exploration_min, exploration_max - (depth * exploration_depth_decay));
  //or square root decay with depth
  //double C = std::max(exploration_min, exploration_max - (sqrt(static_cast<double>(depth)) * exploration_depth_decay));
    
  auto& params = *pool_params[thread_id];
  int num_children = parent->children.size();
  for (int i = 0; i < num_children; i++) {
    double P = parent->children[i].P;
    auto& child = params.nodes[parent->children[i].child_idx];
    double Q;
    if (parent->hash == params.nodes[0].hash) Q = root_N[i] ? root_W[i] / root_N[i] : 0.0;
    else Q = child.N ? -child.W / child.N : 0.0;
    //PUCT formula
    double score = Q + C * P * sqrt(static_cast<double>(parent->N)) / (1.0 + child.N);
    if (score > best_score) {
      best_score = score;
      selected = i;
    }
  }
  return selected;
}

void get_prob(std::vector<std::tuple<double, int, int, uint64_t>>& move_evals) {
    double max_val = -std::numeric_limits<double>::infinity();
    for (const auto& ev : move_evals) {
        if (std::get<0>(ev) > max_val) max_val = std::get<0>(ev);
    }
    double total = 0.0;
    for (const auto& ev : move_evals) {
        total += std::exp((std::get<0>(ev) - max_val)/temperature);
    }
    if (total == 0.0) {  // Rare case: all -inf or underflow
        double uniform = 1.0 / move_evals.size();
        for (auto& ev : move_evals) std::get<0>(ev) = uniform;
        return;
    }
    for (auto& ev : move_evals) {
        std::get<0>(ev) = std::exp((std::get<0>(ev) - max_val)/temperature) / total;
    }
}

// Quiescence search function (recursive, capture-only, minimax)
// called from position_eval(), which is called from make_move(), which is called from eval_and_expand(), which is called from mcts_search()
// calls evaluate_nnue()
// Returns eval in pawns from sideToMove perspective board
// Note: move generation in kingMoves() and piece_moves() are legal, not pseudo-legal!
double quiescence(Board& chess_board, const ZobristHash& board_hash, NNUEContext& ctx, int depth, std::unordered_set<uint64_t>& pos_history) {
  double best = -INFINITY;
  Move move;
	auto [moves, pinned, pinning, checkers, kingSquare] = kingMoves(chess_board);
  move.src = kingSquare;
  move.promoType = PieceTypeNone;
  if (!checkers) {
    // Stand-pat: assume opponent does nothing; if we can beat this, great
    best = evaluate_nnue(chess_board, ctx); 
    if (depth <= 0) return best;
  	uint64_t opponent = chess_board.side[chess_board.sideToMove ^ 1]; 
  	moves &= opponent; //only capture moves
  	//king moves
    while (moves) {
      move.dst = lsBit(moves);
      ZobristHash tmp_hash = board_hash;
      StateInfo state = {};
      Stockfish::DirtyPiece dp;
      updateHash(tmp_hash, chess_board, move, do_move_dp(chess_board, move, state, dp), z); //do the move, update the hash
      if (pos_history.count(tmp_hash.hash) > 0) {//do not recurse on repetition
        log_file("quiescence() debug: repetition detected for king moves, best %f\n", best);
        if (best < 0.0) best = 0.0;
      } else {
        pos_history.insert(tmp_hash.hash);
        accumulator_stack_push(ctx, dp); //for incremental NNUE evaluation, which is one order faster than full evaluation
        double score = -quiescence(chess_board, tmp_hash, ctx, depth - 1, pos_history);
        if (score > best) best = score;
        accumulator_stack_pop(ctx);
        pos_history.erase(tmp_hash.hash);
      }
      undo_move(chess_board, move, state);
      moves &= moves - 1;
    }  
    //other piece moves
    for (PieceType pt = Queen; pt >= Pawn; pt = (PieceType)(pt - 1)) {
    	uint64_t occupations = chess_board.side[chess_board.sideToMove] & chess_board.pieceTypes[pt - 1]; 
    	while (occupations) {
    	  move.src = lsBit(occupations);
        moves = piece_moves(chess_board, pt, move.src, kingSquare, pinned, pinning, checkers);
        moves &= opponent; //captures and checks only excluding en passant
        //plus legal en passant capture if any making sure that the capturing pawn is not pinned as legalEnPassantMoveFromSq() does not check for pins
  			if (pt == Pawn && !(pinned & SQ_BIT(move.src))) {  
    			const Square ep = legalEnPassantMoveFromSq(chess_board, move.src);
    			if (ep != SquareNone) moves |= SQ_BIT(ep);
        } 
    	  while (moves) {
    	    move.dst = lsBit(moves);
        	PieceType startPiece = PieceTypeNone, endPiece = PieceTypeNone;
        	if (promoMove(chess_board, move)) {
        	  startPiece = Knight;
        	  endPiece = Queen;
        	}
      	  for (move.promoType = startPiece; move.promoType <= endPiece; move.promoType = (PieceType)(move.promoType + 1)) { //loop over promotions if any
            ZobristHash tmp_hash = board_hash;
            StateInfo state = {};
            Stockfish::DirtyPiece dp;
            updateHash(tmp_hash, chess_board, move, do_move_dp(chess_board, move, state, dp), z); //do the move, update the hash
            if (pos_history.count(tmp_hash.hash) > 0) { //do not recurse on repetition
             log_file("quiescence() debug: repetition detected for other moves, best %f\n", best);
             if (best < 0.0) best = 0.0;
            } else {
              pos_history.insert(tmp_hash.hash);
              accumulator_stack_push(ctx, dp); //for incremental NNUE evaluation, which is one order faster than full evaluation
              double score = -quiescence(chess_board, tmp_hash, ctx, depth - 1, pos_history);
              if (score > best) best = score;
              accumulator_stack_pop(ctx);
              pos_history.erase(tmp_hash.hash);
            }
            undo_move(chess_board, move, state);
      	  }
          moves &= moves - 1;
        } //end of while (moves)
        occupations &= occupations - 1;
      } //end of while (occupations)
    } //end of for (other than king pieces)
    if (!chess_board.num_moves) {
      chess_board.isStaleMate = true;
      return 0.0; //stalemate
    }
  } else { //check or mate
    //king check evasion moves
    while (moves) {
      move.dst = lsBit(moves);
      ZobristHash tmp_hash = board_hash;
      StateInfo state = {};
      Stockfish::DirtyPiece dp;
      updateHash(tmp_hash, chess_board, move, do_move_dp(chess_board, move, state, dp), z); //do the move, update the hash
      if (pos_history.count(tmp_hash.hash) > 0) {//do not recurse on repetition
        //log_file("quiescence() debug: repetition detected in check for king moves, best %f\n", best);
        if (best < 0.0) best = 0.0;
      } else {
        pos_history.insert(tmp_hash.hash);
        accumulator_stack_push(ctx, dp); //for incremental NNUE evaluation, which is one order faster than full evaluation
        double score = -quiescence(chess_board, tmp_hash, ctx, depth - 1, pos_history);
        if (score > best) best = score;
        accumulator_stack_pop(ctx);
        pos_history.erase(tmp_hash.hash);
      }
      undo_move(chess_board, move, state);
      moves &= moves - 1;
    }  
    if (bitCount(checkers) == 1) {
      uint64_t check_mask = checkers ? checkMask(chess_board, kingSquare, checkers) : 0xffffffffffffffffULL;
      //other pieces moves (checker captures and blocking)
      for (PieceType pt = Queen; pt >= Pawn; pt = (PieceType)(pt - 1)) {
      	uint64_t occupations = chess_board.side[chess_board.sideToMove] & chess_board.pieceTypes[pt - 1]; 
      	while (occupations) {
      	  move.src = lsBit(occupations);
          moves = piece_moves(chess_board, pt, move.src, kingSquare, pinned, pinning, check_mask);
      	  while (moves) {
      	    move.dst = lsBit(moves);
          	PieceType startPiece = PieceTypeNone, endPiece = PieceTypeNone;
          	if (promoMove(chess_board, move)) {
          	  startPiece = Knight;
          	  endPiece = Queen;
          	}
        	  for (move.promoType = startPiece; move.promoType <= endPiece; move.promoType = (PieceType)(move.promoType + 1)) { //loop over promotions if any
              ZobristHash tmp_hash = board_hash;
              StateInfo state = {};
              Stockfish::DirtyPiece dp;
              updateHash(tmp_hash, chess_board, move, do_move_dp(chess_board, move, state, dp), z); //do the move, update the hash
              if (pos_history.count(tmp_hash.hash) > 0) { //do not recurse on repetition
                log_file("quiescence() debug: repetition detected in check for other moves, best %f\n", best);
                if (best < 0.0) best = 0.0;
              } else {
                pos_history.insert(tmp_hash.hash);
                accumulator_stack_push(ctx, dp); //for incremental NNUE evaluation, which is one order faster than full evaluation
                double score = -quiescence(chess_board, tmp_hash, ctx, depth - 1, pos_history);
                if (score > best) best = score;
                accumulator_stack_pop(ctx);
                pos_history.erase(tmp_hash.hash);
              }
              undo_move(chess_board, move, state);
        	  }
            moves &= moves - 1;
          } //end of while (moves)
          occupations &= occupations - 1;
        } //end of while (occupations)
      } //end of for (pt = ...)
    } //end of if (num_checkers == 1)
    if (!chess_board.num_moves) {
      chess_board.isMate = true; //should be careful with assessing this boolean as because of recursion it may be mate for the opponent, not the side to move
      chess_board.isCheck = false;
      return -MATE_SCORE * 0.01; //mate - the score should help to decide who is mated
    }
  } //end of else (num_checkers > 0)
  return best;
}

// Helper: Quick check for "good" captures (SEE >0, optional for perf)
/*bool has_good_captures(const Board& chess_board) {
    // Simple: Scan for captures where attacker value < target value (MVV-LVA)
    // Implement if QS triggers too often; else always QS if not quiet.
    return true;  // Start conservative
}*/

//called from make_move() and mcts_search()
//calls quiescence(), which sets terminal state if any, i.e. .isMate or .isStaleMate, it sets .isCheck and .num_moves too
//returns position evaluation in pawns from chess_board.sideToMove perspective
double position_eval(Board& chess_board, const ZobristHash& board_hash, NNUEContext& ctx, std::unordered_set<uint64_t>& pos_history, const int thread_id) {
  double res;
  auto params = pool_params[thread_id];
	const int pieceCount = bitCount(chess_board.side[ColorWhite] | chess_board.side[ColorBlack]);
	//isCheckMateStaleMate(chess_board); //need to call it even for TB because mcts_search() needs isMate && isStaleMate
	if (pieceCount > TB_LARGEST || chess_board.castlingRights) {
    auto it = params->nnue_cache.find(board_hash.hash);
    if (it != params->nnue_cache.end()) return it->second;
    res = quiescence(chess_board, board_hash, ctx, qs_max_depth, pos_history);
    params->nnue_cache[board_hash.hash] = res;
  } else { //pieceCount <= TB_LARGEST, etc
    unsigned int ep = legalEnPassantMove(chess_board);
    const unsigned int wdl = tb_probe_wdl(chess_board.side[ColorWhite], chess_board.side[ColorBlack], chess_board.pieceTypes[King - 1], chess_board.pieceTypes[Queen - 1], chess_board.pieceTypes[Rook - 1], chess_board.pieceTypes[Bishop - 1], chess_board.pieceTypes[Knight - 1], chess_board.pieceTypes[Pawn - 1], 0, 0, ep == SquareNone ? 0 : ep, chess_board.sideToMove == ColorWhite ? 1 : 0);
    if (wdl == TB_RESULT_FAILED) {
      char fen[MAX_FEN_STRING_LEN];
      log_file("error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, occupations %u, ep %u, halfmoveClock %u, whiteToMove %u, whites %llu, blacks %llu, kings %llu, queens %llu, rooks %llu, bishops %llu, knights %llu, pawns %llu, fen %s, err %s\n", TB_LARGEST, pieceCount, ep, chess_board.halfmoveClock, chess_board.sideToMove == ColorWhite ? 1 : 0, chess_board.side[ColorWhite], chess_board.side[ColorBlack], chess_board.pieceTypes[King - 1], chess_board.pieceTypes[Queen - 1], chess_board.pieceTypes[Rook - 1], chess_board.pieceTypes[Bishop - 1], chess_board.pieceTypes[Knight - 1], chess_board.pieceTypes[Pawn - 1], board2fen(chess_board, fen), strerror(errno));
      //fall back to quiescence
      auto it = params->nnue_cache.find(board_hash.hash);
      if (it != params->nnue_cache.end()) return it->second;
      res = quiescence(chess_board, board_hash, ctx, qs_max_depth, pos_history);        
      params->nnue_cache[board_hash.hash] = res;
    } else { //tb_probe_wdl() succeeded
      //0 - loss, 4 - win, 1..3 - draw
      if (wdl == 4) res = MATE_SCORE * 0.001; //reduce the mate score by an order to emphasise that it is not mate yet
      else if (wdl == 0) res = -MATE_SCORE * 0.001;
      else res = 0.0;
      tbhits.fetch_add(1, std::memory_order_relaxed);
    }
  } //end of else (pieceCount <= TB_LARGEST)
  return res;
}

//called from eval_and_expand()
//calls position_eval(), which calls quiescence()
//returns eval result in pawns from the perspective of chess_board.sideToMove
double make_move(Board& chess_board, const ZobristHash& board_hash, Move& move, NNUEContext& ctx, uint64_t& child_hash, std::unordered_set<uint64_t>& pos_history, const int thread_id) {
  ZobristHash tmp_hash = board_hash;
  StateInfo state = {};
  Stockfish::DirtyPiece dp;
  updateHash(tmp_hash, chess_board, move, do_move_dp(chess_board, move, state, dp), z); //do the move, update the hash
  child_hash = tmp_hash.hash;
  if (pos_history.count(child_hash) > 0) {
    undo_move(chess_board, move, state);
    return 0.0; //repetition
  }
  accumulator_stack_push(ctx, dp); //for incremental NNUE evaluation, which is one order faster than full evaluation
  double res = position_eval(chess_board, tmp_hash, ctx, pos_history, thread_id);
  accumulator_stack_pop(ctx);
  undo_move(chess_board, move, state);
  return -res;
}

//called from mcts_search()
//for each move calls make_move(), which calls position_eval(), which calls quiescence()
//sorts move evaluations and calls expand_node()
double eval_and_expand(Board& chess_board, const ZobristHash& board_hash, NNUEContext& ctx, std::unordered_set<uint64_t>& pos_history, const int thread_id, const uint32_t node_idx) {
  std::vector<std::tuple<double, int, int, uint64_t>> move_evals; //res, move_idx, cp, hash (res is converted to probabilities in get_prob(); hence, we need to preserve it in cp)
  double res;
  Move move;
	auto [moves, pinned, pinning, checkers, kingSquare] = kingMoves(chess_board);
  move.src = kingSquare;
  move.promoType = PieceTypeNone;
  while (moves) {
    move.dst = lsBit(moves);
    uint64_t child_hash = 0;
    res = make_move(chess_board, board_hash, move, ctx, child_hash, pos_history, thread_id);
    move_evals.push_back({res, (move.promoType << 12) | (move.src << 6) | move.dst, static_cast<int>(-res * 100), child_hash});
    moves &= moves - 1;
  }
  if (bitCount(checkers) <= 1) {
    uint64_t check_mask = checkers ? checkMask(chess_board, kingSquare, checkers) : 0xffffffffffffffffULL;
    for (PieceType pt = Queen; pt >= Pawn; pt = (PieceType)(pt - 1)) {
    	uint64_t occupations = chess_board.side[chess_board.sideToMove] & chess_board.pieceTypes[pt - 1]; 
    	while (occupations) {
    	  move.src = lsBit(occupations);
        moves = piece_moves(chess_board, pt, move.src, kingSquare, pinned, pinning, check_mask);
    	  while (moves) {
    	    move.dst = lsBit(moves);
        	PieceType startPiece = PieceTypeNone, endPiece = PieceTypeNone;
        	if (promoMove(chess_board, move)) {
        	  startPiece = Knight;
        	  endPiece = Queen;
        	}
      	  for (move.promoType = startPiece; move.promoType <= endPiece; move.promoType = (PieceType)(move.promoType + 1)) { //loop over promotions if any
      	    uint64_t child_hash = 0;
      	    res = make_move(chess_board, board_hash, move, ctx, child_hash, pos_history, thread_id);
            move_evals.push_back({res, (move.promoType << 12) | (move.src << 6) | move.dst, static_cast<int>(-res * 100), child_hash});
      	  }
          moves &= moves - 1;
        } //end of while (moves) loop
        occupations &= occupations - 1;
      } //end of while (occupaitions) loop
    } //end of for (PieceType pt = ...) loop
  } //end of if (num_checkers <= 1)
  int scorecp = 0;
  if (chess_board.num_moves == 0) {
    if (chess_board.isCheck) {
      chess_board.isMate = true;
      chess_board.isCheck = false;
      scorecp = -MATE_SCORE;
      res = -1.0;
    } else {
      chess_board.isStaleMate = true;
      res = 0.0;
    }
  } else {
    // Sort by res descending - relies on sorting to select the best result
    std::sort(move_evals.begin(), move_evals.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b);});
    get_prob(move_evals); //converts res to prior
    expand_node(node_idx, move_evals, pos_history, thread_id);
    scorecp = -std::get<2>(move_evals[0]);
    res = tanh(scorecp * 0.01 / eval_scale);
  }
  //updating node's cp with improve evaluation 
  auto params = pool_params[thread_id];
  params->nodes[node_idx].cp = scorecp; //look-ahead update
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

//called from persistent_worker_func()
//calls compute_move_evals() and expand_node()
void mcts_search(const int thread_id, NNUEContext& ctx) {
  std::vector<uint32_t> path;  // Track the path from root to leaf
  path.reserve(chessEngine.depth);
  std::unordered_set<uint64_t> pos_history;
  bool repetition = false;
  auto params = pool_params[thread_id];
  params->seldepth.store(0, std::memory_order_relaxed);
  uint32_t node_idx = 0;
  MCTSNode * node = &params->nodes[node_idx];
  //start from the same initial position given by board (at the root node, i.e. at the top of the tree - the up side down tree)
  //copy the board and the hash to preserve it for subsequent iterations
  //char fen[MAX_FEN_STRING_LEN];
  //printf("mcts_search() debug: fen %s\n", board2fen(board, fen));
  Board sim_board = board;
  ZobristHash sim_zh = zh;
  // Selection
  //iterate down the tree updating sim_board by initiating and making moves
  //thread-local map to prevent repetition cycles - it should be global I think but thread-safety may be a problem
  int num_children = node->children.size();
  int root_child_idx = -1;
  Move move;
  while (num_children > 0) { //traversal stops at a leaf or at repetition (mate or stalemate node should not have children)
    //return child node index with the best score using PUCT (Predictor + Upper Confidence Bound)
    //it also adds virtual loss to the node to reduce contention for the same node in multi-threaded engine
    int idx = select_best_child(node, params->seldepth.load(std::memory_order_relaxed), params->thread_id);
    if (node_idx == 0) root_child_idx = idx;
    int move_idx = node->children[idx].move;
    path.push_back(node_idx);  // Add parent node to path
    //continue iterating down the tree by getting next node until no more children
    node_idx = node->children[idx].child_idx;
    node = &params->nodes[node_idx];
    num_children = node->children.size();
    //init and take edge's move that leads to the child node
    move.dst = (Square)(move_idx & 63);
    move.src = (Square)((move_idx >> 6) & 63);
    move.promoType = (PieceType)((move_idx >> 12) & 7);
    move.type = MoveTypeNormal;
    //update Zobrist hash (it is needed so that we can call updateHash() later instead of getHash()
		updateHash(sim_zh, sim_board, move, ff_move(sim_board, move), z);
    params->seldepth.fetch_add(1, std::memory_order_relaxed);
    int global_count = position_history.count(sim_zh.hash); //actual positions that have occured in the game
    int path_count = pos_history.count(sim_zh.hash); //simulated positions ahead of the current one
    if (path_count == 0) pos_history.insert(sim_zh.hash);
    repetition = (global_count + path_count >= 1); 
    if (repetition) break;
  } //end of while(num_children > 0) loop
  char fen[MAX_FEN_STRING_LEN];
  //printf("mcts(): node_idx %d, seldepth %d, move %s%s%c, fen %s\n", node_idx, params->seldepth, square[move.src], square[move.dst], uciPromoLetter[move.promoType], board2fen(sim_board, fen));
  path.push_back(node_idx);  // Add leaf node to path - sim_board corresponds to this node!
  //Here we are at the bottom of the tree, i.e. at a leaf or at the terminal node (mate, stalemate)
  // Evaluation - the node is already evaluated during previous expansion!
  // We could actually improve the eval by using move_evals calculated later in the code for the children nodes before expansion for evaluating its parent (this node), kind of look ahead eval
  int scorecp = 0;
  double result = 0.0;
  if (repetition) {
    node->cp = 0; //just store draw score 0 in cp without node expansion
  } else {
    if (hash_full.load(std::memory_order_relaxed) < 1000) {
      accumulator_stack_reset(ctx);
      result = position_eval(sim_board, sim_zh, ctx, pos_history, thread_id);
      if (params->nodes[node_idx].hash == params->nodes[0].hash) { //root node
        scorecp = static_cast<int>(result * 100);
        result = tanh(result / eval_scale);
        params->nodes[0].cp = scorecp;
        params->nodes[0].N = 1;
        params->nodes[0].W = result;
      }
      if (!sim_board.isMate && !sim_board.isStaleMate) { //expand if not terminal
        result = eval_and_expand(sim_board, sim_zh, ctx, pos_history, thread_id, node_idx);
      } else {
        if (sim_board.isMate) {
          result = -1;
        } //for stalemate the result is initiated to 0, so no need for this check
      }
    } //end of hash_full.load(std::memory_order_relaxed) < 1000)
  }
  // Backpropagation: update node visits and results regardless of whether we expand the node or not
  for (auto n = path.rbegin(); n != path.rend(); ++n) {
    node_idx = *n;
    params->nodes[node_idx].N++;
    params->nodes[node_idx].W += result;
    if (node_idx == 0 && root_child_idx >= 0) root_W[root_child_idx].fetch_add(result, std::memory_order_relaxed);
    result = -result;
  }
  if (root_child_idx >= 0) root_N[root_child_idx].fetch_add(1, std::memory_order_relaxed);
}

void uci_output_thread() {
  auto iter_start = std::chrono::steady_clock::now();

  while (!stopFlag.load(std::memory_order_relaxed) && !search_done.load(std::memory_order_relaxed)) {
    std::unique_lock<std::mutex> lk(search_done_mtx);
    cv_search_done.wait_for(lk, std::chrono::milliseconds(1000), []{return search_done.load(std::memory_order_relaxed);});
    // Calculate nodes (total simulations)
    //find the best move (best_child_idx) from accumulated visits (root_stats[child_idx]) - the highest N
    uint32_t max_N = 0;
    int best_child_idx = -1;
    for (int i = 0; i < board.num_moves; ++i) {
      uint32_t N2 = root_N[i].load(std::memory_order_relaxed);
      if (N2 > max_N) {
        max_N = N2; 
        best_child_idx = i;
      }
    }
    //printf("uci_output_thread() debug: best_child_idx %d, max_N %u, number of moves %d\n", best_child_idx, max_N, board.num_moves);
    //find the best thread - the one with the most visits for the best move - best_child_idx found above
    /*uint32_t N = 0;
    int best_thread = -1;
    for (int thread = 0; thread < chessEngine.optionSpin[Threads].value && best_child_idx >= 0; thread++) {
      auto params = pool_params[thread];
      //auto& root = params->nodes[0];
      auto childN = params->nodes[params->nodes[0].children[best_child_idx].child_idx].N;
      if (N < childN) {
        N = childN;
        best_thread = thread;
      }
    }
    if (best_thread == -1) continue;*/
    //printf("uci_output_thread() debug: best thread id %d\n", best_thread);
    //use params from this best thread
    auto params = pool_params[0];
    //MCTSNode * current_node = &params->nodes[0];
    // Get elapsed time
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
    // Compute NPS
    double nps = elapsed > 0 ? max_N / elapsed : 0;
    //depth should be calculated by traversing the most visited nodes similar to select_best_moves()
    //to avoid locking, we use average seldepth across the threads
    int d = 0;
    for (int thread = 0; thread < chessEngine.optionSpin[Threads].value && best_child_idx >= 0; thread++) {
      auto params = pool_params[thread];
      d += params->seldepth.load(std::memory_order_relaxed);
    }
    d /= chessEngine.optionSpin[Threads].value;
    //std::unordered_set<uint64_t> visited; //hashes for loop detection
    std::vector<std::pair<uint32_t, int>> visits; //N, child_idx
    uint32_t child_idx = 0;
    int num_children = params->nodes[child_idx].children.size();
    for (int i = 0; i < num_children && d < MAX_DEPTH; i++) {
      //printf("uci_output_thread() debug: current_node hash %llx, root hash %llx\n", current_node->hash, params->nodes[0].hash);
      //if (visited.find(params->nodes[child_idx].hash) != visited.end()) {
      //    log_file("uci_output_thread() debug: cycle detected at depth %d, breaking loop\n", d);
      //    break;
      //}
      //visited.insert(params->nodes[child_idx].hash);
      auto& child = params->nodes[params->nodes[child_idx].children[i].child_idx];
      visits.push_back({child.N, i});  
      /*uint32_t N = 0;
      int next_idx = -1;
      for (int i = 0; i < num_children; i++) {
        auto& child = params->nodes[params->nodes[child_idx].children[i].child_idx];
        //collect visits for root's children only
        if (params->nodes[child_idx].hash == params->nodes[0].hash) {
          //printf("uci_output_thread() debug: updating visits N %d, i %d\n", child.N, i);
          visits.push_back({child.N, i}); 
        }
        if (child.N > N) {
          N = child.N;
          next_idx = i;
        } 
      }
      //if (next_idx < 0) break; //meaning current_node is a leaf node, i.e. no children
      //we found next best move (next_idx) - update current_node
      child_idx = params->nodes[child_idx].children[next_idx].child_idx;
      num_children = params->nodes[child_idx].children.size();
      //printf("uci_output_thread() debug: next best child_idx %d, num_children %d\n", next_idx, num_children);
      d++;*/
    }
    //we found the depth d, let's preserve it in global atomic depth
    //printf("uci_output_thread() debug: max depth %d\n", d);
    depth.store(d, std::memory_order_relaxed);
    size_t unique_nodes = 0;
    uint64_t all_children = 0;
    for (int thread = 0; thread < chessEngine.optionSpin[Threads].value; thread++) {
       unique_nodes += pool_params[thread]->next_idx.load(std::memory_order_relaxed);
       all_children += pool_params[thread]->total_children.load(std::memory_order_relaxed);
    }
    //printf("uci_output_thread() debug: unique nodes %zu, total children %llu\n", unique_nodes, all_children);
    size_t total_memory = unique_nodes * (sizeof(MCTSNode) + 24) + all_children * sizeof(Edge);
    size_t max_capacity = chessEngine.optionSpin[Hash].value * 1024 * 1024;  // MB to bytes
    int hashfull = max_capacity ? (total_memory * 1000) / max_capacity : 0;
    if (hashfull > 1000) hashfull = 1000;  // Cap at 1000 per UCI spec
    hash_full.store(hashfull, std::memory_order_relaxed);

    std::sort(visits.begin(), visits.end(), std::greater<>());
    int multiPV = std::min<int>(board.num_moves, (int)chessEngine.optionSpin[MultiPV].value);
    //auto& root = params->nodes[0];
    for (int i = 0; i < multiPV; i++) {
      const int move_idx = params->nodes[0].children[visits[i].second].move;
      //printf("uci_output_thread() debug: move_idx %d\n", move_idx);
      char uci_move[6];
      idx2uci(move_idx, uci_move);
      auto& child = params->nodes[params->nodes[0].children[visits[i].second].child_idx];
      log_file("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", d, seldepth.load(std::memory_order_relaxed), i + 1, -child.cp, max_N, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, uci_move);
      print("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", d, seldepth.load(std::memory_order_relaxed), i + 1, -child.cp, max_N, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, uci_move);
    }
  }
}

void runMCTS() {
  double elapsed = 0.0;
  size_t unique_nodes = 0;
  uint32_t nodes = 0;
  int hashfull = 0;
  std::vector<std::pair<int, std::string>> pvs;
  int multiPV = 1;
  
	isCheckMateStaleMate(board); //it calculates board.num_moves as well as generates all legal moves to detect terminal state
  if (board.num_moves > 1) { //run MCTS using multiple threads
    tbhits.store(0, std::memory_order_relaxed);
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
    std::memset(root_N, 0, sizeof root_N);
    std::memset(root_W, 0, sizeof root_W);
    for (int i = 0; i < num_threads; ++i) {
        cleanup(i); 
        pool_params[i]->nnue_cache.clear();
        pool_params[i]->thread_id = i;
        pool_params[i]->time_alloc = timeAllocated;
        pool_params[i]->seldepth = 0;
        set_root(i); 
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
      uint64_t total_children = 0;
      elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
      for (int i = 0; i < num_threads; ++i) {
        nodes += pool_params[i]->nodes[0].N;
        unique_nodes += pool_params[i]->next_idx.load(std::memory_order_relaxed);
        total_children += pool_params[i]->total_children.load(std::memory_order_relaxed);
      }
      // Calculate hashfull (in per-mille) using unique_nodes
      size_t total_memory = unique_nodes * (sizeof(MCTSNode) + 24) + total_children * sizeof(Edge);
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
    	uint64_t check_mask = checkers ? checkMask(board, kingSquare, checkers) : 0xffffffffffffffffULL;
      move.src = kingSquare;
      move.promoType = PieceTypeNone;
  	  while (moves && move_idx == 0) {
  	    move.dst = lsBit(moves);
  	    move_idx = (move.promoType << 12) | (move.src << 6) | move.dst;
        moves &= moves - 1;
      }
      for (PieceType pt = Queen; pt >= Pawn && move_idx == 0; pt = (PieceType)(pt - 1)) {
      	uint64_t occupations = board.side[board.sideToMove] & board.pieceTypes[pt - 1]; 
      	while (occupations && move_idx == 0) {
      	  move.src = lsBit(occupations);
  	      moves = piece_moves(board, pt, move.src, kingSquare, pinned, pinning, check_mask);
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
      
      NNUEContext ctx;
      init_nnue_context(ctx);
      accumulator_stack_reset(ctx);
      StateInfo state = {};
      do_move(board, move, state);
  		isCheckMateStaleMate(board);
      double res;
      if (board.isMate) res = MATE_SCORE * 0.01;
      else if (board.isStaleMate) res = 0.0;
      else {
        std::unordered_set<uint64_t>pos_history;
        ZobristHash tmp_zh = zh;
        res = -quiescence(board, tmp_zh, ctx, qs_max_depth, pos_history);
      }
      undo_move(board, move, state);
      free_nnue_context(ctx);
      
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
  } //board.num_moves == 0
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
