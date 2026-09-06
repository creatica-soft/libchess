// The contract between the UCI protocol layer and a search engine.
//
// There used to be one uci-*.cpp per engine variant -- fourteen of them, the two live ones
// 785 and 832 lines of near-identical protocol handling. Making a variant meant copying the
// whole file, which is how a bug fixed in one copy stays unfixed in the others.
//
// Everything protocol-shaped now lives once, in uci_frontend.cpp. An engine implements the
// interface below, declares its options, and gets a five-line main().
#ifndef UCI_ENGINE_H
#define UCI_ENGINE_H

#include <cstdint>
#include <string>
#include <vector>
#include "uci_options.h"

namespace uci {

// Everything "go" can carry. Zero or false means "not specified".
struct Limits {
    int64_t movetime_ms = 0;
    int64_t wtime = 0, btime = 0, winc = 0, binc = 0;
    int     movestogo = 0;
    int     depth = 0;
    int64_t nodes = 0;
    int     mate = 0;
    bool    infinite = false;
    bool    ponder = false;
    std::vector<std::string> searchmoves;   // long algebraic, as given
};

struct SearchEngine {
    virtual ~SearchEngine() = default;

    virtual const char* id_name()   const = 0;
    virtual const char* id_author() const = 0;

    // Called once before the protocol loop starts. Declare every tunable here; the
    // "option name ..." block and setoption dispatch are generated from it.
    virtual void declare_options(Options&) = 0;

    // Called after options are declared and before the first command is served. Do the
    // expensive one-time setup here (nets, tables, thread pool) rather than in a
    // constructor, so an engine that is only asked for its options costs nothing.
    virtual void init() {}

    virtual void new_game() = 0;

    // fen is a full FEN; moves are long-algebraic moves to apply from it.
    virtual void set_position(const std::string& fen,
                              const std::vector<std::string>& moves) = 0;

    // MUST NOT BLOCK. Start the search and return; the engine prints its own
    // "bestmove ..." when the search ends. This mirrors what the existing engines already
    // do with a persistent search thread, so it is not a new constraint.
    virtual void go(const Limits&) = 0;

    virtual void stop() = 0;
    virtual void ponderhit() {}

    // Called once when the loop is leaving, before the process exits. Join threads here.
    virtual void quit() {}

    // Anything not part of the protocol -- creatica answers "eval" and "pieces".
    // Return true if the line was handled, false to have the frontend report it unknown.
    virtual bool custom(const std::string& line) { (void)line; return false; }
};

// The single protocol loop. Reads stdin until "quit" or end of input, and returns the
// process exit code.
int uci_main(SearchEngine& engine, int argc, char** argv);

} // namespace uci

#endif
