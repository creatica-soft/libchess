// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -I /Users/ap/libchess -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess -o perft perft.cpp

//perft.cpp - the move-SET test for libchess.
//
//test_pos.cpp checks five invariants (FEN round-trip, reconcile(), incremental updateHash()
//vs getHash(), incremental NNUE accumulator vs a fresh one, and an external Stockfish eval)
//but every one of them is a property of a move the generator ITSELF produced. A generator
//that silently emits a strict SUBSET of the legal moves satisfies all five - which is
//exactly what the pinFinder false-pin bug did until it was fixed. perft is the only test
//that checks the move SET rather than individual moves: it counts leaf nodes and compares
//against published counts.
//
//No NNUE here. Perft needs no evaluation, so init_nnue() is deliberately NOT called and the
//two embedded nets (~110 MB) are never paged in. do_move_dp() still works: its DirtyPiece
//and DirtyThreats arguments are plain out-parameters that it writes and never reads back,
//so a stack-local pair per move is all it needs - no NNUEContext, no AccumulatorStack.
//The one rule is that DirtyThreats must be FRESH for every move: do_move_dp() only appends
//to dts.list (via Stockfish::add_dirty_threat) and never clears it - AccumulatorStack::push()
//is what normally resets it, with a placement new (nnue/nnue/nnue_accumulator.cpp:145).
//Reusing one DirtyThreats across moves would run ValueList<DirtyThreat,96> off its end.

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "nnue/bitboard.h"   //Stockfish::Bitboards::init(), Stockfish::DirtyPiece/DirtyThreats
#include "libchess.h"

#define DEFAULT_DEPTH 4   //deliberately shallow: ./perft perft_suite.txt is then ~11M nodes, a few seconds

//------------------------------------------------------------------------------------------
// globals
//------------------------------------------------------------------------------------------

static Zobrist z = {};              //not used by perft; kept so the documented init order holds
static bool     g_reconcile = false;
static uint64_t g_reconcileFailures = 0;

enum MakeMode { MM_FF = 0, MM_DO = 1, MM_DP = 2 };
static const char * makeModeName[] = { "ff_move", "do_move", "do_move_dp" };

//------------------------------------------------------------------------------------------
// helpers
//------------------------------------------------------------------------------------------

//UCI text for a move. NOTE for chess960: libchess encodes castling as king-takes-own-rook,
//so move.dst is the ROOK square (board.cpp castlingMoves()). That is exactly what Stockfish
//prints with "setoption name UCI_Chess960 value true", so divide output lines up. It must be
//formatted BEFORE the move is made - do_move(), do_move_dp() and ff_move() all rewrite
//move.dst to the standard king square (g1/c1) when they recognise a castling move.
static const char * moveToUci(const Move& mv, char * buf) {
    buf[0] = square[mv.src][0];
    buf[1] = square[mv.src][1];
    buf[2] = square[mv.dst][0];
    buf[3] = square[mv.dst][1];
    const char p = uciPromoLetter[mv.promoType];   //'\0' for PieceTypeNone
    if (p) { buf[4] = p; buf[5] = '\0'; }
    else buf[4] = '\0';
    return buf;
}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char) s[a])) a++;
    while (b > a && isspace((unsigned char) s[b - 1])) b--;
    return s.substr(a, b - a);
}

//fen2board() insists on exactly six space-separated fields (fen.cpp) and strncpy()s into a
//MAX_FEN_STRING_LEN buffer, so pad 4-field FENs (the form the published perft suites use)
//and reject anything too long.
static std::string normalizeFen(const std::string& in) {
    std::istringstream is(in);
    std::vector<std::string> f;
    std::string t;
    while (is >> t) f.push_back(t);
    if (f.size() == 4) { f.push_back("0"); f.push_back("1"); }
    if (f.size() != 6) return std::string();
    std::string out = f[0];
    for (size_t i = 1; i < 6; i++) { out += " "; out += f[i]; }
    if (out.size() + 1 > MAX_FEN_STRING_LEN) return std::string();
    return out;
}

//------------------------------------------------------------------------------------------
// the search
//------------------------------------------------------------------------------------------

template<int Mode, bool Bulk, bool Divide>
static uint64_t perft(Board& board, int depth);

//make -> recurse -> unmake for one move. All three make-move functions take Move& and mutate
//it (they set move.type, and rewrite move.dst for castling), so each gets its own copy and
//the caller's enumeration state is left alone.
template<int Mode, bool Bulk>
static uint64_t descend(Board& board, const Move& parentMove, int depth) {
    Move move = parentMove;
    if constexpr (Mode == MM_FF) {
        //ff_move() keeps no StateInfo, so there is nothing for undo_move() to restore from:
        //this variant has to copy-make.
        Board child = board;
        ff_move(child, move);
        if (g_reconcile && reconcile(child)) g_reconcileFailures++;
        return perft<Mode, Bulk, false>(child, depth - 1);
    } else if constexpr (Mode == MM_DO) {
        StateInfo state = {};
        do_move(board, move, state);
        if (g_reconcile && reconcile(board)) g_reconcileFailures++;
        const uint64_t n = perft<Mode, Bulk, false>(board, depth - 1);
        undo_move(board, move, state);
        if (g_reconcile && reconcile(board)) g_reconcileFailures++;
        return n;
    } else {
        StateInfo state = {};
        Stockfish::DirtyPiece   dp{};
        Stockfish::DirtyThreats dts;   //default-init: list.size_ == 0, everything else is
                                       //written by do_move_dp() before it is read
        do_move_dp(board, move, state, dp, dts);
        if (g_reconcile && reconcile(board)) g_reconcileFailures++;
        const uint64_t n = perft<Mode, Bulk, false>(board, depth - 1);
        undo_move(board, move, state);
        if (g_reconcile && reconcile(board)) g_reconcileFailures++;
        return n;
    }
}

//The canonical staged, pin-aware, legal-only generator loop, copied from test_pos.cpp
//(:145-160) and test_smp.cpp (compute_move_evals(), :482-517).
template<int Mode, bool Bulk, bool Divide>
static uint64_t perft(Board& board, int depth) {
    if (depth <= 0) return 1;

    uint64_t nodes = 0;
    char     ucibuf[8];
    Move     move;

    auto [kingMoveBB, pinned, pinning, checkers, kingSq] = kingMoves(board);

    //king moves first - if there is more than one checker these are the ONLY legal moves
    uint64_t moves = kingMoveBB;
    move.src       = kingSq;
    move.promoType = PieceTypeNone;
    while (moves) {
        move.dst = lsBit(moves);
        const uint64_t sub = (Bulk && depth == 1) ? 1ULL : descend<Mode, Bulk>(board, move, depth);
        if (Divide) printf("%s: %llu\n", moveToUci(move, ucibuf), (unsigned long long) sub);
        nodes += sub;
        moves &= moves - 1;
    }

    if (bitCount(checkers) <= 1) {
        uint64_t check_mask = 0xffffffffffffffffULL, ep_mask = 0ULL;
        if (checkers) {
            const std::pair<uint64_t, uint64_t> cm = checkMask(board, kingSq, checkers);
            check_mask = cm.first;
            ep_mask    = cm.second;
        }
        for (PieceType pt = Queen; pt >= Pawn; --pt) {
            uint64_t occupations = board.side[board.sideToMove] & board.pieceTypes[pt - 1];
            while (occupations) {
                move.src = lsBit(occupations);
                moves = piece_moves(board, pt, move.src, kingSq, pinned, pinning, check_mask, ep_mask);
                while (moves) {
                    move.dst = lsBit(moves);
                    //promotions are expanded by the caller, as everywhere else in this repo
                    PieceType startPiece = PieceTypeNone, endPiece = PieceTypeNone;
                    if (promoMove(board, move)) { startPiece = Knight; endPiece = Queen; }
                    if (Bulk && depth == 1 && !Divide) {
                        nodes += (startPiece == Knight) ? 4ULL : 1ULL;
                    } else {
                        for (move.promoType = startPiece; move.promoType <= endPiece;
                             move.promoType = (PieceType)(move.promoType + 1)) {
                            const uint64_t sub = (Bulk && depth == 1)
                                                     ? 1ULL
                                                     : descend<Mode, Bulk>(board, move, depth);
                            if (Divide) printf("%s: %llu\n", moveToUci(move, ucibuf), (unsigned long long) sub);
                            nodes += sub;
                        }
                        move.promoType = PieceTypeNone;
                    }
                    moves &= moves - 1;
                }
                occupations &= occupations - 1;
            }
        }
    }
    return nodes;
}

#define PERFT_DISPATCH(MODE)                                    \
    (bulk ? (divide ? perft<MODE, true, true>(board, depth)     \
                    : perft<MODE, true, false>(board, depth))   \
          : (divide ? perft<MODE, false, true>(board, depth)    \
                    : perft<MODE, false, false>(board, depth)))

static uint64_t run_perft(Board& board, int depth, int mode, bool bulk, bool divide) {
    switch (mode) {
    case MM_FF: return PERFT_DISPATCH(MM_FF);
    case MM_DO: return PERFT_DISPATCH(MM_DO);
    default:    return PERFT_DISPATCH(MM_DP);
    }
}

//------------------------------------------------------------------------------------------
// suite file
//------------------------------------------------------------------------------------------

struct SuiteEntry {
    std::string fen;
    std::vector<std::pair<int, uint64_t> > expect;
};

//accepts both
//   FEN;depth;expected
//   FEN ;D1 20 ;D2 400 ;D3 8902           (the form the published suites use)
//blank lines and lines whose first non-space character is '#' are skipped
static bool parseSuiteLine(const std::string& raw, SuiteEntry& e) {
    std::string line = raw;
    while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == '\n'))
        line.erase(line.size() - 1);
    const std::string t = trim(line);
    if (t.empty() || t[0] == '#') return false;

    std::vector<std::string> fields;
    std::string cur;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == ';') { fields.push_back(cur); cur.clear(); }
        else cur += line[i];
    }
    fields.push_back(cur);

    e.fen = normalizeFen(fields[0]);
    e.expect.clear();
    if (e.fen.empty()) {
        fprintf(stderr, "perft: bad FEN in suite line: %s\n", t.c_str());
        return false;
    }

    std::vector<std::string> plain;
    for (size_t i = 1; i < fields.size(); i++) {
        const std::string s = trim(fields[i]);
        if (s.empty()) continue;
        if ((s[0] == 'D' || s[0] == 'd') && s.size() > 1 && isdigit((unsigned char) s[1])) {
            size_t p = 1;
            while (p < s.size() && isdigit((unsigned char) s[p])) p++;
            const int d = atoi(s.substr(1, p - 1).c_str());
            while (p < s.size() && !isdigit((unsigned char) s[p])) p++;
            if (p >= s.size()) continue;
            e.expect.push_back(std::make_pair(d, (uint64_t) strtoull(s.c_str() + p, NULL, 10)));
        } else plain.push_back(s);
    }
    if (e.expect.empty() && plain.size() == 2)
        e.expect.push_back(std::make_pair(atoi(plain[0].c_str()),
                                          (uint64_t) strtoull(plain[1].c_str(), NULL, 10)));
    if (e.expect.empty()) {
        fprintf(stderr, "perft: no depth/count pairs in suite line: %s\n", t.c_str());
        return false;
    }
    return true;
}

//------------------------------------------------------------------------------------------

static void usage(const char * argv0) {
    printf("perft - move-generation node counter for libchess\n"
           "\n"
           "usage:\n"
           "  %s [options]                     start position\n"
           "  %s [options] <6 FEN words>       e.g. 8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1\n"
           "  %s [options] <suite file>        lines of  FEN;depth;expected  or  FEN ;D1 n ;D2 n\n"
           "\n"
           "options:\n"
           "  -d, --depth N  depth for a single position; in suite mode an upper CAP on the\n"
           "                 listed depths (default %d; -d 0 runs every listed depth)\n"
           "  -f, --file P   suite file (same as giving it positionally)\n"
           "      --divide   print per-root-move counts; compare with Stockfish 'go perft N'\n"
           "                 (add 'setoption name UCI_Chess960 value true' for a 960 position)\n"
           "      --nobulk   do not bulk-count at depth 1 - make and unmake every leaf move,\n"
           "                 so do_move/undo_move are exercised at every node\n"
           "      --make M   make-move under test: dp (default), do, ff, or all\n"
           "      --check    run reconcile() after every make and unmake (slow)\n"
           "  -h, --help     this text\n"
           "\n"
           "exit status is non-zero if any count differs from the expected one, if the make\n"
           "modes disagree with each other, or if --check found a bitboard/mailbox mismatch.\n",
           argv0, argv0, argv0, DEFAULT_DEPTH);
}

int main(int argc, char ** argv) {
    int  depth  = DEFAULT_DEPTH;
    bool bulk   = true;
    bool divide = false;
    int  modes[3] = { MM_DP, MM_DP, MM_DP };
    int  nmodes = 1;
    std::string file;
    std::vector<std::string> pos;

    for (int i = 1; i < argc; i++) {
        const char * a = argv[i];
        //a lone "-" is a FEN field (castling / en passant), never an option
        if (a[0] == '-' && a[1] != '\0') {
            if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
            else if ((!strcmp(a, "-d") || !strcmp(a, "--depth")) && i + 1 < argc) depth = atoi(argv[++i]);
            else if ((!strcmp(a, "-f") || !strcmp(a, "--file")) && i + 1 < argc) file = argv[++i];
            else if (!strcmp(a, "--nobulk")) bulk = false;
            else if (!strcmp(a, "--divide")) divide = true;
            else if (!strcmp(a, "--check")) g_reconcile = true;
            else if (!strcmp(a, "--make") && i + 1 < argc) {
                const char * m = argv[++i];
                if (!strcmp(m, "ff")) { nmodes = 1; modes[0] = MM_FF; }
                else if (!strcmp(m, "do")) { nmodes = 1; modes[0] = MM_DO; }
                else if (!strcmp(m, "dp")) { nmodes = 1; modes[0] = MM_DP; }
                else if (!strcmp(m, "all")) { nmodes = 3; modes[0] = MM_DP; modes[1] = MM_DO; modes[2] = MM_FF; }
                else { fprintf(stderr, "perft: unknown --make '%s'\n", m); return 2; }
            } else { fprintf(stderr, "perft: unknown option '%s'\n", a); usage(argv[0]); return 2; }
        } else pos.push_back(std::string(a));
    }

    std::string fenArg;
    if (file.empty() && pos.size() == 1) {
        if (pos[0].find('/') == std::string::npos) file = pos[0];   //test_pos convention
        else fenArg = pos[0];                                       //a whole quoted FEN
    } else if (pos.size() >= 4) {
        const size_t n = (pos.size() >= 6) ? 6 : pos.size();
        for (size_t i = 0; i < n; i++) { if (i) fenArg += " "; fenArg += pos[i]; }
    } else if (!pos.empty() && file.empty()) {
        fprintf(stderr, "perft: expected a suite file, a quoted FEN, or 4/6 FEN words\n");
        return 2;
    }

    //required init, in this order. Bitboards::init() builds the sliding-piece attack tables
    //(BetweenBB / LineBB / magics) that board.cpp's generator reads - it segfaults without it.
    Stockfish::Bitboards::init();
    zobristHash(z);
    //deliberately NOT init_nnue() / init_nnue_context() - perft evaluates nothing

    int      failures = 0;
    uint64_t runs = 0;

    if (!file.empty()) {
        std::ifstream in(file.c_str());
        if (!in.is_open()) { fprintf(stderr, "perft: cannot open %s\n", file.c_str()); return 2; }
        std::string line;
        while (std::getline(in, line)) {
            SuiteEntry e;
            if (!parseSuiteLine(line, e)) continue;
            //a FRESH board per FEN: fen2board() never resets board.isChess960 (the assignment
            //at fen.cpp:175 is commented out), so a reused Board would carry isChess960 == true
            //into every position after the first Shredder-FEN one.
            Board root = {};
            if (fen2board(root, e.fen.c_str())) { failures++; continue; }
            for (size_t k = 0; k < e.expect.size(); k++) {
                const int      d    = e.expect[k].first;
                const uint64_t want = e.expect[k].second;
                if (depth > 0 && d > depth) continue;
                uint64_t first = 0;
                for (int m = 0; m < nmodes; m++) {
                    Board board = root;   //fresh copy: a broken make/unmake cannot poison the next run
                    const auto t0 = std::chrono::high_resolution_clock::now();
                    const uint64_t got = run_perft(board, d, modes[m], bulk, false);
                    const double secs = std::chrono::duration<double>(
                        std::chrono::high_resolution_clock::now() - t0).count();
                    runs++;
                    const bool ok = (got == want) && (m == 0 || got == first);
                    if (m == 0) first = got;
                    if (!ok) failures++;
                    printf("[%s] %-10s d%-2d %14llu  want %14llu  %8.3fs  %s\n",
                           ok ? " ok " : "FAIL", makeModeName[modes[m]], d,
                           (unsigned long long) got, (unsigned long long) want, secs,
                           e.fen.c_str());
                    fflush(stdout);
                }
            }
        }
    } else {
        if (depth < 1) { fprintf(stderr, "perft: -d must be >= 1 for a single position\n"); return 2; }
        const std::string fen = normalizeFen(fenArg.empty() ? std::string(startPos) : fenArg);
        if (fen.empty()) { fprintf(stderr, "perft: bad FEN\n"); return 2; }
        Board root = {};
        if (fen2board(root, fen.c_str())) return 2;
        printf("position %s\n", fen.c_str());
        uint64_t first = 0;
        for (int m = 0; m < nmodes; m++) {
            Board board = root;
            if (divide) printf("--- divide %s depth %d ---\n", makeModeName[modes[m]], depth);
            const auto t0 = std::chrono::high_resolution_clock::now();
            const uint64_t got = run_perft(board, depth, modes[m], bulk, divide);
            const double secs = std::chrono::duration<double>(
                std::chrono::high_resolution_clock::now() - t0).count();
            runs++;
            if (m == 0) first = got;
            else if (got != first) {
                failures++;
                fprintf(stderr, "FAIL: %s gives %llu but %s gives %llu\n",
                        makeModeName[modes[m]], (unsigned long long) got,
                        makeModeName[modes[0]], (unsigned long long) first);
            }
            printf("%-10s depth %d  nodes %llu  %.3fs  %.0f nps\n", makeModeName[modes[m]], depth,
                   (unsigned long long) got, secs, secs > 0 ? (double) got / secs : 0.0);
            fflush(stdout);
        }
    }

    if (g_reconcileFailures) {
        fprintf(stderr, "perft: reconcile() failed %llu times\n",
                (unsigned long long) g_reconcileFailures);
        failures++;
    }
    printf("perft: %llu runs, %d failures\n", (unsigned long long) runs, failures);
    return failures ? 1 : 0;
}
