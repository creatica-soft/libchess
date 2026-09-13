// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -O3 -I /Users/ap/libchess -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess -o bench_prior_acc bench_prior_acc.cpp
//
// THE YARDSTICK: how good is the incumbent prior, really?
//
// bench_priors.cpp answers "what does an expansion COST". This answers "what is it
// WORTH" -- the number that decides whether a top-K gate is safe, and the baseline any
// learned ranker has to beat.
//
// It replicates creatica-shared-root's prior exactly:
//     score_i = -position_eval(child_i)              (pawns, mover's perspective)
//     p_i     = softmax(score_i / temperature)       (get_prob(), temperature = 0.58)
// then scores that distribution against the Stockfish PV1 move stored in the lichess
// eval shards.
//
// Reported, on N positions:
//   top-1 agreement      argmax(prior) == SF PV1
//   mean mass on PV1     how much probability the prior puts on the right move
//   P(PV1 in top K)      K = 1,2,4,6,8,10  <-- the recall curve that gates top-K NNUE
//
// Only PV1's MOVE is used, never its cp, so the known white-POV/black sign bug in the
// shard cp field cannot contaminate these numbers.
//
// WHICH FILE: use the lichess_db_pv_eval_*.bin family, NOT lichess_db_eval_*.bin.
// They are different formats. lichess_db_eval_*.bin was written by the now
// commented-out short path at lichess_evals_bin_writer.cpp:504-521 -- pieces, stm,
// castling, ep, one 16-bit cp, align -- and carries NO PV moves at all, so it holds
// no policy labels (verified: 200,000/200,000 records decode under the short layout
// and 0 under the full one). Only lichess_db_pv_eval_*.bin has from_sq/to_sq/promo.
// lichess_db_pv_eval_test.bin is the held-out file.
//
//   ./bench_prior_acc /Users/ap/lichess_db_pv_eval_test.bin [max_positions] [temperature]
//
// MEASURED BASELINE, 20,000 positions of lichess_db_pv_eval_test.bin, temperature 0.58:
//   mean branching     28.6 moves/position
//   top-1 agreement    26.9%      mean mass on PV1   0.184
//   P(PV1 in top K)    K=1 26.9  K=2 44.2  K=4 65.5  K=6 77.4  K=8 84.6  K=10 88.8

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <fstream>
#include <vector>
#include <algorithm>
#include "nnue/nnue/nnue_accumulator.h"
#include "libchess.h"
#include "policy_net.h"

static PolicyNet pnet;
static float pctx[512];
static std::vector<unsigned char> pfeat;

struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;
};
void init_nnue(); void cleanup_nnue();
int  nnue_feature_dims();
int  nnue_features(const Board&, NNUEContext&, unsigned char*);
void init_nnue_context(NNUEContext&); void free_nnue_context(NNUEContext&);
double evaluate_nnue(const Board&, NNUEContext&);
std::pair<Stockfish::DirtyPiece&, Stockfish::DirtyThreats&> accumulator_stack_push(NNUEContext&);
void accumulator_stack_pop(NNUEContext&);
void accumulator_stack_reset(NNUEContext&);

// evaluate_nnue returns this sentinel when the side to move is in check.
static constexpr double NNUE_CHECK_SENTINEL = 0.00001;

namespace Stockfish { namespace Eval { bool use_smallnet(const Board&); } }

NNUEContext ctx; Board board; Zobrist z;

// ---------------------------------------------------------------- shard decoding
// Format (lichess_evals_bin_writer.cpp:17-31), bit-packed, byte-aligned per record:
//   [5]  num_pieces - 1
//   [10] x num_pieces: square(6) | color(1) | type(3)     <- this IS libchess Piece
//   [1]  side to move      [4] castling KQkq   [4] ep file (8 = none)
//   [16] PV1 cp (int16)    [4] num_pvs - 1
//   per PV: [6] from  [6] to  [2] promo (0=none,1=N,2=B,3=Q)  [16] cp (PV1 skips)
struct CompressedPosition {
    uint8_t piecesOnSquares[64];
    int8_t  side_to_move, castling_rights, ep_file, pvs;
    int16_t eval_cp;
    int8_t  from_sq[16], to_sq[16], promo[16];
    int16_t cp[16];
};

// lichess_db_pvs_eval_*.bin stores the PV count in 4 bits (up to 16 PVs); the older
// lichess_db_eval_*.bin used 2 (up to 4). Reading the wrong width desynchronizes the
// bitstream on the first record, and every position after it decodes as garbage -- which
// silently reports a near-chance baseline rather than failing.
static constexpr int PV_COUNT_BITS = 4;

class BitReader {
    std::ifstream& in; uint64_t acc = 0; int nbits = 0;
public:
    explicit BitReader(std::ifstream& i) : in(i) {}
    bool exhausted() { return nbits == 0 && in.peek() == EOF; }
    uint32_t read(int n) {
        while (nbits < n) {
            if (in.peek() == EOF) return 0;
            char c; in.get(c);
            acc |= static_cast<uint64_t>(static_cast<uint8_t>(c)) << nbits;
            nbits += 8;
        }
        uint32_t v = acc & ((1ULL << n) - 1);
        acc >>= n; nbits -= n;
        return v;
    }
    void align() { int r = nbits % 8; acc >>= r; nbits -= r; }
};

static bool read_record(BitReader& r, CompressedPosition& p) {
    if (r.exhausted()) return false;
    std::memset(p.piecesOnSquares, PieceNone, sizeof p.piecesOnSquares);
    int num_pieces = r.read(5) + 1;
    for (int i = 0; i < num_pieces; ++i) {
        int sq = r.read(6), color = r.read(1), type = r.read(3);
        p.piecesOnSquares[sq] = static_cast<uint8_t>((color << 3) | type);
    }
    p.side_to_move    = r.read(1);
    p.castling_rights = r.read(4);
    p.ep_file         = r.read(4);
    p.eval_cp         = static_cast<int16_t>(static_cast<uint16_t>(r.read(16)));
    p.pvs             = r.read(PV_COUNT_BITS);
    for (int i = 0; i <= p.pvs; ++i) {
        p.from_sq[i] = r.read(6);
        p.to_sq[i]   = r.read(6);
        p.promo[i]   = r.read(2);
        p.cp[i] = (i == 0) ? p.eval_cp
                           : static_cast<int16_t>(static_cast<uint16_t>(r.read(16)));
    }
    r.align();
    return true;
}

// Rebuild a FEN and hand it to the library's own loader rather than deriving the
// bitboards here -- fen2board already resolves castlingRooks and computes isCheck,
// and a hand-rolled version is exactly how you get a board that fails reconcile().
static bool to_fen(const CompressedPosition& p, char * out) {
    static const char * glyph = " PNBRQK";
    char * w = out;
    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < 8; ++file) {
            uint8_t pc = p.piecesOnSquares[(rank << 3) | file];
            if (pc == PieceNone) { ++empty; continue; }
            if (empty) { *w++ = char('0' + empty); empty = 0; }
            int type = pc & 7, color = pc >> 3;
            if (type < 1 || type > 6) return false;          // corrupt record
            *w++ = color ? char(glyph[type] + 32) : glyph[type];
        }
        if (empty) *w++ = char('0' + empty);
        if (rank) *w++ = '/';
    }
    *w++ = ' '; *w++ = p.side_to_move ? 'b' : 'w'; *w++ = ' ';
    // writer's bitmask is qkQK from the LSB: 1=K 2=Q 4=k 8=q
    if (!p.castling_rights) *w++ = '-';
    else {
        if (p.castling_rights & 1) *w++ = 'K';
        if (p.castling_rights & 2) *w++ = 'Q';
        if (p.castling_rights & 4) *w++ = 'k';
        if (p.castling_rights & 8) *w++ = 'q';
    }
    *w++ = ' ';
    if (p.ep_file >= 0 && p.ep_file < 8) {
        *w++ = char('a' + p.ep_file);
        *w++ = p.side_to_move == ColorWhite ? '6' : '3';   // writer: Rank6 / Rank3
    } else *w++ = '-';
    std::strcpy(w, " 0 1");
    return true;
}

// A shard record can be illegal (the writer's own header notes ~3M illegal positions
// out of 289M). Anything that would make kingMoves() unsafe is dropped, not fixed.
static bool plausible(const CompressedPosition& p) {
    int wk = 0, bk = 0;
    for (int sq = 0; sq < 64; ++sq) {
        uint8_t pc = p.piecesOnSquares[sq];
        if (pc == PieceNone) continue;
        int type = pc & 7;
        if (type < 1 || type > 6) return false;
        if (type == King) ((pc >> 3) ? bk : wk)++;
        if (type == Pawn && (sq < 8 || sq >= 56)) return false;   // pawn on rank 1/8
    }
    return wk == 1 && bk == 1;
}

// A shard record can carry castling rights that contradict the actual king/rook
// placement (the writer checks for this at lichess_evals_bin_writer.cpp:226-233 but
// the shards still contain them). fen2board then resolves castlingRooks with
// lsBit/msBit over whatever rooks exist and kingMoves() segfaults on the result.
// Clear any right not backed by a king on e-file and a rook on the matching corner.
static void sanitize_castling(CompressedPosition& p) {
    auto at = [&](int sq) { return p.piecesOnSquares[sq]; };
    const bool wk_e1 = at(4)  == WhiteKing, bk_e8 = at(60) == BlackKing;
    int c = p.castling_rights;
    if (!(wk_e1 && at(7)  == WhiteRook)) c &= ~1;   // K
    if (!(wk_e1 && at(0)  == WhiteRook)) c &= ~2;   // Q
    if (!(bk_e8 && at(63) == BlackRook)) c &= ~4;   // k
    if (!(bk_e8 && at(56) == BlackRook)) c &= ~8;   // q
    p.castling_rights = static_cast<int8_t>(c);
}

// ------------------------------------------------------------------- the prior
struct Scored { double score; int src, dst, promo;     double pol = 0.0;   // policy logit for this move
};

// position_eval() recurses through process_check() when the child is in check, and
// evaluate_nnue() cannot be called in check at all -- it returns NNUE_CHECK. Mirror
// that with the same 1-ply minimax process_check() performs, so checking moves are
// scored on the same footing they are during a real search.
static double eval_child(int depth);

static void enumerate(std::vector<Scored>& out, bool score_them, int depth) {
    auto [kmoves, pinned, pinning, checkers, ksq] = kingMoves(board);
    Move move = {}; move.src = ksq; move.promoType = PieceTypeNone;
    auto emit = [&](Move& m) {
        if (!score_them) { out.push_back({0.0, m.src, m.dst, m.promoType}); return; }
        StateInfo st = {};
        auto [dp, dts] = accumulator_stack_push(ctx);
        do_move_dp(board, m, st, dp, dts);
        double s = -eval_child(depth);
        undo_move(board, m, st); accumulator_stack_pop(ctx);
        Scored sc{s, m.src, m.dst, m.promoType};
        out.push_back(sc);   //pol filled below, once the legal set is complete
    };
    for (uint64_t m = kmoves; m; m &= m - 1) { move.dst = lsBit(m); emit(move); }
    if (bitCount(checkers) > 1) return;                       // double check: king only
    auto [cm, em] = checkers ? checkMask(board, ksq, checkers)
                             : std::make_pair(~0ULL, 0ULL);
    for (PieceType pt = Queen; pt >= Pawn; --pt) {
        uint64_t occ = board.side[board.sideToMove] & board.pieceTypes[pt - 1];
        while (occ) {
            move.src = lsBit(occ);
            uint64_t mv = piece_moves(board, pt, move.src, ksq, pinned, pinning, cm, em);
            while (mv) {
                move.dst = lsBit(mv);
                PieceType s = PieceTypeNone, e = PieceTypeNone;
                if (promoMove(board, move)) { s = Knight; e = Queen; }
                for (PieceType q = s; q <= e; q = (PieceType)(q + 1)) {
                    move.promoType = q; emit(move);
                }
                move.promoType = PieceTypeNone;
                mv &= mv - 1;
            }
            occ &= occ - 1;
        }
    }
    if (!score_them) return;
    //THE LEGALITY TERM. It adds Wl * (mean embedding row over the legal moves) to ctx, so it has
    //to be applied once, with every move known, before anything is scored. Omitting it does not
    //degrade the scores gently -- in the piece-indexed net the term reaches 98% of the magnitude
    //of ctx, and leaving it out made this tool report policy-only Top-1 of 29.89% for a net the
    //trainer measures at 34.65%.
    if (pnet.has_wl && !out.empty()) {
        std::vector<size_t> rows;
        rows.reserve(out.size());
        for (const auto& c : out)
            rows.push_back(policy_row(pnet, board.piecesOnSquares[c.src] & 7,
                                      c.src, c.dst, board.sideToMove == ColorBlack));
        policy_apply_legal_bias(pnet, pctx, rows.data(), (int)rows.size());
    }
    for (auto& c : out)
        c.pol = policy_score(pnet, pctx, board.piecesOnSquares[c.src] & 7,
                             c.src, c.dst, board.sideToMove == ColorBlack);
}

// Eval of the position now on the board, from ITS side to move.
static double eval_child(int depth) {
    if (!board.isCheck) return evaluate_nnue(board, ctx);
    if (depth <= 0) return 0.0;                    // give up recursing; neutral
    std::vector<Scored> replies;
    enumerate(replies, true, depth - 1);
    if (replies.empty()) return -327.68;           // mated: huge loss for side to move
    double best = -1e18;
    for (const auto& r : replies) best = std::max(best, r.score);
    return best;
}

int main(int argc, char ** argv) {
    {
        const char* wp = std::getenv("POLICY_WEIGHTS") ? std::getenv("POLICY_WEIGHTS") : "nnue_policy.bin";
        char err[512];
        if (!policy_net_load(pnet, wp, err, sizeof err)) { std::fprintf(stderr, "FATAL: %s\n", err); return 2; }
    }
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <shard.bin> [max_positions] [temperature]\n"
            "  e.g. %s /Users/ap/lichess_db_eval_1.bin 20000 0.58\n", argv[0], argv[0]);
        return 2;
    }
    const size_t MAXPOS = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 20000;
    const double temperature = argc > 3 ? std::atof(argv[3]) : 0.58;  // TEMPERATURE 58 * 0.01

    zobristHash(z); Stockfish::Bitboards::init(); init_nnue(); init_nnue_context(ctx);
    pfeat.resize(nnue_feature_dims());

    std::ifstream f(argv[1], std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    BitReader reader(f);

    const int KS[] = {1, 2, 4, 6, 8, 10};
    const int NK = sizeof KS / sizeof KS[0];
    long long inTopK[NK] = {0};
    long long used = 0, skipped_illegal = 0, skipped_nomove = 0, total_moves = 0;
    //How often does Stockfish route to the SMALL net? A policy head trained on the
    //big net's features needs the big accumulator, which is not maintained on those
    //nodes -- so this is the fraction that would pay an extra update.
    long long smallnet_hits = 0;
    static const double WS[] = {0.0, 0.25, 0.4, 0.45, 0.5, 0.6, 0.75, 1.0};
    static const int NW = sizeof(WS)/sizeof(WS[0]);
    long long w_top1[NW] = {0}, w_top4[NW] = {0}, w_top6[NW] = {0};
    double    w_mass[NW] = {0};
    static const double MS[] = {0.90, 0.95, 0.98, 0.99, 0.999};
    static const int NM = sizeof(MS)/sizeof(MS[0]);
    long long w_mass_keep[NW][NM] = {};
    double    w_mass_moves[NW][NM] = {};
    long long top1 = 0;
    double mass_on_pv1 = 0.0, mass_on_top1 = 0.0;

    const bool TRACE = getenv("YARD_TRACE") != nullptr;
    long long rec = 0;
    CompressedPosition p;
    char fen[MAX_FEN_STRING_LEN];
    std::vector<Scored> moves;

    while (used < (long long)MAXPOS && read_record(reader, p)) {
        ++rec;
        if (TRACE) { std::fprintf(stderr, "[rec %lld] decode ok, pieces/stm=%d\n", rec, p.side_to_move); std::fflush(stderr); }
        sanitize_castling(p);
        if (!plausible(p) || !to_fen(p, fen)) { ++skipped_illegal; continue; }
        if (TRACE) { std::fprintf(stderr, "[rec %lld] fen=%s\n", rec, fen); std::fflush(stderr); }
        if (fen2board(board, fen)) { ++skipped_illegal; continue; }
        if (TRACE) { std::fprintf(stderr, "[rec %lld] fen2board ok\n", rec); std::fflush(stderr); }

        // The side NOT to move must not be in check -- that position is unreachable
        // and kingMoves() is not defined on it.
        {
            Board probe = board;
            probe.sideToMove = Color(1 - board.sideToMove);
            if (getCheckers(probe, kingSquare(probe, Color(1 - board.sideToMove)))) {
                ++skipped_illegal; continue;
            }
        }

        accumulator_stack_reset(ctx);
        nnue_features(board, ctx, pfeat.data());
        policy_context(pnet, pfeat.data(), pctx);
        moves.clear();
        if (TRACE) { std::fprintf(stderr, "[rec %lld] enumerating\n", rec); std::fflush(stderr); }
        enumerate(moves, true, 1);
        if (moves.empty()) { ++skipped_nomove; continue; }

        // get_prob(): softmax over scores at `temperature`
        double mx = -1e18;
        for (const auto& m : moves) mx = std::max(mx, m.score);
        double tot = 0.0;
        for (const auto& m : moves) tot += std::exp((m.score - mx) / temperature);
        if (tot <= 0.0) { ++skipped_nomove; continue; }

        // PV1's move, promo mapped from writer encoding (0=none,1=N,2=B,3=Q)
        static const int PROMO[4] = {PieceTypeNone, Knight, Bishop, Queen};
        const int pv_src = p.from_sq[0], pv_dst = p.to_sq[0], pv_pr = PROMO[p.promo[0] & 3];

        //Blend the two signals in softmax-logit space. Each is divided by the temperature
        //its own distribution was tuned at, so w is a clean mix of two comparable logits:
        //  w = 0  pure 1-ply child evaluation (the incumbent prior)
        //  w = 1  pure policy head
        //The point of the sweep is that these two carry DIFFERENT information. The child
        //evaluation sees one ply ahead by construction; the policy head only sees the
        //current position. A move that is good for reasons visible only after it is played
        //is invisible to the policy and obvious to the evaluation.
        static const int PROMO2[4] = {PieceTypeNone, Knight, Bishop, Queen};
        const int pv_src2 = p.from_sq[0], pv_dst2 = p.to_sq[0], pv_pr2 = PROMO2[p.promo[0] & 3];
        bool pv_present = false;
        for (const auto& m : moves) {
            if (m.src == pv_src2 && m.dst == pv_dst2 &&
                (m.promo == pv_pr2 || (p.promo[0] == 0 && m.promo == PieceTypeNone))) {
                pv_present = true; break;
            }
        }
        if (!pv_present) { ++skipped_nomove; continue; }

        for (int wi = 0; wi < NW; ++wi) {
            const double w = WS[wi];
            //Overall sharpness of the blended logits. A mixture of two disagreeing signals
            //is flatter than either alone, so this restores the concentration the search's
            //exploration constants were fitted to.
            static const double SCALE = std::getenv("BLEND_SCALE") ? std::atof(std::getenv("BLEND_SCALE")) : 1.0;
            int rank = 0;
            double pv_key = -1e18;
            for (const auto& m : moves) {
                if (m.src == pv_src2 && m.dst == pv_dst2 &&
                    (m.promo == pv_pr2 || (p.promo[0] == 0 && m.promo == PieceTypeNone)))
                    pv_key = SCALE * (w * (m.pol / 0.75) + (1.0 - w) * (m.score / 0.58));
            }
            for (const auto& m : moves) {
                const double k = SCALE * (w * (m.pol / 0.75) + (1.0 - w) * (m.score / 0.58));
                if (k > pv_key) ++rank;
            }
            //Concentration of the resulting prior. Ranking is only half of it: the search's
            //exploration constants were fitted to a prior that puts ~0.40 on its top move,
            //and a blend that ranks well but is much flatter or sharper changes how visits
            //spread regardless of the ordering.
            {
                double mx = -1e18;
                for (const auto& m : moves) {
                    const double k = SCALE * (w * (m.pol / 0.75) + (1.0 - w) * (m.score / 0.58));
                    if (k > mx) mx = k;
                }
                double tot = 0.0;
                for (const auto& m : moves) {
                    const double k = SCALE * (w * (m.pol / 0.75) + (1.0 - w) * (m.score / 0.58));
                    tot += std::exp(k - mx);
                }
                if (tot > 0.0) w_mass[wi] += 1.0 / tot;   // exp(mx-mx)=1 for the top move
            }
            //Probability-mass recall: if the moves were sorted by prior and only those
            //whose priors sum to M were kept, how often would Stockfish's PV1 survive?
            //This is what a ProbabilityMass gate would actually cost, measured on the
            //blended prior rather than on the eval-derived one the option was originally
            //tested against.
            {
                std::vector<double> ps;
                ps.reserve(moves.size());
                double mx2 = -1e18;
                for (const auto& m : moves) {
                    const double k = SCALE * (w * (m.pol / 0.75) + (1.0 - w) * (m.score / 0.58));
                    if (k > mx2) mx2 = k;
                }
                double tot2 = 0.0;
                for (const auto& m : moves) {
                    const double k = SCALE * (w * (m.pol / 0.75) + (1.0 - w) * (m.score / 0.58));
                    ps.push_back(std::exp(k - mx2));
                    tot2 += ps.back();
                }
                double pv_p = 0.0;
                for (size_t q = 0; q < moves.size(); ++q) {
                    const auto& m = moves[q];
                    if (m.src == pv_src2 && m.dst == pv_dst2 &&
                        (m.promo == pv_pr2 || (p.promo[0] == 0 && m.promo == PieceTypeNone)))
                        pv_p = ps[q] / tot2;
                }
                std::vector<double> sorted;
                for (double v : ps) sorted.push_back(v / tot2);
                std::sort(sorted.begin(), sorted.end(), std::greater<double>());
                for (int mi = 0; mi < NM; ++mi) {
                    double cum = 0.0; size_t kept = 0;
                    for (double v : sorted) { if (cum >= MS[mi]) break; cum += v; ++kept; }
                    // PV1 survives if its probability is at least the smallest kept one
                    double floor_p = kept ? sorted[kept - 1] : 1.0;
                    if (pv_p >= floor_p) ++w_mass_keep[wi][mi];
                    w_mass_moves[wi][mi] += (double)kept;
                }
            }
            if (rank == 0) ++w_top1[wi];
            if (rank <  4) ++w_top4[wi];
            if (rank <  6) ++w_top6[wi];
        }
        int rank_of_pv1 = 0;   // kept so the legacy counters below still compile
        total_moves += (long long)moves.size();
        ++used;
        if ((used % 2000) == 0) { std::fprintf(stderr, "  %lld positions...\r", used); std::fflush(stderr); }
    }

    std::printf("\nshard              %s\n", argv[1]);
    std::printf("temperature        %.3f   (engine: TEMPERATURE 58 -> 0.58)\n", temperature);
    std::printf("positions scored   %lld\n", used);
    std::printf("  skipped illegal  %lld\n", skipped_illegal);
    std::printf("  skipped no-PV1   %lld   (PV1 move not legal in the decoded position)\n", skipped_nomove);
    if (!used) { free_nnue_context(ctx); cleanup_nnue(); return 1; }
    std::printf("mean branching     %.1f moves/position\n", (double)total_moves / used);
    std::printf("small-net routed   %.2f%%   (|simple_eval| > 962)\n",
                100.0 * smallnet_hits / used);
    std::printf("\n--- incumbent prior quality (softmax over 1-ply NNUE child evals) ---\n");
    std::printf("\n  blend of 1-ply child eval and policy head, %lld positions\n", used);
    std::printf("  %-6s %-8s %-8s %-8s %s\n", "w", "Top-1", "Top-4", "Top-6", "top-move mass");
    for (int wi = 0; wi < NW; ++wi)
        std::printf("  %-6.2f %6.2f%%  %6.2f%%  %6.2f%%   mass %.4f%s\n", WS[wi],
                    100.0 * w_top1[wi] / used, 100.0 * w_top4[wi] / used,
                    100.0 * w_top6[wi] / used, w_mass[wi] / used,
                    WS[wi] == 0.0 ? "   <- incumbent" : WS[wi] == 1.0 ? "   <- policy only" : "");
    std::printf("\n  probability-mass gate:\n");
    std::printf("  %-8s %-14s %s\n", "mass", "PV1 kept", "mean moves kept");
    for (int wi = 0; wi < NW; ++wi) {
        const bool interesting = (WS[wi] > 0.44 && WS[wi] < 0.46) || WS[wi] > 0.99;
        if (!interesting) continue;
        std::printf("  -- prior weight w = %.2f %s\n", WS[wi],
                    WS[wi] > 0.99 ? "(pure policy: the only prior known BEFORE the child evals)"
                                  : "(the deployed blend: known only AFTER them)");
        for (int mi = 0; mi < NM; ++mi)
            std::printf("  %-8.3f %11.2f%%  %14.1f\n", MS[mi],
                        100.0 * w_mass_keep[wi][mi] / used, w_mass_moves[wi][mi] / used);
    }
    std::printf("mean mass on PV1   %.4f\n", mass_on_pv1 / used);
    std::printf("mean mass on top-1 %.4f   (concentration of the prior)\n", mass_on_top1 / used);
    std::printf("\n--- recall curve: P(PV1 within top K) -- gates a top-K NNUE cut ---\n");
    for (int k = 0; k < NK; ++k)
        std::printf("  K = %-3d          %.1f%%\n", KS[k], 100.0 * inTopK[k] / used);
    std::printf("\nA learned ranker must come within 3 points of the K=6 row and clear\n"
                "32%% top-1, or gating is unsafe at any K worth having.\n");

    free_nnue_context(ctx); cleanup_nnue();
    return 0;
}
