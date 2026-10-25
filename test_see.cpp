// A CMake target: cmake . && make test_see.   Takes no arguments; prints one line per case
// and "WRONG" on any mismatch. The exit code is not a verdict -- read the output, as with
// test_pos.
// THE REGRESSION GUARD FOR get_see(). Four defects were fixed here on 2026-10-12..14 and every
// one of them returned a plausible-looking number rather than crashing, so the only way to keep
// them fixed is a set of positions with hand-checked answers.
// What get_see() returns on captures whose RECAPTURES are illegal. The move passed in is legal;
// every recapture the swap loop invents afterwards is not checked by anything.
#include "nnue/bitboard.h"
#include "libchess.h"
#include <cstdio>
#include <cstring>

static Zobrist z;

static void probe(const char * name, const char * fen, const char * uci, int expect) {
    Board b;
    if (fen2board(b, fen) != 0) { std::printf("%-34s FEN DID NOT PARSE\n", name); return; }
    Move m{};
    m.src = (Square)((uci[0] - 'a') + 8 * (uci[1] - '1'));
    m.dst = (Square)((uci[2] - 'a') + 8 * (uci[3] - '1'));
    m.promoType = PieceTypeNone;
    const int see = get_see(b, m);
    std::printf("%-34s %s  get_see = %6d   correct = %5d   %s\n",
                name, uci, see, expect, see == expect ? "ok" : "WRONG");
}

//Expectations are written in PIECES through the table get_see() itself uses, so they cannot drift
//from its units again: they were centipawn literals until get_see() moved to Stockfish's units.
static constexpr int P = pieceValueSF[Pawn], N = pieceValueSF[Knight], R = pieceValueSF[Rook],
                     Q = pieceValueSF[Queen];

int main() {
    Stockfish::Bitboards::init();
    zobristHash(z);
    // The defender of d5 is the black KING, and d5 stays attacked by Rd1 after it captures, so
    // Kxd5 is illegal: exd5 wins a clean pawn.
    probe("king recapture is illegal",
          "8/8/2k5/3p4/4P3/8/8/3RK3 w - - 0 1", "e4d5", P);
    // The defender of d5 is a knight PINNED to its king by Rf1, so Nxd5 is illegal and exd5 again
    // wins a clean pawn. attackers_to() knows nothing about pins.
    probe("pinned defender cannot recapture",
          "5k2/8/5n2/3p4/4P3/8/8/4KR2 w - - 0 1", "e4d5", P);
    // Control: an ordinary defended pawn, where the recapture IS legal. Pawn for pawn.
    probe("control, legal recapture",
          "8/8/2p5/3p4/4P3/8/8/4K3 w - - 0 1", "e4d5", 0);
    probe("king recapture is legal",
          "8/8/2k5/3p4/4P3/8/8/4K3 w - - 0 1", "e4d5", 0);
    // OVER-CORRECTION CHECK. A pinned piece may legally capture ALONG its pin ray, and the only
    // capturable square on that ray is the pinner's own. Here white's Rd3 looks pinned to Kd1 by the
    // black rook on d5 -- but d5 IS the destination, so Rxd5 captures the pinner and is legal.
    // cxd5 wins the rook: +R. If the pin exclusion is too blunt, white cannot recapture and it
    // comes back R - P.
    probe("pinned piece may take its pinner",
          "4k3/8/4p3/3r4/2P5/3R4/8/3K4 w - - 0 1", "c4d5", R);
    // ORDINARY CASES, as regression guards on removing the early break.
    // Queen takes a pawn defended by a pawn: wins a pawn, loses the queen.
    probe("losing capture, QxP defended",
          "4k3/8/2p5/3p4/8/8/3Q4/4K3 w - - 0 1", "d2d5", P - Q);
    // Knight takes a pawn defended by a pawn: wins a pawn, loses the knight.
    probe("losing capture, NxP defended",
          "4k3/8/2p5/3p4/4N3/8/8/4K3 w - - 0 1", "e4d5", P - N);
    // Two attackers against one defender: pawn takes, pawn recaptures, rook takes. Net a pawn.
    probe("two attackers, one defender",
          "4k3/8/2p5/3p4/2P5/3R4/8/4K3 w - - 0 1", "c4d5", P);
    // QUIET MOVES. SEE on a non-capture is not a contradiction: the exchange simply opens with
    // nothing won, and the first thing that happens is the opponent taking the piece that just
    // moved. These cases exist because Step 14 pruning asks about quiet moves, and because
    // pieceValueCP had only seven entries for an eight-value enum -- score[0] = pieceValueCP[victim]
    // read index 7, PieceTypeNone, OUT OF BOUNDS for every quiet move. The path was unreachable
    // while get_see() was only called on captures.
    // A knight stepping onto a square attacked by a pawn and defended by nothing: loses the knight.
    // (A black pawn attacks DOWN the board, so the pawn that covers d6 stands on c7, not c6. The
    // first version of these three cases put it on c6, which attacks d5 and nothing else.)
    probe("quiet move hangs the piece",
          "4k3/2p5/8/8/4N3/8/8/4K3 w - - 0 1", "e4d6", -N);
    // The same square, but now our own pawn defends it: knight for pawn, so still bad but less so.
    probe("quiet move onto a defended square",
          "4k3/2p5/8/2P5/4N3/8/8/4K3 w - - 0 1", "e4d6", -N + P);
    // A quiet move to a square nothing attacks is worth exactly nothing. This is the case that
    // returned garbage from the out-of-bounds read.
    probe("quiet move to a safe square",
          "4k3/8/8/8/4N3/8/8/4K3 w - - 0 1", "e4d6", 0);
    // A rook stepping onto a square a pawn attacks, with no defender: loses the rook.
    probe("quiet move hangs a rook",
          "4k3/2p5/8/8/8/8/8/3RK3 w - - 0 1", "d1d6", -R);
    return 0;
}
