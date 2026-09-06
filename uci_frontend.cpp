// The one UCI protocol loop, shared by every engine in this repository.
//
// compile as part of an engine, e.g.
//   c++ -std=c++20 ... creatica_search.cpp uci_frontend.cpp tbcore.c tbprobe.c -o creatica
//
// Everything here is protocol: reading commands, parsing "position" and "go", generating
// the option block from the engine's declarations, dispatching "setoption". Nothing here
// knows anything about search, evaluation, or this project's engines in particular.
#include "uci_engine.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace uci {

static const char* START_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// ---------------------------------------------------------------- output

static std::mutex out_mutex;

void out(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(out_mutex);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

// ---------------------------------------------------------------- parsing

static std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) out.push_back(tok);
    return out;
}

// "position [startpos | fen <6 fields>] [moves m1 m2 ...]"
//
// Tokenised rather than sliced by character offset. The old version searched for "fen" and
// "moves" at fixed offsets and took substrings between them, which silently produced a
// truncated FEN whenever the spacing differed from what it expected.
static bool parse_position(const std::vector<std::string>& t,
                           std::string& fen, std::vector<std::string>& moves) {
    fen.clear();
    moves.clear();
    size_t i = 1;
    if (i < t.size() && t[i] == "startpos") {
        fen = START_FEN;
        ++i;
    } else if (i < t.size() && t[i] == "fen") {
        ++i;
        // A FEN is six space-separated fields. Take up to six, stopping early at "moves"
        // so a five-field FEN (some GUIs omit the move counters) still works.
        std::string f;
        for (int k = 0; k < 6 && i < t.size() && t[i] != "moves"; ++k, ++i) {
            if (!f.empty()) f += " ";
            f += t[i];
        }
        if (f.empty()) return false;
        fen = f;
    } else {
        return false;
    }
    if (i < t.size() && t[i] == "moves") {
        for (++i; i < t.size(); ++i) moves.push_back(t[i]);
    }
    return true;
}

static void parse_go(const std::vector<std::string>& t, Limits& lim) {
    lim = Limits();
    for (size_t i = 1; i < t.size(); ++i) {
        const std::string& k = t[i];
        auto num = [&](int64_t& dst) {
            if (i + 1 < t.size()) dst = std::strtoll(t[++i].c_str(), nullptr, 10);
        };
        auto numi = [&](int& dst) {
            if (i + 1 < t.size()) dst = (int)std::strtol(t[++i].c_str(), nullptr, 10);
        };
        if      (k == "movetime") num(lim.movetime_ms);
        else if (k == "wtime")    num(lim.wtime);
        else if (k == "btime")    num(lim.btime);
        else if (k == "winc")     num(lim.winc);
        else if (k == "binc")     num(lim.binc);
        else if (k == "nodes")    num(lim.nodes);
        else if (k == "movestogo") numi(lim.movestogo);
        else if (k == "depth")     numi(lim.depth);
        else if (k == "mate")      numi(lim.mate);
        else if (k == "infinite")  lim.infinite = true;
        else if (k == "ponder")    lim.ponder = true;
        else if (k == "searchmoves") {
            for (++i; i < t.size(); ++i) lim.searchmoves.push_back(t[i]);
            break;
        }
    }
}

// "setoption name <words...> value <words...>"
//
// Both the name and the value may contain spaces -- SyzygyPath routinely does -- so this
// splits on the keywords rather than on whitespace.
static bool parse_setoption(const std::string& line,
                            std::string& name, std::string& value) {
    const size_t n = line.find(" name ");
    if (n == std::string::npos) return false;
    const size_t v = line.find(" value ", n);
    if (v == std::string::npos) {
        name = line.substr(n + 6);
        value.clear();
    } else {
        name  = line.substr(n + 6, v - n - 6);
        value = line.substr(v + 7);
    }
    auto trim = [](std::string& s) {
        while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
        while (!s.empty() && isspace((unsigned char)s.back()))  s.pop_back();
    };
    trim(name);
    trim(value);
    return !name.empty();
}

// ---------------------------------------------------------------- the loop

int uci_main(SearchEngine& engine, int argc, char** argv) {
    Options options;
    engine.declare_options(options);
    engine.init();

    // "bench" runs one fixed search and exits, for a quick smoke test after a build.
    const bool bench = (argc == 2 && std::string(argv[1]) == "bench");
    if (bench) {
        engine.new_game();
        engine.set_position(START_FEN, {});
        Limits lim;
        lim.movetime_ms = 10000;
        engine.go(lim);
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        const std::vector<std::string> t = split(line);
        if (t.empty()) continue;
        const std::string& cmd = t[0];

        if (cmd == "uci") {
            out("id name %s\n", engine.id_name());
            out("id author %s\n", engine.id_author());
            {
                std::lock_guard<std::mutex> lock(out_mutex);
                options.print(stdout);
                std::fflush(stdout);
            }
            out("uciok\n");
        } else if (cmd == "isready") {
            out("readyok\n");
        } else if (cmd == "ucinewgame") {
            engine.new_game();
        } else if (cmd == "setoption") {
            std::string name, value;
            if (!parse_setoption(line, name, value))
                out("info string malformed setoption: %s\n", line.c_str());
            else if (!options.set(name, value))
                out("info string unknown option: %s\n", name.c_str());
        } else if (cmd == "position") {
            std::string fen;
            std::vector<std::string> moves;
            if (parse_position(t, fen, moves)) engine.set_position(fen, moves);
            else out("info string malformed position: %s\n", line.c_str());
        } else if (cmd == "go") {
            Limits lim;
            parse_go(t, lim);
            engine.go(lim);
        } else if (cmd == "stop") {
            engine.stop();
        } else if (cmd == "ponderhit") {
            engine.ponderhit();
        } else if (cmd == "quit") {
            break;
        } else if (!engine.custom(line)) {
            out("info string unknown command: %s\n", cmd.c_str());
        }
    }

    // Reached by "quit" and by end of input. EOF must shut down the same way: exiting
    // without joining leaves static destruction to run ~thread on joinable threads, which
    // calls std::terminate -- the "libc++abi: terminating" an engine used to print
    // whenever its stdin closed without a quit.
    engine.quit();
    return 0;
}

} // namespace uci
