// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -O2 -I /Users/ap/libchess -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess -o check_policy_export check_policy_export.cpp
//
// DOES THE ENGINE COMPUTE THE MODEL THAT WAS TRAINED?
//
// nnue_policy_train with EXPORT_WEIGHTS=<net> EXPORT_CHECK=<tsv> writes, for held-out positions,
// the TRAINER's logit for every legal move. This reads that file and computes the same scores
// through policy_net.h, following the steps eval_and_expand() takes: the legal list with
// promotions expanded four-fold, the legality term over the deduplicated rows, the spatial planes
// from that list, one forward, and policy_score() per move.
//
// Every score should match to rounding (~1e-4). A wrong plane, a flipped square, a misread tensor
// or a changed input scale would each show up here as a large difference on specific moves --
// rather than as a point of Top-k that looks like noise, which is how they would show up anywhere
// else.
//
//   ./check_policy_export <net.bin> <check.tsv>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <algorithm>
#include "nnue/nnue/nnue_accumulator.h"
#include "nnue/bitboard.h"
#include "libchess.h"
#include "policy_net.h"

struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;
};
void init_nnue();
int  nnue_features(const Board&, NNUEContext&, unsigned char*);
void init_nnue_context(NNUEContext&);
void accumulator_stack_reset(NNUEContext&);

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <net.bin> <check.tsv>\n", argv[0]); return 2; }
    Stockfish::Bitboards::init();
    Zobrist z; zobristHash(z);
    init_nnue();
    NNUEContext ctx; init_nnue_context(ctx);

    PolicyNet net; char err[512] = "";
    if (!policy_net_load(net, argv[1], err, sizeof err)) { std::fprintf(stderr, "FATAL: %s\n", err); return 2; }
    std::printf("net %s: out=%d wl=%d piece_indexed=%d spatial=%d", argv[1], net.out, net.has_wl,
                net.piece_indexed, net.spatial.loaded);
    if (net.spatial.loaded) std::printf(" (planes %d ch %d layers %d layout %d)", net.spatial.planes,
                                        net.spatial.ch, net.spatial.layers, net.spatial.layout);
    std::printf("\n");

    std::ifstream in(argv[2]);
    if (!in) { std::fprintf(stderr, "FATAL: cannot open %s\n", argv[2]); return 2; }

    PolicySpatialScratch scr;
    unsigned char feat[4096];
    float pctx[512];
    std::string line;
    long positions = 0, moves = 0, set_mismatch = 0, top1_disagree = 0, big = 0;
    double max_abs = 0.0, sum_abs = 0.0;
    std::string worst_fen; int worst_idx = -1; double worst_tr = 0, worst_en = 0;
    long mm_moves = 0, mm_top_changed = 0, mm_trainer_top_illegal = 0;
    double mm_exact_max = 0.0, mm_drift_max = 0.0, mm_drift_sum = 0.0;

    while (std::getline(in, line)) {
        const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos) continue;
        const std::string fen = line.substr(0, t1);
        std::map<int, double> trainer;
        { std::istringstream ss(line.substr(t2 + 1)); std::string tok;
          while (ss >> tok) { const size_t c = tok.find(':'); trainer[std::atoi(tok.c_str())] = std::atof(tok.c_str() + c + 1); } }

        Board board;
        if (fen2board(board, fen.c_str()) != 0) continue;
        const bool flip = (board.sideToMove == ColorBlack);
        accumulator_stack_reset(ctx);
        nnue_features(board, ctx, feat);
        policy_context(net, feat, pctx);

        // The legal list, generated the way eval_and_expand() generates it: king moves first, then
        // Queen..Pawn, promotions expanded Knight..Queen.
        std::vector<Move> legal;
        auto [kmoves, pinned, pinning, checkers, ksq] = kingMoves(board);
        Move move = {}; move.src = ksq; move.promoType = PieceTypeNone;
        for (uint64_t m = kmoves; m; m &= m - 1) { move.dst = lsBit(m); legal.push_back(move); }
        if (bitCount(checkers) <= 1) {
            auto [cm, em] = checkers ? checkMask(board, ksq, checkers) : std::make_pair(~0ULL, 0ULL);
            for (PieceType pt = Queen; pt >= Pawn; --pt) {
                uint64_t occ = board.side[board.sideToMove] & board.pieceTypes[pt - 1];
                while (occ) {
                    move.src = lsBit(occ);
                    uint64_t mv = piece_moves(board, pt, move.src, ksq, pinned, pinning, cm, em);
                    while (mv) {
                        move.dst = lsBit(mv);
                        PieceType s = PieceTypeNone, e = PieceTypeNone;
                        if (promoMove(board, move)) { s = Knight; e = Queen; }
                        for (PieceType q = s; q <= e; q = (PieceType)(q + 1)) { move.promoType = q; legal.push_back(move); }
                        move.promoType = PieceTypeNone;
                        mv &= mv - 1;
                    }
                    occ &= occ - 1;
                }
            }
        }
        if (legal.empty()) continue;

        if (net.has_wl) {
            std::vector<size_t> rows;
            for (const Move& m : legal) rows.push_back(policy_row(net, board.piecesOnSquares[m.src] & 7, m.src, m.dst, flip));
            std::sort(rows.begin(), rows.end());
            rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
            policy_apply_legal_bias(net, pctx, rows.data(), (int)rows.size());
        }
        const PolicySpatialScratch* sp = nullptr;
        if (net.spatial.loaded) {
            std::vector<int> f, t, p;
            for (const Move& m : legal) {
                f.push_back(flip ? (m.src ^ 56) : m.src);
                t.push_back(flip ? (m.dst ^ 56) : m.dst);
                p.push_back(board.piecesOnSquares[m.src] & 7);
            }
            unsigned char planes[26 * 64];
            policy_build_planes(board, net.spatial, f.data(), t.data(), p.data(), (int)legal.size(), planes);
            policy_spatial_forward(net.spatial, planes, scr);
            sp = &scr;
        }

        std::map<int, double> engine;
        for (const Move& m : legal) {
            const int pt = board.piecesOnSquares[m.src] & 7;
            const int idx = (int)policy_row(net, pt, m.src, m.dst, flip);
            engine[idx] = policy_score(net, pctx, pt, m.src, m.dst, flip, sp);
        }

        ++positions;
        bool same_set = engine.size() == trainer.size();
        if (same_set) for (auto& [k, v] : engine) if (!trainer.count(k)) { same_set = false; break; }
        if (!same_set) {
            ++set_mismatch;
            //Rescore from the TRAINER's list (which carries the illegal castling moves): rebuild the
            //legality term's rows and the planes from its indices, then score the moves both lists
            //share. If that reproduces the trainer to rounding, the list is the only difference.
            {
                float c2[512];
                policy_context(net, feat, c2);
                std::vector<size_t> rows; std::vector<int> f, t, p;
                for (auto& [k, v] : trainer) {
                    rows.push_back((size_t)k);
                    f.push_back((k / 64) % 64); t.push_back(k % 64); p.push_back(net.piece_indexed ? k / 4096 + 1 : 1);
                }
                if (net.has_wl) policy_apply_legal_bias(net, c2, rows.data(), (int)rows.size());
                PolicySpatialScratch scr2; const PolicySpatialScratch* sp2 = nullptr;
                if (net.spatial.loaded) {
                    unsigned char planes2[26 * 64];
                    policy_build_planes(board, net.spatial, f.data(), t.data(), p.data(), (int)f.size(), planes2);
                    policy_spatial_forward(net.spatial, planes2, scr2);
                    sp2 = &scr2;
                }
                int best_true = -1, best_tr = -1; double bt = -1e30, btr = -1e30;
                for (const Move& m : legal) {
                    const int pt = board.piecesOnSquares[m.src] & 7;
                    const int idx = (int)policy_row(net, pt, m.src, m.dst, flip);
                    if (!trainer.count(idx)) continue;
                    const double via_trainer_list = policy_score(net, c2, pt, m.src, m.dst, flip, sp2);
                    const double via_true_list    = engine[idx];
                    const double tr = trainer[idx];
                    mm_moves++;
                    mm_exact_max = std::max(mm_exact_max, std::fabs(via_trainer_list - tr));
                    mm_drift_max = std::max(mm_drift_max, std::fabs(via_true_list - tr));
                    mm_drift_sum += std::fabs(via_true_list - tr);
                    if (via_true_list > bt) { bt = via_true_list; best_true = idx; }
                }
                for (auto& [k, v] : trainer) if (v > btr) { btr = v; best_tr = k; }
                if (!engine.count(best_tr)) mm_trainer_top_illegal++;
                else if (best_true != best_tr) mm_top_changed++;
            }
            if (std::getenv("SHOW_MISMATCH") && set_mismatch <= std::atoi(std::getenv("SHOW_MISMATCH"))) {
                auto name = [&](int idx) {
                    static const char* P = "?PNBRQK";
                    const int pt = idx / 4096 + 1, fo = (idx / 64) % 64, to = idx % 64;
                    const int f = flip ? (fo ^ 56) : fo, t = flip ? (to ^ 56) : to;
                    char buf[16]; std::snprintf(buf, sizeof buf, "%c%c%d%c%d", P[pt], 'a' + f % 8, f / 8 + 1, 'a' + t % 8, t / 8 + 1);
                    return std::string(buf);
                };
                std::string only_e, only_t;
                for (auto& [k, v] : engine)  if (!trainer.count(k)) only_e += name(k) + " ";
                for (auto& [k, v] : trainer) if (!engine.count(k))  only_t += name(k) + " ";
                std::printf("MISMATCH %s\n   engine only: %s\n   trainer only: %s\n", fen.c_str(), only_e.c_str(), only_t.c_str());
            }
            continue;
        }

        int best_e = -1, best_t = -1; double be = -1e30, bt = -1e30;
        for (auto& [k, v] : engine) {
            const double tr = trainer[k], d = std::fabs(v - tr);
            ++moves; sum_abs += d;
            if (d > 1e-3) ++big;
            if (d > max_abs) { max_abs = d; worst_fen = fen; worst_idx = k; worst_tr = tr; worst_en = v; }
            if (v > be) { be = v; best_e = k; }
            if (tr > bt) { bt = tr; best_t = k; }
        }
        if (best_e != best_t) ++top1_disagree;
    }

    std::printf("positions %ld, moves %ld\n", positions, moves);
    std::printf("legal-move sets that differ between engine and trainer: %ld\n", set_mismatch);
    std::printf("score |engine - trainer|: max %.3g  mean %.3g  moves off by more than 1e-3: %ld\n",
                max_abs, moves ? sum_abs / moves : 0.0, big);
    std::printf("positions where engine and trainer pick a different top move: %ld\n", top1_disagree);
    if (set_mismatch) {
        std::printf("\nthe %ld positions whose legal lists differ (the trainer lists illegal castling moves):\n", set_mismatch);
        std::printf("  scored from the TRAINER's list:  max |engine - trainer| %.3g over %ld shared moves\n", mm_exact_max, mm_moves);
        std::printf("  scored from the TRUE list:       max |engine - trainer| %.3g, mean %.3g\n",
                    mm_drift_max, mm_moves ? mm_drift_sum / mm_moves : 0.0);
        std::printf("  top move changes because of the true list: %ld   trainer's top move was an illegal castle: %ld\n",
                    mm_top_changed, mm_trainer_top_illegal);
    }
    if (worst_idx >= 0)
        std::printf("worst: idx %d  trainer %.6f  engine %.6f  in %s\n", worst_idx, worst_tr, worst_en, worst_fen.c_str());
    return 0;
}
