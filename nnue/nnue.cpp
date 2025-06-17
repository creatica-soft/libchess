// nnue.cpp
#include "types.h"
#include "position.h"
#include "evaluate.h"
#include "nnue/nnue_common.h"
#include "nnue/network.h"
#include "nnue/nnue_accumulator.h"
#include "nnue/nnue_architecture.h"
#include "nnue/features/half_ka_v2_hm.h"
#include <cstdlib>
#include <pthread.h>
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

void libchess_init_nnue(const char * nnue_file_big = EvalFileDefaultNameBig, const char * nnue_file_small = EvalFileDefaultNameSmall);
void libchess_cleanup_nnue();
void libchess_init_nnue_context(struct NNUEContext * ctx);
void libchess_free_nnue_context(struct NNUEContext * ctx);
float libchess_evaluate_nnue(struct Board * board, struct NNUEContext * ctx);
float libchess_evaluate_nnue_incremental(struct Board * board, struct Move * move, struct NNUEContext * ctx);

void libchess_to_position(const struct Board * board, Stockfish::Position * pos, Stockfish::StateInfo * state) {
  if (board && board->fen) {
      pos->set(board->fen->fenString, board->fen->isChess960, state);
  } else {
      pos->set(startPos, false, state);
  }
}

Stockfish::Move libchess_move_to_stockfish(struct Move * move) {
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

void libchess_init_nnue(const char * nnue_file_big, const char * nnue_file_small) {
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

void libchess_cleanup_nnue() {
    delete networks;
    networks = nullptr;
}

void libchess_init_nnue_context(struct NNUEContext * ctx) {
    ctx->pos = new Stockfish::Position();
    ctx->state = new Stockfish::StateInfo();
    ctx->accumulator_stack = new Stockfish::Eval::NNUE::AccumulatorStack();
    ctx->caches = new Stockfish::Eval::NNUE::AccumulatorCaches(*networks);
}

void libchess_free_nnue_context(struct NNUEContext * ctx) {
    delete ctx->pos;
    delete ctx->state;
    delete ctx->accumulator_stack;
    delete ctx->caches;
    ctx->pos = nullptr;
    ctx->state = nullptr;
    ctx->accumulator_stack = nullptr;
    ctx->caches = nullptr;
}

float libchess_evaluate_nnue(struct Board * board, struct NNUEContext * ctx) {
    if (!board) return 0;
    libchess_to_position(board, ctx->pos, ctx->state);
    if (!ctx->pos->square<Stockfish::KING>(Stockfish::WHITE) || !ctx->pos->square<Stockfish::KING>(Stockfish::BLACK) || ctx->pos->checkers()) return 0;
    ctx->accumulator_stack->reset();
    Stockfish::Value v = Stockfish::Eval::evaluate(*networks, *ctx->pos, *ctx->accumulator_stack, *ctx->caches, 0);
    v = ctx->pos->side_to_move() == Stockfish::WHITE ? v : -v;
    return 0.01 * Stockfish::to_cp(v, *ctx->pos);
}

float libchess_evaluate_nnue_incremental(struct Board * board, struct Move * move, struct NNUEContext * ctx) {
    if (!board) return 0;
    libchess_to_position(board, ctx->pos, ctx->state);
    if (!ctx->pos->square<Stockfish::KING>(Stockfish::WHITE) || !ctx->pos->square<Stockfish::KING>(Stockfish::BLACK)) return 0;
    ctx->accumulator_stack->reset();
    Stockfish::Move sf_move = libchess_move_to_stockfish(move);
    Stockfish::StateInfo * new_state = new Stockfish::StateInfo();
    Stockfish::DirtyPiece dp = ctx->pos->do_move(sf_move, *new_state, ctx->pos->gives_check(sf_move));
    delete ctx->state; // Free previous state
    ctx->state = new_state; // Update state
    if (ctx->pos->checkers()) return 0;
    ctx->accumulator_stack->push(dp);
    Stockfish::Value v = Stockfish::Eval::evaluate(*networks, *ctx->pos, *ctx->accumulator_stack, *ctx->caches, 0);
    ctx->accumulator_stack->pop();
    v = ctx->pos->side_to_move() == Stockfish::WHITE ? v : -v;
    return 0.01 * Stockfish::to_cp(v, *ctx->pos);
}

#ifdef __cplusplus
}
#endif