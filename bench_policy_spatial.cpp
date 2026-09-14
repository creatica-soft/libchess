// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -O3 -march=native -I /Users/ap/libchess -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess -o bench_policy_spatial bench_policy_spatial.cpp
//
// WHAT DOES THE SPATIAL TERM COST THE ENGINE?
//
// The spatial policy model wins about 3 points of top-6 agreement offline. That is only half the
// question: the engine pays for it once per EXPANDED NODE, and creatica's strength comes from the
// number of nodes it can put in the tree. A model that is 3 points better and halves the node rate
// is a loss. This measures the two sides in the same units -- nanoseconds per expanded node -- so
// they can actually be compared.
//
// It times, per position, exactly the work eval_and_expand() does:
//
//   nnue_features     the feature-transformer output, 1024 bytes           (already paid today)
//   policy_context    relu(relu(x/127 @ W1 + b1) @ W2 + b2)                (already paid today)
//   legality + score  Wl term plus one dot product per legal move          (already paid today)
//   ---- the addition ----
//   build_planes      26 binary 8x8 planes; about 32 attacks_bb lookups
//   spatial_forward   two 3x3 convolutions, 26->32->32 channels, then u and v
//   spatial_score     one 16-wide dot product per legal move
//
// and, as the denominator, the leaf evaluation itself:
//
//   evaluate_nnue     what an expansion is really spending its time on
//
//   ./bench_policy_spatial [fen_file] [repeats]
//
// Every stage is timed over the whole position set at once and divided, so a single stage's
// measurement is not dominated by clock overhead. Stages run in a rotating order across repeats so
// that no one stage always pays the cache miss on the position.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <vector>
#include <chrono>
#include <algorithm>
#include "nnue/nnue/nnue_accumulator.h"
#include "libchess.h"
#include "policy_net.h"

// creatica_search.hpp defines these; this benchmark does not include the engine header.
#define POLICY_MAX_IN 2048
#define POLICY_MAX_H2 512

static PolicyNet      pnet;
static PolicySpatial  psp;
static PolicySpatialScratch pscr;

struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;
};
void   init_nnue();
int    nnue_features(const Board&, NNUEContext&, unsigned char*);
void   init_nnue_context(NNUEContext&);
void   accumulator_stack_reset(NNUEContext&);
double evaluate_nnue(const Board&, NNUEContext&);

static NNUEContext ctx;
static Zobrist z;

typedef std::chrono::high_resolution_clock clk;
static double ns(clk::time_point a, clk::time_point b) {
    return (double)std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}

// One position's precomputed move list, in the two forms the two scorers want.
struct Pos {
    Board board;
    std::vector<int>    src, dst, pt;
    std::vector<size_t> rows;     // policy embedding rows, for the legality term
    std::vector<size_t> oriented; // from*64+to with Black mirrored, for u/v
    std::vector<int>    from, to, ptv; // the same, split, plus the piece type -- policy_build_planes()
    unsigned char       feat[POLICY_MAX_IN];
};

static void enumerate(Pos& p) {
    Board& board = p.board;
    const Color us = board.sideToMove;
    const bool flip = (us == ColorBlack);
    auto [kmoves, pinned, pinning, checkers, ksq] = kingMoves(board);
    Move move = {}; move.src = ksq; move.promoType = PieceTypeNone;
    auto emit = [&](Move& m, PieceType t) {
        p.src.push_back(m.src); p.dst.push_back(m.dst); p.pt.push_back((int)t);
        p.rows.push_back(policy_row(pnet, (int)t, m.src, m.dst, flip));
        const int f = flip ? (m.src ^ 56) : m.src, d = flip ? (m.dst ^ 56) : m.dst;
        p.oriented.push_back((size_t)f * 64 + d);
        p.from.push_back(f); p.to.push_back(d); p.ptv.push_back((int)t);
    };
    for (uint64_t m = kmoves; m; m &= m - 1) { move.dst = lsBit(m); emit(move, King); }
    if (bitCount(checkers) > 1) return;
    auto [cm, em] = checkers ? checkMask(board, ksq, checkers) : std::make_pair(~0ULL, 0ULL);
    for (PieceType t = Queen; t >= Pawn; --t) {
        uint64_t occ = board.side[us] & board.pieceTypes[t - 1];
        while (occ) {
            move.src = lsBit(occ);
            uint64_t mv = piece_moves(board, t, move.src, ksq, pinned, pinning, cm, em);
            while (mv) {
                move.dst = lsBit(mv);
                PieceType s = PieceTypeNone, e = PieceTypeNone;
                if (promoMove(board, move)) { s = Knight; e = Queen; }
                for (PieceType q = s; q <= e; q = (PieceType)(q + 1)) { move.promoType = q; emit(move, t); }
                move.promoType = PieceTypeNone;
                mv &= mv - 1;
            }
            occ &= occ - 1;
        }
    }
}

// Weight VALUES do not change the cost, only their pattern of zeros does -- and the only
// zero-dependent skip in the forward is `if (wk == 0.0f) continue`, which a trained weight never
// triggers. So random non-zero weights time exactly like trained ones, and this benchmark does not
// need a trained spatial net to exist yet.
static void fill_random(std::vector<float>& v, size_t n, unsigned& s) {
    v.resize(n);
    for (size_t i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        v[i] = ((float)(s >> 8) / (float)(1u << 24) - 0.5f) * 0.2f;
    }
}

int main(int argc, char ** argv) {
    const char* fenfile = argc > 1 ? argv[1] : "book_fens_50.txt";
    const int   reps    = argc > 2 ? std::atoi(argv[2]) : 200;

    Stockfish::Bitboards::init();
    zobristHash(z);
    init_nnue();
    init_nnue_context(ctx);

    const char* wp = std::getenv("POLICY_WEIGHTS") ? std::getenv("POLICY_WEIGHTS") : "nnue_policy_pi.bin";
    char err[512];
    if (!policy_net_load(pnet, wp, err, sizeof err)) { std::fprintf(stderr, "FATAL: %s\n", err); return 2; }

    psp.planes = 26; psp.dim = 16;
    psp.ch = std::getenv("SPATIAL_CH") ? std::atoi(std::getenv("SPATIAL_CH")) : 32;
    if (psp.ch < 1) psp.ch = 1; else if (psp.ch > 64) psp.ch = 64;   // PolicySpatialScratch holds 64
    unsigned seed = 12345;
    fill_random(psp.Wc1, (size_t)psp.ch * psp.planes * 9, seed);
    fill_random(psp.bc1, psp.ch, seed);
    fill_random(psp.Wc2, (size_t)psp.ch * psp.ch * 9, seed);
    fill_random(psp.bc2, psp.ch, seed);
    fill_random(psp.Wu, (size_t)psp.ch * psp.dim, seed);
    fill_random(psp.Wv, (size_t)psp.ch * psp.dim, seed);
    psp.loaded = true;

    std::vector<Pos> pos;
    std::ifstream in(fenfile);
    if (!in) { std::fprintf(stderr, "FATAL: cannot open %s\n", fenfile); return 2; }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        Pos p;
        if (fen2board(p.board, line.c_str()) != 0) continue;
        accumulator_stack_reset(ctx);
        nnue_features(p.board, ctx, p.feat);
        enumerate(p);
        if (p.src.empty()) continue;
        pos.push_back(std::move(p));
    }
    if (pos.empty()) { std::fprintf(stderr, "FATAL: no usable positions in %s\n", fenfile); return 2; }

    size_t nmoves = 0;
    for (const auto& p : pos) nmoves += p.src.size();
    const double avg_moves = (double)nmoves / (double)pos.size();

    const int NST = 7;
    double tot[NST] = {0,0,0,0,0,0,0};
    const char* name[NST] = { "evaluate_nnue", "nnue_features", "policy_context",
                              "legality+score", "build_planes", "spatial_forward", "spatial_score" };
    volatile double sink = 0;
    float pctx[POLICY_MAX_H2];
    unsigned char planes[26 * 64];

    for (int r = 0; r < reps; ++r) {
      // Rotate which stage goes first, so no single stage always eats the cold-cache penalty.
      for (int o = 0; o < NST; ++o) {
        const int stage = (o + r) % NST;
        clk::time_point t0 = clk::now();
        switch (stage) {
        case 0: for (auto& p : pos) { accumulator_stack_reset(ctx); sink += evaluate_nnue(p.board, ctx); } break;
        case 1: for (auto& p : pos) { accumulator_stack_reset(ctx); sink += nnue_features(p.board, ctx, p.feat); } break;
        case 2: for (auto& p : pos) { policy_context(pnet, p.feat, pctx); sink += pctx[0]; } break;
        case 3: for (auto& p : pos) {
                    policy_context(pnet, p.feat, pctx);
                    policy_apply_legal_bias(pnet, pctx, p.rows.data(), (int)p.rows.size());
                    const bool flip = (p.board.sideToMove == ColorBlack);
                    for (size_t i = 0; i < p.src.size(); ++i)
                        sink += policy_score(pnet, pctx, p.pt[i], p.src[i], p.dst[i], flip);
                } break;
        case 4: for (auto& p : pos) {
                    policy_build_planes(p.board, psp, p.from.data(), p.to.data(), p.ptv.data(), (int)p.from.size(), planes);
                    sink += planes[0];
                } break;
        case 5: for (auto& p : pos) {
                    policy_build_planes(p.board, psp, p.from.data(), p.to.data(), p.ptv.data(), (int)p.from.size(), planes);
                    policy_spatial_forward(psp, planes, pscr);
                    sink += pscr.u[0];
                } break;
        case 6: for (auto& p : pos) {
                    for (size_t i = 0; i < p.oriented.size(); ++i)
                        sink += policy_spatial_score(psp, pscr, (int)(p.oriented[i] >> 6),
                                                                (int)(p.oriented[i] & 63));
                } break;
        }
        tot[stage] += ns(t0, clk::now());
      }
    }

    const double N = (double)pos.size() * (double)reps;
    // Stage 3 includes stage 2's work and stage 5 includes stage 4's; report the increments.
    const double t_eval   = tot[0] / N;
    const double t_feat   = tot[1] / N;
    const double t_ctx    = tot[2] / N;
    const double t_score  = tot[3] / N - t_ctx;
    const double t_planes = tot[4] / N;
    const double t_conv   = tot[5] / N - t_planes;
    const double t_sscore = tot[6] / N;

    const double now_total = t_feat + t_ctx + t_score;
    const double add_total = t_planes + t_conv + t_sscore;

    std::printf("\n%zu positions from %s, %d repeats, %.1f legal moves per position\n",
                pos.size(), fenfile, reps, avg_moves);
    std::printf("policy net: %s  (in=%d h1=%d h2=%d out=%d, legality term %s)\n",
                wp, pnet.in, pnet.h1, pnet.h2, pnet.out, pnet.has_wl ? "on" : "off");
    std::printf("spatial:    planes=%d ch=%d dim=%d\n\n", psp.planes, psp.ch, psp.dim);

    std::printf("  %-18s %10s\n", "stage", "ns/node");
    std::printf("  %-18s %10.1f   <- the leaf evaluation, for scale\n", name[0], t_eval);
    std::printf("  ---- policy as it ships today ----\n");
    std::printf("  %-18s %10.1f\n", name[1], t_feat);
    std::printf("  %-18s %10.1f\n", name[2], t_ctx);
    std::printf("  %-18s %10.1f\n", name[3], t_score);
    std::printf("  %-18s %10.1f\n", "= policy today", now_total);
    std::printf("  ---- what the spatial term adds ----\n");
    std::printf("  %-18s %10.1f\n", name[4], t_planes);
    std::printf("  %-18s %10.1f\n", name[5], t_conv);
    std::printf("  %-18s %10.1f\n", name[6], t_sscore);
    std::printf("  %-18s %10.1f\n\n", "= added", add_total);

    // An expansion pays the leaf evaluation and the policy. Everything else in eval_and_expand --
    // move generation, the child hashes, the softmax -- is not measured here, so this ratio is an
    // UPPER bound on the slowdown: the true denominator is larger than eval + policy.
    const double before = t_eval + now_total;
    const double after  = before + add_total;
    std::printf("  policy is %.1f%% of (leaf eval + policy) today, %.1f%% with the spatial term\n",
                100.0 * now_total / before, 100.0 * (now_total + add_total) / after);
    std::printf("  node rate would fall to at worst %.1f%% of today's (%.2fx slower)\n",
                100.0 * before / after, after / before);
    std::printf("  (upper bound: move generation and the rest of expansion are not in the denominator)\n\n");
    if (sink == 1234.5) std::printf("");
    return 0;
}
