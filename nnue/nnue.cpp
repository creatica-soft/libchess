// nnue.cpp

#if defined(_WIN32) || defined(__CYGWIN__)
  #define CHESS_API __declspec(dllexport)
#else
  #define CHESS_API
#endif

#include "types.h"
#include "position.h"
#include "evaluate.h"
#include "nnue/nnue_common.h"
#include "nnue/network.h"
#include "nnue/nnue_accumulator.h"
#include "nnue/nnue_architecture.h"
#include "nnue/features/half_ka_v2_hm.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
//#include "../libchess.h"

struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack = nullptr;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches = nullptr;    
};

CHESS_API void init_nnue(const char * nnue_file_big = EvalFileDefaultNameBig, const char * nnue_file_small = EvalFileDefaultNameSmall);
CHESS_API void cleanup_nnue();
CHESS_API void init_nnue_context(NNUEContext& ctx);
CHESS_API void free_nnue_context(NNUEContext& ctx);
CHESS_API double evaluate_nnue(Board& board, NNUEContext& ctx);

static Stockfish::Eval::NNUE::Networks * networks = nullptr;

//void init_nnue(const char * nnue_file_big, const char * nnue_file_small) {
void init_nnue() {
    //Stockfish::Bitboards::init();
    //Stockfish::Position::init();
    if (!networks) {
        //Stockfish::Eval::NNUE::NetworkBig big(Stockfish::Eval::NNUE::EvalFile{}, Stockfish::Eval::NNUE::EmbeddedNNUEType::BIG);
        //Stockfish::Eval::NNUE::NetworkSmall small(Stockfish::Eval::NNUE::EvalFile{}, Stockfish::Eval::NNUE::EmbeddedNNUEType::SMALL);
        //big.load("", nnue_file_big);
        //small.load("", nnue_file_small);
        //networks = new Stockfish::Eval::NNUE::Networks(std::move(big), std::move(small));
        networks = new Stockfish::Eval::NNUE::Networks(Stockfish::Eval::NNUE::EvalFile({EvalFileDefaultNameBig, "None", ""}), Stockfish::Eval::NNUE::EvalFile({EvalFileDefaultNameSmall, "None", ""}));
        networks->big.load("", "");
        networks->small.load("", "");
    }
}

void cleanup_nnue() {
    delete networks;
    networks = nullptr;
}

void init_nnue_context(NNUEContext& ctx) {
    ctx.accumulator_stack = new Stockfish::Eval::NNUE::AccumulatorStack();
    ctx.caches = new Stockfish::Eval::NNUE::AccumulatorCaches(*networks);
}

void free_nnue_context(NNUEContext& ctx) {
    delete ctx.caches;
    delete ctx.accumulator_stack;
    ctx.accumulator_stack = nullptr;
    ctx.caches = nullptr;
}
double evaluate_nnue(const Board& board, NNUEContext& ctx) {
    if (board.isCheck) return 0.00001; //special value for checks
    Stockfish::Value v;
    v = Stockfish::Eval::evaluate(*networks, board, *ctx.accumulator_stack, *ctx.caches, 0);
    //comment out next line to keep it from the perspective of the side to move
    //v = ctx.pos->side_to_move() == Stockfish::WHITE ? v : -v; 
    return 0.01 * Stockfish::Eval::to_cp(v, board);
}

std::pair<Stockfish::DirtyPiece&, Stockfish::DirtyThreats&> accumulator_stack_push(NNUEContext& ctx) {
  auto [dirtyPiece, dirtyThreats] = ctx.accumulator_stack->push();
  return {dirtyPiece, dirtyThreats};
}
void accumulator_stack_pop(NNUEContext& ctx) {
  ctx.accumulator_stack->pop();
}
void accumulator_stack_reset(NNUEContext& ctx) {
  ctx.accumulator_stack->reset();
}
std::string nnue_eval(Board& board) {
    return Stockfish::Eval::trace(board, *networks);
}
