// A CMake target: cmake . && make test_san.   Usage: ./test_san [perft_suite.txt ...]
// Prints one line per hand-checked case ("WRONG" on a mismatch), then one summary line per suite
// file. The exit code is not a verdict -- read the output, as with test_pos.
//
// THE REGRESSION GUARD FOR move2san(). Rewritten 2026-11-13: it used to read move.type, which only
// do_move() sets, so a move not classified by the caller lost its 'x' and its castling; en passant
// came out as "d6" for exd6 even when classified; no move got '+' or '#'; and disambiguation added
// a character per rival piece instead of SAN's minimal file / rank / both.
//
// Two parts. First, positions with hand-checked SAN, each covering one of those defects. Second,
// for every legal move along seeded random walks from each suite position: the SAN must parse
// back through san2move() to the same move, no two moves of one position may share a SAN, and the
// suffix must agree with the position after the move.
#include "nnue/bitboard.h"
#include "libchess.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

static Zobrist z;
static int wrong = 0;

static Move from_uci(const char * uci) {
    Move m{};
    uci2move_idx(uci, m);
    return m;
}

static void probe(const char * name, const char * fen, const char * uci, const char * expect) {
    Board b;
    if (fen2board(b, fen) != 0) { std::printf("%-40s FEN DID NOT PARSE\n", name); ++wrong; return; }
    Move m = from_uci(uci);
    normalise_castling(b, m);
    char san[32] = "";
    const char * got = move2san(b, m, san);
    const bool ok = got && std::strcmp(got, expect) == 0;
    if (!ok) ++wrong;
    std::printf("%-40s %-6s -> %-8s expected %-8s %s\n", name, uci, got ? got : "(null)", expect,
                ok ? "ok" : "WRONG");
}

//Every legal move, with the four promotions spelled out: illegal_reason() checks only the squares.
static std::vector<Move> legal_moves(Board& b) {
    std::vector<Move> out;
    for (int s = 0; s < 64; ++s) {
        if (!((b.side[b.sideToMove] >> s) & 1ULL)) continue;
        const bool pawn = PC_TYPE(b.piecesOnSquares[s]) == Pawn;
        for (int d = 0; d < 64; ++d) {
            Move m{};
            m.src = (Square)s;
            m.dst = (Square)d;
            if (!is_legal_move(b, m)) continue;
            if (pawn && (SQ_RANK(m.dst) == 0 || SQ_RANK(m.dst) == 7)) {
                for (PieceType p : {Knight, Bishop, Rook, Queen}) { m.promoType = p; out.push_back(m); }
            } else {
                out.push_back(m);
            }
        }
    }
    return out;
}

struct Totals { long positions = 0, moves = 0, failures = 0; };

static void fail(Totals& t, const Board& b, const Move& m, const char * san, const char * why) {
    if (++t.failures <= 20) {
        char fen[MAX_FEN_STRING_LEN];
        std::printf("  FAIL %s%s%s: SAN \"%s\" -- %s\n     on %s\n", square[m.src], square[m.dst],
                    m.promoType != PieceTypeNone ? "=" : "", san ? san : "(null)", why,
                    board2fen(b, fen));
    }
}

static void check_position(Board& b, Totals& t) {
    ++t.positions;
    std::set<std::string> seen;
    for (Move m : legal_moves(b)) {
        ++t.moves;
        char san[32] = "";
        const char * s = move2san(b, m, san);
        if (!s) { fail(t, b, m, s, "move2san returned null on a legal move"); continue; }
        if (!seen.insert(s).second) fail(t, b, m, s, "two moves share this SAN");

        Move back{};
        if (san2move(b, s, back) != 0) { fail(t, b, m, s, "san2move rejects it"); continue; }
        Move mn = m;
        normalise_castling(b, mn);
        Move bn = back;
        normalise_castling(b, bn);
        if (bn.src != mn.src || bn.dst != mn.dst || bn.promoType != mn.promoType)
            fail(t, b, m, s, "san2move reads it as a different move");

        Board after = b;
        StateInfo st{};
        Move made = m;
        do_move(after, made, st);
        const bool mate = isCheckMateStaleMate(after).mate;
        const bool check = getCheckers(after, kingSquare(after, after.sideToMove)) != 0;
        const char last = s[std::strlen(s) - 1];
        if (mate && last != '#') fail(t, b, m, s, "mates but has no '#'");
        if (!mate && check && last != '+') fail(t, b, m, s, "checks but has no '+'");
        if (!check && (last == '+' || last == '#')) fail(t, b, m, s, "suffix on a move that does not check");
    }
}

static void run_suite(const char * path) {
    std::ifstream in(path);
    if (!in) { std::printf("%s: cannot open\n", path); ++wrong; return; }
    Totals t;
    std::mt19937 rng(20261113);
    std::string line;
    while (std::getline(in, line)) {
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        const std::string fen = line.substr(first, line.find(';') - first);
        Board b;
        if (fen2board(b, fen.c_str()) != 0) continue;
        for (int ply = 0; ply < 300; ++ply) {
            check_position(b, t);
            std::vector<Move> moves = legal_moves(b);
            if (moves.empty()) break;
            Move m = moves[rng() % moves.size()];
            StateInfo st{};
            do_move(b, m, st);
        }
    }
    wrong += (int)t.failures;
    std::printf("%s: %ld positions, %ld moves, %ld failures %s\n", path, t.positions, t.moves,
                t.failures, t.failures ? "WRONG" : "ok");
}

int main(int argc, char ** argv) {
    Stockfish::Bitboards::init();
    zobristHash(z);

    probe("quiet pawn move", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "e2e4", "e4");
    probe("capture, unclassified, mates",
          "r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4", "h5f7", "Qxf7#");
    probe("pawn capture", "4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1", "e4d5", "exd5");
    probe("en passant", "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", "e5d6", "exd6");
    probe("castles kingside", "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1g1", "O-O");
    probe("castles queenside", "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1c1", "O-O-O");
    probe("black castles", "r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1", "e8g8", "O-O");
    probe("castling gives check", "5k2/8/8/8/8/8/8/4K2R w K - 0 1", "e1g1", "O-O+");
    probe("promotion gives check", "4k3/1P6/8/8/8/8/8/4K3 w - - 0 1", "b7b8q", "b8=Q+");
    probe("underpromotion", "4k3/1P6/8/8/8/8/8/4K3 w - - 0 1", "b7b8n", "b8=N");
    probe("capture-promotion", "2r1k3/1P6/8/8/8/8/8/4K3 w - - 0 1", "b7c8q", "bxc8=Q+");
    probe("disambiguate by file", "4k3/8/8/8/8/5N2/8/1N2K3 w - - 0 1", "b1d2", "Nbd2");
    probe("disambiguate by rank (1)", "4k3/8/8/R7/8/8/8/R3K3 w - - 0 1", "a1a3", "R1a3");
    probe("disambiguate by rank (5)", "4k3/8/8/R7/8/8/8/R3K3 w - - 0 1", "a5a3", "R5a3");
    probe("three queens: file and rank", "8/K7/8/8/4Q2Q/k7/8/7Q w - - 0 1", "h4e1", "Qh4e1");
    probe("three queens: file", "8/K7/8/8/4Q2Q/k7/8/7Q w - - 0 1", "e4e1", "Qee1");
    probe("three queens: rank", "8/K7/8/8/4Q2Q/k7/8/7Q w - - 0 1", "h1e1", "Q1e1");
    probe("pinned rival needs no disambiguation", "4r1k1/8/8/8/8/8/4N3/1N2K3 w - - 0 1", "b1c3", "Nc3");
    probe("back-rank mate", "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1", "a1a8", "Ra8#");
    probe("discovered check", "4k3/8/8/8/8/8/4B3/4R1K1 w - - 0 1", "e2d3", "Bd3+");

    if (argc < 2) {
        static char def[] = "perft_suite.txt";
        static char * defv[] = {nullptr, def};
        argv = defv;
        argc = 2;
    }
    for (int i = 1; i < argc; ++i) run_suite(argv[i]);
    std::printf("%s\n", wrong ? "SOME CHECKS WRONG" : "all checks ok");
    return wrong ? 1 : 0;
}
