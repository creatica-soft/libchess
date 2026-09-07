// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -I /Users/ap/libchess -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess bin2fen.cpp -o bin2fen
//
// Read the position bins written by pgn_parser.cpp and print one FEN per line, with the
// Stockfish evaluation that came with it.
//
// These are NOT the same format as lichess_evals_bin_writer.cpp produces. pgn_parser writes
//     5 bits count-1 | per piece: 6 sq + 1 colour + 3 type | 1 stm | 4 castling | 4 ep | 16 eval
// and then aligns -- with NO PV block. The trainer expects a PV count and move list next, so
// pointing it at one of these segfaults. That difference is the whole story of why
// lichess_db_broadcast_*.bin cannot be read by the training pipeline.
//
// It does not matter, because the PVs are not what we want from this data. Stockfish's chosen
// move is the target that saturated -- top-1 went 26.7% -> 33.08% -> 34.83% while Elo went
// +108 -> +113 -> level. What these files are worth is POSITIONS: several tens of millions of
// them from human tournament play, a distribution self-play does not reach on its own. Feed
// them through creatica with VisitDumpFile set and the targets come from a 35-ply search
// instead.
//
// The eval is still printed, for the two jobs it is good for and the one it is not:
//   - curation: skip positions already decided (nothing to learn) or forced (nothing to choose)
//   - drift detection: self-distillation has no external corrective, so tracking agreement with
//     Stockfish over training is how you notice the policy drifting into its own blind spots
//   - NOT a training target; see above.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include "nnue/bitboard.h"
#include "libchess.h"

struct BitReader {
    std::ifstream& in;
    uint64_t buf = 0;
    int bits = 0;
    explicit BitReader(std::ifstream& i) : in(i) {}
    // LSB-first, matching pgn_parser.cpp's BitStream: it accumulates `val << bits_in_buffer`
    // and emits the LOW byte, so the first bit written is the least significant one. Reading it
    // MSB-first produces plausible-looking garbage rather than an error, which is exactly how a
    // format mismatch hides.
    uint32_t read(int n) {
        while (bits < n) {
            const int c = in.get();
            if (c == EOF) break;
            buf |= ((uint64_t)(uint8_t)c) << bits;
            bits += 8;
        }
        const uint32_t v = (uint32_t)(buf & ((1ULL << n) - 1));
        buf >>= n;
        bits -= n;
        return v;
    }
    // The writer pads the final partial byte, so the next record starts on a byte boundary.
    // Discard exactly the padding: bits & 7 -- not the whole buffer, which may already hold a
    // fetched byte belonging to the next record.
    void align() { const int d = bits & 7; buf >>= d; bits -= d; }
    bool eof() { return in.peek() == EOF && bits == 0; }
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <file.bin> [max] [min_abs_cp] [max_abs_cp]\n"
            "  max         stop after this many positions (0 = all)\n"
            "  min/max_abs_cp  keep only positions whose |eval| is in this range.\n"
            "                  A decided position teaches the policy nothing, and a dead-drawn\n"
            "                  one teaches it little; 20..500 is a reasonable band.\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];
    const long   maxn   = argc > 2 ? std::strtol(argv[2], nullptr, 10) : 0;
    const int    mincp  = argc > 3 ? (int)std::strtol(argv[3], nullptr, 10) : 0;
    const int    maxcp  = argc > 4 ? (int)std::strtol(argv[4], nullptr, 10) : 1000000;

    Stockfish::Bitboards::init();

    std::ifstream in(path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
    BitReader r(in);

    long emitted = 0, seen = 0, skipped_filter = 0, skipped_bad = 0;
    char fen[MAX_FEN_STRING_LEN];

    while (!r.eof() && (maxn == 0 || emitted < maxn)) {
        const int np = (int)r.read(5) + 1;
        if (np < 2 || np > 32) { ++skipped_bad; break; }   // desync: stop rather than emit rubbish

        Board board = Board{};
        bool ok = true;
        for (int i = 0; i < np; ++i) {
            const int sq  = (int)r.read(6);
            const int col = (int)r.read(1);
            const int ty  = (int)r.read(3);
            if (ty == 0 || ty > 6) { ok = false; break; }
            const Piece pc = (Piece)((col << 3) | ty);
            board.piecesOnSquares[sq] = pc;
            board.side[col] |= (1ULL << sq);
            board.pieceTypes[ty - 1] |= (1ULL << sq);
        }
        board.sideToMove     = (Color)r.read(1);
        board.castlingRights = (uint8_t)r.read(4);
        board.enPassant      = (File)r.read(4);
        const int cp = (int16_t)(uint16_t)r.read(16);
        r.align();
        ++seen;
        if (!ok) { ++skipped_bad; continue; }

        // A record can carry a castling right with no rook or king behind it; msBit()/lsBit()
        // assert on an empty bitboard, so clear anything not actually backed by pieces. Same
        // sanitisation the trainer does, and for the same reason.
        const bool wk = board.piecesOnSquares[SquareE1] == WhiteKing;
        const bool bk = board.piecesOnSquares[SquareE8] == BlackKing;
        if (!(wk && board.piecesOnSquares[SquareH1] == WhiteRook)) board.castlingRights &= ~1;
        if (!(wk && board.piecesOnSquares[SquareA1] == WhiteRook)) board.castlingRights &= ~2;
        if (!(bk && board.piecesOnSquares[SquareH8] == BlackRook)) board.castlingRights &= ~4;
        if (!(bk && board.piecesOnSquares[SquareA8] == BlackRook)) board.castlingRights &= ~8;
        if (board.castlingRights & 1) board.castlingRooks |= SQ_BIT(SquareH1);
        if (board.castlingRights & 2) board.castlingRooks |= SQ_BIT(SquareA1);
        if (board.castlingRights & 4) board.castlingRooks |= SQ_BIT(SquareH8);
        if (board.castlingRights & 8) board.castlingRooks |= SQ_BIT(SquareA8);

        // Both kings, or it is not a position.
        if (!(board.side[ColorWhite] & board.pieceTypes[King - 1]) ||
            !(board.side[ColorBlack] & board.pieceTypes[King - 1])) { ++skipped_bad; continue; }

        const int acp = cp < 0 ? -cp : cp;
        if (acp < mincp || acp > maxcp) { ++skipped_filter; continue; }

        std::printf("%s\t%d\n", board2fen(board, fen), cp);
        ++emitted;
    }
    std::fprintf(stderr, "  read %ld, emitted %ld, filtered out %ld, malformed %ld\n",
                 seen, emitted, skipped_filter, skipped_bad);
    return 0;
}
