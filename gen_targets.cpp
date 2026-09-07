// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -I /Users/ap/libchess -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess gen_targets.cpp -o gen_targets
//
// Search a list of positions with creatica and let it dump the root visit distribution for each
// one, producing policy-distillation targets.
//
// Reads "FEN<TAB>stockfish_cp" lines on stdin -- the output of bin2fen -- and drives the eng
// over the usual named pipes. The eng writes the dataset itself via VisitDumpFile; this only
// feeds it positions and waits.
//
// Why this rather than playing games: a training record needs a POSITION and a SEARCH, and
// playing is a slow way to obtain positions. You pay real time for both sides, the clock, the
// network and the opponent's thinking, and you get whatever positions the games happen to visit.
// Measured on this machine, playing bot games yields roughly 5,000 records a night; searching
// stored positions at 5 s each yields about 17,000, and at 1 s about 86,000 -- with the depth
// chosen rather than inherited from a clock.
//
// ReuseTree is turned OFF here, deliberately. Consecutive positions from a list are unrelated,
// so there is no subtree to inherit -- but the tree would still accumulate across all of them,
// grow monotonically, and hit the Hash ceiling, at which point every later search is degraded.
// Reuse is a win when positions follow each other in a game and a liability when they do not.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>
#include "nnue/bitboard.h"
#include "libchess.h"

static Engine     eng;
static Evaluation evaluation;
static Evaluation * evaluations[1] = { &evaluation };

static const char * env_or(const char * k, const char * d) {
    const char * v = std::getenv(k);
    return (v && *v) ? v : d;
}
static long env_l(const char * k, long d) {
    const char * v = std::getenv(k);
    return (v && *v) ? std::strtol(v, nullptr, 10) : d;
}

int main(int argc, char ** argv) {
    const char * enginePath = env_or("CREATICA_ENGINE", "/Users/ap/libchess/creatica");
    // Defaults to EMPTY, meaning "no dump, report on stdout". An empty environment variable is
    // indistinguishable from an unset one here, so VISIT_DUMP= could not have selected that mode
    // if the default were a filename -- it would silently fall back to writing targets.tsv and
    // print nothing, which is exactly what happened the first time.
    const char * dumpPath   = env_or("VISIT_DUMP",      "");
    const char * tag        = env_or("GAME_TAG",        "positions");
    const char * syzygy     = env_or("SYZYGY_PATH",     "/Users/ap/syzygy");
    const long   movetime   = env_l("MOVETIME", 3000);
    const long   threads    = env_l("THREADS",  4);
    const long   hash_mb    = env_l("HASH",     2048);
    const long   maxn       = env_l("MAX",      0);     // 0 = no limit
    const long   skip       = env_l("SKIP",     0);     // resume: drop this many inputs first
    const long   stride     = env_l("STRIDE",   1);     // take every Nth input
    // Where to record how far we got, rewritten as we go.
    //
    // Printing the resume point only at the end is not enough: Ctrl-C kills the whole foreground
    // process group, so a wrapper script never gets to read it and the run's progress is lost.
    // Writing it here means an interrupted run resumes from where it actually stopped instead of
    // re-searching hours of positions and duplicating them in the dataset.
    const char * progress = env_or("PROGRESS_FILE", "");

    if (argc > 1 && std::strcmp(argv[1], "--help") == 0) {
        std::fprintf(stderr,
          "usage: bin2fen <file.bin> | %s\n"
          "  env: CREATICA_ENGINE VISIT_DUMP GAME_TAG SYZYGY_PATH\n"
          "       MOVETIME(3000) THREADS(4) HASH(2048) MAX(0) SKIP(0) STRIDE(1)\n"
          "\n"
          "  STRIDE matters more than it looks. bin2fen emits EVERY position of every game, so\n"
          "  consecutive inputs differ by one move and their searches largely repeat each other.\n"
          "  A stride spreads the same number of searches over far more distinct material.\n", argv[0]);
        return 2;
    }

    Stockfish::Bitboards::init();

    initChessEngine(eng, enginePath, movetime, 0, (int)hash_mb, (int)threads,
                    syzygy, 1, false, false, 0);

    if (dumpPath[0]) {
        setEngineStringOption(eng, "VisitDumpFile", dumpPath);
        setEngineStringOption(eng, "GameTag",       tag);
    }
    // Off by default: at any stride above 1 the positions are from different games, so there is
    // no subtree to inherit and the tree would merely accumulate. At STRIDE=1 they ARE
    // consecutive plies and reuse is worth having -- set REUSE=1.
    setEngineCheck(eng, "ReuseTree",             env_l("REUSE", 0) != 0);
    // Always off here. We are replaying positions, not playing them: whatever the engine picks,
    // the next position is whatever the game actually played. Re-rooting onto the engine's
    // choice and sweeping would free the branch the next search needs. set_root() finds nodes by
    // hash, so leaving the tree alone costs nothing and the threshold collection still bounds it.
    setEngineCheck(eng, "PostMoveCollect",       false);
    setEngineCheck(eng, "FinalInfoLines",        false);
    setEngineCheck(eng, "IntermittentInfoLines", false);
    setOptions(eng);

    if (!isReady(eng)) { std::fprintf(stderr, "eng not ready\n"); return 1; }
    newGame(eng);

    const auto t0 = std::chrono::steady_clock::now();
    long read = 0, searched = 0, failed = 0;
    char line[512];

    while (std::fgets(line, sizeof line, stdin)) {
        char * tab = std::strchr(line, '\t');
        if (tab) *tab = '\0';
        char * nl = std::strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] == '\0') continue;

        ++read;
        if (read <= skip) continue;
        if (stride > 1 && ((read - skip - 1) % stride) != 0) continue;

        std::snprintf(eng.position, sizeof eng.position, "%s", line);
        eng.moves[0] = '\0';
        if (!position(eng)) { ++failed; continue; }
        // go() blocks until bestmove because neither infinite nor ponder is set, which is exactly
        // the pacing we want: one search per position, no queueing.
        if (go(eng, evaluations)) { ++failed; continue; }
        ++searched;

        // With no VISIT_DUMP the engine is not creatica and cannot dump anything, so report its
        // choice on stdout instead. That makes this a generic "search these positions with this
        // engine" tool -- which is how Stockfish gets used as an ARBITER over the mistake set.
        //
        // Its verdict is not a training target. Agreement with Stockfish across all positions is
        // the metric that saturated (top-1 26.7% -> 33.08% -> 34.83% while Elo went +108 -> +113
        // -> level). On the narrow set where creatica's deep search overruled its own shallow one,
        // it answers a different and still-useful question: is the move genuinely good, or do both
        // creatica searches share an NNUE blind spot? The second kind must not be trained on.
        if (!dumpPath[0]) {
            std::printf("%s\t%s\t%d\n", line,
                        evaluations[0]->bestmove[0] ? evaluations[0]->bestmove : "(none)",
                        evaluations[0]->scorecp);
            std::fflush(stdout);
        }

        if (progress[0] && (searched % 50) == 0) {
            // Write-and-rename so a kill mid-write cannot leave a truncated number behind.
            std::string tmp = std::string(progress) + ".tmp";
            if (FILE * pf = std::fopen(tmp.c_str(), "w")) {
                std::fprintf(pf, "%ld\n", read);
                std::fclose(pf);
                std::rename(tmp.c_str(), progress);
            }
        }
        if ((searched % 200) == 0) {
            const double secs = std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - t0).count();
            std::fprintf(stderr, "  %ld searched, %.1f/min, %ld failed\n",
                         searched, searched / (secs / 60.0), failed);
        }
        if (maxn && searched >= maxn) break;
    }

    const double secs = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "  done: read %ld, searched %ld, failed %ld, %.0f s (%.1f/min)\n"
                         "  resume with SKIP=%ld\n",
                 read, searched, failed, secs, searched / (secs / 60.0), read);
    if (progress[0]) {
        std::string tmp = std::string(progress) + ".tmp";
        if (FILE * pf = std::fopen(tmp.c_str(), "w")) {
            std::fprintf(pf, "%ld\n", read);
            std::fclose(pf);
            std::rename(tmp.c_str(), progress);
        }
    }
    quit(eng);
    releaseChessEngine(eng);
    return 0;
}
