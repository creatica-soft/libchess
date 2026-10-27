// cmake . && make creatica_fp      (or, on Apple Silicon: c++ -std=c++20 -Wall -DNDEBUG -DUSE_PTHREADS -O3 -funroll-loops -mdynamic-no-pic -DIS_64BIT -DUSE_POPCNT -DUSE_NEON=8 -march=armv8.2-a+dotprod -DUSE_NEON_DOTPROD -flto=full -I . -I nnue creatica_fp.cpp uci_frontend.cpp board.cpp engine.cpp fen.cpp move.cpp pgn.cpp tag.cpp zobrist-hash.cpp nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/nnue/features/full_threats.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp -o creatica_fp -lpthread)
//
// CREATICA FP -- alpha-beta from first principles.
//
// Built up in layers, each added only when it has been measured, so that every mechanism in here
// is one a reader can explain and whose cost and benefit are known. creatica_ab, the port of
// Stockfish 18, stays the ceiling it is measured against.
//
// LAYER 1, THE CORE: negamax alpha-beta with a principal variation search, iterative deepening, a
// transposition table, a quiescence search over captures, and Stockfish's NNUE at the leaves.
// The only move ordering is the table's move first. No pruning, no reductions, no extensions.
//
// LAYER 2, ORDERING: captures by most valuable victim, least valuable attacker; quiet moves by
// history.
//
// LAYER 3, SELECTIVITY: the null move; late move reductions; futility at the child and the parent.
//
// UNITS. Scores are Stockfish's internal units (a pawn is 208), as evaluate_nnue_value() returns
// them; they become centipawns only on the info line, through Stockfish's win-rate model.
//
// One search thread.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include "nnue/nnue/nnue_accumulator.h"
#include "nnue/bitboard.h"
#include "nnue/evaluate.h"
#include "libchess.h"
#include "uci_engine.h"

static Zobrist z;

static const int MAX_PLY   = 128;
static const int MAX_MOVES = 256;
static const int INF       = 32001;
static const int MATE      = 32000;
static const int MATE_IN_MAX = MATE - MAX_PLY;     //scores beyond this are mates
static const int FUTILITY_MARGIN = 150;            //per ply, about 0.7 of a pawn

static inline bool is_mate_score(int v) { return v >= MATE_IN_MAX || v <= -MATE_IN_MAX; }

static std::string uci_of(const Move& m) {
    std::string s = std::string(square[m.src]) + square[m.dst];
    if (m.promoType >= Knight && m.promoType <= Queen && uciPromoLetter[m.promoType])
        s += uciPromoLetter[m.promoType];
    return s;
}
static inline bool same(const Move& a, const Move& b) {
    return a.src == b.src && a.dst == b.dst && a.promoType == b.promoType;
}

//A score as UCI shows it: centipawns through Stockfish's win-rate model, or a mate in moves.
static std::string format_score(int v, const Board& b) {
    if (!is_mate_score(v)) return "cp " + std::to_string((int)Stockfish::Eval::to_cp(v, b));
    const int plies = v > 0 ? MATE - v : -(MATE + v);
    return "mate " + std::to_string((plies > 0 ? plies + 1 : plies) / 2);
}

//=== THE TRANSPOSITION TABLE ===
//One entry per slot. A slot is overwritten unless it holds a deeper result for another position
//from this same search: results from earlier searches give way, and so do shallower ones.
enum Bound : uint8_t { BOUND_NONE, BOUND_UPPER, BOUND_LOWER, BOUND_EXACT };
struct TTEntry {
    uint64_t key = 0;
    int16_t  score = 0;
    uint8_t  src = 0, dst = 0, promo = PieceTypeNone;
    int8_t   depth = 0;
    uint8_t  bound = BOUND_NONE;
    uint8_t  gen = 0;
};
static std::vector<TTEntry> tt;

//Late move reductions, in plies, by depth and move number: log(depth) * log(move) / 2.25.
static int lmr[64][64];
static void init_lmr() {
    for (int d = 1; d < 64; ++d)
        for (int m = 1; m < 64; ++m)
            lmr[d][m] = (int)(0.75 + std::log((double)d) * std::log((double)m + 1) / 2.25);
}
static uint8_t tt_gen = 0;

static inline TTEntry& tt_slot(uint64_t key) {
    return tt[(size_t)(((unsigned __int128)key * tt.size()) >> 64)];
}
//A mate score is stored as distance from this node, not from the root, so that it means the same
//wherever the position is met again.
static inline int to_tt(int v, int ply)   { return v >= MATE_IN_MAX ? v + ply : v <= -MATE_IN_MAX ? v - ply : v; }
static inline int from_tt(int v, int ply) { return v >= MATE_IN_MAX ? v - ply : v <= -MATE_IN_MAX ? v + ply : v; }

static void tt_store(uint64_t key, int depth, int score, Bound bound, const Move& m, int ply) {
    TTEntry& e = tt_slot(key);
    if (e.key != key && e.gen == tt_gen && e.depth > depth) return;
    const bool keep_move = e.key == key && m.src == SquareNone;
    e.key = key;
    e.score = (int16_t)to_tt(score, ply);
    e.depth = (int8_t)std::clamp(depth, -1, 127);
    e.bound = bound;
    e.gen = tt_gen;
    if (!keep_move) {
        e.src = m.src == SquareNone ? 0 : (uint8_t)m.src;
        e.dst = m.src == SquareNone ? 0 : (uint8_t)m.dst;
        e.promo = (uint8_t)m.promoType;
    }
}

static int tt_hashfull() {
    int used = 0;
    for (size_t i = 0; i < std::min<size_t>(1000, tt.size()); ++i) used += tt[i].gen == tt_gen && tt[i].key;
    return used;
}

//=== SEARCH LIMITS, set by go and read by the search ===
static std::atomic<bool> stop_flag{false};
static long long node_limit = 0;            //go nodes
static int depth_limit = 0;                 //go depth
static long long deadline_ms = 0;           //0: no deadline
static std::chrono::steady_clock::time_point search_start;
static long long elapsed_ms() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - search_start).count();
}

//=== THE SEARCHER ===
struct Searcher {
    Board board;
    ZobristHash zh;
    NNUEContext ctx;
    long long nodes = 0;
    int seldepth = 0;

    //Every position since the last irreversible move, the game's and then the search path's, for
    //the repetition test.
    std::vector<uint64_t> history;

    //The move list of every ply, here rather than on the stack: a std::thread's stack is 512 KB on
    //macOS, and MAX_PLY frames of 256 moves would overflow it.
    Move moves[MAX_PLY + 2][MAX_MOVES];
    Move pv[MAX_PLY + 1][MAX_PLY + 1];
    int  pv_len[MAX_PLY + 1];

    //Ordering statistics: at every node that failed high, which move did it (1 = the first).
    long long cut_nodes = 0, first_move_cuts = 0, cut_index_sum = 0;

    //--- MOVE GENERATION: legal moves only, from libchess's pin-aware generator ---
    //Captures, en passant and promotions count as "noisy"; in the quiescence search only those are
    //generated, and only queen promotions among them, unless the side to move is in check, when
    //every evasion is.
    int generate(Move * out, bool noisy_only, bool& in_check) {
        int n = 0;
        auto [kmoves, pinned, pinning, checkers, ksq] = kingMoves(board);
        in_check = checkers != 0;
        if (in_check) noisy_only = false;
        const uint64_t them = board.side[1 - board.sideToMove];
        auto add = [&](Square s, Square d, PieceType promo) {
            Move m{}; m.src = s; m.dst = d; m.promoType = promo;
            out[n++] = m;
        };
        for (uint64_t b = kmoves; b; b &= b - 1) {
            const Square d = lsBit(b);
            if (!noisy_only || ((them >> d) & 1ULL)) add(ksq, d, PieceTypeNone);
        }
        if (bitCount(checkers) > 1) return n;       //double check: only the king moves
        auto [cm, em] = checkers ? checkMask(board, ksq, checkers) : std::make_pair(~0ULL, 0ULL);
        for (PieceType pt = Pawn; pt <= Queen; ++pt) {
            for (uint64_t pcs = board.side[board.sideToMove] & board.pieceTypes[pt - 1]; pcs; pcs &= pcs - 1) {
                const Square s = lsBit(pcs);
                for (uint64_t mv = piece_moves(board, pt, s, ksq, pinned, pinning, cm, em); mv; mv &= mv - 1) {
                    const Square d = lsBit(mv);
                    const bool capture = ((them >> d) & 1ULL) || (pt == Pawn && ((s ^ d) & 7));
                    Move m{}; m.src = s; m.dst = d; m.promoType = PieceTypeNone;
                    if (pt == Pawn && promoMove(board, m)) {
                        add(s, d, Queen);
                        if (!noisy_only) { add(s, d, Rook); add(s, d, Bishop); add(s, d, Knight); }
                    } else if (!noisy_only || capture) add(s, d, PieceTypeNone);
                }
            }
        }
        return n;
    }

    //--- MAKING MOVES: the board, the hash and the NNUE accumulator move together ---
    struct Undo { StateInfo st; ZobristHash zh; };
    void make(Move& m, Undo& u) {
        u.zh = zh;
        u.st = {};
        auto [dp, dts] = accumulator_stack_push(ctx);
        updateHash(zh, board, m, do_move_dp(board, m, u.st, dp, dts), z);
        history.push_back(zh.hash);
        ++nodes;
    }
    void unmake(const Move& m, const Undo& u) {
        history.pop_back();
        undo_move(board, m, u.st);
        accumulator_stack_pop(ctx);
        zh = u.zh;
    }

    //--- DRAWS ---
    //A position that occurred before, since the last capture or pawn move, is scored as a draw:
    //if repeating it was good for one side, the other could have avoided it.
    bool is_draw() {
        if (board.halfmoveClock >= 100) return true;
        const int n = (int)history.size();
        const int back = std::min<int>(board.halfmoveClock, n - 1);
        for (int i = 4; i <= back; i += 2)
            if (history[(size_t)(n - 1 - i)] == zh.hash) return true;
        return false;
    }

    bool null_before[MAX_PLY + 2] = {};     //the move into this ply was a pass
    bool quiet[MAX_PLY + 2][MAX_MOVES];

    //After the move: is the side now to move in check?
    bool gives_check() { return getCheckers(board, kingSquare(board, board.sideToMove)) != 0; }

    int non_pawn_material() {
        const uint64_t own = board.side[board.sideToMove];
        return bitCount(own & ~board.pieceTypes[Pawn - 1] & ~board.pieceTypes[King - 1]);
    }

    bool out_of_time() {
        if (stop_flag.load(std::memory_order_relaxed)) return true;
        if (node_limit && nodes >= node_limit) { stop_flag.store(true); return true; }
        if (deadline_ms && (nodes & 1023) == 0 && elapsed_ms() >= deadline_ms) { stop_flag.store(true); return true; }
        return false;
    }

    //--- HISTORY: how often a quiet move has caused a cutoff, by side, origin and destination ---
    //On a cutoff the move that caused it is rewarded and every quiet move tried before it penalised,
    //by depth squared, since a deeper cutoff saved more work. The update pulls each entry toward the
    //bonus's sign by the bonus's size, so entries stay within +-HIST_MAX and old evidence fades.
    static const int HIST_MAX = 16384;
    int quiet_hist[2][64][64] = {};
    void hist_update(int stm, const Move& m, int bonus) {
        int& h = quiet_hist[stm][m.src][m.dst];
        bonus = std::clamp(bonus, -HIST_MAX, HIST_MAX);
        h += bonus - h * std::abs(bonus) / HIST_MAX;
    }

    //--- ORDERING ---
    //Each move gets a score once, and the loop takes the best remaining one when it gets there, so a
    //cutoff on the first move costs no sorting of the rest.
    int scores[MAX_PLY + 2][MAX_MOVES];

    //What a move captures, by Stockfish's piece values; a promotion counts as winning the new piece
    //less the pawn. En passant is a pawn.
    int gain(const Move& m) {
        const int victim = board.piecesOnSquares[m.dst];
        int v = victim != PieceNone ? pieceValueSF[victim & 7] : 0;
        const int mover = board.piecesOnSquares[m.src] & 7;
        if (mover == Pawn && victim == PieceNone && ((m.src ^ m.dst) & 7)) v = pieceValueSF[Pawn];
        if (m.promoType != PieceTypeNone) v += pieceValueSF[m.promoType] - pieceValueSF[Pawn];
        return v;
    }

    //Most valuable victim first and, among equal victims, the least valuable attacker: a capture that
    //wins the most for the least risk is the likeliest refutation. Noisy moves come before quiet ones.
    void score_moves(int ply, const Move * moves, int n, const Move& tt_move) {
        for (int i = 0; i < n; ++i) {
            const Move& m = moves[i];
            const int g = gain(m);
            int sc = 0;
            if (same(m, tt_move))  sc = 1 << 30;
            else if (g > 0)        sc = (1 << 24) + 16 * g - (board.piecesOnSquares[m.src] & 7);
            else                   sc = quiet_hist[board.sideToMove][m.src][m.dst];
            scores[ply][i] = sc;
        }
    }

    //The best-scored of moves[i..n), moved to position i.
    void pick(int ply, Move * moves, int i, int n) {
        int b = i;
        for (int j = i + 1; j < n; ++j) if (scores[ply][j] > scores[ply][b]) b = j;
        std::swap(moves[i], moves[b]);
        std::swap(scores[ply][i], scores[ply][b]);
    }

    void update_pv(int ply, const Move& m) {
        pv[ply][0] = m;
        for (int i = 0; i < pv_len[ply + 1]; ++i) pv[ply][i + 1] = pv[ply + 1][i];
        pv_len[ply] = pv_len[ply + 1] + 1;
    }

    //--- QUIESCENCE SEARCH: resolve captures so that the evaluation is taken in a quiet position ---
    //The side to move may "stand pat" on the static evaluation instead of capturing, except in check,
    //where it must answer the check.
    int qsearch(int alpha, int beta, int ply) {
        pv_len[ply] = 0;
        if (ply > seldepth) seldepth = ply;
        if (out_of_time()) return 0;
        if (is_draw()) return 0;
        Move * moves = this->moves[ply];
        bool in_check;
        const int n = generate(moves, true, in_check);
        if (ply >= MAX_PLY) return in_check ? 0 : evaluate_nnue_value(board, ctx, 0);
        int best = -INF;
        if (!in_check) {
            best = evaluate_nnue_value(board, ctx, 0);
            if (best >= beta) return best;
            alpha = std::max(alpha, best);
        } else if (n == 0) return -MATE + ply;
        score_moves(ply, moves, n, Move{});
        for (int i = 0; i < n; ++i) {
            pick(ply, moves, i, n);
            Undo u;
            make(moves[i], u);
            const int v = -qsearch(-beta, -alpha, ply + 1);
            unmake(moves[i], u);
            if (stop_flag.load(std::memory_order_relaxed)) return 0;
            if (v > best) {
                best = v;
                if (v > alpha) {
                    alpha = v;
                    update_pv(ply, moves[i]);
                    if (v >= beta) break;
                }
            }
        }
        return best;
    }

    //--- THE MAIN SEARCH: principal variation search ---
    //The first move is searched with the full window; every later one with a null window, which only
    //asks whether it beats the best so far, and is searched again with the full window if it does.
    int search(int depth, int alpha, int beta, int ply) {
        const bool pv_node = beta - alpha > 1;
        if (depth <= 0) return qsearch(alpha, beta, ply);
        pv_len[ply] = 0;
        if (ply > seldepth) seldepth = ply;
        if (out_of_time()) return 0;
        if (ply > 0 && is_draw()) return 0;
        if (ply >= MAX_PLY) return evaluate_nnue_value(board, ctx, 0);

        //The table: a result at least this deep, with a bound that settles the window, ends the node.
        //Not at PV nodes, so that the principal variation is always searched out.
        const TTEntry& e = tt_slot(zh.hash);
        Move tt_move{};
        if (e.key == zh.hash) {
            if (e.src != e.dst) { tt_move.src = (Square)e.src; tt_move.dst = (Square)e.dst; tt_move.promoType = (PieceType)e.promo; }
            const int v = from_tt(e.score, ply);
            if (!pv_node && e.depth >= depth
                && (e.bound == BOUND_EXACT || (e.bound == BOUND_LOWER && v >= beta) || (e.bound == BOUND_UPPER && v <= alpha)))
                return v;
        }

        Move * moves = this->moves[ply];
        bool in_check;
        const int n = generate(moves, false, in_check);
        if (n == 0) return in_check ? -MATE + ply : 0;

        //NULL MOVE: give the opponent a free move. If a reduced search still fails high, the position
        //is good enough that a real move would too, and the node is cut without searching one.
        //Passing is legal in no position, so it is not tried in check; nor where the side to move has
        //only pawns, where zugzwang (every move making things worse) is common and passing would be
        //the best move; nor twice in a row, which would just hand the move back.
        const int eval = in_check ? -INF : evaluate_nnue_value(board, ctx, 0);

        //REVERSE FUTILITY: so far above beta that a few plies of search are not going to bring the
        //score back below it. The margin is how far the score can plausibly move per ply.
        if (!pv_node && !in_check && depth <= 6 && eval - FUTILITY_MARGIN * depth >= beta && !is_mate_score(beta))
            return eval;

        if (!pv_node && !in_check && ply > 0 && !null_before[ply] && depth >= 2 && non_pawn_material()
            && eval >= beta) {
            const int R = 3 + depth / 3;
            const ZobristHash saved_zh = zh;
            const File saved_ep = board.enPassant;
            const uint8_t saved_r50 = board.halfmoveClock;
            board.sideToMove = (Color)(1 - board.sideToMove);
            board.enPassant = FileNone;
            board.halfmoveClock = 0;            //no repetition reaches back across a pass
            zh.hash ^= z.blackMove;
            if (saved_ep != FileNone) zh.hash ^= z.enPassant[saved_ep];
            zh.prevEnPassant = 0;
            history.push_back(zh.hash);
            null_before[ply + 1] = true;
            const int v = -search(depth - 1 - R, -beta, -beta + 1, ply + 1);
            null_before[ply + 1] = false;
            history.pop_back();
            board.sideToMove = (Color)(1 - board.sideToMove);
            board.enPassant = saved_ep;
            board.halfmoveClock = saved_r50;
            zh = saved_zh;
            if (stop_flag.load(std::memory_order_relaxed)) return 0;
            if (v >= beta) return is_mate_score(v) ? beta : v;     //a mate found after a pass is not real
        }

        //ORDERING: the table's move first, then captures, then the quiet moves.
        score_moves(ply, moves, n, tt_move);

        const int alpha0 = alpha;
        int best = -INF;
        Move best_move{};
        bool * quiet = this->quiet[ply];
        for (int i = 0; i < n; ++i) {
            pick(ply, moves, i, n);
            quiet[i] = gain(moves[i]) == 0;
            Undo u;
            make(moves[i], u);
            //FUTILITY, at the parent: near the leaves, a quiet move that does not give check cannot
            //lift a static evaluation this far below alpha back above it.
            if (i > 0 && !pv_node && !in_check && quiet[i] && depth <= 6 && !is_mate_score(alpha)
                && eval + FUTILITY_MARGIN * depth <= alpha && !gives_check()) {
                unmake(moves[i], u);
                if (best < eval + FUTILITY_MARGIN * depth) best = eval + FUTILITY_MARGIN * depth;
                continue;
            }
            int v;
            if (i == 0) v = -search(depth - 1, -beta, -alpha, ply + 1);
            else {
                //LATE MOVE REDUCTION: with ordering this reliable, a quiet move this far down the list
                //rarely matters, so it is searched shallower first, and at full depth only if it beats
                //alpha there. The reduction grows with the logarithms of the depth and the move number,
                //one less at PV nodes; captures, promotions, evasions and checks are not reduced.
                int r = 0;
                if (depth >= 3 && !in_check && quiet[i] && !gives_check())
                    r = std::clamp(lmr[std::min(depth, 63)][std::min(i, 63)] - (pv_node ? 1 : 0), 0, depth - 2);
                v = -search(depth - 1 - r, -alpha - 1, -alpha, ply + 1);
                if (r > 0 && v > alpha) v = -search(depth - 1, -alpha - 1, -alpha, ply + 1);
                if (v > alpha && v < beta) v = -search(depth - 1, -beta, -alpha, ply + 1);
            }
            unmake(moves[i], u);
            if (stop_flag.load(std::memory_order_relaxed)) return 0;
            if (v > best) {
                best = v;
                if (v > alpha) {
                    alpha = v;
                    best_move = moves[i];
                    update_pv(ply, moves[i]);
                    if (v >= beta) {
                        ++cut_nodes; cut_index_sum += i + 1; first_move_cuts += i == 0;
                        if (quiet[i]) {
                            const int stm = board.sideToMove, bonus = depth * depth * 16;
                            hist_update(stm, moves[i], bonus);
                            for (int j = 0; j < i; ++j)
                                if (quiet[j]) hist_update(stm, moves[j], -bonus);
                        }
                        break;
                    }
                }
            }
        }
        const Bound b = best >= beta ? BOUND_LOWER : best > alpha0 ? BOUND_EXACT : BOUND_UPPER;
        tt_store(zh.hash, depth, best, b, best_move, ply);
        return best;
    }

    //--- ITERATIVE DEEPENING ---
    //Depth 1, 2, 3, ...: each iteration's best move is the next one's table move at the root, and an
    //iteration cut short by the clock still has the last complete one to fall back on.
    Move iterate() {
        nodes = 0;
        cut_nodes = first_move_cuts = cut_index_sum = 0;
        accumulator_stack_reset(ctx);
        Move best{};
        long long prev_nodes = 0;
        for (int d = 1; d <= (depth_limit ? depth_limit : MAX_PLY - 1); ++d) {
            seldepth = 0;
            cut_nodes = first_move_cuts = cut_index_sum = 0;
            const long long before = nodes;
            const int v = search(d, -INF, INF, 0);
            if (stop_flag.load() && d > 1) break;
            if (pv_len[0] > 0) best = pv[0][0];
            const long long ms = elapsed_ms();
            std::string line = "info depth " + std::to_string(d) + " seldepth " + std::to_string(seldepth)
                             + " multipv 1 score " + format_score(v, board) + " nodes " + std::to_string(nodes)
                             + " nps " + std::to_string(ms > 0 ? nodes * 1000 / ms : nodes)
                             + " hashfull " + std::to_string(tt_hashfull())
                             + " time " + std::to_string(ms) + " pv";
            for (int i = 0; i < pv_len[0]; ++i) line += " " + uci_of(pv[0][i]);
            std::printf("%s\n", line.c_str());
            //The effective branching factor: this iteration's nodes over the previous one's.
            const long long iter = nodes - before;
            std::printf("info string iter %d nodes %lld ebf %.2f firstcut %.1f%% cutindex %.2f\n", d, iter,
                        prev_nodes ? (double)iter / prev_nodes : 0.0,
                        cut_nodes ? 100.0 * first_move_cuts / cut_nodes : 0.0,
                        cut_nodes ? (double)cut_index_sum / cut_nodes : 0.0);
            std::fflush(stdout);
            prev_nodes = iter;
            if (stop_flag.load()) break;
        }
        return best;
    }
};

//=== THE UCI ENGINE ===
class FirstPrinciplesEngine : public uci::SearchEngine {
public:
    const char * id_name()   const override { return "Creatica FP (alpha-beta from first principles, NNUE)"; }
    const char * id_author() const override { return "Arkadi Poliakevitch"; }

    void declare_options(uci::Options& o) override {
        o.spin("Hash",    &o_hash, 64, 1, 32768, [this] { apply_hash(); });
        o.spin("Threads", &o_threads, 1, 1, 1);
        o.spin("MultiPV", &o_multipv, 1, 1, 1);
    }

    void init() override {
        Stockfish::Bitboards::init();
        zobristHash(z);
        init_nnue();
        init_nnue_context(s_.ctx);
        init_lmr();
        apply_hash();
        set_position("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", {});
    }

    void new_game() override {
        stop();
        std::fill(tt.begin(), tt.end(), TTEntry{});
        std::memset(s_.quiet_hist, 0, sizeof s_.quiet_hist);
    }

    void set_position(const std::string& fen, const std::vector<std::string>& moves) override {
        stop();
        Board b{};
        if (fen2board(b, fen.c_str()) != 0) {
            std::printf("info string unparseable fen, position ignored\n");
            std::fflush(stdout);
            return;
        }
        ZobristHash h{};
        getHash(h, b, z);
        game_.clear();
        game_.push_back(h.hash);
        for (const std::string& ms : moves) {
            Move m{};
            uci2move_idx(ms.c_str(), m);
            normalise_castling(b, m);
            if (const char * why = illegal_reason(b, m)) {
                std::printf("info string REFUSED move %s -- %s; the rest are dropped\n", ms.c_str(), why);
                std::fflush(stdout);
                break;
            }
            updateHash(h, b, m, ff_move(b, m), z);
            game_.push_back(h.hash);
        }
        //Rebuilt from its own FEN: fen2board() also sets libchess's castling tables for it.
        char buf[MAX_FEN_STRING_LEN];
        root_fen_ = board2fen(b, buf);
        root_ = Board{};
        fen2board(root_, root_fen_.c_str());
        root_hash_ = h;
    }

    void go(const uci::Limits& lim) override {
        stop();
        search_start = std::chrono::steady_clock::now();
        stop_flag.store(false);
        node_limit = lim.nodes;
        depth_limit = lim.depth;
        deadline_ms = 0;
        infinite_ = lim.infinite || lim.ponder;
        if (!infinite_) set_clock(lim);
        ++tt_gen;
        s_.board = root_;
        s_.zh = root_hash_;
        s_.history = game_;
        driver_ = std::thread([this] { run(); });
    }

    void stop() override {
        stop_flag.store(true);
        infinite_ = false;
        if (driver_.joinable()) driver_.join();
    }

    void ponderhit() override { infinite_ = false; }

    void quit() override { stop(); }

private:
    void run() {
        Move best = s_.iterate();
        //go infinite and go ponder answer only when told to stop.
        while (infinite_ && !stop_flag.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (best.src == SquareNone) {
            Move moves[MAX_MOVES]; bool chk;
            s_.board = root_;
            if (s_.generate(moves, false, chk) > 0) best = moves[0];
        }
        std::printf("bestmove %s\n", best.src == SquareNone ? "0000" : uci_of(best).c_str());
        std::fflush(stdout);
    }

    //creatica's time management: the remaining time over the moves left, plus half the increment.
    void set_clock(const uci::Limits& lim) {
        if (lim.movetime_ms > 0) { deadline_ms = (long long)(lim.movetime_ms * 0.99); return; }
        if (!lim.wtime && !lim.btime) return;
        const int64_t remaining = root_.sideToMove == ColorWhite ? lim.wtime : lim.btime;
        const int64_t inc       = root_.sideToMove == ColorWhite ? lim.winc  : lim.binc;
        int moves_left = lim.movestogo ? lim.movestogo : MAX_MOVES_REMAINING - root_.moveNumber;
        if (moves_left < MIN_MOVES_REMAINING) moves_left = MIN_MOVES_REMAINING;
        int64_t r = remaining;
        if (r > TIME_SAFETY_BUFFER) r -= TIME_SAFETY_BUFFER;
        double alloc = (double)r / moves_left + (double)inc * 0.5 - 300.0;
        if (remaining < MIN_TIME_THRESHOLD) alloc = std::min(alloc, (double)remaining * 0.5);
        if (inc < 1000 && alloc < 3000) alloc = 100;
        deadline_ms = (long long)std::max(alloc, 100.0);
    }

    void apply_hash() {
        const size_t n = std::max<size_t>(1, (size_t)o_hash * 1024 * 1024 / sizeof(TTEntry));
        if (n != tt.size()) tt.assign(n, TTEntry{});
    }

    Searcher s_;
    Board root_{};
    ZobristHash root_hash_{};
    std::string root_fen_;
    std::vector<uint64_t> game_;
    std::thread driver_;
    std::atomic<bool> infinite_{false};
    int64_t o_hash = 64, o_threads = 1, o_multipv = 1;
};

int main(int argc, char ** argv) {
    FirstPrinciplesEngine engine;
    return uci::uci_main(engine, argc, argv);
}
