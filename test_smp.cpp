//c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -I /Users/ap/libchess  -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess tbcore.c tbprobe.c test_smp.cpp -o test_smp

#include "nnue/types.h"
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
#include <iostream>
#include "tbprobe.h"
#include "libchess.h"
//8/5p2/7p/2p1n2P/1k2N3/1P2P3/2K5/8 b - - 1 62
#define MULTI_PV 5
#define SYZYGY_PATH_DEFAULT "<empty>"
#define SYZYGY_PATH "/Users/ap/syzygy"
#define HASH 1024 //default, GUI may set it via Hash option (once full, expansion won't happen!)
#define MAX_DEPTH 100
#define THREADS 8
#define PV_PLIES 16
#define TEMPERATURE 60
#define EXPLORATION_MIN 50 // used in formular for exploration constant decay with depth
#define EXPLORATION_MAX 200 //smaller value favor exploitation, i.e. deeper tree vs wider tree
#define EXPLORATION_DEPTH_DECAY 8 //linear decay of EXPLORATION CONSTANT with depth using formula:
                      // C * 100 = max(EXPLORATION_MIN, (EXPLORATION_MAX - seldepth * EXPLORATION_DEPTH_DECAY))
#define PROBABILITY_MASS 100 //% - cumulative probability - how many moves we consider
#define VIRTUAL_LOSS 4
#define EVAL_SCALE 6.0 //used in conversion to result = tanh(cp * 0.01 / eval_scale), i.e.
                       //how pessimistic evaluation in pawns (cp * 0.01) is, higher values -> lower result
                       //another words, what is considered a definite win or loss in pawns
                       //for example, for an extra queen, result = tanh(9 / 6) = 0.9 - almost a win, it is close to 1
                       //but if eval_scale = 4, then tanh(9 / 4) = ~0.98 - even closer to 1
#define PONDER false
#define DISPLAY_INTERMITTENT_INFO_LINES true
#define DISPLAY_FINAL_INFO_LINES true

double timeAllocated = 30000; //ms
char best_move[6];
std::atomic<bool> search_done{false}; // Signals search completion
std::condition_variable cv_search_done;
std::mutex search_done_mtx;
double exploration_max, exploration_min, exploration_depth_decay;
//double probability_mass;
double virtual_loss;
double eval_scale;
double temperature;

struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack = nullptr;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches = nullptr;    
};
void init_nnue(const char * nnue_file_big, const char * nnue_file_small);
void cleanup_nnue();
void init_nnue_context(NNUEContext& ctx);
void free_nnue_context(NNUEContext& ctx);
double evaluate_nnue(const Board& chess_board, NNUEContext& ctx);
void accumulator_stack_push(NNUEContext& ctx, Stockfish::DirtyPiece& dp);
void accumulator_stack_pop(NNUEContext& ctx);
void accumulator_stack_reset(NNUEContext& ctx);
void compute_move_evals(Board& chess_board, const ZobristHash& board_hash, NNUEContext& ctx, const std::unordered_set<uint64_t>& pos_history, std::vector<std::tuple<double, int, int, uint64_t>>& move_evals);
double position_eval(Board& chess_board, ZobristHash& board_hash, NNUEContext& ctx, const std::unordered_set<uint64_t>& pos_history);

Board board;
ZobristHash zh;
Zobrist z;
Engine chessEngine;
std::mutex probe_mutex, log_mtx, print_mtx;
std::shared_mutex map_mutex;
std::atomic<uint64_t> total_children{0};
std::atomic<int> hash_full{0};
std::atomic<uint64_t> tbhits{0};
std::atomic<int> generation{0};

std::atomic<int> depth{0};
std::atomic<int> seldepth{0};
  
struct Edge {
    std::atomic<int> move {0};             // The move that leads to the child position
    std::atomic<double> P {0.0};            // Prior probability - model move_probs for a given move in the node
    std::atomic<struct MCTSNode *> child {nullptr}; // Pointer to the child node
};

struct MCTSNode {
    std::atomic<uint64_t> hash{0};
    std::atomic<uint64_t> N{0};  // Atomic for lock-free updates
    std::atomic<double> W{0};
    std::atomic<int> cp {NO_MATE_SCORE}; //position evaluation in centipawns 
    std::atomic<int> num_children{0};
    std::atomic<int> generation{0};
    std::shared_mutex mutex;  // For protecting children expansion
    std::atomic<struct Edge *> children {nullptr};
};

// Custom hasher that uses the key directly
struct NoOpHash {
    std::size_t operator()(uint64_t key) const noexcept {
        return key; // Directly use the key as the hash
    }
};

struct MCTSSearch {
    MCTSNode * root = nullptr;
    std::unordered_map<uint64_t, MCTSNode *, NoOpHash> tree;
};

static MCTSSearch search;

// Remove MCTSSearch from ThreadParams (shared now)
struct ThreadParams {
    int thread_id;
    uint64_t time_alloc;
    int seldepth;
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
      Edge * children = node->children.load(std::memory_order_relaxed);
      delete[] children;
      delete node;
  }
  search.tree.clear();
  search.root = nullptr;
  total_children.store(0, std::memory_order_relaxed);
}

//no locking, call it before starting other threads (search, etc)
void set_root() {
  auto it = search.tree.find(zh.hash);
  MCTSNode * root = (it != search.tree.end()) ? it->second : nullptr;
  if (!root) {
    root = new MCTSNode();
    root->hash.store(zh.hash, std::memory_order_relaxed);
    root->generation.store(generation.load(std::memory_order_relaxed), std::memory_order_relaxed);
    search.tree.emplace(zh.hash, root);
  }
  search.root = root;    
}

//called from expand_node()
//returns new or existing node
MCTSNode * make_child(const unsigned long hash, const int cp) {
  MCTSNode * child = nullptr;
  //first, try to find hash in the tree
  std::shared_lock search_lock(map_mutex);
  auto it = search.tree.find(hash);
  child = (it != search.tree.end()) ? it->second : nullptr;
  search_lock.unlock();
  if (!child) { 
    child = new MCTSNode();
    child->hash.store(hash, std::memory_order_relaxed);
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
    }
  }
  return child; //may not be nullptr
}
  
struct TempEdge {
    int move = 0;
    double P = 0.0; 
    MCTSNode * child = nullptr;
};
//called from mcts_search() and next_moves()
//calls make_child()
void expand_node(MCTSNode * parent, const std::vector<std::tuple<double, int, int, uint64_t>>& top_moves, const std::unordered_set<uint64_t>& pos_history) {
  //parent is locked for the expansion with unique_lock in caller - mcts_search()
  if (parent->num_children.load(std::memory_order_relaxed) > 0) return; //already expanded by other threads, perhaps
  int num_moves = top_moves.size();
  assert(num_moves > 0);
  Edge * children = new Edge[num_moves];
  for (int i = 0; i < num_moves; ++i) {
      auto [prior, move_idx, child_cp, child_hash] = top_moves[i];
      MCTSNode * child = make_child(child_hash, child_cp);
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
//returns multiPV and PVs
int select_best_moves(std::vector<std::pair<int, std::string>>& pvs) { 
  int num_children = search.root->num_children.load(std::memory_order_relaxed);
  if (!num_children) {
    print("select_best_moves() warning: root node has no children!\n");
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
      prior = children2->P.load(std::memory_order_relaxed);
      idx2uci(children2[idx].move.load(std::memory_order_relaxed), uci_move);
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
int select_best_child(MCTSNode * parent, int depth) {
  // 1. Dynamic Exploration Constant - linear decay with depth
  // setting exploration_depth_decay to 0 will make exploration constant static = exploration_max
  // Decay: Start at exploration_constant, then for example drop by 0.05 - 0.1 per ply, floor at 0.25 - all tunable
  double C = std::max(exploration_min, exploration_max - (depth * exploration_depth_decay));
  //or square root decay with depth
  //double C = std::max(exploration_min, exploration_max - (sqrt(static_cast<double>(depth)) * exploration_depth_decay));
  
  // OPTIONAL: Bonus for Root Node (Depth 0) to ensure wide scanning
  //if (depth == 0) C = 2.0;

  int num_children = parent->num_children.load(std::memory_order_acquire);
  Edge * children = parent->children.load(std::memory_order_acquire);    
  
  double best_score = -INFINITY;
  int selected;
  for (int i = 0; i < num_children; i++) {
    parent->mutex.lock_shared();
    double P = children[i].P.load(std::memory_order_relaxed);
    MCTSNode * child = children[i].child.load(std::memory_order_acquire);
    parent->mutex.unlock_shared();
    uint64_t N = child->N.load(std::memory_order_relaxed);
    double W = -child->W.load(std::memory_order_relaxed); //parent perspective
    double Q = N ? W / N : 0.0;
    double score = Q + C * P * sqrt(static_cast<double>(parent->N.load(std::memory_order_acquire))) / (1.0 + N);
    if (score > best_score) {
      best_score = score;
      selected = i;
    }
  }
  // Apply virtual loss to selected child to avoid contention among threads for the same node
  //children = parent->children.load(std::memory_order_acquire);
  parent->mutex.lock_shared();
  MCTSNode * child = children[selected].child.load(std::memory_order_acquire);
  parent->mutex.unlock_shared();
  child->N.fetch_add(1, std::memory_order_release);
  child->W.fetch_sub(virtual_loss, std::memory_order_release);
  return selected;
}

void get_prob(std::vector<std::tuple<double, int, int, uint64_t>>& move_evals) {
    //size_t n = move_evals.size();
    //if (n == 0) return 0;
    double max_val = -std::numeric_limits<double>::infinity();
    for (const auto& move_eval : move_evals) {
        if (std::get<0>(move_eval) > max_val) max_val = std::get<0>(move_eval);
    }
    double total = 0.0;
    for (const auto& move_eval : move_evals) {
        total += std::exp((std::get<0>(move_eval) - max_val)/temperature);
    }
    if (total == 0.0) {  // Rare case: all -inf or underflow
        double uniform = 1.0 / move_evals.size();
        for (auto& move_eval : move_evals) std::get<0>(move_eval) = uniform;
        return;
    }
    //double cum_mass = 0.0;
    //int effective = 0;
    for (auto& move_eval : move_evals) {
        std::get<0>(move_eval) = std::exp((std::get<0>(move_eval) - max_val)/temperature) / total;
        //cum_mass += std::get<0>(move_eval);
        //++effective;
        //if (cum_mass >= prob_mass) break;
    }
    //return effective;
}

//called from do_move() and run_MCTS()
//calls make_child(), compute_move_evals() and expand_node()
//returns the result in pawns from the temp_board->sideToMove perspective
double process_check(Board& temp_board, ZobristHash& board_hash, NNUEContext& ctx, const std::unordered_set<uint64_t>& pos_history) {
  MCTSNode * node = make_child(board_hash.hash, NO_MATE_SCORE);
  int stored_cp = node->cp.load(std::memory_order_relaxed);
  if (stored_cp == NO_MATE_SCORE) { //make_child() returned new node without a parent, let's update its cp and expand it
    //we will link this node to the parent during a call expand_node() made later from mcts_search() 
    std::vector<std::tuple<double, int, int, uint64_t>> move_evals; 
    //use 1.0 for probability mass to try all moves - when in check, there shouldn't be too many moves
    compute_move_evals(temp_board, board_hash, ctx, pos_history, move_evals); 
    int cp = -std::get<2>(move_evals[0]); //select the best cp for check evasion, may not be the best one though
                                          //for example, capture moves would often have high priors
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

//called from do_move()
//calls evaluate_nnue() or process_check() or tb_probe_wdl()
//returns position evaluation in pawns from board_fen->sideToMove perspective
double position_eval(Board& chess_board, ZobristHash& board_hash, NNUEContext& ctx, const std::unordered_set<uint64_t>& pos_history) {
  double res;
	const int pieceCount = bitCount(chess_board.side[ColorWhite] | chess_board.side[ColorBlack]);
	isCheckMateStaleMate(chess_board);
	if (pieceCount > TB_LARGEST || ((unsigned int *)chess_board.castlingRook)[0] != 0x08080808) {
		//debug
		//char fen[MAX_FEN_STRING_LEN];
		//board2fen(chess_board, fen);
    if (chess_board.isMate) {
      res = -MATE_SCORE * 0.01; //chess_board.sideToMove loses
      //print("position_eval() debug: mate detected, returning %f, fen %s\n", res, fen);
    }
    else if (chess_board.isStaleMate) {
      //print("position_eval() debug: stalemate detected, returning 0, fen %s\n", fen);
      res = 0.0;
    } else if (chess_board.isCheck) {
      res = process_check(chess_board, board_hash, ctx, pos_history);
      //print("position_eval() debug: check detected, processing and returning %f, fen %s\n", res, fen);
    } else {
      res = evaluate_nnue(chess_board, ctx); //evaluate_nnue() returns result in pawns (not centipawns!)
    }
  } else { //pieceCount <= TB_LARGEST, etc
    unsigned int ep = enPassantLegal(chess_board);     
    const unsigned int wdl = tb_probe_wdl(chess_board.side[ColorWhite], chess_board.side[ColorBlack], chess_board.pieceTypes[King - 1], chess_board.pieceTypes[Queen - 1], chess_board.pieceTypes[Rook - 1], chess_board.pieceTypes[Bishop - 1], chess_board.pieceTypes[Knight - 1], chess_board.pieceTypes[Pawn - 1], 0, 0, ep == SquareNone ? 0 : ep, chess_board.sideToMove == ColorWhite ? 1 : 0);
    if (wdl == TB_RESULT_FAILED) {
      char fen[MAX_FEN_STRING_LEN];
      print("error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, occupations %u, ep %u, halfmoveClock %u, whiteToMove %u, whites %llu, blacks %llu, kings %llu, queens %llu, rooks %llu, bishops %llu, knights %llu, pawns %llu, fen %s, err %s\n", TB_LARGEST, pieceCount, ep, chess_board.halfmoveClock, chess_board.sideToMove == ColorWhite ? 1 : 0, chess_board.side[ColorWhite], chess_board.side[ColorBlack], chess_board.pieceTypes[King - 1], chess_board.pieceTypes[Queen - 1], chess_board.pieceTypes[Rook - 1], chess_board.pieceTypes[Bishop - 1], chess_board.pieceTypes[Knight - 1], chess_board.pieceTypes[Pawn - 1], board2fen(chess_board, fen), strerror(errno));
  		//isCheckMateStaleMate(chess_board);
      if (chess_board.isMate) res = -MATE_SCORE * 0.01;
      else if (chess_board.isStaleMate) {
        res = 0.0; 
      } else if (chess_board.isCheck) {
        res = process_check(chess_board, board_hash, ctx, pos_history);
      } else {
        res = evaluate_nnue(chess_board, ctx); //evaluate_nnue() returns result in pawns (not centipawns!)
      }
    } else { //tb_probe_wdl() succeeded
      //0 - loss, 4 - win, 1..3 - draw
      if (wdl == 4) res = MATE_SCORE * 0.001;
      else if (wdl == 0) res = -MATE_SCORE * 0.001;
      else res = 0.0;
      tbhits.fetch_add(1, std::memory_order_relaxed);
    }
  } //end of else (pieceCount <= TB_LARGEST)
  return res;
}

//called from compute_move_evals()
//calls evaluate_nnue()
//returns eval result in pawns from the perspective of chess_board.sideToMove
double make_move(Board& chess_board, const ZobristHash& board_hash, Move& move, NNUEContext& ctx, uint64_t& child_hash, const std::unordered_set<uint64_t>& pos_history) {
  ZobristHash tmp_hash = board_hash;
  StateInfo state = {};
  Stockfish::DirtyPiece dp;
  updateHash(tmp_hash, chess_board, move, do_move_dp(chess_board, move, state, dp), z);
  child_hash = tmp_hash.hash;
  if (pos_history.count(child_hash) > 0) {
    undo_move(chess_board, move, state);
    return 0.0; 
  }
  accumulator_stack_push(ctx, dp);
  double res = position_eval(chess_board, tmp_hash, ctx, pos_history); //evaluate the position
  undo_move(chess_board, move, state);
  accumulator_stack_pop(ctx);
  return -res;
}

//called from mcts_search() and process_check()
//calls do_move()
//computes and returns move_evals tuple given chess_board, prob_mass and pos_history
void compute_move_evals(Board& chess_board, const ZobristHash& board_hash, NNUEContext& ctx, const std::unordered_set<uint64_t>& pos_history, std::vector<std::tuple<double, int, int, uint64_t>>& move_evals) {
      double res;
      Move move = {};
    	MovesContext movesContext = {};
     	KingSquare kingSq;
     	move.src = getKingSquare(chess_board, kingSq);
     	uint64_t attackedSquares = getAttackedSquares(chess_board, movesContext);
  	  uint64_t moves = kingMoves(chess_board, move.src, kingSq, movesContext, attackedSquares);
  	  while (moves) {
  	    move.dst = lsBit(moves);
  	    uint64_t child_hash = 0;
  	    res = make_move(chess_board, board_hash, move, ctx, child_hash, pos_history);
        move_evals.push_back({res, (move.promoType << 12) | (move.src << 6) | move.dst, static_cast<int>(-res * 100), child_hash});
        moves &= moves - 1;
      }
      if (movesContext.num_checkers > 1) {
        //if (chess_board.num_moves == 0) chess_board.isMate = true;
        goto sort;
      }
      
      for (PieceType pt = Queen; pt >= Pawn; pt = (PieceType)(pt - 1)) {
      	uint64_t occupations = chess_board.side[chess_board.sideToMove] & chess_board.pieceTypes[pt - 1]; 
      	while (occupations) {
      	  move.src = lsBit(occupations);
  	      moves = piece_moves(pt, move.src, movesContext, kingSq, chess_board);
      	  while (moves) {
      	    move.dst = lsBit(moves);
          	PieceType startPiece = PieceTypeNone, endPiece = PieceTypeNone;
          	if (promoMove(chess_board, move)) {
          	  startPiece = Knight;
          	  endPiece = Queen;
          	}
        	  for (move.promoType = startPiece; move.promoType <= endPiece; move.promoType = (PieceType)(move.promoType + 1)) { //loop over promotions if any
        	    uint64_t child_hash = 0;
        	    res = make_move(chess_board, board_hash, move, ctx, child_hash, pos_history);
              move_evals.push_back({res, (move.promoType << 12) | (move.src << 6) | move.dst, static_cast<int>(-res * 100), child_hash});
        	  }
            moves &= moves - 1;
          }
          occupations &= occupations - 1;
        }
      }
sort:
      // Sort by res descending
      std::sort(move_evals.begin(), move_evals.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b);});
      get_prob(move_evals);
      //move_evals.resize(effective_branching);
}

//called from thread_search()
//calls expand()
void mcts_search(ThreadParams * params, NNUEContext& ctx) {
  std::vector<MCTSNode *> path;  // Track the path from root to leaf
  path.reserve(chessEngine.depth);
  bool repetition = false;
  params->seldepth = 0;
  MCTSNode * node = search.root;
  //start from the same initial position given by board (at the root node, i.e. at the top of the tree - the up side down tree)
  //copy the board to preserve it for subsequent iterations
  Board sim_board = board;
  ZobristHash sim_zh = zh;
  // Selection
  //iterate down the tree updating sim_board by initiating and making moves
  //thread-local map to prevent repetition cycles - it should be global I think but thread-safety may be a problem
  std::unordered_set<uint64_t> pos_history;
  while (node->num_children.load(std::memory_order_relaxed) > 0) { //traversal stops at a leaf or at 3x repetition (mate or stalemate node should not have children)
    //return child node index with the best score using PUCT (Predictor + Upper Confidence Bound)
    //it also adds virtual loss to the node to reduce contention for the same node in multi-threaded engine
    int idx = select_best_child(node, params->seldepth);
    Edge * children = node->children.load(std::memory_order_acquire);
    std::shared_lock lock(node->mutex);
    int move_idx = children[idx].move.load(std::memory_order_relaxed);
    path.push_back(node);  // Add parent node to path (the move is made, so the node is a parent one)
    //continue iterating down the tree by getting next node until no more children
    node = children[idx].child.load(std::memory_order_acquire);
    lock.unlock();
    //for debugging only
    //char fen[MAX_FEN_STRING_LEN];
    //board2fen(sim_board, fen);
    //init and take edge's move that leads to the child node
    Move move;
    move.dst = (Square)(move_idx & 63);
    move.src = (Square)((move_idx >> 6) & 63);
    move.promoType = (PieceType)((move_idx >> 12) & 7);
    //move.type = MoveTypeNormal; //this is done in ff_move()
    //update Zobrist hash (it is needed so that we can call updateHash() later instead of getHash()
    //debug
    /*char fen[MAX_FEN_STRING_LEN];
    printf("mcts_search() debug: position fen %s moves %s%s\n", board2fen(sim_board, fen), square[move.src], square[move.dst]);*/
		updateHash(sim_zh, sim_board, move, ff_move(sim_board, move), z);
    params->seldepth++;
    int path_count = pos_history.count(sim_zh.hash); //simulated positions ahead of the current one
    if (path_count == 0) pos_history.insert(sim_zh.hash);
    repetition = (path_count >= 1); 
    if (repetition) break;
  } //end of while(node->num_children > 0) loop
  path.push_back(node);  // Add leaf to path - sim_board corresponds to this node!
  //Here we are at the bottom of the tree, i.e. at a leaf or at the terminal node (mate, stalemate)
  // Evaluation - the node (except root) is already evaluated during previous expansion!
  // We could actually improve the eval by using move_evals calculated later in the code for the children nodes before expansion for evaluating its parent (this node), kind of look ahead eval
  // Expansion - add more children - increase the depth of the tree using NNUE evals
  // in theory, if children evaluation is noticably different from its parent, 
  // then we need to continue selectively expanding until position is quiet
  // otherwise, this difference gets propagated to the root and may affect selection, leading to suboptimal play
  int scorecp = 0;
  double result = 0.0;
  if (repetition) {
    // do not expand on repetition, just set its eval to draw
    node->cp.store(scorecp, std::memory_order_relaxed);
  } else {
    if (node->mutex.try_lock()) { //the leaf node in a tree is locked only for expansion
                                //nodes locked in selection phase are not leaf nodes, i.e. nodes without children
                                //if leaf node is already locked, it means that other thread is expanding it already
  		//isCheckMateStaleMate(sim_board);
      //if (!sim_board.isMate && !sim_board.isStaleMate && hash_full.load(std::memory_order_relaxed) < 1000) {
      if (hash_full.load(std::memory_order_relaxed) < 1000) {  
        //the node has been already evaluated except if it is root node
        //here we call position_eval() to reset the accumulator_stack, so subsequent evaluate_nnue() calls are incremental, hence faster
        accumulator_stack_reset(ctx);
        result = position_eval(sim_board, sim_zh, ctx, pos_history); //calls isCheckMateStaleMate(sim_board)
        if (node == search.root) {
          scorecp = static_cast<int>(result * 100);
          result = tanh(result / eval_scale);
          node->cp.store(scorecp, std::memory_order_relaxed);
          node->N.store(1, std::memory_order_relaxed);
          node->W.store(result, std::memory_order_relaxed);
        }
        if (!sim_board.isMate && !sim_board.isStaleMate) {
        	std::vector<std::tuple<double, int, int, uint64_t>> move_evals; //res, move_idx, cp, hash (res is converted to probabilities in get_prob(), hence we need to preserve it in cp)
          compute_move_evals(sim_board, sim_zh, ctx, pos_history, move_evals);
          //updating node's cp with improve evaluation 
          if (move_evals.size() == 0) {
            char fen[MAX_FEN_STRING_LEN];
            printf("mcts_search() debug: move_evals size is 0, fen %s\n", board2fen(sim_board, fen));
            exit(1);
          }
          scorecp = -std::get<2>(move_evals[0]);
          node->cp.store(scorecp, std::memory_order_relaxed); //look-ahead update
          result = tanh(scorecp * 0.01 / eval_scale);
          //before expanding, it would be nice to insure that position is quiet for correct cp, i.e. evals are correct!
          //we could try to iteratively play moves that are different in evals from scorecp by at least 1 pawn
          expand_node(node, move_evals, pos_history);
        } else {
          if (sim_board.isMate) {
            scorecp = -MATE_SCORE;
            result = -1;
          }
        }
      } //end of if (!sim_board.isMate && !sim_board.isStaleMate && hash_full.load(std::memory_order_relaxed) < 1000)
      node->mutex.unlock();
    } //end of if (node->mutex.try_lock())
    else {
      scorecp = node->cp.load(std::memory_order_relaxed);
      if (scorecp == NO_MATE_SCORE) { //node has not been evaluated yet, return without a backprop (a bit of a waste)
        if (node != search.root) { //undo virtual loss for non-root node - this is unlikely to happen
          for (size_t j = 1; j < path.size(); ++j) {  // From first child to leaf
            MCTSNode * nd = path[j];
            nd->N.fetch_sub(1, std::memory_order_relaxed);
            nd->W.fetch_add(virtual_loss, std::memory_order_relaxed);
          }      
        }
        return; //do nothing if node has no evaluation - only happens briefly at the beginning of search for root node
      } //end of NO_MATE_SCORE
      result = tanh(scorecp * 0.01 / eval_scale);
    } //end of else (unable to lock)
  } //end of (!repetition)
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
  
void thread_search(ThreadParams * params) {
  NNUEContext ctx;
  init_nnue_context(ctx);
   
  auto iter_start = std::chrono::steady_clock::now();
  double elapsed = 0.0;
  while (depth.load(std::memory_order_relaxed) < chessEngine.depth && elapsed < (params->time_alloc * 0.001) && hash_full.load(std::memory_order_relaxed) < 1000) {
      mcts_search(params, ctx); //single sim

      // Update seldepth
      int expected = seldepth.load(std::memory_order_relaxed);
      while (params->seldepth > expected && !seldepth.compare_exchange_strong(expected, params->seldepth, std::memory_order_relaxed)) {
          expected = seldepth.load(std::memory_order_relaxed);
      }
      
      if ((chessEngine.depth && depth.load(std::memory_order_relaxed) >= chessEngine.depth) || (chessEngine.nodes && search.root->N.load(std::memory_order_relaxed) >= chessEngine.nodes)) break;        

      elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
  }
  free_nnue_context(ctx);
}

void uci_output_thread() {
  auto iter_start = std::chrono::steady_clock::now();

  while (!search_done.load(std::memory_order_relaxed)) {
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
    std::unordered_set<uint64_t> visited;
    std::vector<std::pair<uint64_t, int>> visits; //N, child_idx
    int num_root_children = current_node->num_children.load(std::memory_order_relaxed);
    while (current_node && d < MAX_DEPTH) {
      uint64_t current_hash = current_node->hash.load(std::memory_order_relaxed);
      if (visited.find(current_hash) != visited.end()) {
          print("uci_output_thread() debug: cycle detected at depth %d, breaking loop\n", d);
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
      print("info depth %d seldepth %d multipv %d score cp %d nodes %llu nps %.0f hashfull %d tbhits %lld time %.0f pv %s\n", d, seldepth.load(std::memory_order_relaxed), i + 1, -child->cp.load(std::memory_order_relaxed), nodes, nps, hashfull, tbhits.load(std::memory_order_relaxed), elapsed * 1000, uci_move);
    }
  }
}

void runMCTS() {
  double elapsed = 0.0;
  size_t unique_nodes = 0;
  uint64_t nodes = 0;
  int hashfull = 0;
  std::vector<std::pair<int, std::string>> pvs;
  int multiPV = 1;
  //MovesContext movesContext = {};
  //uint64_t movesFromSquares[64] = {0};
	//generateMoves(board, movesFromSquares);
	isCheckMateStaleMate(board);

  if (board.num_moves > 1) {
    tbhits.store(0, std::memory_order_relaxed);
    std::vector<ThreadParams> thread_params(chessEngine.optionSpin[Threads].value);
    for (int i = 0; i < chessEngine.optionSpin[Threads].value; ++i) {
      thread_params[i].thread_id = i;
      thread_params[i].time_alloc = timeAllocated;
      thread_params[i].seldepth = 0;
    }
    set_root();      
    
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
      {
        std::lock_guard<std::mutex> lk(search_done_mtx);
        search_done.store(true, std::memory_order_relaxed); // Signal search complete
      }
      cv_search_done.notify_one();
      output_thread.join();
    } else {
      for (auto& t : threads) t.join();
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
  } else if (board.num_moves == 1) {
      auto iter_start = std::chrono::steady_clock::now();
      int move_idx = 0;
      Move move = {};
      MovesContext movesContext = {};
     	KingSquare kingSq;
     	move.src = getKingSquare(board, kingSq);
  	  uint64_t moves = kingMoves(board, move.src, kingSq, movesContext, getAttackedSquares(board, movesContext));
  	  while (moves && move_idx == 0) {
  	    move.dst = lsBit(moves);
  	    move_idx = (move.promoType << 12) | (move.src << 6) | move.dst;
        moves &= moves - 1;
      }
      for (PieceType pt = Queen; pt >= Pawn && move_idx == 0; pt = (PieceType)(pt - 1)) {
      	uint64_t occupations = board.side[board.sideToMove] & board.pieceTypes[pt - 1]; 
      	while (occupations && move_idx == 0) {
      	  move.src = lsBit(occupations);
  	      moves = piece_moves(pt, move.src, movesContext, kingSq, board);
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
      else if (board.isCheck) {
        std::unordered_set<uint64_t>pos_history;
        ZobristHash tmp_zh = zh;
        res = -process_check(board, tmp_zh, ctx, pos_history);
      } else {
        res = -evaluate_nnue(board, ctx);
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
  } //end of if (move_number == 1)
  else {
      if (board.isMate) {
          print("info depth 0 score mate 0\n");
          print("bestmove (none)\n");
      }
      else if (board.isStaleMate) {
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

void setEngineOptions() {
    strcpy(chessEngine.id, "Creatica Chess Engine 1.0");
    strcpy(chessEngine.authors, "Creatica");
    chessEngine.numberOfCheckOptions = 3;
	  chessEngine.numberOfComboOptions = 0;
	  chessEngine.numberOfSpinOptions = 10;
		chessEngine.numberOfStringOptions = 1;
		chessEngine.numberOfButtonOptions = 0;
	  strcpy(chessEngine.optionCheck[Ponder].name, "Ponder");
	  chessEngine.optionCheck[Ponder].defaultValue = PONDER;
	  chessEngine.optionCheck[Ponder].value = chessEngine.optionCheck[Ponder].defaultValue;
	  strcpy(chessEngine.optionCheck[FinalInfoLines].name, "FinalInfoLines");
	  chessEngine.optionCheck[FinalInfoLines].defaultValue = DISPLAY_FINAL_INFO_LINES;
	  chessEngine.optionCheck[FinalInfoLines].value = chessEngine.optionCheck[FinalInfoLines].defaultValue;
	  strcpy(chessEngine.optionCheck[IntermittentInfoLines].name, "IntermittentInfoLines");
	  chessEngine.optionCheck[IntermittentInfoLines].defaultValue = DISPLAY_INTERMITTENT_INFO_LINES;
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
	  //strcpy(chessEngine.optionSpin[ProbabilityMass].name, "ProbabilityMass");
	  //chessEngine.optionSpin[ProbabilityMass].defaultValue = PROBABILITY_MASS;
	  //chessEngine.optionSpin[ProbabilityMass].value = chessEngine.optionSpin[ProbabilityMass].defaultValue;
	  //chessEngine.optionSpin[ProbabilityMass].min = 1;
	  //chessEngine.optionSpin[ProbabilityMass].max = 100;
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
    //probability_mass = static_cast<double>(chessEngine.optionSpin[ProbabilityMass].value) * 0.01;
    //noise = static_cast<double>(chessEngine.optionSpin[Noise].value) * 0.01;
    virtual_loss = static_cast<double>(chessEngine.optionSpin[VirtualLoss].value);
    eval_scale = static_cast<double>(chessEngine.optionSpin[EvalScale].value);
    temperature = static_cast<double>(chessEngine.optionSpin[Temperature].value) * 0.01; //used in calculating probabilities for moves in softmax exp((eval - max_eval)/temperature) / eval_sum
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
    zobristHash(z);
    init_magic_bitboards();
    init_nnue("nn-1c0000000000.nnue", "nn-37f18f62d772.nnue");
    setEngineOptions();
    char fenString[MAX_FEN_STRING_LEN] = "";
    char uciMove[6] = "";
    uint64_t hash = 0;
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
  	if (fen2board(board, fenString)) {
		  printf("test_nnue error: fen2board() failed; FEN %s\n", fenString);
		  return 1;
	  }
	  getHash(zh, board, z);
    srand(time(NULL)); 
    
    runMCTS();
    bool move_given = (strlen(uciMove) > 0);
    int move_idx;
    Move move = {};
    if (move_given) {
      move_idx = uci2move_idx(uciMove, move);
      printf("searching for move %s (from %s to %s (promo %c), idx %d) in root's children...\n", uciMove, square[move.src], square[move.dst], uciPromoLetter[move.promoType], move_idx);
    }
    int num_children = search.root->num_children.load(std::memory_order_relaxed);
    Edge * children = search.root->children.load(std::memory_order_relaxed);
    std::vector<std::tuple<uint64_t, double, double, int, int>> move_evals; //N, W, P, move_idx, cp    
    
    for (int i = 0; i < num_children; i++) {
      int idx = children[i].move.load(std::memory_order_relaxed);
      double P = children[i].P.load(std::memory_order_relaxed);
      MCTSNode * child = children[i].child.load(std::memory_order_relaxed);
      double W = child->W.load(std::memory_order_relaxed);
      uint64_t N = child->N.load(std::memory_order_relaxed);
      int cp = child->cp.load(std::memory_order_relaxed);
      move_evals.push_back({N, W, P, idx, cp});
    }
    std::sort(move_evals.begin(), move_evals.end(), std::greater<>());
    for (const auto& move_eval : move_evals) {
      char uci_move[6];
      if (move_given && move_idx == std::get<3>(move_eval))
        printf("found child: %s, P %f, W %f, N %llu, Q %f, cp %d\n", idx2uci(std::get<3>(move_eval), uci_move), std::get<2>(move_eval), -std::get<1>(move_eval), std::get<0>(move_eval), -std::get<1>(move_eval) / std::get<0>(move_eval), -std::get<4>(move_eval));
      else 
        printf("move: %s, P %f, W %f, N %llu, Q %f, cp %d\n", idx2uci(std::get<3>(move_eval), uci_move), std::get<2>(move_eval), -std::get<1>(move_eval), std::get<0>(move_eval), -std::get<1>(move_eval) / std::get<0>(move_eval), -std::get<4>(move_eval));
    }
    if (move_given) {
      printf("searching for position from move %s (from %s to %s (promo %c), idx %d) in MCT...\n", uciMove, square[move.src], square[move.dst], uciPromoLetter[move.promoType], move_idx);
      updateHash(zh, board, move, ff_move(board, move), z); //do the move, update the hash
      auto it = search.tree.find(zh.hash);
      MCTSNode * node = (it != search.tree.end()) ? it->second : nullptr;
      if (node) {
          double W = node->W.load(std::memory_order_relaxed);
          uint64_t N = node->N.load(std::memory_order_relaxed);
          int cp = node->cp.load(std::memory_order_relaxed);
          printf("found node with W = %f, N = %llu, W/N = %f, cp = %d\n", W, N, N > 0 ? W / N : 0, cp);        
      } else printf("node not found\n");
    }
    if (hash) {
      printf("searching for position %llu in MCT...\n", hash);
      auto it = search.tree.find(hash);
      MCTSNode * node = (it != search.tree.end()) ? it->second : nullptr;
      if (node) {
          double W = node->W.load(std::memory_order_relaxed);
          uint64_t N = node->N.load(std::memory_order_relaxed);
          int cp = node->cp.load(std::memory_order_relaxed);
          printf("found node with W = %f, N = %llu, W/N = %f, cp = %d\n", W, N, N > 0 ? W / N : 0, cp);        
      } else printf("node not found\n");      
    }
    cleanup();
    cleanup_nnue();
    cleanup_magic_bitboards();
    return 0;
}
