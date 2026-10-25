// c++ -std=c++20 -Wall -O2 -I .. -L .. epd2tsv.cpp -o epd2tsv -Wl,-lchess,-rpath,/Users/ap/libchess
//
// Converts an Arasan-style EPD suite into the 9-column TSV that abprobe5 reads. The prober needs
// column 2 to be a full six-field FEN and column 7 to be the acceptable moves in UCI; the other
// columns exist so the row count matches what the harness checks and are filled with placeholders.
//
// The suite lives in the repo rather than in a scratch directory on purpose: arasan2026.tsv
// existed only under /private/tmp, which was pruned between sessions, and a whole session's
// measurements referenced a file that no longer existed. Anything a result depends on has to
// survive the session that produced it.
#include <cstdio>
#include <cstring>
#include <string>
#include "nnue/bitboard.h"
#include "libchess.h"


int main(int argc, char ** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: epd2tsv <file.epd>\n"); return 2; }
    Stockfish::Bitboards::init();
    std::FILE * f = std::fopen(argv[1], "r");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    //std::printf("game\tply\tfen\tplayed_uci\tplayed_san\tbest_cp\tplayed_cp\tacceptable\tsf_lines\n");
    char line[1024];
    int n = 0, bad = 0;
    while (std::fgets(line, sizeof line, f)) {
        if (!std::strstr(line, "bm ")) continue;
        // The EPD position is everything before "bm"; it carries four fields, so the halfmove and
        // fullmove counters are appended to make a FEN that fen2board will accept.
        // The position is the FIRST FOUR whitespace-separated fields; everything after them is
        // operations, and their order is not fixed -- arasan2026.36 puts `id` before `bm`, which a
        // split on "bm " mis-parses into the FEN.
        char pos[256] = {0};
        char p1[80], p2[8], p3[16], p4[8];
        if (std::sscanf(line, "%79s %7s %15s %7s", p1, p2, p3, p4) != 4) { ++bad; continue; }
        std::snprintf(pos, sizeof pos, "%s %s %s %s 0 1", p1, p2, p3, p4);
        const std::string fen = pos;
        char * bm = std::strstr(line, "bm ");
        if (!bm) { ++bad; continue; }
        char san[64] = {0};
        if (std::sscanf(bm + 3, "%63[^;]", san) != 1) { ++bad; continue; }
        std::string s(san);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
        char id[64] = "unknown";
        if (const char * ip = std::strstr(line, "id \"")) std::sscanf(ip + 4, "%63[^\"]", id);
        Board b{};
        if (fen2board(b, fen.c_str()) != 0) {
            std::fprintf(stderr, "FEN rejected: %s\n", fen.c_str()); ++bad; continue;
        }
        Move mv{};
        if (san2move(b, s.c_str(), mv) != 0) {
            std::fprintf(stderr, "SAN rejected for %s: %s\n", id, s.c_str()); ++bad; continue;
        }
        std::string uci = std::string(square[mv.src]) + square[mv.dst];
        if (mv.promoType >= Knight && mv.promoType <= Queen && uciPromoLetter[mv.promoType])
            uci += uciPromoLetter[mv.promoType];
        //std::printf("%s\t0\t%s\t%s\t%s\t0\t0\t%s\t-\n", id, fen.c_str(), uci.c_str(), s.c_str(), uci.c_str());
        std::printf("%s\t%s\n", fen.c_str(), uci.c_str());
        ++n;
    }
    std::fclose(f);
    std::fprintf(stderr, "converted %d positions, %d rejected\n", n, bad);
    return bad ? 1 : 0;
}
