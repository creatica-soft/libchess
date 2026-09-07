// c++ -std=c++20 -w -O2 -I /Users/ap/libchess -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess -o find_960_bug find_960_bug.cpp
//
// Finds the FIRST move after which reconcile() disagrees, and prints the exact path to it.
//
// ./perft --check reports a COUNT of reconcile failures, which is enough to know something is
// wrong but not to say what. Worse, feeding one of its printed FENs back in does not reproduce
// the original fault: board2fen() renders from a board that is already inconsistent, so the FEN
// is a lossy picture of the damage and re-parsing it produces a DIFFERENT broken board. The
// first divergence has to be caught in place, on the live board, which is what this does.

#include "nnue/bitboard.h"
#include "libchess.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Zobrist z = {};
static std::vector<std::string> path;
static bool found = false;

static const char * uci(const Move& mv, char * buf) {
    std::snprintf(buf, 8, "%s%s", square[mv.src], square[mv.dst]);
    return buf;
}

static void report(const char * when, Board& b, const Move& mv) {
    char buf[8];
    printf("\nFIRST DIVERGENCE after %s of %s\n", when, uci(mv, buf));
    printf("  path: ");
    for (auto& s : path) printf("%s ", s.c_str());
    printf("\n");
    char fen[MAX_FEN_STRING_LEN];
    printf("  board2fen (renders the MAILBOX): %s\n", board2fen(b, fen));
    printf("  isChess960=%d  castlingRights=%x  castlingRooks=%llx\n",
           (int)b.isChess960, (unsigned)b.castlingRights, (unsigned long long)b.castlingRooks);
    printf("  white rooks  bb=%016llx   black rooks bb=%016llx\n",
           (unsigned long long)(b.pieceTypes[Rook - 1] & b.side[ColorWhite]),
           (unsigned long long)(b.pieceTypes[Rook - 1] & b.side[ColorBlack]));
    printf("  mailbox rank1: ");
    for (int f = 0; f < 8; ++f) printf("%d ", (int)b.piecesOnSquares[f]);
    printf("\n  mailbox rank8: ");
    for (int f = 56; f < 64; ++f) printf("%d ", (int)b.piecesOnSquares[f]);
    printf("\n");
    found = true;
}

static void walk(Board& board, int depth) {
    if (found || depth <= 0) return;
    auto [kingMoveBB, pinned, pinning, checkers, kingSq] = kingMoves(board);

    auto try_move = [&](Move mv) {
        if (found) return;
        char buf[8];
        std::string txt = uci(mv, buf);
        StateInfo state = {};
        Move m = mv;
        do_move(board, m, state);
        if (reconcile(board)) { path.push_back(txt); report("MAKE", board, mv); return; }
        path.push_back(txt);
        walk(board, depth - 1);
        path.pop_back();
        undo_move(board, m, state);
        if (!found && reconcile(board)) { path.push_back(txt); report("UNMAKE", board, mv); }
    };

    Move move;
    move.src = kingSq; move.promoType = PieceTypeNone;
    uint64_t moves = kingMoveBB;
    while (moves && !found) { move.dst = (Square)popLSB(moves); try_move(move); }
    if (bitCount(checkers) > 1) return;

    auto [check_mask, ep_mask] = checkers ? checkMask(board, kingSq, checkers)
                                          : std::make_pair(0xffffffffffffffffULL, 0ULL);
    for (PieceType pt = Queen; pt >= Pawn && !found; --pt) {
        uint64_t occ = board.side[board.sideToMove] & board.pieceTypes[pt - 1];
        while (occ && !found) {
            move.src = (Square)popLSB(occ);
            uint64_t mv = piece_moves(board, pt, (Square)move.src, kingSq, pinned, pinning, check_mask, ep_mask);
            while (mv && !found) {
                move.dst = (Square)popLSB(mv);
                move.promoType = PieceTypeNone;
                if (pt == Pawn && promoMove(board, move)) {
                    for (PieceType p = Knight; p <= Queen && !found; ++p) { move.promoType = p; try_move(move); }
                    move.promoType = PieceTypeNone;
                } else try_move(move);
            }
        }
    }
}

int main(int argc, char ** argv) {
    Stockfish::Bitboards::init();
    zobristHash(z);

    std::string fen;
    for (int i = 1; i < argc - 1; ++i) { if (i > 1) fen += " "; fen += argv[i]; }
    int depth = (argc > 1) ? atoi(argv[argc - 1]) : 3;
    if (fen.empty()) fen = "qnnrkbbr/pppppppp/8/8/8/8/PPPPPPPP/QNNRKBBR w HDhd - 0 1";

    Board board = {};
    if (fen2board(board, fen.c_str())) { printf("bad FEN: %s\n", fen.c_str()); return 2; }
    printf("root: %s   depth %d\n", fen.c_str(), depth);
    if (reconcile(board)) { printf("ROOT ITSELF fails reconcile -- fen2board is the culprit\n"); return 1; }
    printf("root reconciles cleanly.\n");

    walk(board, depth);
    if (!found) printf("no divergence found to depth %d\n", depth);
    return found ? 1 : 0;
}
