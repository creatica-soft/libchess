// cmake . && make creatica_ab      (or, on Apple Silicon: c++ -std=c++20 -Wall -DNDEBUG -DUSE_PTHREADS -O3 -funroll-loops -mdynamic-no-pic -DIS_64BIT -DUSE_POPCNT -DUSE_NEON=8 -march=armv8.2-a+dotprod -DUSE_NEON_DOTPROD -flto=full -I . -I nnue creatica_ab.cpp uci_frontend.cpp tbprobe.c board.cpp engine.cpp fen.cpp move.cpp pgn.cpp tag.cpp zobrist-hash.cpp nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/nnue/features/full_threats.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp -o creatica_ab -lcurl -lpthread)
//
// CREATICA AB -- the alpha-beta engine.
// Most things get ported from Stockfish 18
//
// UNITS. Everything inside the search is Stockfish's internal units (PawnValue 208): evaluations
// come from evaluate_nnue_value(), and every margin is upstream's literal. Scores
// become centipawns only on the info line, through the same win-rate model Stockfish uses.
// FEATURES:
//   - persistent Lazy SMP threads, so the history tables carry from move to move as upstream's do
//     and are cleared only by ucinewgame;
//   - the game history in the repetition rules, which a test position does not have;
//   - Syzygy: upstream's Step 5 inside the tree, and a DTZ probe at the root;
//   - the online 7-piece tablebase, asked in parallel with the search;
//   - the table generation, so entries from earlier moves of the game age out as upstream's do;
//   - creatica's time management.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>
#include <limits>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include "nnue/nnue/nnue_accumulator.h"
#include "nnue/bitboard.h"
#include "nnue/evaluate.h"
#include "libchess.h"
#include "tbprobe.h"

//`z` is shared because nothing writes it after init. Everything a search writes is thread_local.
static Zobrist z;

//Upstream's values (types.h). -MATE_CP - 1 is -VALUE_INFINITE.
static const int MATE_CP       = 32000;                    //VALUE_MATE
static const int MAX_PLY       = 246;
static const int MATE_IN_MAX   = MATE_CP - MAX_PLY;        //VALUE_MATE_IN_MAX_PLY
static const int VALUE_TB      = MATE_IN_MAX - 1;
static const int TB_WIN_IN_MAX = VALUE_TB - MAX_PLY;       //VALUE_TB_WIN_IN_MAX_PLY
static inline bool is_win(int v)      { return v >=  TB_WIN_IN_MAX; }
static inline bool is_loss(int v)     { return v <= -TB_WIN_IN_MAX; }
static inline bool is_decisive(int v) { return is_win(v) || is_loss(v); }
static const int MAX_MOVES = 256;
struct Mv { uint8_t src, dst, promo; };

//Upstream's partial_insertion_sort (movepick.cpp), index for index. It is not stable: promoting a
//move fills the hole with whatever sat at sortedEnd + 1, so the order below the limit depends on
//the algorithm, and reproducing upstream's order needs the algorithm itself.
static inline void partial_insertion_sort(Mv * mv, int * v, int n, int limit) {
    int se = 0;
    for (int p = 1; p < n; ++p)
        if (v[p] >= limit) {
            const Mv tm = mv[p]; const int tv = v[p];
            ++se;
            mv[p] = mv[se]; v[p] = v[se];
            int q = se;
            while (q != 0 && v[q - 1] < tv) { mv[q] = mv[q - 1]; v[q] = v[q - 1]; --q; }
            mv[q] = tm; v[q] = tv;
        }
}
static inline bool same(const Mv& a, const Mv& b) {
    return a.src == b.src && a.dst == b.dst && a.promo == b.promo;
}
static inline Move to_move(const Mv& m) {
    Move mm = {};
    mm.src = (Square)m.src; mm.dst = (Square)m.dst; mm.promoType = (PieceType)m.promo;
    return mm;
}
static std::string uci_of(const Mv& m) {
    std::string s = std::string(square[m.src]) + square[m.dst];
    if (m.promo >= Knight && m.promo <= Queen && uciPromoLetter[m.promo])
        s += uciPromoLetter[m.promo];
    return s;
}

//Upstream's RootMove::averageScore and meanSquaredScore, per root move, rebuilt every search.
struct RootHist { Mv m{0,0,0}; int avg = -MATE_CP - 1; long long msq = 0; bool used = false; };

//=== THE SEARCH STACK, upstream's Stack fields as parallel arrays indexed by ply ===
//ss->staticEval, holding upstream's VALUE_NONE where it has none. The improving and hindsight
//terms do their arithmetic on that value exactly as upstream's do, so it is a value, not a flag.
static const int VALUE_NONE = 32002;
static inline bool is_promo_pc(uint8_t p) { return p >= Knight && p <= Queen; }

//=== HISTORY TABLES ===
//mainHistory and lowPlyHistory are keyed by from (6 bits), to (6 bits) and promotion piece (3 bits), as upstream's raw()
//move encoding is, 15 bits total
enum { MVIDX_N = 1 << 15 }; //32,168
static inline int mv_idx(uint8_t from, uint8_t to, uint8_t promo) {
    return (int)from | ((int)to << 6) | (((int)promo & 7) << 12);
}
static const int HIST_LIMIT = 7183;          //mainHistory
static const int MAIN_HIST_DEFAULT = 68;     //upstream's mainHistoryDefault
static const int CAP_LIMIT = 10692;          //CapturePieceToHistory
static const int CONT_LIMIT = 30000;         //PieceToHistory
static const int LOW_PLY_SIZE = 5;           //First dim in LowPlyHistory
static const int PAWN_HIST_BASE = 8192;      //PAWN_HISTORY_BASE_SIZE
static const int CORR_LIMIT = 1024;          //CORRECTION_HISTORY_LIMIT
static const int CORR_BASE = 1 << 16;        //CORRHIST_BASE_SIZE, 65,536
enum { CONT_LEVELS = 6 };

static const int cont_w[CONT_LEVELS] = { 1133, 683, 312, 582, 149, 474 };   //conthist_bonuses

//PAWN AND CORRECTION HISTORY ARE SHARED BETWEEN THE THREADS, as upstream's SharedHistories are,
//and scale with the thread count the same way: the base size times the thread count rounded up to
//a power of two. Relaxed atomics, which compile to plain loads and stores, so a single thread
//pays nothing; the read-modify-write races are the ones upstream accepts.
using AI16 = std::atomic<int16_t>;
struct CorrBundle { AI16 pawn, minor, npw, npb; };
static std::unique_ptr<AI16[]> pawn_hist;          //[index][piece 0..11][to]
static std::unique_ptr<CorrBundle[]> corr_tab;     //[index][colour]
static size_t pawn_hist_mask = PAWN_HIST_BASE - 1, corr_mask = CORR_BASE - 1;
static size_t shared_hist_threads = 0;

static inline void stat_add(int& e, int bonus, int limit) {
    const int b = std::clamp(bonus, -limit, limit);
    e = e + b - e * std::abs(b) / limit;
}
static inline void stat_add(int16_t& e, int bonus, int limit) {
    const int b = std::clamp(bonus, -limit, limit);
    const int v = e;
    e = (int16_t)(v + b - v * std::abs(b) / limit);
}
static inline void stat_add(AI16& e, int bonus, int limit) {
    const int b = std::clamp(bonus, -limit, limit);
    const int v = e.load(std::memory_order_relaxed);
    e.store((int16_t)(v + b - v * std::abs(b) / limit), std::memory_order_relaxed);
}
static inline AI16& pawn_entry(uint64_t pk, int pc, int to) {
    return pawn_hist[(((size_t)(pk & pawn_hist_mask)) * 12 + (size_t)pc) * 64 + (size_t)to];
}
static inline int pawn_read(uint64_t pk, int pc, int to) {
    return pawn_entry(pk, pc, to).load(std::memory_order_relaxed);
}

//=== CORRECTION HISTORY ===
struct CorrKeys { uint64_t pawn = 0, minor = 0; uint64_t np[2] = {0, 0}; };
static inline CorrKeys corr_keys(const Board& b) {
    CorrKeys k;
    for (int c = 0; c < 2; ++c) {
        uint64_t pw = b.side[c] & b.pieceTypes[Pawn - 1];
        while (pw) { const int sq = lsBit(pw); pw &= pw - 1; k.pawn ^= z.pawns[c][sq - 8]; }
        for (int pt = Knight; pt <= King; ++pt) {
            uint64_t bb = b.side[c] & b.pieceTypes[pt - 1];
            while (bb) {
                const int sq = lsBit(bb); bb &= bb - 1;
                const uint64_t r = z.nonPawns[c][pt - 2][sq];
                k.np[c] ^= r;
                if (pt == Knight || pt == Bishop) k.minor ^= r;
            }
        }
    }
    return k;
}
static inline CorrBundle& corr_entry(uint64_t key, int stm) {
    return corr_tab[(size_t)(key & corr_mask) * 2 + (size_t)stm];
}
//to_corrected_static_eval: clamped inside the tablebase band, so a corrected evaluation is never
//mistaken for a proof.
static inline int corr_apply(int v, int cv) {
    return std::clamp(v + cv / 131072, -TB_WIN_IN_MAX + 1, TB_WIN_IN_MAX - 1);
}

//The shared half of clear(), sized for the thread count.
static void reset_shared_histories(size_t threads) {
    size_t m = 1;
    while (m < threads) m *= 2;
    if (m != shared_hist_threads) {
        shared_hist_threads = m;
        pawn_hist.reset(new AI16[(size_t)PAWN_HIST_BASE * m * 12 * 64]);
        corr_tab.reset(new CorrBundle[(size_t)CORR_BASE * m * 2]);
        pawn_hist_mask = (size_t)PAWN_HIST_BASE * m - 1;
        corr_mask = (size_t)CORR_BASE * m - 1;
    }
    const size_t np = (size_t)PAWN_HIST_BASE * m * 12 * 64, nc = (size_t)CORR_BASE * m * 2;
    for (size_t i = 0; i < np; ++i) pawn_hist[i].store(-1238, std::memory_order_relaxed);
    for (size_t i = 0; i < nc; ++i) {
        corr_tab[i].pawn.store(0, std::memory_order_relaxed);
        corr_tab[i].minor.store(0, std::memory_order_relaxed);
        corr_tab[i].npw.store(0, std::memory_order_relaxed);
        corr_tab[i].npb.store(0, std::memory_order_relaxed);
    }
}

//=== STOPPING: upstream's threads.stop, its node limit and its clock ===
static const int MAX_THREADS = 64;
static std::atomic<bool> out_of_time{false};
static std::atomic<bool> unlimited{false};          //go infinite / go ponder: never stop on the clock
static std::atomic<long long> deadline_ns{0};       //steady_clock, in nanoseconds
static std::atomic<long long> thread_nodes[MAX_THREADS];
static std::atomic<long long> thread_tbhits[MAX_THREADS];
static long long fixnodes = 0;
static int fixdepth = 0;
static int search_threads = 1;
static inline long long now_ns() {
    return (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
static long long total_nodes() {
    long long n = 0;
    for (int t = 0; t < search_threads; ++t) n += thread_nodes[t].load(std::memory_order_relaxed);
    return n;
}
static long long total_tbhits() {
    long long n = 0;
    for (int t = 0; t < search_threads; ++t) n += thread_tbhits[t].load(std::memory_order_relaxed);
    return n;
}

//=== THE TRANSPOSITION TABLE, shared by every thread ===
//The payload goes in one 64-bit word and `key ^ payload` in the other, so an entry torn between
//two writers fails the key check and is ignored rather than believed.
enum { TT_NONE = 0, TT_EXACT = 1, TT_LOWER = 2, TT_UPPER = 3 };
static const int16_t EVAL_NONE = INT16_MIN;
struct TTData {
    int16_t score = 0;
    int16_t eval  = EVAL_NONE;     //unadjustedStaticEval, or VALUE_NONE
    uint8_t depth = 0;
    uint8_t flag  = TT_NONE;
    uint8_t src = 0, dst = 0, promo = 0;
    uint8_t ttpv = 0;
    uint8_t unsearched = 0;        //DEPTH_UNSEARCHED: an evaluation with no search behind it
    uint8_t gen = 0;               //upstream's generation, five bits
};
static inline uint64_t tt_pack(const TTData& d) {
    return  (uint64_t)(uint16_t)d.score
         | ((uint64_t)d.depth << 16)
         | ((uint64_t)(d.flag  & 3u)  << 24)
         | ((uint64_t)(d.src   & 63u) << 26)
         | ((uint64_t)(d.dst   & 63u) << 32)
         | ((uint64_t)(d.promo & 7u)  << 38)
         | ((uint64_t)(uint16_t)d.eval << 41)
         | ((uint64_t)(d.ttpv  & 1u)  << 57)
         | ((uint64_t)(d.unsearched & 1u) << 58)
         | ((uint64_t)(d.gen & 31u) << 59);
}
static inline TTData tt_unpack(uint64_t w) {
    TTData d;
    d.score = (int16_t)(uint16_t)(w & 0xffff);
    d.eval  = (int16_t)(uint16_t)((w >> 41) & 0xffff);
    d.depth = (uint8_t)(w >> 16);
    d.flag  = (uint8_t)((w >> 24) & 3u);
    d.src   = (uint8_t)((w >> 26) & 63u);
    d.dst   = (uint8_t)((w >> 32) & 63u);
    d.promo = (uint8_t)((w >> 38) & 7u);
    d.ttpv  = (uint8_t)((w >> 57) & 1u);
    d.unsearched = (uint8_t)((w >> 58) & 1u);
    d.gen   = (uint8_t)((w >> 59) & 31u);
    return d;
}
struct TTEntry {
    uint64_t key = 0;      //hash ^ payload
    uint64_t data = 0;
};
static const int TT_WAYS = 3;                       //upstream's ClusterSize
static std::vector<TTEntry> tt;
static size_t tt_buckets = 1;
static uint8_t tt_gen = 0;                          //bumped once per search, as new_search() does

static inline bool tt_key_match(const TTEntry * e, uint64_t hash) {
    const uint64_t d = e->data, k = e->key;
    if (d == 0 && k == 0) return false;
    return (k ^ d) == hash;
}
//An entry with a bound for this position. An evaluation-only entry has none.
static inline bool tt_read(const TTEntry * e, uint64_t hash, TTData& out) {
    if (!tt_key_match(e, hash)) return false;
    out = tt_unpack(e->data);
    return out.flag != TT_NONE;
}
static inline bool tt_read_eval(const TTEntry * e, uint64_t hash, int& out) {
    if (!tt_key_match(e, hash)) return false;
    const int16_t ev = (int16_t)(uint16_t)((e->data >> 41) & 0xffff);
    if (ev == EVAL_NONE) return false;
    out = ev;
    return true;
}
//depth8 - DEPTH_ENTRY_OFFSET: -2 for an unsearched entry, -3 for an empty one.
static inline int tt_updepth(uint64_t w) {
    const int fl = (int)((w >> 24) & 3u);
    const bool unsearched = ((w >> 58) & 1u) != 0;
    return (unsearched || fl == TT_NONE) ? -2 : (int)(uint8_t)(w >> 16);
}
//relative_age(), in depth units: 8 per search since the entry was written.
static inline int tt_rel_age(uint64_t w) {
    return 8 * (int)(((unsigned)tt_gen - (unsigned)((w >> 59) & 31u)) & 31u);
}
static inline size_t tt_index(uint64_t key) {
    return (size_t)(((unsigned __int128)key * (unsigned __int128)tt_buckets) >> 64);
}
//probe(): the entry holding this position, or the one to replace, which is the shallowest after
//age is taken off. The node keeps the slot and writes back into it, as upstream's TTWriter does,
//even if its subtree has since written something else there.
//prefetch(tt.first_entry(key())): the child's bucket is fetched while the move is still being
//made, as upstream's do_move() fetches it, so the probe at the child's entry does not stall.
static inline void tt_prefetch(uint64_t hash) {
    __builtin_prefetch(&tt[tt_index(hash) * (size_t)TT_WAYS]);
}
static inline TTEntry * tt_probe(uint64_t hash) {
    TTEntry * base = &tt[tt_index(hash) * (size_t)TT_WAYS];
    for (int i = 0; i < TT_WAYS; ++i)
        if (tt_key_match(base + i, hash)) return base + i;
    const auto worth = [](const TTEntry * e) {
        const bool empty = e->data == 0 && e->key == 0;
        return (empty ? -3 : tt_updepth(e->data)) - tt_rel_age(e->data);
    };
    TTEntry * victim = base;
    for (int i = 1; i < TT_WAYS; ++i)
        if (worth(victim) > worth(base + i)) victim = base + i;
    return victim;
}
//TTEntry::save(): a new move always replaces the stored one; the rest is overwritten only for an
//exact bound, another position, enough depth, or an entry from an earlier search.
static inline void tt_write(TTEntry * e, uint64_t hash, const TTData& in) {
    TTData d = in;
    d.gen = tt_gen;
    const uint64_t raw = e->data, k = e->key;
    const bool empty = (raw == 0 && k == 0);
    const bool mine  = !empty && ((k ^ raw) == hash);
    TTData old;
    if (!empty) old = tt_unpack(raw);
    if (mine && !d.src && !d.dst && (old.src || old.dst)) {
        d.src = old.src; d.dst = old.dst; d.promo = old.promo;
    }
    const int d_new = (d.unsearched || d.flag == TT_NONE) ? -2 : (int)d.depth;
    const int d_old = empty ? -3 : tt_updepth(raw);
    if (!(d.flag == TT_EXACT || !mine || d_new + 2 * (int)d.ttpv > d_old - 4 || tt_rel_age(raw))) {
        if (mine && (d.src || d.dst)
            && (old.src != d.src || old.dst != d.dst || old.promo != d.promo)) {
            old.src = d.src; old.dst = d.dst; old.promo = d.promo;
            const uint64_t w = tt_pack(old);
            e->data = w;
            e->key  = hash ^ w;
        }
        return;
    }
    const uint64_t w = tt_pack(d);
    e->data = w;
    e->key  = hash ^ w;                       //published last
}
static inline void tt_store_eval(TTEntry * e, uint64_t hash, int ev, int ttpv) {
    TTData d;                                 //VALUE_NONE, BOUND_NONE, DEPTH_UNSEARCHED
    d.eval = (int16_t)ev;
    d.unsearched = 1;
    d.ttpv = (uint8_t)(ttpv ? 1 : 0);
    tt_write(e, hash, d);
}
//hashfull(0): the first thousand buckets, counting entries written by this search.
static int tt_hashfull() {
    const size_t nb = std::min<size_t>(1000, tt_buckets);
    size_t cnt = 0;
    for (size_t i = 0; i < nb * (size_t)TT_WAYS; ++i)
        if ((tt[i].data || tt[i].key) && tt_rel_age(tt[i].data) == 0) ++cnt;
    return nb ? (int)(cnt * 1000 / (nb * (size_t)TT_WAYS)) : 0;
}
//value_to_tt() and value_from_tt(). A mate or tablebase score the fifty-move rule could still
//undo is read back as the highest non-tablebase score.
static inline int tt_to_store(int v, int ply) {
    return is_win(v) ? v + ply : is_loss(v) ? v - ply : v;
}

//=== REPETITIONS ===
//The positions the game reached before the root, oldest first, and their st->repetition.
//Rebuilt by every `position` command and read-only during a search.
static std::vector<uint64_t> game_hashes;
static std::vector<int> game_rep;

//UPCOMING REPETITION and its cuckoo table (Stockfish position.cpp). A reversible non-pawn move
//changes the key by a constant depending only on (piece, s1, s2); if the key difference to a
//position some plies back is such a constant, a single move repeats it. libchess XORs a key for
//empty squares as well, so a move's constant has four square terms, not upstream's two.
enum { CUCKOO_SIZE = 8192 };
static uint64_t cuckoo_key[CUCKOO_SIZE];
static uint8_t  cuckoo_s1[CUCKOO_SIZE], cuckoo_s2[CUCKOO_SIZE]; //two slots, each for its own indexing function
static bool     cuckoo_used[CUCKOO_SIZE];
static inline int cuckoo_h1(uint64_t k) { return (int)(k & 0x1fffULL); } //indexing function 1, k is cuckoo key
static inline int cuckoo_h2(uint64_t k) { return (int)((k >> 16) & 0x1fffULL); } //indexing function 2
static int cuckoo_build() {
    std::memset(cuckoo_key, 0, sizeof cuckoo_key);
    std::memset(cuckoo_used, 0, sizeof cuckoo_used);
    int count = 0;
    for (int c = 0; c < Color_NB; ++c)
        for (PieceType pt = Knight; pt <= King; pt = (PieceType)(pt + 1))
            for (int s1 = 0; s1 < Square_NB; ++s1)
                for (int s2 = s1 + 1; s2 < Square_NB; ++s2) {
                    const Stockfish::Square a = (Stockfish::Square)s1;
                    uint64_t reach = 0;
                    switch (pt) {
                        case Knight: reach = Stockfish::attacks_bb<Stockfish::KNIGHT>(a, 0); break;
                        case Bishop: reach = Stockfish::attacks_bb<Stockfish::BISHOP>(a, 0); break;
                        case Rook:   reach = Stockfish::attacks_bb<Stockfish::ROOK>(a, 0); break;
                        case Queen:  reach = Stockfish::attacks_bb<Stockfish::BISHOP>(a, 0)
                                           | Stockfish::attacks_bb<Stockfish::ROOK>(a, 0); break;
                        case King:   reach = Stockfish::attacks_bb<Stockfish::KING>(a, 0); break;
                        default: break;
                    }
                    if (!((reach >> s2) & 1ULL)) continue;
                    uint64_t key = z.nonPawns[c][pt - 2][s1] ^ z.nonPawns[c][pt - 2][s2]
                                 ^ z.emptySquares[s1] ^ z.emptySquares[s2] ^ z.blackMove;
                    uint8_t a1 = (uint8_t)s1, a2 = (uint8_t)s2;
                    int i = cuckoo_h1(key);
                    for (;;) {
                        std::swap(cuckoo_key[i], key);
                        std::swap(cuckoo_s1[i], a1);
                        std::swap(cuckoo_s2[i], a2);
                        const bool was_used = cuckoo_used[i];
                        cuckoo_used[i] = true;
                        if (!was_used) break;
                        i = (i == cuckoo_h1(key)) ? cuckoo_h2(key) : cuckoo_h1(key);
                    }
                    ++count;
                }
    return count;   //3668, upstream's own assertion
}

static inline uint64_t attacks_by_pawns(const Board& b, Color c) {
    const uint64_t p = b.pieceTypes[Pawn - 1] & b.side[c];
    return c == ColorWhite ? (((p & ~FILE_A_BB) << 7) | ((p & ~FILE_H_BB) << 9))
                           : (((p & ~FILE_A_BB) >> 9) | ((p & ~FILE_H_BB) >> 7));
}
static inline uint64_t attacks_by_type(const Board& b, Color c, PieceType pt) {
    const uint64_t occ = b.side[ColorWhite] | b.side[ColorBlack];
    uint64_t out = 0;
    for (uint64_t m = b.pieceTypes[pt - 1] & b.side[c]; m; m &= m - 1) {
        const Stockfish::Square sq = (Stockfish::Square)lsBit(m);
        if (pt == Knight)      out |= Stockfish::attacks_bb<Stockfish::KNIGHT>(sq, occ);
        else if (pt == Bishop) out |= Stockfish::attacks_bb<Stockfish::BISHOP>(sq, occ);
        else if (pt == Rook)   out |= Stockfish::attacks_bb<Stockfish::ROOK>(sq, occ);
        else if (pt == Queen)  out |= Stockfish::attacks_bb<Stockfish::BISHOP>(sq, occ)
                                    | Stockfish::attacks_bb<Stockfish::ROOK>(sq, occ);
    }
    return out;
}
//Upstream's StateInfo check data, for the side to move against the enemy king: the squares each
//piece type would check from, and our pieces that block one of our own sliders (discoveries).
struct CheckInfo {
    uint64_t chk[6];
    uint64_t blockers;
    int      eksq;
    bool     valid;
};
static CheckInfo check_info(const Board& b) {
    CheckInfo ci{};
    const int stm = (int)b.sideToMove;
    const uint64_t kbb = b.pieceTypes[King - 1] & b.side[1 - stm];
    if (!kbb) { ci.valid = false; return ci; }
    ci.valid = true;
    ci.eksq = lsBit(kbb);
    const auto e = (Stockfish::Square)ci.eksq;
    const uint64_t occ = b.side[ColorWhite] | b.side[ColorBlack];
    ci.chk[Pawn - 1] = (stm == ColorWhite)
        ? (((kbb & ~FILE_H_BB) >> 7) | ((kbb & ~FILE_A_BB) >> 9))
        : (((kbb & ~FILE_A_BB) << 7) | ((kbb & ~FILE_H_BB) << 9));
    ci.chk[Knight - 1] = Stockfish::attacks_bb<Stockfish::KNIGHT>(e);
    ci.chk[Bishop - 1] = Stockfish::attacks_bb<Stockfish::BISHOP>(e, occ);
    ci.chk[Rook - 1]   = Stockfish::attacks_bb<Stockfish::ROOK>(e, occ);
    ci.chk[Queen - 1]  = ci.chk[Bishop - 1] | ci.chk[Rook - 1];
    ci.chk[King - 1]   = 0;
    uint64_t snipers = (Stockfish::attacks_bb<Stockfish::ROOK>(e, 0)
                        & (b.pieceTypes[Rook - 1] | b.pieceTypes[Queen - 1]))
                     | (Stockfish::attacks_bb<Stockfish::BISHOP>(e, 0)
                        & (b.pieceTypes[Bishop - 1] | b.pieceTypes[Queen - 1]));
    snipers &= b.side[stm];
    const uint64_t rest = occ ^ snipers;
    while (snipers) {
        const int sq = lsBit(snipers);
        snipers &= snipers - 1;
        const uint64_t btw = Stockfish::between_bb(e, (Stockfish::Square)sq) & rest;
        if (btw && !Stockfish::more_than_one(btw)) ci.blockers |= btw;
    }
    return ci;
}
//Position::gives_check(): direct checks, discoveries, promotions, en passant and castling.
static inline bool gives_check_ci(const Board& b, const CheckInfo& ci, const Mv& m) {
    if (!ci.valid) return false;
    const int pt = (m.promo >= Knight && m.promo <= Queen)
                   ? (int)m.promo : (int)PC_TYPE((Piece)b.piecesOnSquares[m.src]);
    if (m.promo >= Knight && m.promo <= Queen) {
        const uint64_t occ = ((b.side[ColorWhite] | b.side[ColorBlack]) & ~(1ULL << m.src));
        const auto d = (Stockfish::Square)m.dst;
        uint64_t att = 0;
        switch ((int)m.promo) {
            case Knight: att = Stockfish::attacks_bb<Stockfish::KNIGHT>(d);      break;
            case Bishop: att = Stockfish::attacks_bb<Stockfish::BISHOP>(d, occ); break;
            case Rook:   att = Stockfish::attacks_bb<Stockfish::ROOK>(d, occ);   break;
            default:     att = Stockfish::attacks_bb<Stockfish::BISHOP>(d, occ)
                             | Stockfish::attacks_bb<Stockfish::ROOK>(d, occ);   break;
        }
        if ((att >> ci.eksq) & 1ULL) return true;
    }
    else if (pt >= Pawn && pt <= King && ((ci.chk[pt - 1] >> m.dst) & 1ULL)) return true;
    if ((ci.blockers >> m.src) & 1ULL) {
        const uint64_t ln = Stockfish::line_bb((Stockfish::Square)m.src, (Stockfish::Square)ci.eksq);
        if (!((ln >> m.dst) & 1ULL)) return true;
    }
    const int stm = (int)b.sideToMove;
    const auto e  = (Stockfish::Square)ci.eksq;
    const uint64_t occ0 = b.side[ColorWhite] | b.side[ColorBlack];
    const int mpt = (int)PC_TYPE((Piece)b.piecesOnSquares[m.src]);
    if (mpt == Pawn && (m.src & 7) != (m.dst & 7) && b.piecesOnSquares[m.dst] == PieceNone) {
        const int capsq = (m.dst & 7) | (m.src & 56);
        const uint64_t occ = (occ0 & ~(1ULL << m.src) & ~(1ULL << capsq)) | (1ULL << m.dst);
        const uint64_t qr = (b.pieceTypes[Queen - 1] | b.pieceTypes[Rook - 1]) & b.side[stm];
        const uint64_t qb = (b.pieceTypes[Queen - 1] | b.pieceTypes[Bishop - 1]) & b.side[stm];
        if ((Stockfish::attacks_bb<Stockfish::ROOK>(e, occ) & qr)
            || (Stockfish::attacks_bb<Stockfish::BISHOP>(e, occ) & qb))
            return true;
    }
    if (mpt == King && ((m.src > m.dst) ? (m.src - m.dst) : (m.dst - m.src)) == 2) {
        const int rto = (m.dst > m.src) ? (m.dst - 1) : (m.dst + 1);
        const uint64_t occ = (occ0 & ~(1ULL << m.src)) | (1ULL << m.dst);
        if ((Stockfish::attacks_bb<Stockfish::ROOK>((Stockfish::Square)rto, occ) >> ci.eksq) & 1ULL)
            return true;
    }
    return false;
}
//=== SYZYGY IN THE TREE, upstream's Step 5 ===
static unsigned tb_cardinality = 0;      //min(SyzygyProbeLimit, TB_LARGEST); 0 when no tables
static int  tb_probe_depth = 1;          //SyzygyProbeDepth
static bool tb_rule50 = true;            //Syzygy50MoveRule

//=== THE MOVE PICKER, upstream's MovePicker ===
//What kingMoves() computes once per node: the legal king moves, our pinned pieces, the checkers.
//The picker generates PSEUDO-LEGAL moves stage by stage and the search tests legality when a move
//is yielded, as upstream does: a node that cuts off on its table move or its first capture never
//generates, scores or sorts the rest.
struct NodeInfo { uint64_t kmoves = 0, pinned = 0, checkers = 0; Square ksq = SquareNone; };

enum PickStage {
    MAIN_TT, CAPTURE_INIT, GOOD_CAPTURE, QUIET_INIT, GOOD_QUIET, BAD_CAPTURE, BAD_QUIET,
    EVASION_TT, EVASION_INIT, EVASION,
    PROBCUT_TT, PROBCUT_INIT, PROBCUT,
    QSEARCH_TT, QCAPTURE_INIT, QCAPTURE
};
struct PickerState {
    const NodeInfo * ni = nullptr;
    const CheckInfo * ci = nullptr;   //the node's check squares, for the quiet scorer
    Mv tt{0, 0, 0};
    int stage = MAIN_TT;
    int depth = 0, ply = 0, threshold = 0;
    bool skip_quiets = false;
    Mv moves[MAX_MOVES];
    int val[MAX_MOVES];
    int cur = 0, end_cur = 0, end_bad = 0, end_caps = 0, end_gen = 0;

    bool is_tt(const Mv& m) const { return (tt.src || tt.dst) && same(m, tt); }
};

//reductions[] and reduction(), in 1024ths of a ply.
static int red_tab[MAX_MOVES + MAX_PLY];
static void init_reductions() {
    for (int i = 0; i < MAX_MOVES + MAX_PLY; ++i)
        red_tab[i] = (int)(2747 / 128.0 * std::log((double)(i < 1 ? 1 : i)));
}
static inline int sf_red1024(bool improving, int d, int mn, int delta, int rdelta) {
    const int scale = red_tab[d] * red_tab[std::min(mn, MAX_MOVES + MAX_PLY - 1)];
    return scale - delta * 608 / std::max(1, rdelta) + (improving ? 0 : scale * 238 / 512) + 1182;
}

//==================================================================================================
//THE ENGINE LAYER: threads, the root, and UCI.
//==================================================================================================
#include <pthread.h>
#include "uci_engine.h"
#include "json.hpp"
#include <curl/curl.h>

//The root, parsed once on the protocol side and copied by every thread: fen2board() rebuilds
//libchess's process-wide castling tables, which must not happen under a running search.
static std::string root_fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
static Board root_board;
static int root_move_number = 1;
static std::atomic<bool> ponder_flag{false};
static bool infinite_flag = false;
static int go_mate = 0;
static std::chrono::steady_clock::time_point search_start;
static long long elapsed_ms() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - search_start).count();
}

//A score as UCI shows it (Stockfish's Score and format_score): centipawns through the win-rate
//model, a tablebase result as 20000 less its distance, a mate in moves.
static std::string format_score(int v, const Board& b) {
    if (!is_decisive(v)) return "cp " + std::to_string((int)Stockfish::Eval::to_cp(v, b));
    if (std::abs(v) <= VALUE_TB) {
        const int dist = VALUE_TB - std::abs(v);
        return "cp " + std::to_string(v > 0 ? 20000 - dist : -20000 + dist);
    }
    const int plies = v > 0 ? MATE_CP - v : -(MATE_CP + v);
    return "mate " + std::to_string((plies > 0 ? plies + 1 : plies) / 2);
}

//What each thread hands back: upstream's rootMoves[0] and completedDepth.
struct ThreadResult {
    bool have = false;
    Mv best{0, 0, 0};
    int score = -MATE_CP - 1;
    int depth = 0;
    int pvlen = 0;
    Mv pv[MAX_PLY];
};
static ThreadResult results[MAX_THREADS];

//get_best_thread(): a vote by score and completed depth, with proven wins and losses preferred
//by distance.
static int best_thread(int nthreads) {
    int best = 0;
    if (nthreads <= 1) return 0;
    int min_score = MATE_CP + 1;
    for (int t = 0; t < nthreads; ++t) if (results[t].have) min_score = std::min(min_score, results[t].score);
    const auto voting = [&](int t) {
        return (long long)(results[t].score - min_score + 14) * results[t].depth;
    };
    std::vector<std::pair<int, long long>> votes;   //move key, vote
    const auto key = [](const Mv& m) { return (int)m.src | ((int)m.dst << 6) | ((int)m.promo << 12); };
    const auto vote_of = [&](int k) -> long long& {
        for (auto& p : votes) if (p.first == k) return p.second;
        votes.push_back({k, 0});
        return votes.back().second;
    };
    for (int t = 0; t < nthreads; ++t) if (results[t].have) vote_of(key(results[t].best)) += voting(t);
    for (int t = 0; t < nthreads; ++t) {
        if (!results[t].have) continue;
        if (!results[best].have) { best = t; continue; }
        const int bs = results[best].score, ns = results[t].score;
        const long long bv = vote_of(key(results[best].best)), nv = vote_of(key(results[t].best));
        const bool b_win = is_win(bs), n_win = is_win(ns);
        const bool b_loss = bs != -MATE_CP - 1 && is_loss(bs);
        const bool n_loss = ns != -MATE_CP - 1 && is_loss(ns);
        const bool better = voting(t) * (results[t].pvlen > 2 ? 1 : 0)
                          > voting(best) * (results[best].pvlen > 2 ? 1 : 0);
        if (b_win) { if (ns > bs) best = t; }
        else if (b_loss) { if (n_loss && ns < bs) best = t; }
        else if (n_win || n_loss || (!is_loss(ns) && (nv > bv || (nv == bv && better)))) best = t;
    }
    return best;
}

//=== THE WORKER: one per search thread, as upstream's Search::Worker ===
//Everything a search writes lives here, reached through `this`. Declared thread_local, the
//same state cost a thread-local lookup on every access -- 8% of the search's time on macOS.
struct Worker {
    Board board;
    NNUEContext ctx;
    ZobristHash zh;
    //=== PER-SEARCH STATE, one copy per thread ===
    int seldepth = 0;
    int root_delta = 1;        //upstream's rootDelta, the root window width
    int nmp_min_ply = 0;       //upstream's nmpMinPly
    int root_depth = 0;        //upstream's rootDepth
    //The root's TT move is upstream's rootMoves[0].pv[0], never the table's: the first legal move at
    //depth 1, then the best move of each root search that was not stopped (upstream's stable_sort).
    Mv root_first{0, 0, 0};
    bool have_root_first = false;
    //Whether a root move has been scored in the current iteration, and with what: upstream's
    //rootMoves[0] after an interrupted iteration.
    bool root_scored = false;
    int root_scored_v = 0;
    int tt_move_hist = 0;      //upstream's ttMoveHistory
    int root_optimism = 0;     //optimism[us], in Stockfish units
    int root_stm = 0;
    long long nodes_sf = 0;    //moves made, which is what upstream calls a node
    long long tbhits = 0;
    RootHist root_hist[MAX_MOVES];
    int root_hist_n = 0;
    int root_avg = -MATE_CP - 1;
    long long root_msq = 0;
    int  ss_eval[MAX_PLY];
    int  ss_cutoff_cnt[MAX_PLY + 4];     //ss->cutoffCnt
    int  ss_rep[MAX_PLY + 4];            //st->repetition
    int  ss_pfn[MAX_PLY];                //st->pliesFromNull; -1 when no null move
    bool next_is_null = false;
    //The move made at each ply: the moved piece (the pawn on a promotion) for the continuation and
    //history keys, the piece that lands (piece_on(to)) for the readers that ask the board after it.
    //Index 12 is "no move", the null move and the plies before the root alike.
    uint8_t ss_mv_pc[MAX_PLY], ss_mv_onto[MAX_PLY], ss_mv_to[MAX_PLY];
    uint8_t ss_mv_from[MAX_PLY], ss_mv_promo[MAX_PLY];
    uint16_t ss_mv_count[MAX_PLY];       //ss->moveCount
    int  ss_stat[MAX_PLY];               //ss->statScore
    int8_t ss_cap_type[MAX_PLY + 4];     //captured_piece() - 1 on entry; -1 none
    bool ss_ttpv[MAX_PLY + 4];           //ss->ttPv
    bool ss_incheck[MAX_PLY + 4];        //ss->inCheck
    bool ss_tthit[MAX_PLY];              //ss->ttHit
    int  ss_reduction[MAX_PLY + 4];      //ss->reduction
    //Whether the move made at ply p-1 was capture_stage, as the destination square or 64: the
    //`capture` index of the continuation table that move selects.
    uint8_t prev_cap_sq[MAX_PLY];
    Mv   sing_excl[MAX_PLY];             //ss->excludedMove
    bool sing_excl_set[MAX_PLY];
    uint64_t path_hash[MAX_PLY + 8];
    Mv  pv_line[MAX_PLY][MAX_PLY];
    int pv_len[MAX_PLY];
    int16_t * cont_rows[MAX_PLY][CONT_LEVELS];   //each ply's continuation rows, by level
    CorrKeys ss_keys[MAX_PLY + 4];         //pawn, minor and non-pawn keys, per ply
    uint64_t key_pre[MAX_PLY][8];          //the bitboards before the move made at each ply
    int history[2][MVIDX_N];
    int16_t cap_hist[12][64][7];               //[piece][to][captured type, 0 none]
    int16_t low_ply_hist[LOW_PLY_SIZE][MVIDX_N];
    //ONE continuation table, as upstream has: continuationHistory[inCheck][capture][piece][to] is the
    //row a move selects, and every level reads the row its own earlier move selected. A move one ply
    //back and the same move four plies back therefore share their entries.
    int16_t cont_hist[2][2][13][64][13][64];
    int16_t corr_cont[13][64][13][64];         //continuationCorrectionHistory
    int my_tid = 0;
    unsigned check_calls = 0;
    bool ctx_ready = false;

    inline RootHist * root_hist_find(const Mv& m) {
        for (int i = 0; i < root_hist_n; ++i)
            if (same(root_hist[i].m, m)) return &root_hist[i];
        if (root_hist_n >= MAX_MOVES) return nullptr;
        RootHist * r = &root_hist[root_hist_n++];
        r->m = m; r->avg = -MATE_CP - 1; r->msq = 0; r->used = false;
        return r;
    }

    inline int hist_pc(int stm, uint8_t src) {
        return stm * 6 + (int)PC_TYPE((Piece)board.piecesOnSquares[src]) - 1;
    }

    //libchess's Zobrist state carries no pawn key, so one is mixed from the pawn bitboards.
    //The pawn-history index: a mix of the two pawn bitboards, a few operations and so not worth
    //keeping incrementally. (Indexing by the Zobrist pawn key instead changed which structures
    //share a slot, and with it the tree.)
    //inline uint64_t pawn_key(int ply) {
    inline uint64_t pawn_key() {
        //(void)ply; //to avoid compiler warning of unused variable
        uint64_t k = board.pieceTypes[Pawn - 1] & board.side[ColorWhite];
        const uint64_t b = board.pieceTypes[Pawn - 1] & board.side[ColorBlack];
        k = k * 0x9E3779B97F4A7C15ULL ^ (b + 0x165667B19E3779F9ULL);
        k ^= k >> 29; k *= 0xBF58476D1CE4E5B9ULL; k ^= k >> 32;
        return k;
    }

    //(ss - level - 1)->continuationHistory, as a row pointer. Set once per node by enter_node():
    //a node's rows depend only on moves made at earlier plies, which stay fixed while it is active.
    inline void set_cont_rows(int ply) {
        for (int level = 0; level < CONT_LEVELS; ++level) {
            const int q = ply - (level + 1);        //the ply at which that earlier move was made
            if (q < 0 || ss_mv_pc[q] == 12) { cont_rows[ply][level] = &cont_hist[0][0][12][0][0][0]; continue; }   //upstream's sentinel
            const int pchk = ss_incheck[q] ? 1 : 0;
            const int pcap = (q + 1 < MAX_PLY && prev_cap_sq[q + 1] != 64) ? 1 : 0;
            cont_rows[ply][level] = &cont_hist[pchk][pcap][ss_mv_pc[q]][ss_mv_to[q]][0][0];
        }
    }
    inline int16_t * cont_slot(int level, int ply, int pc, int to) {
        return cont_rows[ply][level] + pc * 64 + to;
    }

    inline int stat_score(int ply, int pc, uint8_t to, uint8_t from, uint8_t promo, int stm) {
        if (pc < 0 || pc > 11) return 0;
        return 2 * history[stm][mv_idx(from, to, promo)]
             + (int)*cont_slot(0, ply, pc, to)
             + (int)*cont_slot(1, ply, pc, to);
    }

    //update_continuation_histories: the first two levels only when in check, and only levels whose
    //move exists (is_ok: not before the root, not a null move).
    inline void cont_update_pc(int ply, int pc, uint8_t to, int bonus) {
        if (pc < 0 || pc > 11) return;
        const bool chk = (ply >= 0 && ply < MAX_PLY) ? ss_incheck[ply] : false;
        for (int L = 0; L < CONT_LEVELS; ++L) {
            if (chk && L > 1) break;
            const int q = ply - (L + 1);
            if (q < 0 || ss_mv_pc[q] == 12) continue;
            stat_add(*cont_slot(L, ply, pc, to), bonus * cont_w[L] / 1024 + (L == 0 ? 88 : 0), CONT_LIMIT);
        }
    }

    //update_quiet_histories.
    inline void quiet_hist_update(int ply, uint8_t from, uint8_t to, uint8_t promo,
                                         int pc, int stm, int bonus) {
        if (pc < 0 || pc > 11) return;
        stat_add(history[stm][mv_idx(from, to, promo)], bonus, HIST_LIMIT);
        if (ply >= 0 && ply < LOW_PLY_SIZE)
            stat_add(low_ply_hist[ply][mv_idx(from, to, promo)], bonus * 805 / 1024, 7183);
        cont_update_pc(ply, pc, to, bonus * 896 / 1024);
        stat_add(pawn_entry(pawn_key(), pc, to), bonus * (bonus > 0 ? 905 : 505) / 1024, 8192);
    }

    //The continuation rows: piece_on(to) of the move one ply back, against the moves two and four back.
    inline void corr_cont_idx(int ply, int& p1, int& t1, int& p2, int& t2, int& p4, int& t4) {
        p1 = ply >= 1 ? ss_mv_onto[ply - 1] : 12; t1 = ply >= 1 ? ss_mv_to[ply - 1] : 0;
        p2 = ply >= 2 ? ss_mv_pc[ply - 2] : 12;   t2 = ply >= 2 ? ss_mv_to[ply - 2] : 0;
        p4 = ply >= 4 ? ss_mv_pc[ply - 4] : 12;   t4 = ply >= 4 ? ss_mv_to[ply - 4] : 0;
    }

    //correction_value(): upstream substitutes 8 when there is no previous move.
    inline int corr_value(const CorrKeys& k, int stm, int ply) {
        const auto rd = [](const AI16& e) { return (int)e.load(std::memory_order_relaxed); };
        int cv = 10347 * rd(corr_entry(k.pawn, stm).pawn) + 8821 * rd(corr_entry(k.minor, stm).minor)
               + 11665 * (rd(corr_entry(k.np[0], stm).npw) + rd(corr_entry(k.np[1], stm).npb));
        int p1, t1, p2, t2, p4, t4;
        corr_cont_idx(ply, p1, t1, p2, t2, p4, t4);
        const int cnt = (ply >= 1 && p1 != 12)
                      ? (int)corr_cont[p2][t2][p1][t1] + (int)corr_cont[p4][t4][p1][t1] : 8;
        return cv + 7841 * cnt;
    }

    inline void corr_update(const CorrKeys& k, int stm, int ply, int bonus) {
        stat_add(corr_entry(k.pawn,  stm).pawn,  bonus,             CORR_LIMIT);
        stat_add(corr_entry(k.minor, stm).minor, bonus * 156 / 128, CORR_LIMIT);
        stat_add(corr_entry(k.np[0], stm).npw,   bonus * 178 / 128, CORR_LIMIT);
        stat_add(corr_entry(k.np[1], stm).npb,   bonus * 178 / 128, CORR_LIMIT);
        int p1, t1, p2, t2, p4, t4;
        corr_cont_idx(ply, p1, t1, p2, t2, p4, t4);
        if (ply >= 1 && p1 != 12) {
            stat_add(corr_cont[p2][t2][p1][t1], bonus * 127 / 128, CORR_LIMIT);
            stat_add(corr_cont[p4][t4][p1][t1], bonus *  59 / 128, CORR_LIMIT);
        }
    }

    //Worker::clear(), for this thread's tables. ucinewgame only: the tables are statistics about the
    //game and carry from move to move.
    void clear_thread_histories() {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < MVIDX_N; ++i) history[c][i] = MAIN_HIST_DEFAULT;
        std::fill(&cap_hist[0][0][0], &cap_hist[0][0][0] + sizeof cap_hist / sizeof(int16_t), (int16_t)-689);
        std::fill(&cont_hist[0][0][0][0][0][0],
                  &cont_hist[0][0][0][0][0][0] + sizeof cont_hist / sizeof(int16_t), (int16_t)-529);
        std::fill(&corr_cont[0][0][0][0],
                  &corr_cont[0][0][0][0] + sizeof corr_cont / sizeof(int16_t), (int16_t)8);
        tt_move_hist = 0;
    }

    //At the start of every search (iterative_deepening): lowPlyHistory refilled, mainHistory pulled
    //a quarter of the way back toward its default.
    void age_thread_histories() {
        std::fill(&low_ply_hist[0][0], &low_ply_hist[0][0] + sizeof low_ply_hist / sizeof(int16_t), (int16_t)97);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < MVIDX_N; ++i)
                history[c][i] = (history[c][i] - MAIN_HIST_DEFAULT) * 3 / 4 + MAIN_HIST_DEFAULT;
    }

    //value_draw(): upstream's draw score, nudged by the node count so repetition lines do not all look
    //alike to the search.
    inline int value_draw() { return -1 + (int)(nodes_sf & 0x2); }
    inline bool timed_out() {
        if (out_of_time.load(std::memory_order_relaxed)) return true;
        thread_nodes[my_tid].store(nodes_sf, std::memory_order_relaxed);
        ++check_calls;
        if (fixnodes > 0) {
            //One thread compares its own count at every node, which is exact; several sum the others'
            //published counts every 64 nodes.
            const long long n = search_threads == 1 ? nodes_sf
                              : ((check_calls & 63) == 0 ? total_nodes() : 0);
            if (n >= fixnodes) { out_of_time.store(true); return true; }
        }
        if ((check_calls & 1023) == 0 && !unlimited.load(std::memory_order_relaxed)
            && now_ns() >= deadline_ns.load(std::memory_order_relaxed))
            out_of_time.store(true);
        return out_of_time.load(std::memory_order_relaxed);
    }

    inline int tt_from_store(int v, int ply) {
        const int r50c = (int)board.halfmoveClock;
        if (is_win(v)) {
            if (v >= MATE_IN_MAX && MATE_CP - v > 100 - r50c) return TB_WIN_IN_MAX - 1;
            if (VALUE_TB - v > 100 - r50c) return TB_WIN_IN_MAX - 1;
            return v - ply;
        }
        if (is_loss(v)) {
            if (v <= -MATE_IN_MAX && MATE_CP + v > 100 - r50c) return -TB_WIN_IN_MAX + 1;
            if (VALUE_TB + v > 100 - r50c) return -TB_WIN_IN_MAX + 1;
            return v + ply;
        }
        return v;
    }

    //The key d plies before ply `ply`: the search path first, then the game.
    inline bool hist_key(int ply, int d, uint64_t& out) {
        if (d < 0) return false;
        if (d <= ply) { out = path_hash[ply - d]; return true; }
        const size_t back = (size_t)(d - ply);
        if (back > game_hashes.size()) return false;
        out = game_hashes[game_hashes.size() - back];
        return true;
    }

    inline int hist_rep(int ply, int d) {
        if (d <= ply) return ss_rep[ply - d];
        const size_t back = (size_t)(d - ply);
        return back <= game_rep.size() ? game_rep[game_rep.size() - back] : 0;
    }

    //st->repetition for the position at `ply`, computed at entry instead of in do_move: 0, or the
    //distance to the earlier occurrence, negative when that one was itself a repetition. The walk is
    //bounded by min(rule50, pliesFromNull), as upstream's is.
    inline void set_repetition(int ply) {
        ss_rep[ply] = 0;
        const int end = std::min((int)board.halfmoveClock, ss_pfn[ply]);
        for (int i = 4; i <= end; i += 2) {
            uint64_t k = 0;
            if (!hist_key(ply, i, k)) break;
            if (k == zh.hash) { ss_rep[ply] = hist_rep(ply, i) ? -i : i; break; }
        }
    }

    //is_repetition(): a repetition inside the search, or a third occurrence through the game.
    inline bool repeated(int ply) {
        return ply < MAX_PLY && ss_rep[ply] != 0 && ss_rep[ply] < ply;
    }

    bool upcoming_repetition(int ply) {
        const int end = std::min((int)board.halfmoveClock, ss_pfn[ply]);
        if (end < 3) return false;
        uint64_t cur = 0, prev = 0;
        if (!hist_key(ply, 0, cur) || !hist_key(ply, 1, prev)) return false;
        uint64_t other = cur ^ prev ^ z.blackMove;
        for (int i = 3; i <= end; i += 2) {
            uint64_t k1 = 0, back = 0;
            if (!hist_key(ply, i - 1, k1) || !hist_key(ply, i, back)) return false;
            other ^= k1 ^ back ^ z.blackMove;
            if (other != 0) continue;
            const uint64_t move_key = cur ^ back;
            int j = cuckoo_h1(move_key);
            if (!(cuckoo_used[j] && cuckoo_key[j] == move_key)) {
                j = cuckoo_h2(move_key);
                if (!(cuckoo_used[j] && cuckoo_key[j] == move_key)) continue;
            }
            const int s1 = cuckoo_s1[j], s2 = cuckoo_s2[j];
            const uint64_t occ = board.side[ColorWhite] | board.side[ColorBlack];
            if ((Stockfish::between_bb((Stockfish::Square)s1, (Stockfish::Square)s2) ^ (1ULL << s2)) & occ)
                continue;
            if (ply > i) return true;
            //At or before the root the move must repeat a position, not reach this one.
            if (hist_rep(ply, i) != 0) return true;
        }
        return false;
    }

    //=== MOVE GENERATION, in upstream's MoveList order ===
    //Pawns by direction (pushes, double pushes, promotions right, left and straight, captures right
    //and left, en passant), each group by destination; then knights to queens; the king last.
    int generate(Mv * out, bool captures_only) {
        int n = 0;
        auto push = [&](uint8_t a, uint8_t b, uint8_t c) { if (n < MAX_MOVES) out[n++] = {a, b, c}; };
        auto [kmoves, pinned, pinning, checkers, ksq] = kingMoves(board);
        const uint64_t them = board.side[1 - board.sideToMove];
        const auto push_king = [&]() {
            for (uint64_t m = kmoves; m; m &= m - 1) {
                const Square d = (Square)lsBit(m);
                if (captures_only && !((them >> d) & 1ULL)) continue;
                push((uint8_t)ksq, (uint8_t)d, (uint8_t)PieceTypeNone);
            }
        };
        if (bitCount(checkers) > 1) { push_king(); return n; }
        auto [cm, em] = checkers ? checkMask(board, ksq, checkers) : std::make_pair(~0ULL, 0ULL);
        {
            const int up = (board.sideToMove == ColorWhite) ? 8 : -8;
            struct PD { uint8_t src, dst; };
            PD p1[16], p2[16], prR[16], prL[16], prP[16], cR[16], cL[16], ep[16];
            int n1 = 0, n2 = 0, nR = 0, nL = 0, nP = 0, ncR = 0, ncL = 0, nep = 0;
            uint64_t occp = board.side[board.sideToMove] & board.pieceTypes[Pawn - 1];
            while (occp) {
                const Square src = (Square)lsBit(occp);
                uint64_t mvb = piece_moves(board, Pawn, src, ksq, pinned, pinning, cm, em);
                while (mvb) {
                    const Square dst = (Square)lsBit(mvb);
                    mvb &= mvb - 1;
                    const int diff = (int)dst - (int)src;
                    const bool occupied = ((them >> dst) & 1ULL) != 0;
                    Move pm = {}; pm.src = src; pm.dst = dst; pm.promoType = PieceTypeNone;
                    const bool is_promo = promoMove(board, pm);
                    PD e = { (uint8_t)src, (uint8_t)dst };
                    if (is_promo) {
                        if (diff == up)                      prP[nP++] = e;
                        else if (diff == up + 1)             prR[nR++] = e;
                        else                                 prL[nL++] = e;
                    } else if (diff == up)                   p1[n1++] = e;
                    else if (diff == up + up)                p2[n2++] = e;
                    else if (!occupied)                      ep[nep++] = e;
                    else if (diff == up + 1)                 cR[ncR++] = e;
                    else                                     cL[ncL++] = e;
                }
                occp &= occp - 1;
            }
            auto by_dst = [](PD * a, int k) {
                for (int i = 1; i < k; ++i) {
                    const PD t = a[i]; int j = i - 1;
                    while (j >= 0 && a[j].dst > t.dst) { a[j + 1] = a[j]; --j; }
                    a[j + 1] = t;
                }
            };
            by_dst(p1, n1); by_dst(p2, n2); by_dst(prR, nR); by_dst(prL, nL); by_dst(prP, nP);
            by_dst(cR, ncR); by_dst(cL, ncL); by_dst(ep, nep);
            auto emit_plain = [&](PD * a, int k) {
                for (int i = 0; i < k; ++i)
                    if (!captures_only || ((them >> a[i].dst) & 1ULL))
                        push(a[i].src, a[i].dst, (uint8_t)PieceTypeNone);
            };
            auto emit_promo = [&](PD * a, int k) {
                for (int i = 0; i < k; ++i) {
                    push(a[i].src, a[i].dst, (uint8_t)Queen);
                    push(a[i].src, a[i].dst, (uint8_t)Rook);
                    push(a[i].src, a[i].dst, (uint8_t)Bishop);
                    push(a[i].src, a[i].dst, (uint8_t)Knight);
                }
            };
            emit_plain(p1, n1);
            emit_plain(p2, n2);
            emit_promo(prR, nR); emit_promo(prL, nL); emit_promo(prP, nP);
            emit_plain(cR, ncR); emit_plain(cL, ncL);
            for (int i = 0; i < nep; ++i) push(ep[i].src, ep[i].dst, (uint8_t)PieceTypeNone);
        }
        Move move = {}; move.promoType = PieceTypeNone;
        for (PieceType pt = Knight; pt <= Queen; ++pt) {
            uint64_t occ = board.side[board.sideToMove] & board.pieceTypes[pt - 1];
            while (occ) {
                move.src = (Square)lsBit(occ);
                uint64_t mvb = piece_moves(board, pt, move.src, ksq, pinned, pinning, cm, em);
                while (mvb) {
                    move.dst = (Square)lsBit(mvb);
                    if (!captures_only || ((them >> move.dst) & 1ULL))
                        push((uint8_t)move.src, (uint8_t)move.dst, (uint8_t)PieceTypeNone);
                    mvb &= mvb - 1;
                }
                occ &= occ - 1;
            }
        }
        push_king();
        return n;
    }

    //=== CHECKS ===
    inline bool stm_in_check() {
        const uint64_t k = board.side[board.sideToMove] & board.pieceTypes[King - 1];
        if (!k) return false;
        return getCheckers(board, (Square)lsBit(k)) != 0;
    }

    //=== MOVE PREDICATES ===
    inline int victim_value(const Mv& m) {
        const int p = board.piecesOnSquares[m.dst];
        return p == PieceNone ? 0 : pieceValueSF[p & 7];
    }

    inline bool is_ep(const Mv& m) {
        const uint64_t pawnRanks[] = { RANK5, RANK4 }; 
        const int p = board.piecesOnSquares[m.src];
        return board.enPassant != FileNone 
            && (p & 7) == Pawn 
            && board.piecesOnSquares[m.dst] == PieceNone
            && (SQ_BIT(m.src) & pawnRanks[board.sideToMove])
            && ((m.src ^ m.dst) & 7) != 0;
    }

    //King takes own rook is how libchess encodes Chess960 castling.
    inline bool is_castle(const Mv& m) {
        const int p = board.piecesOnSquares[m.src];
        if (p == PieceNone || (p & 7) != King) return false;
        const int df = (int)(m.dst & 7) - (int)(m.src & 7);
        if (df >= 2 || df <= -2) return true;
        const int q = board.piecesOnSquares[m.dst];
        return q != PieceNone && (q & 7) == Rook && ((q ^ p) & 8) == 0;
    }

    //capture_stage(): captures, queen promotions and en passant.
    inline bool cap_stage(const Mv& m) {
        if (is_castle(m)) return false;
        return victim_value(m) != 0 || m.promo == Queen || is_ep(m);
    }

    //type_of(captured_piece()) - 1: the pawn for en passant, -1 for none.
    inline int cap_type(const Mv& m) {
        if (is_ep(m)) return Pawn - 1;
        const int p = board.piecesOnSquares[m.dst];
        if (p == PieceNone || is_castle(m)) return -1;
        return (int)PC_TYPE((Piece)p) - 1;
    }

    //type_of(piece_on(to)), 0 for an empty square: the capture-history slot the move picker reads.
    inline int cap_slot_to(const Mv& m) {
        const int p = board.piecesOnSquares[m.dst];
        return (p == PieceNone) ? 0 : (int)PC_TYPE((Piece)p);
    }

    //Position::see_ge(): does the exchange on m.dst win at least `threshold`? Upstream's
    //algorithm, which stops as soon as the answer is known instead of computing the whole
    //exchange. A promotion, en passant or castling counts as 0, and the king as worth nothing.
    //Pieces pinned to their own king may not join the exchange while a pinner remains.
    uint64_t see_key = 0;
    bool see_key_set = false;
    uint64_t see_blockers[2] = {0, 0}, see_pinners[2] = {0, 0};   //see_pinners[c]: c's sliders pinning ~c's pieces
    //set_check_info()'s blockersForKing and pinners, for both colours, once per position.
    void see_info() {
        if (see_key_set && see_key == zh.hash) return;
        see_key = zh.hash; see_key_set = true;
        const uint64_t occ0 = board.side[ColorWhite] | board.side[ColorBlack];
        for (int c = 0; c < 2; ++c) {
            see_blockers[c] = see_pinners[c ^ 1] = 0;
            const uint64_t kbb = board.side[c] & board.pieceTypes[King - 1];
            if (!kbb) continue;
            const auto ksq = (Stockfish::Square)lsBit(kbb);
            uint64_t snipers =
                ((Stockfish::attacks_bb<Stockfish::ROOK>(ksq, 0) & (board.pieceTypes[Rook - 1] | board.pieceTypes[Queen - 1]))
               | (Stockfish::attacks_bb<Stockfish::BISHOP>(ksq, 0) & (board.pieceTypes[Bishop - 1] | board.pieceTypes[Queen - 1])))
              & board.side[c ^ 1];
            const uint64_t occupancy = occ0 ^ snipers;
            while (snipers) {
                const auto sq = (Stockfish::Square)lsBit(snipers);
                snipers &= snipers - 1;
                const uint64_t b = Stockfish::between_bb(ksq, sq) & occupancy;
                if (b && !(b & (b - 1))) {
                    see_blockers[c] |= b;
                    if (b & board.side[c]) see_pinners[c ^ 1] |= 1ULL << sq;
                }
            }
        }
    }
    bool see_ge(const Mv& m, int threshold) {
        if (m.promo != PieceTypeNone || is_ep(m) || is_castle(m)) return 0 >= threshold;
        static const int pv[8] = { 0, 208, 781, 825, 1276, 2538, 0, 0 };   //upstream's PieceValue
        const int to = m.dst;
        int swap = pv[PC_TYPE((Piece)board.piecesOnSquares[to]) & 7] - threshold;
        if (swap < 0) return false;
        swap = pv[PC_TYPE((Piece)board.piecesOnSquares[m.src]) & 7] - swap;
        if (swap <= 0) return true;
        see_info();
        uint64_t occupied = (board.side[ColorWhite] | board.side[ColorBlack]) ^ (1ULL << m.src) ^ (1ULL << to);
        uint64_t attackers = attackers_to(board, (Square)to, occupied);
        const uint64_t bq = board.pieceTypes[Bishop - 1] | board.pieceTypes[Queen - 1];
        const uint64_t rq = board.pieceTypes[Rook - 1] | board.pieceTypes[Queen - 1];
        const auto tsq = (Stockfish::Square)to;
        int stm = (int)board.sideToMove;
        int res = 1;
        for (;;) {
            stm ^= 1;
            attackers &= occupied;
            uint64_t stm_att = attackers & board.side[stm];
            if (!stm_att) break;
            if (see_pinners[stm ^ 1] & occupied) {
                stm_att &= ~see_blockers[stm];
                if (!stm_att) break;
            }
            res ^= 1;
            uint64_t bb;
            if ((bb = stm_att & board.pieceTypes[Pawn - 1])) {
                if ((swap = 208 - swap) < res) break;
                occupied ^= bb & (0 - bb);
                attackers |= Stockfish::attacks_bb<Stockfish::BISHOP>(tsq, occupied) & bq;
            } else if ((bb = stm_att & board.pieceTypes[Knight - 1])) {
                if ((swap = 781 - swap) < res) break;
                occupied ^= bb & (0 - bb);
            } else if ((bb = stm_att & board.pieceTypes[Bishop - 1])) {
                if ((swap = 825 - swap) < res) break;
                occupied ^= bb & (0 - bb);
                attackers |= Stockfish::attacks_bb<Stockfish::BISHOP>(tsq, occupied) & bq;
            } else if ((bb = stm_att & board.pieceTypes[Rook - 1])) {
                if ((swap = 1276 - swap) < res) break;
                occupied ^= bb & (0 - bb);
                attackers |= Stockfish::attacks_bb<Stockfish::ROOK>(tsq, occupied) & rq;
            } else if ((bb = stm_att & board.pieceTypes[Queen - 1])) {
                if ((swap = 2538 - swap) < res) break;
                occupied ^= bb & (0 - bb);
                attackers |= (Stockfish::attacks_bb<Stockfish::BISHOP>(tsq, occupied) & bq)
                           | (Stockfish::attacks_bb<Stockfish::ROOK>(tsq, occupied) & rq);
            } else
                //The king takes last; it may not if the other side still attacks.
                return (attackers & ~board.side[stm]) ? res ^ 1 : res;
        }
        return res != 0;
    }

    inline int see_stage_threshold(int capscore) { return -capscore / 18; }
    inline int non_pawn_material(int stm) {
        const uint64_t np = board.side[stm] & ~board.pieceTypes[Pawn - 1] & ~board.pieceTypes[King - 1];
        int v = 0;
        for (int pt = Knight; pt <= Queen; ++pt) v += bitCount(np & board.pieceTypes[pt - 1]) * pieceValueSF[pt];
        return v;
    }

    //=== EVALUATION: Stockfish's own Value, with optimism for the side to move at the root ===
    inline int static_eval() {
        const int opt = board.sideToMove == root_stm ? root_optimism : -root_optimism;
        return evaluate_nnue_value(board, ctx, opt);
    }

    //WDL from the side to move's view, -2..2, or false if the position is not in the tables. Fathom
    //fails whenever rule50 != 0, which the caller has already required, and on castling rights.
    inline bool tb_probe_node(int& wdl) {
        const Square ep = legalEnPassantMove(board);
        const unsigned r = tb_probe_wdl(
            board.side[ColorWhite], board.side[ColorBlack], board.pieceTypes[King - 1],
            board.pieceTypes[Queen - 1], board.pieceTypes[Rook - 1], board.pieceTypes[Bishop - 1],
            board.pieceTypes[Knight - 1], board.pieceTypes[Pawn - 1],
            0, 0, ep == SquareNone ? 0 : (unsigned)ep,
            board.sideToMove == ColorWhite ? 1 : 0);
        if (r == TB_RESULT_FAILED) return false;
        wdl = (int)r - 2;
        return true;
    }

    inline NodeInfo node_info() {
        auto [km, pn, pg, ch, ks] = kingMoves(board);
        (void)pg;
        NodeInfo ni; ni.kmoves = km; ni.pinned = pn; ni.checkers = ch; ni.ksq = ks;
        return ni;
    }

    //Position::legal() for a pseudo-legal move. King moves come from the legal set, en passant (whose
    //discovered checks are irregular) asks the library, and a pinned piece must stay on its pin line.
    inline bool legal(const NodeInfo& ni, const Mv& m) {
        if (m.src == ni.ksq) return true; //king's move - legal from kingMoves()
        //En passant, as upstream's legal() tests it: with both pawns gone and the capturer on its
        //destination, no enemy slider may see the king -- a rook or queen along the rank, or a
        //bishop or queen along a diagonal the captured pawn was blocking. Outside check is the only
        //case that reaches here: evasions come from libchess's legal list and table moves are
        //validated by is_legal_move().
        if (is_ep(m)) {
            const int capsq = (m.dst & 7) | (m.src & 56);           //the captured pawn: dst's file, src's rank
            const uint64_t occ = ((board.side[ColorWhite] | board.side[ColorBlack])
                                  ^ SQ_BIT(m.src) ^ SQ_BIT(capsq)) | SQ_BIT(m.dst);
            const uint64_t them = board.side[1 - board.sideToMove];
            const auto k = (Stockfish::Square)ni.ksq;
            return !(Stockfish::attacks_bb<Stockfish::ROOK>(k, occ) & them & (board.pieceTypes[Rook - 1] | board.pieceTypes[Queen - 1]))
                && !(Stockfish::attacks_bb<Stockfish::BISHOP>(k, occ) & them & (board.pieceTypes[Bishop - 1] | board.pieceTypes[Queen - 1]));
        }
        if (!((ni.pinned >> m.src) & 1ULL)) return true;
        return (Stockfish::line_bb((Stockfish::Square)m.src, (Stockfish::Square)ni.ksq) >> m.dst) & 1ULL;
    }

    //MoveList<CAPTURES> and MoveList<QUIETS> when not in check, in upstream's order: pawn promotions
    //(captures right, left, then pushes), pawn captures right and left, en passant, knights to queens,
    //the king; quiets are pushes, double pushes, under-promotion pushes, the pieces, the king.
    int gen_pseudo(const NodeInfo& ni, Mv * out, bool captures) {
        int n = 0;
        const int stm = (int)board.sideToMove;
        const uint64_t us = board.side[stm], them = board.side[1 - stm];
        const uint64_t empty = ~(us | them);
        const uint64_t pawns = us & board.pieceTypes[Pawn - 1];
        //Pawn moves by bitboard shift, as upstream's generate_pawn_moves: each direction's
        //destinations come out in square order, and the source is the destination less the step.
        const bool white = stm == ColorWhite;
        const int up = white ? 8 : -8;
        const uint64_t rank8 = white ? 0xFF00000000000000ULL : 0x00000000000000FFULL;   //promotion rank
        const uint64_t rank3 = white ? 0x0000000000FF0000ULL : 0x0000FF0000000000ULL;   //after a double push's first step
        const auto shift = [&](uint64_t b, int d) { return d > 0 ? b << d : b >> -d; };
        //Toward the h-file is up + 1, toward the a-file up - 1; the source file must allow it.
        const uint64_t right = shift(pawns & ~FILE_H_BB, up + 1), left = shift(pawns & ~FILE_A_BB, up - 1);
        const uint64_t push1 = shift(pawns, up) & empty;
        auto emit = [&](uint64_t b, int step, std::initializer_list<uint8_t> promos) {
            for (; b; b &= b - 1) {
                const int dst = lsBit(b);
                for (uint8_t q : promos) if (n < MAX_MOVES) out[n++] = { (uint8_t)(dst - step), (uint8_t)dst, q };
            }
        };
        if (captures) {
            emit(right & them & rank8, up + 1, {Queen, Rook, Bishop, Knight});
            emit(left & them & rank8, up - 1, {Queen, Rook, Bishop, Knight});
            emit(push1 & rank8, up, {Queen});
            emit(right & them & ~rank8, up + 1, {PieceTypeNone});
            emit(left & them & ~rank8, up - 1, {PieceTypeNone});
            if (board.enPassant != FileNone) {
                const int epsq = (white ? 40 : 16) + (int)board.enPassant;
                const uint64_t epb = 1ULL << epsq;
                //The capturers in square order, as the generator met them.
                uint64_t from = (shift(epb & ~FILE_A_BB, -up - 1) | shift(epb & ~FILE_H_BB, -up + 1)) & pawns;
                for (; from; from &= from - 1)
                    if (n < MAX_MOVES) out[n++] = { (uint8_t)lsBit(from), (uint8_t)epsq, (uint8_t)PieceTypeNone };
            }
        } else {
            emit(push1 & ~rank8, up, {PieceTypeNone});
            emit(shift(push1 & rank3, up) & empty, up + up, {PieceTypeNone});
            emit(push1 & rank8, up, {Rook, Bishop, Knight});
        }
        const uint64_t target = captures ? them : empty;
        const uint64_t occ = us | them;
        for (PieceType pt = Knight; pt <= Queen; ++pt)
            for (uint64_t pcs = us & board.pieceTypes[pt - 1]; pcs; pcs &= pcs - 1) {
                const auto src = (Stockfish::Square)lsBit(pcs);
                uint64_t att = 0;
                switch (pt) {
                    case Knight: att = Stockfish::attacks_bb<Stockfish::KNIGHT>(src); break;
                    case Bishop: att = Stockfish::attacks_bb<Stockfish::BISHOP>(src, occ); break;
                    case Rook:   att = Stockfish::attacks_bb<Stockfish::ROOK>(src, occ); break;
                    default:     att = Stockfish::attacks_bb<Stockfish::BISHOP>(src, occ)
                                     | Stockfish::attacks_bb<Stockfish::ROOK>(src, occ); break;
                }
                for (att &= target; att; att &= att - 1)
                    if (n < MAX_MOVES) out[n++] = { (uint8_t)src, (uint8_t)lsBit(att), (uint8_t)PieceTypeNone };
            }
        //King moves, castling included, from the legal set: captures onto enemy pieces, the rest quiet.
        for (uint64_t km = ni.kmoves & (captures ? them : ~them); km; km &= km - 1)
            if (n < MAX_MOVES) out[n++] = { (uint8_t)ni.ksq, (uint8_t)lsBit(km), (uint8_t)PieceTypeNone };
        return n;
    }

    //The picker for the main search (depth > 0) and quiescence (depth 0). A table move must be legal
    //to be yielded; upstream asks pseudo_legal() here and legal() in the search, and asking legality
    //once is the same answer.
    void pick_init(PickerState& p, const NodeInfo& n, const Mv& ttm, bool have_tt, int d, int pl) {
        p.ni = &n; p.depth = d; p.ply = pl;
        const bool ok = have_tt && is_legal_move(board, to_move(ttm));
        if (ok) p.tt = ttm;
        p.stage = (n.checkers ? EVASION_TT : (d > 0 ? MAIN_TT : QSEARCH_TT)) + (ok ? 0 : 1);
    }

    //ProbCut's: captures with SEE at least `th`; a capture_stage table move first, with no SEE test.
    void pick_init_probcut(PickerState& p, const NodeInfo& n, const Mv& ttm, bool have_tt, int th) {
        p.ni = &n; p.threshold = th;
        const bool ok = have_tt && cap_stage(ttm) && is_legal_move(board, to_move(ttm));
        if (ok) p.tt = ttm;
        p.stage = PROBCUT_TT + (ok ? 0 : 1);
    }

    void pick_score_captures(PickerState& p, int from, int to) {
        const int stm = (int)board.sideToMove;
        for (int i = from; i < to; ++i) {
            const Mv& m = p.moves[i];
            const int pc = hist_pc(stm, m.src);
            p.val[i] = 7 * victim_value(m) + ((pc >= 0 && pc <= 11) ? (int)cap_hist[pc][m.dst][cap_slot_to(m)] : 0);
        }
    }

    void pick_score_quiets(PickerState& p, int from, int to) {
        const int stm = (int)board.sideToMove;
        const Color them = (Color)(1 - stm);
        uint64_t thr[6];                                  //threatByLesser, by PieceType - 1
        const uint64_t pawn_atk = attacks_by_pawns(board, them);
        const uint64_t minor_atk = attacks_by_type(board, them, Knight) | attacks_by_type(board, them, Bishop);
        thr[Pawn - 1] = 0;
        thr[Knight - 1] = thr[Bishop - 1] = pawn_atk;
        thr[Rook - 1] = pawn_atk | minor_atk;
        thr[Queen - 1] = thr[Rook - 1] | attacks_by_type(board, them, Rook);
        thr[King - 1] = 0;                               //PieceValue[KING] is 0: the term vanishes
        //const uint64_t pk = pawn_key(p.ply);
        const uint64_t pk = pawn_key();
        for (int i = from; i < to; ++i) {
            const Mv& m = p.moves[i];
            const int pc = hist_pc(stm, m.src);
            int v = 2 * history[stm][mv_idx(m.src, m.dst, m.promo)];
            if (pc >= 0 && pc <= 11) {
                v += 2 * pawn_read(pk, pc, m.dst);
                static const int lv[] = { 0, 1, 2, 3, 5 };
                for (int L : lv) v += (int)*cont_slot(L, p.ply, pc, m.dst);
            }
            //pos.check_squares(pt) & to: the squares the moved piece would check from, computed on
            //the board as it stands, as upstream's are -- so a slider leaving its own line to the
            //king earns no bonus.
            const int mpt = (int)PC_TYPE((Piece)board.piecesOnSquares[m.src]);
            if (p.ci && p.ci->valid && ((p.ci->chk[mpt - 1] >> m.dst) & 1ULL) && see_ge(m, -75)) v += 16384;
            const int pt = (int)PC_TYPE((Piece)board.piecesOnSquares[m.src]) - 1;
            const int t = ((thr[pt] >> m.dst) & 1ULL) ? -19 : (((thr[pt] >> m.src) & 1ULL) ? 20 : 0);
            v += pieceValueSF[pt + 1] * t * (pt + 1 == King ? 0 : 1);
            if (p.ply < LOW_PLY_SIZE) v += 8 * (int)low_ply_hist[p.ply][mv_idx(m.src, m.dst, m.promo)] / (1 + p.ply);
            p.val[i] = v;
        }
    }

    void pick_score_evasions(PickerState& p, int from, int to) {
        const int stm = (int)board.sideToMove;
        for (int i = from; i < to; ++i) {
            const Mv& m = p.moves[i];
            if (cap_stage(m)) { p.val[i] = victim_value(m) + (1 << 28); continue; }
            const int pc = hist_pc(stm, m.src);
            p.val[i] = history[stm][mv_idx(m.src, m.dst, m.promo)]
                   + ((pc >= 0 && pc <= 11) ? (int)*cont_slot(0, p.ply, pc, m.dst) : 0);
        }
    }

    bool pick_next(PickerState& p, Mv& out) {
        for (;;) switch (p.stage) {
        case MAIN_TT: case EVASION_TT: case QSEARCH_TT: case PROBCUT_TT:
            ++p.stage; out = p.tt; return true;
        case CAPTURE_INIT: case PROBCUT_INIT: case QCAPTURE_INIT:
            p.cur = p.end_bad = 0;
            p.end_cur = p.end_caps = gen_pseudo(*p.ni, p.moves, true);
            pick_score_captures(p, 0, p.end_cur);
            partial_insertion_sort(p.moves, p.val, p.end_cur, std::numeric_limits<int>::min());
            ++p.stage;
            break;
        case GOOD_CAPTURE:
            for (; p.cur < p.end_cur; ++p.cur) {
                if (p.is_tt(p.moves[p.cur])) continue;
                if (see_ge(p.moves[p.cur], see_stage_threshold(p.val[p.cur]))) { out = p.moves[p.cur++]; return true; }
                { std::swap(p.moves[p.end_bad], p.moves[p.cur]); std::swap(p.val[p.end_bad], p.val[p.cur]); ++p.end_bad; }
            }
            ++p.stage;
            [[fallthrough]];
        case QUIET_INIT:
            p.end_gen = p.end_caps;
            if (!p.skip_quiets) {
                p.end_cur = p.end_gen = p.end_caps + gen_pseudo(*p.ni, p.moves + p.end_caps, false);
                pick_score_quiets(p, p.end_caps, p.end_gen);
                partial_insertion_sort(p.moves + p.end_caps, p.val + p.end_caps, p.end_gen - p.end_caps, -3560 * p.depth);
            }
            p.cur = p.end_caps;
            ++p.stage;
            [[fallthrough]];
        case GOOD_QUIET:
            if (!p.skip_quiets)
                for (; p.cur < p.end_cur; ++p.cur)
                    if (!p.is_tt(p.moves[p.cur]) && p.val[p.cur] > -14000) { out = p.moves[p.cur++]; return true; }
            p.cur = 0; p.end_cur = p.end_bad;
            ++p.stage;
            [[fallthrough]];
        case BAD_CAPTURE:
            for (; p.cur < p.end_cur; ++p.cur)
                if (!p.is_tt(p.moves[p.cur])) { out = p.moves[p.cur++]; return true; }
            p.cur = p.end_caps; p.end_cur = p.end_gen;
            ++p.stage;
            [[fallthrough]];
        case BAD_QUIET:
            if (!p.skip_quiets)
                for (; p.cur < p.end_cur; ++p.cur)
                    if (!p.is_tt(p.moves[p.cur]) && p.val[p.cur] <= -14000) { out = p.moves[p.cur++]; return true; }
            return false;
        case EVASION_INIT:
            p.cur = 0;
            p.end_cur = p.end_gen = generate(p.moves, false);
            pick_score_evasions(p, 0, p.end_cur);
            partial_insertion_sort(p.moves, p.val, p.end_cur, std::numeric_limits<int>::min());
            ++p.stage;
            [[fallthrough]];
        case EVASION: case QCAPTURE:
            for (; p.cur < p.end_cur; ++p.cur)
                if (!p.is_tt(p.moves[p.cur])) { out = p.moves[p.cur++]; return true; }
            return false;
        case PROBCUT:
            for (; p.cur < p.end_cur; ++p.cur)
                if (!p.is_tt(p.moves[p.cur]) && see_ge(p.moves[p.cur], p.threshold)) { out = p.moves[p.cur++]; return true; }
            return false;
        default:
            return false;
        }
    }

    //The node-entry bookkeeping both searches share: pliesFromNull, the path key, st->repetition.
    inline void enter_node(int ply) {
        const bool from_null = next_is_null;
        next_is_null = false;
        if (ply == 0) ss_pfn[0] = (int)game_hashes.size();
        else          ss_pfn[ply] = from_null ? 0 : ss_pfn[ply - 1] + 1;
        path_hash[ply] = zh.hash;
        set_repetition(ply);
        set_cont_rows(ply);
    }

    //Records the move about to be made at `ply` for the child's history and continuation keys.
    //The child's key for an ordinary move, from the board before it, so its table bucket can be
    //fetched while the move and its NNUE update are still being made (upstream prefetches inside
    //do_move()). Castling, en passant, double pushes and moves that may change castling rights
    //are left to the prefetch after the move; a wrong guess would only fetch an unused line.
    inline bool predict_child_key(const Mv& m, uint64_t& out) {
        const int pc = board.piecesOnSquares[m.src];
        const int pt = PC_TYPE((Piece)pc), c = (int)board.sideToMove;
        if (is_castle(m) || is_ep(m)) return false;
        if (pt == Pawn && (m.src ^ m.dst) == 16) return false;
        if (board.castlingRights && (pt == King || pt == Rook || m.dst == 0 || m.dst == 7 || m.dst == 56 || m.dst == 63))
            return false;
        const auto key = [&](int col, int t, int sq) {
            return t == Pawn ? z.pawns[col][sq - 8] : z.nonPawns[col][t - 2][sq];
        };
        uint64_t k = zh.hash ^ zh.prevEnPassant ^ z.blackMove;
        k ^= key(c, pt, m.src) ^ z.emptySquares[m.src];
        const int victim = PC_TYPE((Piece)board.piecesOnSquares[m.dst]);
        k ^= board.piecesOnSquares[m.dst] == PieceNone ? z.emptySquares[m.dst] : key(1 - c, victim, m.dst);
        k ^= key(c, is_promo_pc(m.promo) ? (int)m.promo : pt, m.dst);
        out = k;
        return true;
    }
    inline void record_move(int ply, const Mv& m) {
        const int moved = PC_TYPE(board.piecesOnSquares[m.src]);
        const int onto  = is_promo_pc(m.promo) ? m.promo : moved;
        ss_mv_pc[ply]    = (uint8_t)(board.sideToMove * 6 + moved - 1);
        ss_mv_onto[ply]  = (uint8_t)(board.sideToMove * 6 + onto - 1);
        ss_mv_to[ply]    = m.dst;
        ss_mv_from[ply]  = m.src;
        ss_mv_promo[ply] = m.promo;
        prev_cap_sq[ply + 1] = cap_stage(m) ? (uint8_t)m.dst : (uint8_t)64;
        ss_cap_type[ply + 1] = (int8_t)cap_type(m);
        for (int c = 0; c < 2; ++c) key_pre[ply][c] = board.side[c];
        for (int t = 0; t < 6; ++t) key_pre[ply][2 + t] = board.pieceTypes[t];
    }
    //The child's keys, from the parent's and the squares the move changed for each colour and
    //piece type -- which covers captures, en passant, promotions and castling without cases.
    inline void update_keys(int ply) {
        CorrKeys k = ss_keys[ply];
        for (int c = 0; c < 2; ++c)
            for (int t = 0; t < 6; ++t) {
                uint64_t d = (key_pre[ply][c] & key_pre[ply][2 + t]) ^ (board.side[c] & board.pieceTypes[t]);
                for (; d; d &= d - 1) {
                    const int sq = lsBit(d);
                    if (t == Pawn - 1) { k.pawn ^= z.pawns[c][sq - 8]; continue; }
                    const uint64_t r = z.nonPawns[c][t - 1][sq];
                    k.np[c] ^= r;
                    if (t == Knight - 1 || t == Bishop - 1) k.minor ^= r;
                }
            }
        ss_keys[ply + 1] = k;
    }

    inline void update_pv(int ply, const Mv& m) {
        pv_line[ply][0] = m;
        for (int k = 0; k < pv_len[ply + 1] && k + 1 < MAX_PLY; ++k) pv_line[ply][k + 1] = pv_line[ply + 1][k];
        pv_len[ply] = std::min(pv_len[ply + 1] + 1, MAX_PLY - 1);
    }

    //=== QUIESCENCE, upstream's qsearch ===
    int qsearch(int alpha, int beta, int ply, bool pv) {
        if (pv && seldepth < ply + 1) seldepth = ply + 1;
        if (pv) pv_len[ply] = 0;
        if (timed_out()) return 0;
        const bool in_check = stm_in_check();
        if (ply >= MAX_PLY - 2) return in_check ? 0 : static_eval();
        ss_incheck[ply] = in_check;
        enter_node(ply);
        if (ply > 0 && alpha < 0 && upcoming_repetition(ply)) {
            alpha = value_draw();
            if (alpha >= beta) return alpha;
        }
        if (ply > 0 && repeated(ply)) return 0;
        if (ply > 0 && board.halfmoveClock > 99) {
            bool claimable = !in_check;
            if (!claimable) { Mv dm[MAX_MOVES]; claimable = generate(dm, false) > 0; }
            if (claimable) return 0;
        }
        //Step 3: the table. pvHit is the entry's own bit; a cutoff needs a non-PV node and a bound.
        TTEntry * const slot = tt_probe(zh.hash);
        Mv qtt_move{0, 0, 0}; bool have_qtt_move = false;
        const bool q_key_hit = tt_key_match(slot, zh.hash);
        const bool q_pvhit = q_key_hit && ((slot->data >> 57) & 1u);
        int q_tt_score = 0, q_tt_flag = -1; bool q_have_tt_score = false;
        {
            TTData d;
            if (tt_read(slot, zh.hash, d)) {
                qtt_move = {d.src, d.dst, d.promo};
                have_qtt_move = !(d.src == 0 && d.dst == 0);
                q_tt_score = tt_from_store(d.score, ply);
                q_tt_flag = (int)d.flag; q_have_tt_score = true;
                if (!pv && !d.unsearched) {
                    const int sc = q_tt_score;
                    if (d.flag == TT_EXACT) return sc;
                    if (d.flag == TT_LOWER && sc >= beta)  return sc;
                    if (d.flag == TT_UPPER && sc <= alpha) return sc;
                }
            }
        }
        //Step 4: the stand pat, corrected, and improved by a usable table bound.
        int stand = -MATE_CP + ply;
        int stand_raw = -MATE_CP + ply;     //before correction, which is what the table holds
        int qbv = -MATE_CP + ply;           //bestValue
        if (!in_check) {
            int ev = 0;
            stand = tt_read_eval(slot, zh.hash, ev) ? ev : static_eval();
            stand_raw = stand;
            const int qcv = corr_value(ss_keys[ply], board.sideToMove, ply);
            if (qcv) stand = corr_apply(stand, qcv);
            qbv = stand;
            if (q_have_tt_score && !is_decisive(q_tt_score)) {
                const bool usable = (q_tt_score > qbv)
                                    ? (q_tt_flag == TT_LOWER || q_tt_flag == TT_EXACT)
                                    : (q_tt_flag == TT_UPPER || q_tt_flag == TT_EXACT);
                if (usable) qbv = q_tt_score;
            }
            if (qbv >= beta) {
                if (!q_key_hit) {
                    TTData d;
                    d.score = (int16_t)(!is_decisive(qbv) ? (qbv + beta) / 2 : qbv);
                    d.unsearched = 1;
                    d.flag  = TT_LOWER;
                    d.eval  = (int16_t)stand_raw;
                    d.ttpv  = 0;
                    tt_write(slot, zh.hash, d);
                }
                return !is_decisive(qbv) ? (qbv + beta) / 2 : qbv;
            }
            if (qbv > alpha) alpha = qbv;
        }
        //Evasions in check, otherwise the table move (any legal one, a quiet included) and then
        //captures and queen promotions, as upstream's QSEARCH picker yields them.
        const NodeInfo ni = node_info();
        PickerState mp;
        pick_init(mp, ni, qtt_move, have_qtt_move, 0, ply);
        int best = qbv;
        Mv qbest{0, 0, 0};
        int q_count = 0;
        const CheckInfo qci = check_info(board);
        const int fut_base = in_check ? -MATE_CP - 1 : stand + 351;    //futilityBase
        Mv m;
        while (pick_next(mp, m)) {
            if (!legal(ni, m)) continue;
            ++q_count;
            //Step 6: pruning.
            if (!is_loss(best)) {
                const bool is_promo  = is_promo_pc(m.promo);
                const bool recapture = (ply >= 1 && ss_mv_pc[ply - 1] != 12 && ss_mv_to[ply - 1] == m.dst);
                if (!is_promo && !recapture && !is_loss(fut_base) && !gives_check_ci(board, qci, m)) {
                    if (q_count > 2) continue;
                    const int fv = fut_base + (is_castle(m) ? pieceValueSF[Rook] : victim_value(m));
                    if (fv <= alpha) {
                        if (fv > best) best = fv;
                        continue;
                    }
                    if (!see_ge(m, alpha - fut_base)) {
                        const int cap = std::min(alpha, fut_base);
                        if (cap > best) best = cap;
                        continue;
                    }
                }
                if (!cap_stage(m)) continue;
                if (!see_ge(m, -80)) continue;
            }
            Move mm = to_move(m);
            StateInfo st = {};
            auto [dp, dts] = accumulator_stack_push(ctx);
            const ZobristHash saved = zh;
            if (ply < MAX_PLY) record_move(ply, m);
            ++nodes_sf;
            { uint64_t ck; if (predict_child_key(m, ck)) tt_prefetch(ck); }
            updateHash(zh, board, mm, do_move_dp(board, mm, st, dp, dts), z);
            tt_prefetch(zh.hash);
            update_keys(ply);
            const int v = -qsearch(-beta, -alpha, ply + 1, pv);
            undo_move(board, mm, st); accumulator_stack_pop(ctx); zh = saved;
            if (out_of_time) return 0;
            if (v > best) {
                best = v;
                if (v > alpha) {
                    qbest = m;
                    if (pv && ply + 1 < MAX_PLY) update_pv(ply, m);
                }
            }
            if (best > alpha) alpha = best;
            if (alpha >= beta) break;
        }
        if (in_check && q_count == 0) return -MATE_CP + ply;      //no legal move: mated
        if (!is_decisive(best) && best > beta) best = (best + beta) / 2;
        //A stalemate qsearch cannot otherwise see: no capture, no piece but pawns, after a capture of
        //at least a rook.
        if (!in_check && q_count == 0 && ss_cap_type[ply] >= (Rook - 1)
            && !(board.side[board.sideToMove] & ~board.pieceTypes[Pawn - 1] & ~board.pieceTypes[King - 1])) {
            Mv sm[MAX_MOVES];
            if (generate(sm, false) == 0) best = 0;
        }
        if (!out_of_time) {
            TTData d;
            d.score = (int16_t)tt_to_store(best, ply);
            d.depth = 0;
            d.eval  = in_check ? EVAL_NONE : (int16_t)stand_raw;
            d.flag  = (uint8_t)(best >= beta ? TT_LOWER : TT_UPPER);
            d.src = qbest.src; d.dst = qbest.dst; d.promo = qbest.promo;
            d.ttpv = q_pvhit ? 1 : 0;
            tt_write(slot, zh.hash, d);
        }
        return best;
    }

    //=== THE MAIN SEARCH, upstream's search<NodeType> ===
    int search(int depth, int alpha, int beta, int ply, bool cut_node, bool pv) {
        if (pv && seldepth < ply + 1) seldepth = ply + 1;
        pv_len[ply] = 0;
        if (timed_out()) return 0;
        if (depth <= 0) return qsearch(alpha, beta, ply, pv);
        if (ply >= MAX_PLY - 2) return stm_in_check() ? value_draw() : static_eval();
        depth = std::min(depth, MAX_PLY - 1);
        enter_node(ply);
        ss_cutoff_cnt[ply + 2] = 0;
        ss_mv_count[ply] = 0; ss_stat[ply] = 0;
        const int prior_reduction = ply >= 1 ? ss_reduction[ply - 1] : 0;
        if (ply >= 1) ss_reduction[ply - 1] = 0;
        const bool pv_node  = pv;
        const bool all_node = !(pv_node || cut_node);
        const int stm = (int)board.sideToMove;
        //Step 2: draws. The fifty-move rule loses to a mate delivered on the hundredth half-move.
        if (ply > 0 && board.halfmoveClock > 99) {
            bool claimable = !stm_in_check();
            if (!claimable) { Mv dm[MAX_MOVES]; claimable = generate(dm, false) > 0; }
            if (claimable) return value_draw();
        }
        if (ply > 0 && repeated(ply)) return value_draw();
        if (ply > 0 && alpha < 0 && upcoming_repetition(ply)) {
            alpha = value_draw();
            if (alpha >= beta) return alpha;
        }
        //Step 3: mate distance pruning.
        if (ply > 0) {
            alpha = std::max(-MATE_CP + ply, alpha);
            beta  = std::min(MATE_CP - (ply + 1), beta);
            if (alpha >= beta) return alpha;
        }
        const bool in_check = stm_in_check();
        //kingMoves() once per node, when something first needs it: ProbCut or the move loop.
        NodeInfo ni; bool ni_ready = false;
        const auto node_ni = [&]() -> const NodeInfo& {
            if (!ni_ready) { ni = node_info(); ni_ready = true; }
            return ni;
        };
        ss_incheck[ply] = in_check;
        const bool prior_capture = ss_cap_type[ply] >= 0;
        const bool prev_ok = ply > 0 && ss_mv_pc[ply - 1] != 12;    //prevSq != SQ_NONE

        //Step 4: the table.
        const bool excluded = sing_excl_set[ply];
        TTEntry * const slot = tt_probe(zh.hash);
        Mv tt_move{0, 0, 0}; bool have_tt_move = false;
        int tt_score = 0, tt_flag = -1, tt_depth = -3;
        const bool tt_hit = tt_key_match(slot, zh.hash);
        ss_tthit[ply] = tt_hit;
        {
            TTData d;
            if (tt_read(slot, zh.hash, d)) {
                tt_move = {d.src, d.dst, d.promo};
                have_tt_move = !(d.src == 0 && d.dst == 0);
                tt_score = tt_from_store(d.score, ply);
                tt_flag = (int)d.flag;
                tt_depth = d.unsearched ? -2 : (int)d.depth;
            } else if (tt_hit) tt_depth = -2;
        }
        const bool have_tt_score = tt_flag == TT_LOWER || tt_flag == TT_UPPER || tt_flag == TT_EXACT;
        if (!excluded) ss_ttpv[ply] = pv_node || (tt_hit && ((slot->data >> 57) & 1u));
        if (ply == 0 && have_root_first) { tt_move = root_first; have_tt_move = true; }
        const bool tt_capture = have_tt_move && cap_stage(tt_move);

        //Step 6: the static evaluation. In check it is the one two plies back; in the singular search
        //it is the node's own, already corrected.
        int nev = VALUE_NONE, nev_raw = VALUE_NONE;     //ss->staticEval and unadjustedStaticEval
        const CorrKeys& ck = ss_keys[ply];
        const int cv = corr_value(ck, stm, ply);
        if (in_check) {
            nev = ply >= 2 ? ss_eval[ply - 2] : VALUE_NONE;
        } else if (excluded) {
            nev = nev_raw = ss_eval[ply];
        } else {
            if (!(tt_hit && tt_read_eval(slot, zh.hash, nev_raw))) {
                nev_raw = static_eval();
                if (!tt_hit) tt_store_eval(slot, zh.hash, nev_raw, ss_ttpv[ply] ? 1 : 0);
            }
            nev = corr_apply(nev_raw, cv);
        }
        ss_eval[ply] = nev;
        //`eval`: a usable table bound is a better estimate than the static evaluation.
        int nev_pr = nev;
        if (!in_check && !excluded && have_tt_score) {
            const bool usable = (tt_score > nev) ? (tt_flag == TT_LOWER || tt_flag == TT_EXACT)
                                                 : (tt_flag == TT_UPPER || tt_flag == TT_EXACT);
            if (usable) nev_pr = tt_score;
        }
        const int eval_prev  = ply >= 1 ? ss_eval[ply - 1] : VALUE_NONE;
        const int eval_prev2 = ply >= 2 ? ss_eval[ply - 2] : VALUE_NONE;
        bool improving = nev > eval_prev2;
        const bool opp_worsening = nev > -eval_prev;
        //Hindsight: the reduction the parent applied, judged by the evaluations it led to.
        if (prior_reduction >= 3 && !opp_worsening) ++depth;
        if (prior_reduction >= 2 && depth >= 2 && nev + eval_prev > 173) --depth;

        //The early cutoff, at non-PV nodes, with a usable bound.
        if (tt_flag >= 0 && !pv_node && !excluded
            && tt_depth > depth - ((tt_score <= beta) ? 1 : 0)
            && (cut_node == (tt_score >= beta) || depth > 5)) {
            const int sc = tt_score;
            const bool cut_ok = (tt_flag == TT_EXACT)
                             || (tt_flag == TT_LOWER && sc >= beta)
                             || (tt_flag == TT_UPPER && sc <= alpha);
            if (cut_ok) {
                if (have_tt_move && sc >= beta) {
                    if (!tt_capture)
                        quiet_hist_update(ply, tt_move.src, tt_move.dst, tt_move.promo,
                                          hist_pc(stm, tt_move.src), stm, std::min(132 * depth - 72, 985));
                    if (prev_ok && ss_mv_count[ply - 1] < 4 && !prior_capture)
                        cont_update_pc(ply - 1, (int)ss_mv_onto[ply - 1], ss_mv_to[ply - 1], -2060);
                }
                //Graph history interaction: no cutoff late in the fifty-move count, and at depth the
                //child's entry has to agree.
                if ((int)board.halfmoveClock < 96) {
                    if (depth >= 8 && have_tt_move && !is_decisive(sc) && is_legal_move(board, to_move(tt_move))) {
                        Move vmm = to_move(tt_move);
                        StateInfo vst;
                        const ZobristHash vsaved = zh;
                        updateHash(zh, board, vmm, do_move(board, vmm, vst), z);
                        TTData cdat;
                        const bool child_valid = tt_read(tt_probe(zh.hash), zh.hash, cdat);
                        undo_move(board, vmm, vst);
                        zh = vsaved;
                        if (!child_valid || (sc >= beta) == (-(int)cdat.score >= beta)) return sc;
                    }
                    else return sc;
                }
            }
        }

        //Step 5: the tablebases.
        int best = -MATE_CP - 1;          //bestValue
        int tb_max = MATE_CP + 1;         //maxValue
        if (ply > 0 && !excluded && tb_cardinality) {
            const int pieces = bitCount(board.side[ColorWhite] | board.side[ColorBlack]);
            int wdl = 0;
            if (pieces <= (int)tb_cardinality
                && (pieces < (int)tb_cardinality || depth >= tb_probe_depth)
                && board.halfmoveClock == 0 && !board.castlingRights && tb_probe_node(wdl)) {
                thread_tbhits[my_tid].store(++tbhits, std::memory_order_relaxed);
                const int draw = tb_rule50 ? 1 : 0;
                const int tbv = VALUE_TB - ply;
                const int value = wdl < -draw ? -tbv : wdl > draw ? tbv : 2 * wdl * draw;
                const int b = wdl < -draw ? TT_UPPER : wdl > draw ? TT_LOWER : TT_EXACT;
                if (b == TT_EXACT || (b == TT_LOWER ? value >= beta : value <= alpha)) {
                    TTData d;
                    d.score = (int16_t)tt_to_store(value, ply);
                    d.depth = (uint8_t)std::min(MAX_PLY - 1, depth + 6);
                    d.flag  = (uint8_t)b;
                    d.ttpv  = ss_ttpv[ply] ? 1 : 0;
                    tt_write(slot, zh.hash, d);
                    return value;
                }
                if (pv_node) {
                    if (b == TT_LOWER) { best = value; alpha = std::max(alpha, best); }
                    else tb_max = value;
                }
            }
        }

        if (!in_check) {
            //The evaluation difference teaches the previous move's history.
            if (prev_ok && !ss_incheck[ply - 1] && !prior_capture) {
                const int eval_diff = std::clamp(-(eval_prev + nev), -209, 167) + 59;
                stat_add(history[1 - stm][mv_idx(ss_mv_from[ply - 1], ss_mv_to[ply - 1], ss_mv_promo[ply - 1])],
                         eval_diff * 9, HIST_LIMIT);
                const int ppc = (int)ss_mv_onto[ply - 1];
                if (!tt_hit && (ppc % 6) != (Pawn - 1) && !is_promo_pc(ss_mv_promo[ply - 1]))
                    stat_add(pawn_entry(pawn_key(), ppc, ss_mv_to[ply - 1]), eval_diff * 13, 8192);
            }
            //Step 7: razoring.
            if (!pv_node && nev_pr < alpha - 485 - 281 * depth * depth)
                return qsearch(alpha, beta, ply, false);
            //Step 8: futility pruning, child node.
            {
                const int fmult  = tt_hit ? 76 : 53;
                const int margin = fmult * depth
                                 - (2474 * (improving ? 1 : 0) + 331 * (opp_worsening ? 1 : 0)) * fmult / 1024
                                 + std::abs(cv) / 174665;
                if (!ss_ttpv[ply] && depth < 14 && nev_pr - margin >= beta && nev_pr >= beta
                    && (!have_tt_move || tt_capture) && !is_loss(beta) && !is_win(nev_pr))
                    return (2 * beta + nev_pr) / 3;
            }
            //Step 9: the null move, verified at depth.
            if (cut_node && nev >= beta - 18 * depth + 350 && !excluded && non_pawn_material(stm)
                && ply >= nmp_min_ply && !is_loss(beta)) {
                const int R = 7 + depth / 3;
                const ZobristHash saved = zh;
                const File saved_ep = board.enPassant;
                board.sideToMove = (Color)(1 - stm);
                board.enPassant  = FileNone;
                //do_null_move()'s key update: the side to move flips and en passant goes.
                zh.hash ^= z.blackMove;
                if (saved_ep != FileNone) zh.hash ^= z.enPassant[saved_ep];
                zh.prevEnPassant = 0;
                tt_prefetch(zh.hash);
                ss_mv_pc[ply] = 12; ss_mv_onto[ply] = 12; ss_mv_to[ply] = 0;
                ss_mv_promo[ply] = PieceTypeNone;
                prev_cap_sq[ply + 1] = 64;
                ss_cap_type[ply + 1] = ss_cap_type[ply];   //the null state is a copy: captured_piece() carries over
                next_is_null = true;
                ss_keys[ply + 1] = ss_keys[ply];
                const int v = -search(depth - R, -beta, -beta + 1, ply + 1, false, false);
                next_is_null = false;
                board.sideToMove = (Color)stm;
                board.enPassant  = saved_ep;
                zh = saved;
                if (out_of_time) return 0;
                if (v >= beta && !is_win(v)) {
                    if (nmp_min_ply || depth < 16) return v;
                    nmp_min_ply = ply + 3 * (depth - R) / 4;
                    const int vv = search(depth - R, beta - 1, beta, ply, false, false);
                    nmp_min_ply = 0;
                    pv_len[ply] = 0;
                    if (out_of_time) return 0;
                    if (vv >= beta) return v;
                }
            }
            if (nev >= beta) improving = true;
            //Step 10: internal iterative reductions.
            if (!all_node && depth >= 6 && !have_tt_move && prior_reduction <= 3) --depth;
            //Step 11: ProbCut.
            const int pcbeta = beta + 235 - 63 * (improving ? 1 : 0);
            if (depth >= 3 && !is_decisive(beta) && !(tt_flag >= 0 && tt_score < pcbeta)) {
                const int see_need = pcbeta - nev;
                const int cdepth = std::clamp(depth - 5 - (nev - beta) / 315, 0, depth);
                const NodeInfo& pni = node_ni();
                PickerState pp;
                pick_init_probcut(pp, pni, tt_move, have_tt_move, see_need);
                Mv m;
                while (pick_next(pp, m)) {
                    if (excluded && same(m, sing_excl[ply])) continue;
                    if (!legal(pni, m)) continue;
                    record_move(ply, m);
                    ss_mv_count[ply] = 0;
                    ss_stat[ply] = 0;
                    Move mm = to_move(m);
                    StateInfo st = {};
                    auto [dp, dts] = accumulator_stack_push(ctx);
                    const ZobristHash saved = zh;
                    ++nodes_sf;
                    { uint64_t ck; if (predict_child_key(m, ck)) tt_prefetch(ck); }
                    updateHash(zh, board, mm, do_move_dp(board, mm, st, dp, dts), z);
                    tt_prefetch(zh.hash);
                    update_keys(ply);
                    int v = -qsearch(-pcbeta, -pcbeta + 1, ply + 1, false);
                    if (v >= pcbeta && cdepth > 0)
                        v = -search(cdepth, -pcbeta, -pcbeta + 1, ply + 1, !cut_node, false);
                    undo_move(board, mm, st); accumulator_stack_pop(ctx); zh = saved;
                    if (out_of_time) return 0;
                    if (v >= pcbeta) {
                        TTData d;
                        d.score = (int16_t)tt_to_store(v, ply);
                        d.depth = (uint8_t)std::min(cdepth + 1, 255);
                        d.flag  = (uint8_t)TT_LOWER;
                        d.src = m.src; d.dst = m.dst; d.promo = m.promo;
                        d.eval  = (int16_t)nev_raw;
                        d.ttpv  = ss_ttpv[ply] ? 1 : 0;
                        tt_write(slot, zh.hash, d);
                        if (!is_decisive(v)) return v - (pcbeta - beta);
                    }
                }
            }
        }

        //moves_loop. Step 12: a small ProbCut on the table bound alone.
        if ((tt_flag == TT_LOWER || tt_flag == TT_EXACT) && tt_depth >= depth - 4
            && tt_score >= beta + 418 && !is_decisive(beta) && !is_decisive(tt_score))
            return beta + 418;

        node_ni();
        const CheckInfo ci = check_info(board);
        PickerState mp;
        mp.ci = &ci;
        pick_init(mp, ni, tt_move, have_tt_move, depth, ply);
        const bool sing_shuffling =
            have_tt_move && !cap_stage(tt_move)
            && (int)board.halfmoveClock >= 10 && ply >= 20 && ss_pfn[ply] > 6
            && tt_move.src == ss_mv_to[ply - 2] && ss_mv_from[ply - 2] == ss_mv_to[ply - 4];

        //Step 13: the move loop.
        const int npm_us = non_pawn_material(stm);
        Mv best_mv{0, 0, 0}; bool have_bm = false;
        Mv q_tried[32]; int n_q_tried = 0;
        Mv c_tried[32]; int n_c_tried = 0;
        int move_seen = 0;                 //moveCount
        Mv m;
        while (pick_next(mp, m)) {
            if (excluded && same(m, sing_excl[ply])) continue;
            if (!legal(ni, m)) continue;
            ss_mv_count[ply] = (uint16_t)(++move_seen);
            const bool quiet = !cap_stage(m);
            const bool is_tt = have_tt_move && same(m, tt_move);
            const int victim_slot = cap_slot_to(m);
            const int sc_cap_val = is_ep(m) ? pieceValueSF[Pawn] : victim_value(m);
            const int sc_cap_ct  = cap_type(m);
            const int sc_pc      = hist_pc(stm, m.src);
            const bool gchk      = gives_check_ci(board, ci, m);
            int new_depth = depth - 1;
            int r1024 = sf_red1024(improving, depth, move_seen, beta - alpha, root_delta);
            if (ss_ttpv[ply]) r1024 += 946;
            //Step 14: pruning at shallow depth.
            if (ply > 0 && npm_us && !is_loss(best)) {
                if (move_seen >= (3 + depth * depth) / (2 - (improving ? 1 : 0))) mp.skip_quiets = true;
                int lmr_depth = new_depth - r1024 / 1024;
                if (!quiet || gchk) {
                    const int capt_hist = (sc_pc >= 0 && sc_pc <= 11) ? (int)cap_hist[sc_pc][m.dst][victim_slot] : 0;
                    if (!gchk && lmr_depth < 7
                        && nev + 232 + 217 * lmr_depth + victim_value(m) + 131 * capt_hist / 1024 <= alpha)
                        continue;
                    const int margin = std::max(166 * depth + capt_hist / 29, 0);
                    const int moved_val = pieceValueSF[PC_TYPE((Piece)board.piecesOnSquares[m.src])];
                    if ((alpha >= 0 || npm_us != moved_val) && !see_ge(m, -margin)) continue;
                } else {
                    int hist = 0;
                    if (sc_pc >= 0 && sc_pc <= 11)
                        hist = (int)*cont_slot(0, ply, sc_pc, m.dst) + (int)*cont_slot(1, ply, sc_pc, m.dst)
                             + pawn_read(pawn_key(), sc_pc, m.dst);
                    if (hist < -4083 * depth) continue;
                    hist += 69 * history[stm][mv_idx(m.src, m.dst, m.promo)] / 32;
                    lmr_depth += hist / 3208;
                    const int fv = nev + 42 + 161 * (have_bm ? 0 : 1) + 127 * lmr_depth + (nev > alpha ? 85 : 0);
                    if (!in_check && lmr_depth < 13 && fv <= alpha) {
                        if (best <= fv && !is_decisive(best) && !is_win(fv)) best = fv;
                        continue;
                    }
                    lmr_depth = std::max(lmr_depth, 0);
                    if (!see_ge(m, -25 * lmr_depth * lmr_depth)) continue;
                }
            }
            //Step 15: singular extension, multi-cut and the negative extensions, for the table move.
            //The excluded search shares this ply's stack entry, so its moveCount is what the table
            //move's child reads afterwards, as upstream's does.
            int extension = 0;
            if (is_tt && !excluded && ply > 0 && depth >= 6 + (ss_ttpv[ply] ? 1 : 0)
                && (tt_flag == TT_LOWER || tt_flag == TT_EXACT) && !is_decisive(tt_score)
                && tt_depth >= depth - 3 && !sing_shuffling) {
                const int sb = tt_score - (53 + 75 * ((ss_ttpv[ply] && !pv_node) ? 1 : 0)) * depth / 60;
                sing_excl[ply] = m; sing_excl_set[ply] = true;
                const int sv = search(new_depth / 2, sb - 1, sb, ply, cut_node, false);
                sing_excl_set[ply] = false;
                pv_len[ply] = 0;
                if (out_of_time) return 0;
                if (sv < sb) {
                    const int corr_adj = std::abs(cv) / 230673;
                    const int dm = -4 + 199 * (pv_node ? 1 : 0) - 201 * (tt_capture ? 0 : 1) - corr_adj
                                 - 897 * tt_move_hist / 127649 - ((ply > root_depth) ? 42 : 0);
                    const int tm = 73 + 302 * (pv_node ? 1 : 0) - 248 * (tt_capture ? 0 : 1)
                                 + 90 * (ss_ttpv[ply] ? 1 : 0) - corr_adj
                                 - ((ply * 2 > root_depth * 3) ? 50 : 0);
                    extension = 1 + ((sv < sb - dm) ? 1 : 0) + ((sv < sb - tm) ? 1 : 0);
                    ++depth;
                }
                else if (sv >= beta && !is_decisive(sv)) {
                    stat_add(tt_move_hist, std::max(-400 - 100 * depth, -4000), 8192);
                    return sv;
                }
                else if (tt_score >= beta) extension = -3;
                else if (cut_node)         extension = -2;
            }
            new_depth += extension;
            record_move(ply, m);
            //Step 16: make the move.
            Move mm = to_move(m);
            StateInfo st = {};
            auto [dp, dts] = accumulator_stack_push(ctx);
            const ZobristHash saved = zh;
            ++nodes_sf;
            { uint64_t ck; if (predict_child_key(m, ck)) tt_prefetch(ck); }
            updateHash(zh, board, mm, do_move_dp(board, mm, st, dp, dts), z);
            tt_prefetch(zh.hash);
            update_keys(ply);
            int cd = new_depth;
            if (ss_ttpv[ply])
                r1024 -= 2719 + (pv_node ? 983 : 0)
                       + ((!have_tt_score || tt_score > alpha) ? 922 : 0)
                       + ((have_tt_score && tt_depth >= depth) ? (934 + (cut_node ? 1011 : 0)) : 0);
            r1024 += 714;
            r1024 -= move_seen * 73;
            r1024 -= std::abs(cv) / 30370;
            if (cut_node)   r1024 += 3372 + 997 * (have_tt_move ? 0 : 1);
            if (tt_capture) r1024 += 1119;
            if (ss_cutoff_cnt[ply + 1] > 1)
                r1024 += 256 + 1024 * (ss_cutoff_cnt[ply + 1] > 2 ? 1 : 0) + 1024 * (all_node ? 1 : 0);
            if (is_tt) r1024 -= 2151;
            int ss_sc;
            if (!quiet) {
                ss_sc = 868 * sc_cap_val / 128;
                if (sc_pc >= 0 && sc_pc <= 11 && sc_cap_ct >= -1 && sc_cap_ct < 6)
                    ss_sc += (int)cap_hist[sc_pc][m.dst][sc_cap_ct + 1];
            } else
                ss_sc = stat_score(ply, (int)ss_mv_pc[ply], m.dst, m.src, m.promo, stm);
            ss_stat[ply] = ss_sc;
            r1024 -= ss_sc * 850 / 8192;
            if (all_node) r1024 += r1024 / (depth + 1);
            int v = best;
            //Step 17: late move reduction, which may also extend.
            if (depth >= 2 && move_seen > 1) {
                const int d_lmr = std::max(1, std::min(cd - r1024 / 1024, cd + 2)) + (pv_node ? 1 : 0);
                ss_reduction[ply] = cd - d_lmr;
                v = -search(d_lmr, -alpha - 1, -alpha, ply + 1, true, false);
                ss_reduction[ply] = 0;
                if (!out_of_time && v > alpha) {
                    const bool deeper    = d_lmr < cd && v > best + 50;
                    const bool shallower = v < best + 9;
                    cd += (deeper ? 1 : 0) - (shallower ? 1 : 0);
                    if (cd > d_lmr) v = -search(cd, -alpha - 1, -alpha, ply + 1, !cut_node, false);
                    cont_update_pc(ply, (int)ss_mv_pc[ply], m.dst, 1365);
                }
            }
            //Step 18: a full-depth null-window search, reduced once or twice when LMR is skipped.
            else if (!pv_node || move_seen > 1) {
                const int r18 = r1024 + (have_tt_move ? 0 : 1140);
                v = -search(cd - ((r18 > 3957) ? 1 : 0) - ((r18 > 5654 && cd > 2) ? 1 : 0),
                            -alpha - 1, -alpha, ply + 1, !cut_node, false);
            }
            //The PV search, for the first move and for any that beat alpha.
            if (pv_node && !out_of_time && (move_seen == 1 || v > alpha)) {
                if (is_tt && ((have_tt_score && is_decisive(tt_score) && tt_depth > 0) || tt_depth > 1))
                    cd = std::max(cd, 1);
                v = -search(cd, -beta, -alpha, ply + 1, false, true);
            }
            undo_move(board, mm, st); accumulator_stack_pop(ctx); zh = saved;
            if (out_of_time) return 0;
            //Step 19: the root's per-move averages, and its scored move.
            if (ply == 0) {
                if (RootHist * rh = root_hist_find(m)) {
                    rh->avg = rh->used ? (v + rh->avg) / 2 : v;
                    const long long sq = (long long)v * (v < 0 ? -(long long)v : (long long)v);
                    rh->msq = rh->used ? (sq + rh->msq) / 2 : sq;
                    rh->used = true;
                }
            }
            const bool root_record = (ply == 0 && (move_seen == 1 || v > alpha));
            if (root_record) { root_scored = true; root_scored_v = v; }
            const int inc = (v == best && ply + 2 >= root_depth && ((int)nodes_sf & 14) == 0
                             && !is_win(std::abs(v) + 1)) ? 1 : 0;
            if (v + inc > best) {
                best = v;
                if (v + inc > alpha) {
                    best_mv = m; have_bm = true;
                    update_pv(ply, m);
                    if (v >= beta) {
                        if (extension < 2 || pv_node) ++ss_cutoff_cnt[ply];
                        break;
                    }
                    if (depth > 2 && depth < 14 && !is_decisive(v)) depth -= 2;
                    alpha = v;
                }
            }
            if (root_record && pv_len[ply] == 0) update_pv(ply, m);
            if (!same(best_mv, m) && move_seen <= 32) {
                if (quiet) { if (n_q_tried < 32) q_tried[n_q_tried++] = m; }
                else       { if (n_c_tried < 32) c_tried[n_c_tried++] = m; }
            }
        }

        //Step 21.
        if (best >= beta && !is_decisive(best) && !is_decisive(alpha))
            best = (best * depth + beta) / (depth + 1);
        if (move_seen == 0)
            best = excluded ? alpha : (in_check ? -MATE_CP + ply : 0);
        else if (have_bm) {
            //update_all_stats.
            const int bonus = std::min(116 * depth - 81, 1515) + 347 * ((have_tt_move && same(best_mv, tt_move)) ? 1 : 0)
                            + (ply >= 1 ? ss_stat[ply - 1] / 32 : 0);
            const int malus = std::min(848 * depth - 207, 2446) - 17 * move_seen;
            if (!cap_stage(best_mv)) {
                quiet_hist_update(ply, best_mv.src, best_mv.dst, best_mv.promo, hist_pc(stm, best_mv.src), stm,
                                  bonus * 910 / 1024);
                for (int qi = 0; qi < n_q_tried; ++qi) {
                    const int i1 = qi + 1;
                    int am = malus * 1085 / 1024;
                    if (i1 > 5) am -= am * (i1 - 5) / i1;
                    const Mv& q = q_tried[qi];
                    quiet_hist_update(ply, q.src, q.dst, q.promo, hist_pc(stm, q.src), stm, -am);
                }
            } else {
                const int pc = hist_pc(stm, best_mv.src);
                if (pc >= 0 && pc <= 11) stat_add(cap_hist[pc][best_mv.dst][cap_slot_to(best_mv)], bonus * 1395 / 1024, CAP_LIMIT);
            }
            if (prev_ok && !prior_capture && (int)ss_mv_count[ply - 1] == 1 + (ss_tthit[ply - 1] ? 1 : 0))
                cont_update_pc(ply - 1, (int)ss_mv_onto[ply - 1], ss_mv_to[ply - 1], -malus * 602 / 1024);
            for (int i = 0; i < n_c_tried; ++i) {
                const Mv& c = c_tried[i];
                const int pc = hist_pc(stm, c.src);
                if (pc >= 0 && pc <= 11) stat_add(cap_hist[pc][c.dst][cap_slot_to(c)], -malus * 1448 / 1024, CAP_LIMIT);
            }
            if (!pv_node)
                stat_add(tt_move_hist, (have_tt_move && same(best_mv, tt_move)) ? 809 : -865, 8192);
        }
        //The prior quiet move that this fail low refutes is rewarded.
        else if (prev_ok && !prior_capture) {
            int bscale = -215;
            bscale -= ss_stat[ply - 1] / 100;
            bscale += std::min(56 * depth, 489);
            bscale += 184 * ((int)ss_mv_count[ply - 1] > 8 ? 1 : 0);
            if (!in_check && best <= nev - 107) bscale += 147;
            if (!ss_incheck[ply - 1] && best <= -eval_prev - 65) bscale += 156;
            bscale = std::max(bscale, 0);
            const int scaled = std::min(141 * depth - 87, 1351) * bscale;
            cont_update_pc(ply - 1, (int)ss_mv_onto[ply - 1], ss_mv_to[ply - 1], scaled * 406 / 32768);
            stat_add(history[1 - stm][mv_idx(ss_mv_from[ply - 1], ss_mv_to[ply - 1], ss_mv_promo[ply - 1])],
                     scaled * 243 / 32768, HIST_LIMIT);
            const int ppc = (int)ss_mv_onto[ply - 1];
            if ((ppc % 6) != (Pawn - 1) && !is_promo_pc(ss_mv_promo[ply - 1]))
                stat_add(pawn_entry(pawn_key(), ppc, ss_mv_to[ply - 1]), scaled * 290 / 8192, 8192);
        }
        //And so is a prior capture.
        else if (prev_ok && prior_capture) {
            const int ppc = (int)ss_mv_onto[ply - 1];
            const int ct  = (int)ss_cap_type[ply];
            if (ppc >= 0 && ppc <= 11 && ct >= 0 && ct < 6)
                stat_add(cap_hist[ppc][ss_mv_to[ply - 1]][ct + 1], 1012, CAP_LIMIT);
        }
        if (pv_node) best = std::min(best, tb_max);
        //A fail low under a ttPv parent makes this node part of the PV tree.
        if (best <= alpha && ply > 0 && ss_ttpv[ply - 1]) ss_ttpv[ply] = true;
        if (!out_of_time && !excluded) {
            TTData d;
            d.score = (int16_t)tt_to_store(best, ply);
            d.depth = (uint8_t)(move_seen ? std::min(depth, 255) : std::min(MAX_PLY - 1, depth + 6));
            d.flag  = (uint8_t)(best >= beta ? TT_LOWER : (pv_node && have_bm) ? TT_EXACT : TT_UPPER);
            d.src = best_mv.src; d.dst = best_mv.dst; d.promo = best_mv.promo;
            d.eval  = in_check ? EVAL_NONE : (int16_t)nev_raw;
            d.ttpv  = ss_ttpv[ply] ? 1 : 0;
            tt_write(slot, zh.hash, d);
        }
        //Correction history learns from a quiet best move, or a fail low, that disagreed with the
        //static evaluation in the direction of the bound.
        if (!in_check && !out_of_time) {
            const bool bm_capture = have_bm && ((victim_value(best_mv) != 0 && !is_castle(best_mv)) || is_ep(best_mv));
            if (!bm_capture && (best > nev) == have_bm) {
                const int bonus = std::clamp((best - nev) * depth / (have_bm ? 10 : 8),
                                             -CORR_LIMIT / 4, CORR_LIMIT / 4);
                corr_update(ck, stm, ply, bonus);
            }
        }
        return best;
    }

    void print_info(int d, int v, const Mv * pv, int pvlen) {
        const long long ms = elapsed_ms();
        const long long n  = total_nodes();
        std::string line = "info depth " + std::to_string(d) + " seldepth " + std::to_string(seldepth)
                         + " multipv 1 score " + format_score(v, root_board)
                         + " nodes " + std::to_string(n)
                         + " nps " + std::to_string(ms > 0 ? n * 1000 / ms : n)
                         + " hashfull " + std::to_string(tt_hashfull())
                         + " tbhits " + std::to_string(total_tbhits())
                         + " time " + std::to_string(ms) + " pv";
        for (int i = 0; i < pvlen; ++i) line += " " + uci_of(pv[i]);
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }

    //ITERATIVE DEEPENING, one thread's worth (Worker::iterative_deepening). Lazy SMP: every thread
    //searches the same root and the threads meet only in the table and the shared histories.
    void iterate(int tid) {
        if (!ctx_ready) { init_nnue_context(ctx); ctx_ready = true; }
        ThreadResult& out = results[tid];
        out = ThreadResult{};
        board = root_board;
        accumulator_stack_reset(ctx);
        getHash(zh, board, z);
        age_thread_histories();
        ss_keys[0] = corr_keys(board);
        nodes_sf = 0; tbhits = 0; seldepth = 0; nmp_min_ply = 0;
        root_hist_n = 0; root_avg = -MATE_CP - 1; root_msq = 0;
        root_optimism = 0; root_stm = (int)board.sideToMove;
        //A fresh Stack: nothing before the root, no move made, no evaluation.
        std::memset(ss_cutoff_cnt, 0, sizeof ss_cutoff_cnt);
        std::memset(ss_rep, 0, sizeof ss_rep);
        std::memset(ss_tthit, 0, sizeof ss_tthit);
        std::memset(ss_ttpv, 0, sizeof ss_ttpv);
        std::memset(ss_incheck, 0, sizeof ss_incheck);
        std::memset(ss_reduction, 0, sizeof ss_reduction);
        std::memset(ss_mv_count, 0, sizeof ss_mv_count);
        std::memset(ss_stat, 0, sizeof ss_stat);
        std::memset(ss_mv_pc, 12, sizeof ss_mv_pc);
        std::memset(ss_mv_onto, 12, sizeof ss_mv_onto);
        std::memset(ss_mv_to, 0, sizeof ss_mv_to);
        std::memset(ss_mv_from, 0, sizeof ss_mv_from);
        std::memset(ss_mv_promo, PieceTypeNone, sizeof ss_mv_promo);
        std::memset(prev_cap_sq, 64, sizeof prev_cap_sq);
        std::memset(ss_cap_type, -1, sizeof ss_cap_type);
        std::memset(sing_excl_set, 0, sizeof sing_excl_set);
        std::memset(path_hash, 0, sizeof path_hash);
        for (int i = 0; i < MAX_PLY; ++i) { ss_eval[i] = VALUE_NONE; ss_pfn[i] = 0; }
        next_is_null = false;
        //rootMoves starts in generation order, so depth 1 opens on the first legal move.
        {
            Mv rm0[MAX_MOVES];
            const int rn = generate(rm0, false);
            have_root_first = rn > 0;
            if (rn > 0) root_first = rm0[0];
        }
        Mv last_pv[MAX_PLY]; int last_pvlen = 0;
        int iters_done = 0;
        for (int d = 1; d < MAX_PLY - 4; ++d) {
            root_depth = d;
            seldepth = 0;
            //The window's centre and size, and the optimism, from the move that sorts first.
            if (have_root_first) {
                RootHist * rh0 = root_hist_find(root_first);
                if (rh0 && rh0->used) { root_avg = rh0->avg; root_msq = rh0->msq; }
            }
            {
                const int a = root_avg < 0 ? -root_avg : root_avg;
                root_optimism = 142 * root_avg / (a + 91);
            }
            int v;
            root_scored = false;
            if (iters_done >= 1) {
                int delta = 5 + tid % 8 + (int)((root_msq < 0 ? -root_msq : root_msq) / 9000);
                int lo = std::max(root_avg - delta, -MATE_CP - 1);
                int hi = std::min(root_avg + delta,  MATE_CP + 1);
                int failed_high = 0;
                for (;;) {
                    const int ad = std::max(1, d - failed_high);
                    root_delta = std::max(1, hi - lo);
                    root_scored = false;
                    v = search(ad, lo, hi, 0, false, true);
                    if (out_of_time) break;
                    if (pv_len[0] > 0) root_first = pv_line[0][0];   //the stable_sort after every root search
                    if (v <= lo) {
                        hi = lo;
                        lo = std::max(v - delta, -MATE_CP - 1);
                        failed_high = 0;
                    } else if (v >= hi) {
                        lo = std::max(hi - delta, lo);
                        hi = std::min(v + delta, MATE_CP + 1);
                        ++failed_high;
                    } else break;
                    delta += delta / 3;
                }
            } else {
                root_delta = 2 * MATE_CP + 2;
                v = search(d, -MATE_CP - 1, MATE_CP + 1, 0, false, true);
                if (!out_of_time && pv_len[0] > 0) root_first = pv_line[0][0];
            }
            if (out_of_time) {
                //An interrupted iteration still reports its best searched root move, unless that is
                //an unproven loss, which upstream replaces by the last completed iteration's.
                if (root_scored && pv_len[0] > 0 && !is_loss(root_scored_v)) {
                    out.have = true; out.best = pv_line[0][0]; out.score = root_scored_v;
                    out.pvlen = std::min(pv_len[0], MAX_PLY);
                    for (int i = 0; i < out.pvlen; ++i) out.pv[i] = pv_line[0][i];
                }
                break;
            }
            ++iters_done;
            if (pv_len[0] > 0) {
                last_pvlen = std::min(pv_len[0], MAX_PLY);
                for (int i = 0; i < last_pvlen; ++i) last_pv[i] = pv_line[0][i];
                out.have = true; out.best = last_pv[0]; out.score = v; out.depth = d;
                out.pvlen = last_pvlen;
                for (int i = 0; i < last_pvlen; ++i) out.pv[i] = last_pv[i];
                if (tid == 0) print_info(d, v, last_pv, last_pvlen);
            }
            if (tid != 0) continue;
            if (go_mate && ((v >= MATE_IN_MAX && MATE_CP - v <= 2 * go_mate)
                            || (v <= -MATE_IN_MAX && MATE_CP + v <= 2 * go_mate))) break;
            if (fixdepth && d >= fixdepth) break;
        }
        if (tid != 0) return;
        //go infinite and go ponder never answer on their own: wait for stop or ponderhit.
        while (!out_of_time.load() && (ponder_flag.load() || infinite_flag))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        out_of_time.store(true);            //the main thread stops the helpers
    }

};

//=== PERSISTENT SEARCH THREADS ===
//Kept alive between searches, each with its Worker, because a Worker's tables are the per-thread
//histories, which upstream carries from move to move. 8 MB stacks: a search frame holds the move
//picker's arrays, and the tree can be MAX_PLY deep.
enum { JOB_SEARCH = 1, JOB_CLEAR = 2 };
static std::mutex pool_mtx;
static std::condition_variable pool_cv, pool_done_cv;
static uint64_t pool_epoch = 0;
static int pool_job = 0, pool_busy = 0;
static bool pool_quit = false;
static std::vector<pthread_t> pool;
static std::vector<std::unique_ptr<Worker>> workers;     //workers[tid], owned here
static void * pool_main(void * arg) {
    const int tid = (int)(intptr_t)arg;
    Worker& w = *workers[(size_t)tid];
    w.my_tid = tid;
    uint64_t seen = 0;
    for (;;) {
        int job;
        {
            std::unique_lock<std::mutex> lk(pool_mtx);
            pool_cv.wait(lk, [&] { return pool_quit || pool_epoch != seen; });
            if (pool_quit) return nullptr;
            seen = pool_epoch; job = pool_job;
        }
        if (job == JOB_SEARCH) w.iterate(tid);
        else if (job == JOB_CLEAR) w.clear_thread_histories();
        std::lock_guard<std::mutex> lk(pool_mtx);
        if (--pool_busy == 0) pool_done_cv.notify_all();
    }
}
//Runs a job on every thread and waits for all of them.
static void pool_run(int job) {
    std::unique_lock<std::mutex> lk(pool_mtx);
    pool_job = job;
    pool_busy = (int)pool.size();
    ++pool_epoch;
    pool_cv.notify_all();
    pool_done_cv.wait(lk, [] { return pool_busy == 0; });
}
static void pool_stop() {
    { std::lock_guard<std::mutex> lk(pool_mtx); pool_quit = true; }
    pool_cv.notify_all();
    for (pthread_t h : pool) pthread_join(h, nullptr);
    pool.clear();
    workers.clear();
    pool_quit = false;
}
static void pool_start(int n) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    //Created before their threads, which index them from the first instruction. Value-initialised,
    //so every table starts zeroed; JOB_CLEAR then sets upstream's defaults.
    for (int t = 0; t < n; ++t) workers.push_back(std::make_unique<Worker>());
    for (int t = 0; t < n; ++t) {
        pthread_t h;
        if (pthread_create(&h, &attr, pool_main, (void *)(intptr_t)t) == 0) pool.push_back(h);
    }
    workers.resize(pool.size());
    pthread_attr_destroy(&attr);
    search_threads = (int)pool.size();
}

//=== THE ONLINE TABLEBASE, lichess's 7-piece set ===
//ASYNCHRONOUS: the request is posted, the search runs exactly as if there were no tables, and
//the answer is used only if it has arrived for this move by the time the search ends. A probe
//on the move's own clock used to cost its timeout and a full search whenever it failed.
static std::mutex              ol_mtx;
static std::condition_variable ol_cv;
static std::string             ol_req_fen;
static uint64_t                ol_req_id = 0;
static bool                    ol_req_live = false;
static bool                    ol_quit = false;
static std::thread             ol_worker;
static bool                    ol_worker_started = false;
//The answer, stamped with its request so a late reply is never played for the wrong position.
static std::mutex              ol_ans_mtx;
static uint64_t                ol_ans_id = 0;
static std::string             ol_ans_move;
static int                     ol_ans_score = 0;
static std::unordered_map<std::string, std::pair<int, std::string>> ol_cache;
static const int TB_CP = 20000;

static size_t ol_write(void * p, size_t sz, size_t n, void * ud) {
    ((std::string *)ud)->append((char *)p, sz * n);
    return sz * n;
}
//One handle, reused by the worker thread alone, so the connection stays warm.
static bool ol_probe(const std::string& fen, int& score, std::string& move) {
    static CURL * curl = nullptr;
    if (!curl) curl = curl_easy_init();
    if (!curl) return false;
    std::string body;
    char * esc = curl_easy_escape(curl, fen.c_str(), (int)fen.size());
    const std::string url = std::string("http://tablebase.lichess.ovh/standard?fen=")
                          + (esc ? esc : fen.c_str());
    if (esc) curl_free(esc);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ol_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    const CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    if (rc != CURLE_OK || http != 200 || body.empty()) return false;
    try {
        const nlohmann::json d = nlohmann::json::parse(body);
        if (d.contains("category")) {
            const std::string c = d["category"];
            //"maybe-win" and "maybe-loss" depend on the fifty-move rule, as a local WDL probe does.
            if (c == "win" || c == "syzygy-win" || c == "maybe-win")         score =  TB_CP;
            else if (c == "loss" || c == "syzygy-loss" || c == "maybe-loss") score = -TB_CP;
            else                                                             score = 0;
        }
        if (d.contains("moves") && d["moves"].is_array() && !d["moves"].empty()
            && d["moves"][0].contains("uci"))
            move = d["moves"][0].value("uci", "");
    } catch (const nlohmann::json::parse_error&) {
        return false;
    }
    return !move.empty();
}

class AlphaBetaEngine : public uci::SearchEngine {
public:
    const char * id_name()   const override { return "Creatica AB (alpha-beta, NNUE)"; }
    const char * id_author() const override { return "Arkadi Poliakevitch"; }

    //No search options: Stockfish's behaviour is the only configuration. What remains is what
    //upstream exposes for resources and tablebases, and the online tablebase.
    void declare_options(uci::Options& o) override {
        o.spin("Hash",    &o_hash,    64, 1, 32768);
        o.spin("Threads", &o_threads,  1, 1, MAX_THREADS);
        o.string("SyzygyPath", &syzygy_path_, "", [this] { on_syzygy_path(); });
        o.spin("SyzygyProbeDepth", &o_tbdepth, 1, 1, 100);
        o.check("Syzygy50MoveRule", &o_tbrule50, true);
        o.spin("SyzygyProbeLimit", &o_tblimit, 7, 0, 7);
        //On by default, for parity with creatica, which probes lichess whenever its local tables
        //cannot answer. It costs nothing on the clock. Turn it off for a network-isolated run.
        o.check("OnlineTablebase", &o_online, true);
    }

    void init() override {
        Stockfish::Bitboards::init();
        zobristHash(z);
        //After zobristHash(z): every cuckoo key is made from those numbers.
        {
            const int n = cuckoo_build();
            if (n != 3668) std::printf("info string cuckoo table built %d keys, expected 3668\n", n);
        }
        init_nnue();
        init_reductions();
        fen2board(root_board, root_fen.c_str());
        apply_hash();
        apply_threads();
    }

    void new_game() override {
        //Worker::clear() on every thread, the shared histories and the table.
        stop();
        std::fill(tt.begin(), tt.end(), TTEntry{});
        game_hashes.clear();
        game_rep.clear();
        reset_shared_histories((size_t)search_threads);
        pool_run(JOB_CLEAR);
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
        //Rebuilt from scratch: the driver replays the whole move list on every command.
        game_hashes.clear();
        game_rep.clear();
        game_hashes.push_back(h.hash);
        game_rep.push_back(0);
        size_t move_index = 0;
        for (const std::string& ms : moves) {
            ++move_index;
            Move m = {};
            uci2move_idx(ms.c_str(), m);
            const Move raw = m;
            normalise_castling(b, m);
            if (const char * why = illegal_reason(b, m)) {
                //Stop at the first refused move, and say enough to reproduce the divergence.
                char fbuf[MAX_FEN_STRING_LEN];
                std::printf("info string REFUSED move %zu of %zu: \"%s\"", move_index, moves.size(),
                            ms.c_str());
                if (raw.src != m.src || raw.dst != m.dst || raw.promoType != m.promoType) {
                    const std::string norm = std::string(square[m.src]) + square[m.dst];
                    std::printf(" (normalised to \"%s\")", norm.c_str());
                }
                std::printf(" -- %s\n", why);
                std::printf("info string   position it was refused on: %s\n", board2fen(b, fbuf));
                std::printf("info string   our board and the driver's have DIVERGED; dropping the "
                            "remaining %zu move(s), so this root is NOT the driver's position\n",
                            moves.size() - move_index);
                std::fflush(stdout);
                break;
            }
            updateHash(h, b, m, ff_move(b, m), z);
            game_hashes.push_back(h.hash);
            //st->repetition of the position just reached, bounded by rule50 and pliesFromNull.
            int rep = 0;
            const int idx = (int)game_hashes.size() - 1;
            const int end = std::min(idx, (int)b.halfmoveClock);
            for (int i = 4; i <= end; i += 2)
                if (game_hashes[(size_t)(idx - i)] == h.hash) {
                    rep = game_rep[(size_t)(idx - i)] ? -i : i;
                    break;
                }
            game_rep.push_back(rep);
        }
        //The root is the search's ply 0; the history is what came before it.
        game_hashes.pop_back();
        game_rep.pop_back();
        char fenbuf[MAX_FEN_STRING_LEN];
        root_fen = board2fen(b, fenbuf);
        root_move_number = b.moveNumber;
        root_board = Board{};
        fen2board(root_board, root_fen.c_str());
    }

    //SYZYGY AT THE ROOT: a DTZ probe that answers without searching. Probed by whether it
    //answers, not by piece count -- a partial 6-piece set raises TB_LARGEST to 6 for tables that
    //were never downloaded. Castling rights are excluded because Syzygy cannot express them.
    bool probe_tb_root(std::string& best, int& score) {
        if (!tb_ready_ || TB_LARGEST == 0) return false;
        const Board& b = root_board;
        if (b.castlingRights) return false;
        const unsigned pieces = (unsigned)bitCount(b.side[ColorWhite] | b.side[ColorBlack]);
        if (pieces > TB_LARGEST) return false;
        const Square ep = legalEnPassantMove(b);
        const unsigned res = tb_probe_root(
            b.side[ColorWhite], b.side[ColorBlack], b.pieceTypes[King - 1],
            b.pieceTypes[Queen - 1], b.pieceTypes[Rook - 1], b.pieceTypes[Bishop - 1],
            b.pieceTypes[Knight - 1], b.pieceTypes[Pawn - 1],
            b.halfmoveClock, 0, ep == SquareNone ? 0 : (unsigned)ep,
            (b.sideToMove ^ 1) == ColorBlack ? 1 : 0, NULL);
        if (res == TB_RESULT_FAILED) return false;
        //Mate and stalemate carry no move; the search handles having none.
        if (res == TB_RESULT_STALEMATE || res == TB_RESULT_CHECKMATE) return false;
        //A tablebase win is not a mate: DTZ counts to zeroing, so the score is the tablebase
        //band's, less the distance, and claims no mate.
        const unsigned wdl = TB_GET_WDL(res);
        const int dtz = (int)TB_GET_DTZ(res);
        score = wdl == 4 ? TB_CP - dtz : (wdl == 0 ? -(TB_CP - dtz) : 0);
        const unsigned from = TB_GET_FROM(res), to = TB_GET_TO(res), promo = TB_GET_PROMOTES(res);
        best = std::string(square[from]) + square[to];
        if (uciPromoLetter[6 - promo]) best += uciPromoLetter[6 - promo];
        return true;
    }

    void go(const uci::Limits& lim) override {
        stop();
        last_limits_ = lim;
        apply_threads();
        apply_hash();
        out_of_time.store(false);
        for (int t = 0; t < MAX_THREADS; ++t) { thread_nodes[t].store(0); thread_tbhits[t].store(0); }
        tt_gen = (uint8_t)((tt_gen + 1) & 31);       //new_search()
        search_start = std::chrono::steady_clock::now();
        fixnodes = lim.nodes;
        fixdepth = lim.depth;
        go_mate = lim.mate;
        ponder_flag.store(lim.ponder);
        infinite_flag = lim.infinite;
        set_clock(lim);
        //Tablebases::Config: the in-tree probe covers what the tables hold, and probes at every
        //depth when the limit asks for more pieces than they have.
        tb_rule50 = o_tbrule50;
        tb_probe_depth = (int)o_tbdepth;
        tb_cardinality = tb_ready_ ? (unsigned)o_tblimit : 0;
        if (tb_cardinality > TB_LARGEST) { tb_cardinality = TB_LARGEST; tb_probe_depth = 0; }
        if (!lim.searchmoves.empty()) {
            std::printf("info string searchmoves is not implemented; searching all moves\n");
            std::fflush(stdout);
        }
        //EVERY SEARCH TAKES A NEW ID, whether or not it posts a request. An answer is stamped with
        //the id that asked for it, so one arriving after its search has ended can never match a
        //later search -- which it did while only a posted request moved the id on: a 6-piece
        //position's late answer was played as the bestmove of the 32-piece position after it.
        ++online_id_;
        //Answered by the local tables: nothing to search. Not after go ponder, which must not
        //answer on its own; the answer is still there on ponderhit.
        if (!lim.ponder) {
            std::string tb_best; int tb_score = 0;
            if (probe_tb_root(tb_best, tb_score)) {
                std::printf("info depth 1 score cp %d nodes 0 time 0 pv %s\n", tb_score, tb_best.c_str());
                std::printf("bestmove %s\n", tb_best.c_str());
                std::fflush(stdout);
                return;
            }
        }
        //THE LOCAL TABLES COULD NOT ANSWER, so ask lichess, posted before the search so the two
        //overlap: at most seven pieces, no castling, not for a ponder search, whose position is a
        //guess. Decided by the probe failing, not by the piece count -- a partial 6-piece set raises
        //TB_LARGEST to 6, and a piece-count test then never asks about the 6-piece tables that
        //were not downloaded. creatica_mcts decides the same way. Only the root is ever sent:
        //lichess rate-limits, so the tree uses the local tables alone.
        if (!lim.ponder && o_online && !root_board.castlingRights
            && bitCount(root_board.side[ColorWhite] | root_board.side[ColorBlack]) <= 7)
            post_online_probe(root_fen);
        driver_ = std::thread([this] { this->run(); });
    }

    void stop() override {
        out_of_time.store(true);
        if (driver_.joinable()) driver_.join();
    }

    //A ponder search has no deadline; ponderhit installs the real one, and the running search
    //becomes a timed one without restarting.
    void ponderhit() override {
        uci::Limits lim = last_limits_;
        lim.ponder = false;
        last_limits_ = lim;
        set_clock(lim);
        ponder_flag.store(false);
    }

    void quit() override {
        stop();
        pool_stop();
        if (ol_worker_started) {
            { std::lock_guard<std::mutex> lk(ol_mtx); ol_quit = true; }
            ol_cv.notify_all();
            if (ol_worker.joinable()) ol_worker.join();
            curl_global_cleanup();
        }
    }

    bool custom(const std::string& line) override {
        if (line == "eval") {
            //The evaluation every node of this search sees, with no optimism.
            Worker& w = uci_worker();
            if (!w.ctx_ready) { init_nnue_context(w.ctx); w.ctx_ready = true; }
            w.board = root_board;
            accumulator_stack_reset(w.ctx);
            w.root_optimism = 0;
            if (w.stm_in_check()) std::printf("info string side to move is in check; no static eval\n");
            else std::printf("info string static eval %s (side to move)\n",
                             format_score(w.static_eval(), w.board).c_str());
            std::fflush(stdout);
            return true;
        } else if (line == "pieces") {
            std::printf("%d\n", bitCount(root_board.side[ColorWhite] | root_board.side[ColorBlack]));
            std::fflush(stdout);
            return true;
        }
        return false;
    }

private:
    void run() {
        pool_run(JOB_SEARCH);
        const int bt = best_thread(search_threads);
        const ThreadResult& r = results[bt];
        std::string best = r.have ? uci_of(r.best) : std::string();
        std::string ponder = (r.have && r.pvlen > 1) ? uci_of(r.pv[1]) : std::string();
        //THE ONLINE ANSWER, if it arrived for this move while we searched. A tablebase verdict
        //beats a search.
        {
            std::lock_guard<std::mutex> lk(ol_ans_mtx);
            if (o_online && ol_ans_id == online_id_ && !ol_ans_move.empty()) {
                std::printf("info depth 1 score cp %d nodes 0 time %lld tbhits 1 pv %s\n",
                            ol_ans_score, elapsed_ms(), ol_ans_move.c_str());
                best = ol_ans_move;
                ponder.clear();
            }
        }
        //A search that found nothing still has to answer, or the GUI waits forever.
        if (best.empty()) best = any_legal_move();
        if (best.empty())         std::printf("bestmove 0000\n");
        else if (ponder.empty())  std::printf("bestmove %s\n", best.c_str());
        else                      std::printf("bestmove %s ponder %s\n", best.c_str(), ponder.c_str());
        std::fflush(stdout);
    }

    //A Worker for the protocol thread's own uses of the search's functions, made on first use.
    Worker& uci_worker() {
        if (!uci_worker_) uci_worker_ = std::make_unique<Worker>();
        return *uci_worker_;
    }

    std::string any_legal_move() {
        Worker& w = uci_worker();
        w.board = root_board;
        Mv mv[MAX_MOVES];
        const int n = w.generate(mv, false);
        return n > 0 ? uci_of(mv[0]) : std::string();
    }

    //The worker, started on demand and joined in quit().
    void start_online_worker() {
        if (ol_worker_started) return;
        ol_worker_started = true;
        curl_global_init(CURL_GLOBAL_DEFAULT);
        ol_worker = std::thread([] {
            for (;;) {
                std::string fen; uint64_t id;
                {
                    std::unique_lock<std::mutex> lk(ol_mtx);
                    ol_cv.wait(lk, [] { return ol_req_live || ol_quit; });
                    if (ol_quit) return;
                    fen = ol_req_fen; id = ol_req_id; ol_req_live = false;
                }
                int score = 0; std::string move;
                bool answered = false;
                {
                    std::lock_guard<std::mutex> lk(ol_ans_mtx);
                    auto it = ol_cache.find(fen);
                    if (it != ol_cache.end()) {
                        score = it->second.first; move = it->second.second;
                        answered = !move.empty();
                    }
                }
                if (!answered) {
                    answered = ol_probe(fen, score, move);
                    std::lock_guard<std::mutex> lk(ol_ans_mtx);
                    ol_cache[fen] = { score, answered ? move : std::string() };
                }
                if (!answered) continue;
                std::lock_guard<std::mutex> lk(ol_ans_mtx);
                ol_ans_id = id; ol_ans_move = move; ol_ans_score = score;
            }
        });
    }
    void post_online_probe(const std::string& fen) {
        start_online_worker();
        std::lock_guard<std::mutex> lk(ol_mtx);
        ol_req_fen = fen;
        ol_req_id = online_id_;
        ol_req_live = true;
        ol_cv.notify_one();
    }

    //tb_init() is not re-entrant here, the same guard creatica.cpp uses.
    void on_syzygy_path() {
        if (tb_ready_) {
            std::printf("info string SyzygyPath: tablebases already initialised; ignoring\n");
            std::fflush(stdout);
            return;
        }
        if (syzygy_path_.empty()) return;
        tb_init(syzygy_path_.c_str());
        if (TB_LARGEST == 0) {
            std::printf("info string error no tablebases found in %s\n", syzygy_path_.c_str());
        } else {
            tb_ready_ = true;
            std::printf("info string tablebases initialised in %s, max %u pieces\n",
                        syzygy_path_.c_str(), TB_LARGEST);
        }
        std::fflush(stdout);
    }

    //CREATICA'S OWN TIME MANAGEMENT, formula for formula: the latency allowance, the capping
    //below MIN_TIME_THRESHOLD and the rest are documented against measurements in creatica.cpp,
    //and an alpha-beta search has no reason to want a different allocation.
    void set_clock(const uci::Limits& lim) {
        if (lim.ponder || lim.infinite) { unlimited.store(true); return; }
        double alloc;
        if (lim.movetime_ms > 0) {
            alloc = (double)lim.movetime_ms * 0.99;
        } else if (!lim.wtime && !lim.btime) {
            unlimited.store(true);          //"go", "go depth N", "go nodes N": nothing to budget
            return;
        } else {
            const int64_t remaining = root_board.sideToMove == ColorWhite ? lim.wtime : lim.btime;
            const int64_t inc       = root_board.sideToMove == ColorWhite ? lim.winc  : lim.binc;
            int moves_left = lim.movestogo ? lim.movestogo : MAX_MOVES_REMAINING - root_move_number;
            if (moves_left < MIN_MOVES_REMAINING) moves_left = MIN_MOVES_REMAINING;
            int64_t r = remaining;
            if (r > TIME_SAFETY_BUFFER) r -= TIME_SAFETY_BUFFER;
            alloc = (double)r / moves_left + (double)inc * 0.5;
            alloc -= LATENCY_ALLOWANCE;
            if (remaining < MIN_TIME_THRESHOLD) alloc = std::min(alloc, (double)remaining * 0.5);
            if (inc < 1000 && alloc < 3000) alloc = 100;
            if (alloc < 100) alloc = 100;
        }
        const long long start_ns = (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       search_start.time_since_epoch()).count();
        deadline_ns.store(start_ns + (long long)(alloc * 1e6));
        unlimited.store(false);
    }

    //Three entries per bucket, as upstream's clusters; resized only when the size changes,
    //because a resize empties the table.
    void apply_hash() {
        const size_t buckets = std::max<size_t>(1, (size_t)o_hash * 1024 * 1024 / (sizeof(TTEntry) * TT_WAYS));
        if (buckets != tt_buckets || tt.empty()) {
            tt_buckets = buckets;
            tt.assign(tt_buckets * TT_WAYS, TTEntry{});
        }
    }
    //A new thread count is a new pool with cleared tables, as upstream's resize clears them.
    void apply_threads() {
        if ((int)pool.size() == (int)o_threads) return;
        pool_stop();
        pool_start((int)o_threads);
        reset_shared_histories((size_t)search_threads);
        pool_run(JOB_CLEAR);
    }

    //The bestmove has to reach the driver and the next position come back, and that time comes
    //off the clock without ever having been in the allocation (creatica.cpp).
    static constexpr double LATENCY_ALLOWANCE = 300.0;

    std::thread driver_;
    std::unique_ptr<Worker> uci_worker_;
    uci::Limits last_limits_;
    int64_t o_hash = 64, o_threads = 1;
    int64_t o_tbdepth = 1, o_tblimit = 7;
    bool o_tbrule50 = true;
    bool o_online = true;
    uint64_t online_id_ = 0;
    std::string syzygy_path_;
    bool tb_ready_ = false;
};

int main(int argc, char ** argv) {
    AlphaBetaEngine engine;
    return uci::uci_main(engine, argc, argv);
}
