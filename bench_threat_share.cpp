// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -O3 -I /Users/ap/libchess -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess -o bench_threat_share bench_threat_share.cpp
//
// HOW MUCH OF THE POLICY'S OPINION IS JUST THE THREAT FEATURES TALKING?
//
// The policy head is
//     ctx      = relu(relu(x/127 @ W1 + b1) @ W2 + b2)     once per position
//     score(m) = ctx . emb[from*64 + to]                   once per legal move
// where x is the NNUE feature transformer's output. That transformer's inputs are
// HalfKAv2_hm (king-bucketed piece placement) and FullThreats. A FullThreats feature is
// exactly (attacker, from, to, attacked) with `to` OCCUPIED -- see
// nnue/nnue/features/full_threats.cpp, where every attack bitboard is masked with
// `& occupied` before an index is emitted.
//
// So the only move-shaped information in the policy's input is the set of attacks onto
// occupied squares. Among LEGAL moves those are precisely the captures. Every quiet move
// exists in the output table (emb has a row per from*64+to) but has no corresponding input
// feature: the model can only infer it from how the placement embedding changes.
//
// This measures the consequence:
//   * what share of the policy's top-1 and top-6 are capture (threat-represented) moves,
//     against the share of ALL legal moves that are captures -- the base rate;
//   * how often the search's own preferred move lands in the policy's top-6, split by
//     whether that move is a capture or quiet. If the policy is leaning on the threat
//     features, it should find captures far more reliably than quiet moves.
//
// Input is the TSV written by VisitDumpFile: tag, fen, simulations, rootQ, rootCP, ponder,
// seldepth, visits, where visits is "move:visits:prior ..." in descending visit order, so
// the first entry is the search's own best move.
//
//   ./bench_threat_share bot1_visits.tsv [max_positions]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include "nnue/nnue/nnue_accumulator.h"
#include "libchess.h"
#include "policy_net.h"

static PolicyNet   pnet;
static float       pctx[512];
static std::vector<unsigned char> pfeat;

struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;
};
void init_nnue();
int  nnue_feature_dims();
int  nnue_features(const Board&, NNUEContext&, unsigned char*);
void init_nnue_context(NNUEContext&);
void accumulator_stack_reset(NNUEContext&);

NNUEContext ctx; Board board; Zobrist z;

struct Cand { float score; int src, dst, promo; bool capture; bool ep; };

// A move is "threat-represented" when its destination holds an enemy piece, because that is
// the one case FullThreats emits an index for. En passant is deliberately counted apart: it
// captures, but the destination square is EMPTY, so it carries no threat feature either.
static void enumerate(std::vector<Cand>& out) {
    out.clear();
    const Color us = board.sideToMove;
    const uint64_t them = board.side[1 - us];
    const int epFile = board.enPassant;
    auto [kmoves, pinned, pinning, checkers, ksq] = kingMoves(board);
    Move move = {}; move.src = ksq; move.promoType = PieceTypeNone;
    auto emit = [&](Move& m, PieceType pt) {
        const bool cap = (them >> m.dst) & 1ULL;
        const bool ep  = !cap && pt == Pawn && epFile != -1
                       && (m.dst & 7) == epFile && (m.src & 7) != (m.dst & 7);
        out.push_back({ policy_score(pnet, pctx, board.piecesOnSquares[m.src] & 7,
                                     m.src, m.dst, us == ColorBlack),
                        m.src, m.dst, m.promoType, cap, ep });
    };
    for (uint64_t m = kmoves; m; m &= m - 1) { move.dst = lsBit(m); emit(move, King); }
    if (bitCount(checkers) > 1) return;
    auto [cm, em] = checkers ? checkMask(board, ksq, checkers)
                             : std::make_pair(~0ULL, 0ULL);
    for (PieceType pt = Queen; pt >= Pawn; --pt) {
        uint64_t occ = board.side[us] & board.pieceTypes[pt - 1];
        while (occ) {
            move.src = lsBit(occ);
            uint64_t mv = piece_moves(board, pt, move.src, ksq, pinned, pinning, cm, em);
            while (mv) {
                move.dst = lsBit(mv);
                PieceType s = PieceTypeNone, e = PieceTypeNone;
                if (promoMove(board, move)) { s = Knight; e = Queen; }
                for (PieceType q = s; q <= e; q = (PieceType)(q + 1)) {
                    move.promoType = q; emit(move, pt);
                }
                move.promoType = PieceTypeNone;
                mv &= mv - 1;
            }
            occ &= occ - 1;
        }
    }
}

static int sq(const char * s) {
    if (s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8') return -1;
    return (s[1] - '1') * 8 + (s[0] - 'a');
}

int main(int argc, char ** argv) {
    const char* wp = std::getenv("POLICY_WEIGHTS") ? std::getenv("POLICY_WEIGHTS")
                                                   : "nnue_policy.bin";
    char err[512];
    if (!policy_net_load(pnet, wp, err, sizeof err)) {
        std::fprintf(stderr, "FATAL: %s\n", err); return 2;
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <visits.tsv> [max_positions]\n", argv[0]); return 2;
    }
    const long MAXPOS = argc > 2 ? std::strtol(argv[2], nullptr, 10) : 1000000;

    zobristHash(z); Stockfish::Bitboards::init(); init_nnue(); init_nnue_context(ctx);
    pfeat.resize(nnue_feature_dims());
    if (nnue_feature_dims() != pnet.in) {
        std::fprintf(stderr, "FATAL: net wants %d inputs, NNUE emits %d\n",
                     pnet.in, nnue_feature_dims());
        return 2;
    }
    std::printf("policy net %s: %d -> %d -> %d -> %d\n", wp, pnet.in, pnet.h1, pnet.h2, pnet.out);

    std::ifstream in(argv[1]);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }

    long long used = 0, skipped = 0;
    long long legal_total = 0, cap_total = 0, ep_total = 0;
    long long top1_cap = 0, top6_cap = 0, top6_slots = 0;
    long long lab_cap = 0, lab_quiet = 0, lab_cap_in6 = 0, lab_quiet_in6 = 0;
    long long lab_cap_in1 = 0, lab_quiet_in1 = 0;
    std::vector<Cand> cands;
    std::string line;
    std::getline(in, line);                                  // header

    while (used < MAXPOS && std::getline(in, line)) {
        // fen is field 2, visits is field 8
        std::vector<std::string> f;
        { std::stringstream ss(line); std::string t;
          while (std::getline(ss, t, '\t')) f.push_back(t); }
        if (f.size() < 8) { ++skipped; continue; }
        char fen[MAX_FEN_STRING_LEN];
        std::snprintf(fen, sizeof fen, "%s", f[1].c_str());
        if (fen2board(board, fen)) { ++skipped; continue; }
        if (board.isCheck) { ++skipped; continue; }          // policy is not the prior in check

        accumulator_stack_reset(ctx);
        nnue_features(board, ctx, pfeat.data());
        policy_context(pnet, pfeat.data(), pctx);
        enumerate(cands);
        if (cands.size() < 7) { ++skipped; continue; }       // top-6 is meaningless below this

        legal_total += (long long)cands.size();
        for (const auto& c : cands) { if (c.capture) ++cap_total; if (c.ep) ++ep_total; }

        std::sort(cands.begin(), cands.end(),
                  [](const Cand& a, const Cand& b) { return a.score > b.score; });
        if (cands[0].capture) ++top1_cap;
        for (int i = 0; i < 6; ++i) { if (cands[i].capture) ++top6_cap; ++top6_slots; }

        // The search's own best move: first entry of the visits field.
        const char * v = f[7].c_str();
        int lsrc = sq(v), ldst = sq(v + 2);
        if (lsrc < 0 || ldst < 0) { ++used; continue; }
        int rank = -1; bool lcap = false;
        for (size_t i = 0; i < cands.size(); ++i)
            if (cands[i].src == lsrc && cands[i].dst == ldst) { rank = (int)i; lcap = cands[i].capture; break; }
        if (rank >= 0) {
            if (lcap) { ++lab_cap;   if (rank < 6) ++lab_cap_in6;   if (rank == 0) ++lab_cap_in1; }
            else      { ++lab_quiet; if (rank < 6) ++lab_quiet_in6; if (rank == 0) ++lab_quiet_in1; }
        }
        ++used;
    }

    std::printf("\npositions used %lld  (skipped %lld)\n", used, skipped);
    std::printf("mean branching  %.1f legal moves\n", (double)legal_total / used);
    std::printf("\nBASE RATE over all legal moves\n");
    std::printf("  captures (destination occupied, so threat-represented)  %6.2f%%  (%lld of %lld)\n",
                100.0 * cap_total / legal_total, cap_total, legal_total);
    std::printf("  en passant (captures but destination EMPTY, no feature) %6.3f%%\n",
                100.0 * ep_total / legal_total);
    std::printf("\nWHAT THE POLICY PUTS AT THE TOP\n");
    std::printf("  top-1 is a capture   %6.2f%%   (%.2fx the base rate)\n",
                100.0 * top1_cap / used, (double)top1_cap / used / ((double)cap_total / legal_total));
    std::printf("  top-6 are captures   %6.2f%%   (%.2fx the base rate)   %lld of %lld slots\n",
                100.0 * top6_cap / top6_slots,
                (double)top6_cap / top6_slots / ((double)cap_total / legal_total),
                top6_cap, top6_slots);
    std::printf("\nCAN IT FIND THE SEARCH'S OWN BEST MOVE?\n");
    std::printf("  best move is a capture in %lld positions, quiet in %lld\n", lab_cap, lab_quiet);
    if (lab_cap)   std::printf("    capture best move in policy top-1 %6.2f%%   top-6 %6.2f%%\n",
                               100.0 * lab_cap_in1 / lab_cap, 100.0 * lab_cap_in6 / lab_cap);
    if (lab_quiet) std::printf("    quiet   best move in policy top-1 %6.2f%%   top-6 %6.2f%%\n",
                               100.0 * lab_quiet_in1 / lab_quiet, 100.0 * lab_quiet_in6 / lab_quiet);
    return 0;
}
