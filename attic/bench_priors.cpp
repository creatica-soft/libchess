// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -O3 -I /Users/ap/libchess -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess -o bench_priors bench_priors.cpp
//
// What does ONE MCTS expansion currently cost in NNUE evaluations?
// This is the number a policy-head forward pass has to beat: compute_move_evals()
// plays every legal move and evaluates it, so the cost is (branching factor) x
// (do_move_dp + evaluate_nnue + undo_move).
#include <chrono>
#include <cstdio>
#include <string>
#include <fstream>
#include <vector>
#include "nnue/nnue/nnue_accumulator.h"
#include "libchess.h"

struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;
};
void init_nnue(); void cleanup_nnue();
void init_nnue_context(NNUEContext&); void free_nnue_context(NNUEContext&);
double evaluate_nnue(const Board&, NNUEContext&);
std::pair<Stockfish::DirtyPiece&, Stockfish::DirtyThreats&> accumulator_stack_push(NNUEContext&);
void accumulator_stack_pop(NNUEContext&);
void accumulator_stack_reset(NNUEContext&);

NNUEContext ctx; Board board; Zobrist z;

int main(int argc, char ** argv) {
  zobristHash(z); Stockfish::Bitboards::init(); init_nnue(); init_nnue_context(ctx);
  std::vector<std::string> fens;
  std::ifstream f(argc > 1 ? argv[1] : "test_fen_strings");
  for (std::string l; std::getline(f, l); ) if (!l.empty()) fens.push_back(l);

  long long total_moves = 0, expansions = 0; double total_s = 0;
  const int REPS = 20;
  for (const auto& fen : fens) {
    if (fen2board(board, fen.c_str())) continue;
    for (int r = 0; r < REPS; ++r) {
      accumulator_stack_reset(ctx);
      auto t0 = std::chrono::steady_clock::now();
      int n = 0;
      auto [kmoves, pinned, pinning, checkers, ksq] = kingMoves(board);
      Move move = {}; move.src = ksq; move.promoType = PieceTypeNone;
      uint64_t m = kmoves;
      while (m) { move.dst = lsBit(m); StateInfo st = {};
        auto [dp, dts] = accumulator_stack_push(ctx);
        do_move_dp(board, move, st, dp, dts);
        if (!board.isCheck) (void)evaluate_nnue(board, ctx);
        undo_move(board, move, st); accumulator_stack_pop(ctx); ++n; m &= m - 1; }
      if (bitCount(checkers) <= 1) {
        auto [cm, em] = checkers ? checkMask(board, ksq, checkers) : std::make_pair(~0ULL, 0ULL);
        for (PieceType pt = Queen; pt >= Pawn; --pt) {
          uint64_t occ = board.side[board.sideToMove] & board.pieceTypes[pt - 1];
          while (occ) { move.src = lsBit(occ);
            uint64_t mv = piece_moves(board, pt, move.src, ksq, pinned, pinning, cm, em);
            while (mv) { move.dst = lsBit(mv);
              PieceType s = PieceTypeNone, e = PieceTypeNone;
              if (promoMove(board, move)) { s = Knight; e = Queen; }
              for (PieceType p = s; p <= e; p = (PieceType)(p + 1)) { move.promoType = p;
                StateInfo st = {}; auto [dp, dts] = accumulator_stack_push(ctx);
                do_move_dp(board, move, st, dp, dts);
                if (!board.isCheck) (void)evaluate_nnue(board, ctx);
                undo_move(board, move, st); accumulator_stack_pop(ctx); ++n; }
              move.promoType = PieceTypeNone; mv &= mv - 1; }
            occ &= occ - 1; } } }
      total_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      total_moves += n; ++expansions;
    }
  }
  printf("positions          %zu   (x%d reps)\n", fens.size(), REPS);
  printf("expansions timed   %lld\n", expansions);
  printf("mean branching     %.1f moves/expansion\n", (double)total_moves / expansions);
  printf("cost per EXPANSION %.1f us      <-- the number a policy forward must beat\n", total_s / expansions * 1e6);
  printf("cost per MOVE      %.2f us\n", total_s / total_moves * 1e6);
  free_nnue_context(ctx); cleanup_nnue(); return 0;
}
