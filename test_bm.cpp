// c++ -std=c++20 -Wall -O3 -Wl,-lchess,-rpath,$HOME/libchess -L $HOME/libchess -o test_bm test_bm.cpp

//NNUE full eval vs incremental eval test confirms that incremental is faster by one order of magnitude, i.e. about 10 times on average
//full eval is tens of microseconds per position vs incremental eval is just microseconds per position on M1

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <chrono>
#include "nnue/nnue/nnue_accumulator.h"
#include "libchess.h"

//stockfish-macos-m1-apple-silicon under $HOME, resolved at RUN TIME instead of being written in as one machine's absolute
//path -- this is a public repository. Where the caller reads an environment variable it still wins; this
//is only the fallback. Resolved once, on the first call, before any thread is started.
static const char * stockfish_path_default(void) {
    static char buf[512];
    if (!buf[0]) {
        const char * h = std::getenv("HOME");
        std::snprintf(buf, sizeof buf, "%s/libchess/stockfish-macos-m1-apple-silicon", (h && *h) ? h : ".");
    }
    return buf;
}
const char * engine_path;
int movetime = 2000, depth = 0, nodes = 0, hash = 256, threads = 1;
//one value per position for each statistic, for the mean and the standard deviation. Doubles, because
//a sum of squared node deviations is ~1e14 and overflows any int
struct Stat { const char * name; std::vector<double> v; };
Stat stats[] = { {"time", {}}, {"depth", {}}, {"seldepth", {}}, {"nodes", {}}, {"nps", {}}, {"hashful", {}}, {"tbhits", {}} };
//syzygy under $HOME, resolved at RUN TIME instead of being written in as one machine's absolute
//path -- this is a public repository. Where the caller reads an environment variable it still wins; this
//is only the fallback. Resolved once, on the first call, before any thread is started.
static const char * syzygy_path_default(void) {
    static char buf[512];
    if (!buf[0]) {
        const char * h = std::getenv("HOME");
        std::snprintf(buf, sizeof buf, "%s/syzygy", (h && *h) ? h : ".");
    }
    return buf;
}
#define SYZYGY_PATH syzygy_path_default()
#define LOGGING false
#define LIMIT_STRENGTH false
#define ELO 2600

//NNUEContext and the nnue wrapper functions come from libchess.h. This file used to declare its own
//copy; so did creatica_search.hpp, test_nnue.cpp and test_smp.cpp. Four copies to keep in step by
//hand, which is what made adding evaluate_nnue()'s in_check parameter a link-time problem instead
//of a compile-time one.
struct Evaluation evaluation = {};
struct Evaluation * evaluations[1] = { nullptr };
struct Engine chess_engine;

std::string test(std::string fenString) {
  newGame(chess_engine);
  strncpy(chess_engine.position, fenString.c_str(), MAX_FEN_STRING_LEN);
  if (!position(chess_engine)) {
    fprintf(stderr, "Error in engine communication\n");
    exit(1);        
  }
  if (go(chess_engine, evaluations)) {
    fprintf(stderr, "Error in engine communication\n");
    exit(1);
  }
  const double values[] = { (double)evaluations[0]->time, (double)evaluations[0]->depth, (double)evaluations[0]->seldepth,
    (double)evaluations[0]->nodes, (double)evaluations[0]->nps, (double)evaluations[0]->hashful, (double)evaluations[0]->tbhits };
  for (int i = 0; i < 7; i++) stats[i].v.push_back(values[i]);
  std::cout << fenString << " bm " << evaluations[0]->bestmove << " depth " << evaluations[0]->depth << " seldepth " << evaluations[0]->seldepth << " nodes " << evaluations[0]->nodes << " nps " << evaluations[0]->nps << " hashful " << evaluations[0]->hashful << " tbhits " << evaluations[0]->tbhits << " time " << evaluations[0]->time;
  return evaluations[0]->bestmove;
}

int main(int argc, char ** argv) {
  if (std::getenv("THREADS")) threads = atoi(std::getenv("THREADS"));
  if (std::getenv("HASH")) hash = atoi(std::getenv("HASH"));
  if (std::getenv("ENGINE")) engine_path = std::getenv("ENGINE"); else engine_path = stockfish_path_default();
  if (std::getenv("MOVETIME")) movetime = atoi(std::getenv("MOVETIME"));
  if (std::getenv("DEPTH")) depth = atoi(std::getenv("DEPTH"));
  if (std::getenv("NODES")) nodes = atoi(std::getenv("NODES"));
  evaluations[0] = &evaluation;
  evaluations[0]->maxPlies = 1;
	initChessEngine(chess_engine, engine_path, movetime, depth, hash, threads, SYZYGY_PATH, 1, LOGGING, LIMIT_STRENGTH, ELO, nodes); //MultiPV, logging, limitStrength, Elo, nodes
	if (argc == 2) {
	  std::string filename = argv[1];
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return 1;
    }
    std::string line;
    int positions = 0;
    int solved = 0;
    while (std::getline(file, line)) {
      if (line.empty()) continue;
      std::vector<std::string> col;
      { std::stringstream ss(line); std::string c; while (std::getline(ss, c, '\t')) col.push_back(c); }
      if (col.size() != 2) continue;
      //one call per FEN, and do not stop at the first failure - report them all
      positions++;
      std::string bm = test(col[0]);
      std::string result = " solved";
      if (col[1] == bm) {
        solved++;
      } else result = " failed, correct " + col[1];
      std::cout << " pos " << positions << result << std::endl;
    }
    std::cout << "Total Arasan positions: " << positions << ", solved: " << solved << std::endl;
    for (int i = 0; i < 7; i++) {
      const Stat & s = stats[i];
      if (s.v.empty()) break;
      double mean = 0, var = 0;
      for (double x : s.v) mean += x;
      mean /= s.v.size();
      for (double x : s.v) var += (x - mean) * (x - mean);
      //depths to one decimal, as a fraction of a ply matters there; the rest are whole numbers
      const int decimals = (i == 1 || i == 2) ? 1 : 0;
      std::printf("Avg %s %.*f +/- %.*f\n", s.name, decimals, mean, decimals, std::sqrt(var / s.v.size()));
    }
	}
	else if (argc >= 8) {
	  std::string fenString = "", bm = argv[7];
	  for (int i = 1; i < 7; i++) {
	    fenString += argv[i];
	    if (i != 6) fenString += " ";
	   }
	  if (bm == test(fenString)) std::cout << " solved" << std::endl;
	  else std::cout << " failed, correct " << bm << std::endl;
	} else std::cerr << "usage: ENGINE=path MOVETIME=ms DEPTH=x NODES=y THREADS=z HASH=mb ./test_bm <file.tsv | FEN bm>" << std::endl;
	quit(chess_engine);
    releaseChessEngine(chess_engine);
    return 0;
}
