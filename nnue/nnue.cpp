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
//#include <pthread.h>
#include <fstream>
#include <iostream>
#include <string>
#include "../libchess.h"

#ifdef __cplusplus
extern "C" {
#endif

struct NNUEContext {
    Stockfish::StateInfo * state;
    Stockfish::Position * pos;
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;    
};

CHESS_API void init_nnue(const char * nnue_file_big = EvalFileDefaultNameBig, const char * nnue_file_small = EvalFileDefaultNameSmall);
CHESS_API void cleanup_nnue();
CHESS_API void init_nnue_context(struct NNUEContext * ctx);
CHESS_API void free_nnue_context(struct NNUEContext * ctx);
CHESS_API double evaluate_nnue(struct Board * board, struct Move * move, struct NNUEContext * ctx);
//CHESS_API double evaluate_nnue_incremental(struct Board * board, struct Move * move, struct NNUEContext * ctx);

void boardtopos(const struct Board * board, Stockfish::Position * pos, Stockfish::StateInfo * state) {
  if (board && board->fen) {
      //state will be zeroed in pos->set()
      pos->set(board->fen->fenString, board->fen->isChess960, state);
  } else {
      pos->set(startPos, false, state);
  }
}

Stockfish::Move movetomove(struct Move * move) {
    Stockfish::Square from = (Stockfish::Square)move->sourceSquare.name;
    Stockfish::Square to = (Stockfish::Square)move->destinationSquare.name;
    Stockfish::PieceType pt;
    int rookRank, rookSquare;
    if (move->type & 16) { // MoveTypePromotion = 16
        pt = (Stockfish::PieceType)(move->chessBoard->promoPiece & 7);
        return Stockfish::Move::make<Stockfish::PROMOTION>(from, to, pt);
    } else if ((move->type & 34) == 34) { // MoveTypeEnPassant = 32 | MoveTypeCapture = 2
        return Stockfish::Move::make<Stockfish::EN_PASSANT>(from, to);
    } else if (move->type & 12) { // CastlingSideKingside = 4 | CastlingSideQueenside = 8
      rookRank = move->chessBoard->fen->sideToMove == 0 ? 0 : 7; 
      rookSquare = 8 * rookRank + move->chessBoard->fen->castlingRook[((move->type & 12) >> 2) - 1][move->chessBoard->fen->sideToMove];
      to = (Stockfish::Square)rookSquare;
      return Stockfish::Move::make<Stockfish::CASTLING>(from, to);
    }
    return Stockfish::Move::make<Stockfish::NORMAL>(from, to);
}

static Stockfish::Eval::NNUE::Networks * networks = nullptr;

void init_nnue(const char * nnue_file_big, const char * nnue_file_small) {
    Stockfish::Bitboards::init();
    Stockfish::Position::init();
    if (!networks) {
        Stockfish::Eval::NNUE::NetworkBig big(Stockfish::Eval::NNUE::EvalFile{}, Stockfish::Eval::NNUE::EmbeddedNNUEType::BIG);
        Stockfish::Eval::NNUE::NetworkSmall small(Stockfish::Eval::NNUE::EvalFile{}, Stockfish::Eval::NNUE::EmbeddedNNUEType::SMALL);
        big.load("", nnue_file_big);
        small.load("", nnue_file_small);
        networks = new Stockfish::Eval::NNUE::Networks(std::move(big), std::move(small));
    }
}

void cleanup_nnue() {
    delete networks;
    networks = nullptr;
}

void init_nnue_context(struct NNUEContext * ctx) {
    ctx->pos = new Stockfish::Position();
    ctx->state = new Stockfish::StateInfo();
    ctx->state->previous = nullptr;
    ctx->accumulator_stack = new Stockfish::Eval::NNUE::AccumulatorStack();
    ctx->caches = new Stockfish::Eval::NNUE::AccumulatorCaches(*networks);
}

void free_nnue_context(struct NNUEContext * ctx) {
    while (ctx->state) {
      Stockfish::StateInfo * state = ctx->state->previous;
      delete ctx->state;
      ctx->state = state;
    }
    delete ctx->pos;
    delete ctx->caches;
    delete ctx->accumulator_stack;
    ctx->pos = nullptr;
    ctx->state = nullptr;
    ctx->accumulator_stack = nullptr;
    ctx->caches = nullptr;
}
//move is optional, could be NULL but if provided, the function returns eval for the move, not for the board
//it should first be called without a move to reset accumulators
//subsequent moves forward and backward will cause NUUE updated incrementally (faster) 
//from the initial position given by board
//this is to evaluate various moves from a single position
//to evaluate a game, after calling this function with startpos, then call evaluate_nnue_incremental() for subsequent moves
double evaluate_nnue(struct Board * board, struct Move * move, struct NNUEContext * ctx) {
    if (!board) return 0;
    if (!move && board->isCheck) return NNUE_CHECK; //special value for checks
    //state will be zeroed in stockfish
    boardtopos(board, ctx->pos, ctx->state);
    Stockfish::StateInfo * new_state;
    Stockfish::Value v;
    if (move) {
      Stockfish::Move sf_move = movetomove(move);
      new_state = new Stockfish::StateInfo();
      Stockfish::DirtyPiece dp = ctx->pos->do_move(sf_move, *new_state, ctx->pos->gives_check(sf_move));
      if (ctx->pos->checkers()) {
        delete new_state;
        return NNUE_CHECK; //special value for checks
      }
      new_state->previous = ctx->state; // Preserve previous state
      ctx->state = new_state; // Update state
      ctx->accumulator_stack->push(dp);
      //here we negate the result of evaluate() because the move is made and perspective is changed to opponent
      v = -Stockfish::Eval::evaluate(*networks, *ctx->pos, *ctx->accumulator_stack, *ctx->caches, 0);
      ctx->accumulator_stack->pop();
      //restore previous state
      new_state = ctx->state->previous;
      new_state->previous = nullptr;
      delete ctx->state;
      ctx->state = new_state;
    } else {
      ctx->accumulator_stack->reset();
      v = Stockfish::Eval::evaluate(*networks, *ctx->pos, *ctx->accumulator_stack, *ctx->caches, 0);      
    }
    //v = ctx->pos->side_to_move() == Stockfish::WHITE ? v : -v; //keep it from the perspective of the side to move
    return 0.01 * Stockfish::to_cp(v, *ctx->pos);
}
/*
double evaluate_nnue_incremental(struct Board * board, struct Move * move, struct NNUEContext * ctx) {
    if (!board) return 0;
    Stockfish::Move sf_move = movetomove(move);
    Stockfish::StateInfo * new_state = new Stockfish::StateInfo();
    Stockfish::DirtyPiece dp = ctx->pos->do_move(sf_move, *new_state, ctx->pos->gives_check(sf_move));
    new_state->previous = ctx->state; // Preserve previous state
    ctx->state = new_state; // Update state
    ctx->accumulator_stack->push(dp);
    if (ctx->pos->checkers()) return NNUE_CHECK;
    Stockfish::Value v = Stockfish::Eval::evaluate(*networks, *ctx->pos, *ctx->accumulator_stack, *ctx->caches, 0);
    //v = ctx->pos->side_to_move() == Stockfish::WHITE ? v : -v; //keep it from the perspective of the side to move
    return 0.01 * Stockfish::to_cp(v, *ctx->pos);
}
*/
#ifdef __cplusplus
}
#endif