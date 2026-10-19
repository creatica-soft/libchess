// nnue.cpp -- the wrapper the rest of libchess uses to reach the NNUE evaluation.
//
// This file is written for this project; it has no upstream Stockfish counterpart.
// It is nevertheless part of the GPL-3 work in this directory, because it is only
// usable as part of the Stockfish-derived evaluation it wraps.
//
// Copyright (C) 2025-2026 creatica-soft
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See ../COPYING for the full text, and ../THIRD-PARTY-NOTICES.md for
// how this directory relates to the MIT-licensed sources above it.

#if defined(_WIN32) || defined(__CYGWIN__)
  #define CHESS_API __declspec(dllexport)
#else
  #define CHESS_API
#endif

#include "types.h"
//#include "position.h" //not used: Stockfish::Position is never referenced here,
                        //and this was the last thing pulling Stockfish::StateInfo
                        //into a TU that also has libchess's own StateInfo in scope.
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
CHESS_API double evaluate_nnue(const Board& board, NNUEContext& ctx, bool in_check);
CHESS_API double evaluate_nnue_opt(const Board& board, NNUEContext& ctx, bool in_check, int optimism);

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
//`in_check` is passed IN, not read off the Board. It used to test board.isCheck, a cache that only
//kingMoves() writes -- so it fired on positions that were not in check and stayed false for ones
//that were. The caller always has the truth already: kingMoves() returns `checkers`, and
//isCheckMateStaleMate() returns a BoardState. See the note on BoardState in libchess.h.
//
//The sentinel is kept rather than made a precondition, so that a caller which passes the wrong
//thing gets NNUE_CHECK -- a value every existing caller already tests for -- instead of a plausible
//evaluation of a position whose king is under attack.
double evaluate_nnue(const Board& board, NNUEContext& ctx, bool in_check) {
    return evaluate_nnue_opt(board, ctx, in_check, 0);
}

double evaluate_nnue_opt(const Board& board, NNUEContext& ctx, bool in_check, int optimism) {
    if (in_check) return 0.00001; //special value for checks
    Stockfish::Value v;
    v = Stockfish::Eval::evaluate(*networks, board, *ctx.accumulator_stack, *ctx.caches, optimism);
    //comment out next line to keep it from the perspective of the side to move
    //v = ctx.pos->side_to_move() == Stockfish::WHITE ? v : -v; 
    return 0.01 * Stockfish::Eval::to_cp(v, board);
}

//Size of the NNUE feature vector: the feature transformer's post-activation output, the
//same buffer the value network's first affine layer reads. 1024 bytes for the big net.
int nnue_feature_dims() {
    return (int)Stockfish::Eval::NNUE::FeatureTransformer<
                    Stockfish::Eval::NNUE::TransformedFeatureDimensionsBig>::BufferSize;
}

//Write that vector for `board` into `out`, which must hold nnue_feature_dims() bytes.
//This is the representation a policy head would train on: it is maintained incrementally
//by the accumulator stack, so at a search node it is already computed and free to read --
//which is what lets a SINGLE position be scored without assembling a GPU batch.
int nnue_features(const Board& board, NNUEContext& ctx, unsigned char* out) {
    if (!networks || !ctx.accumulator_stack || !ctx.caches) return 0;
    return (int)networks->big.transform_features(board, *ctx.accumulator_stack,
                                                 ctx.caches->big, out);
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
//`in_check` for the same reason as evaluate_nnue(): the caller knows, the Board's cache does not.
std::string nnue_eval(Board& board, bool in_check) {
    return Stockfish::Eval::trace(board, *networks, in_check);
}
