#include "nnue/types.h"
#include "nnue/position.h"
#include "nnue/evaluate.h"
#include "nnue/nnue/nnue_common.h"
#include "nnue/nnue/network.h"
#include "nnue/nnue/nnue_accumulator.h"

#ifdef __GNUC__ // g++ on Alpine Linux
#include <mutex>
#include <atomic>
#include <cstdarg>
#endif
//#include <fcntl.h>
//#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <random>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "tbprobe.h"
#include "libchess.h"

#define THREADS 8
#define MULTI_PV 5
#define SYZYGY_PATH_DEFAULT "<empty>"
#define SYZYGY_PATH "/Users/ap/syzygy"
#define HASH 1024 //this is just for hashfull stats - no memory restriction is in place for tree growth
#define EXPLORATION_CONSTANT 80 //smaller value favor exploitation, i.e. deeper tree vs wider tree - varies per thread
#define PROBABILITY_MASS 60 //cumulative probability - how many moves we consider - varies per thread [0.5..0.99]
//#define DIRICHLET_ALPHA 3  // Tune: higher for more often noise
//#define DIRICHLET_EPSILON 25  // Blend factor: (1 - epsilon) * P + epsilon * noise -- magnitude of noise
#define NOISE 5 //Default noise added to move priors to reduce contention between search threads and increase diversity 
#define VIRTUAL_LOSS 2
#define PV_PLIES 16

// Shared variables
std::mutex mtx, log_mtx, print_mtx;
std::condition_variable cv;
bool searchFlag = false;
std::atomic<bool> stopFlag = false;
bool quitFlag = false;
FILE * logfile;
double timeAllocated = 0.0; //ms
struct Board * board = nullptr;
char best_move[6] = "";

extern "C" {  
  struct Engine chessEngine;
  bool tb_init_done = false;

  void init_nnue(const char * nnue_file_big, const char * nnue_file_small);
  void cleanup_nnue();
  extern void runMCTS();

  //UCI protocol in uci.c
  void uciLoop();
  void handleUCI(void);
  void handleIsReady(void);
  void handleNewGame();
  void handlePosition(char * command);
  void handleOption(char * command);
  void handleGo(char * command);
  void handleStop();
  void handleQuit(void);

void log_file(const char * message, ...) {
  std::lock_guard<std::mutex> lock(log_mtx);
  va_list args;
  va_start(args, message);
  vfprintf(logfile, message, args);
  va_end(args);
  fflush(logfile);
}

void print(const char * message, ...) {
  std::lock_guard<std::mutex> lock(print_mtx);
  va_list args;
  va_start(args, message);
  vprintf(message, args);
  va_end(args);
  fflush(stdout);
}
  
// Search thread function
void search_thread_func() {
    while (!quitFlag) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [] { return searchFlag || quitFlag; }); // Wait for "go"
        lock.unlock();
        if (quitFlag) break;
        if (searchFlag) runMCTS();
        else continue;
        lock.lock();
        searchFlag = false;
        stopFlag = false;
        cv.notify_one(); // Signal search is done
    }
}
  
  void uciLoop() {
      char line[2048];
      print("id name %s\n", chessEngine.id);
      log_file("id name %s\n", chessEngine.id);
      while (!quitFlag) {
          if (fgets(line, sizeof(line), stdin) == NULL) {
              log_file("uciloop() error: fgets() returned NULL\n");
              exit(-1);
          }
          line[strcspn(line, "\n")] = 0;
  
          if (strcmp(line, "ucinewgame") == 0) {
              log_file("ucinewgame\n");
              handleNewGame();
          }
          else if (strcmp(line, "uci") == 0) {
              log_file("uci\n");
              handleUCI();
          }
          else if (strncmp(line, "setoption", 9) == 0) {
              log_file("%s\n", line);
              handleOption(line);
          }
          else if (strcmp(line, "isready") == 0) {
              log_file("isready\n");
              handleIsReady();
          }
          else if (strncmp(line, "position", 8) == 0) {
              log_file("%s\n", line);
              handlePosition(line);
          }
          else if (strncmp(line, "go", 2) == 0) {
              log_file("%s\n", line);
              handleGo(line);
          }
          else if (strcmp(line, "stop") == 0) {
              handleStop();
          }
          else if (strcmp(line, "quit") == 0) {
              handleQuit();
              break;
          }
      }
  }
  
  void handleUCI(void) {
      print("id name %s\nid author %s\n\n", chessEngine.id, chessEngine.authors);
      log_file("id name %s\nid author %s\n\n", chessEngine.id, chessEngine.authors);
      for (int i = 0; i < 5; i++) {
        int num = 0;
        if (i == Check) num = chessEngine.numberOfCheckOptions;
        else if (i == Combo) num = chessEngine.numberOfComboOptions;
        else if (i == Spin) num = chessEngine.numberOfSpinOptions;
        else if (i == String) num = chessEngine.numberOfStringOptions;
        else if (i == Button) num = chessEngine.numberOfButtonOptions;
        for (int j = 0; j < num; j++) {
          if (i == Check) {
            print("option name %s type check default %s\n", chessEngine.optionCheck[j].name, chessEngine.optionCheck[j].defaultValue ? "true" : "false");
            log_file("option name %s type check default %s\n", chessEngine.optionCheck[j].name, chessEngine.optionCheck[j].defaultValue ? "true" : "false");            
          } else if (i == Combo) {
          } else if (i == Spin) {
            print("option name %s type spin default %ld min %ld max %ld\n", chessEngine.optionSpin[j].name, chessEngine.optionSpin[j].defaultValue, chessEngine.optionSpin[j].min, chessEngine.optionSpin[j].max);
            log_file("option name %s type spin default %ld min %ld max %ld\n", chessEngine.optionSpin[j].name, chessEngine.optionSpin[j].defaultValue, chessEngine.optionSpin[j].min, chessEngine.optionSpin[j].max);
          } else if (i == String) {
            print("option name %s type string default %s\n", chessEngine.optionString[j].name, chessEngine.optionString[j].defaultValue);
            log_file("option name %s type string default %s\n", chessEngine.optionString[j].name, chessEngine.optionString[j].defaultValue);      
          } else if (i == Button) { 
            print("option name %s type button\n", chessEngine.optionButton[j].name);
            log_file("option name %s type button\n", chessEngine.optionButton[j].name);                  
          }
        }
      }
      print("uciok\n");
      log_file("uciok\n");
  }
  
  void handleOption(char * command) {
    char * token = strtok(command, " "); //setoption
    token = strtok(NULL, " ");
    if (strncmp(token, "name", 4) == 0) {
      char * name = strtok(NULL, " ");
      token = strtok(NULL, " ");
      if (strncmp(token, "value", 5) == 0) {
        char * value = strtok(NULL, " ");
        int idx, type;
        for (int i = Button; i <= String; i++) {
          idx = nametoindex(&chessEngine, name, i);
          if (idx >= 0) {
            type = i;
            break;
          }
        }
        if (idx < 0) {
            log_file("info string error unknown option name %s\n", name);
            print("info string error unknown option name %s\n", name);
        } else {
          if (type == Check) chessEngine.optionCheck[idx].value = strncmp(value, "true", 4) == 0 ? true : false;
          else if (type == Combo) strncpy(chessEngine.optionCombo[idx].value, value, MAX_UCI_OPTION_STRING_LEN);
          else if (type == Spin) {
            int v = atoi(value);
            if (v >= chessEngine.optionSpin[idx].min && v <= chessEngine.optionSpin[idx].max)
              chessEngine.optionSpin[idx].value = v;
            else {
              log_file("info string error option name %s min %lld max %lld\n", name, chessEngine.optionSpin[idx].min, chessEngine.optionSpin[idx].max);
              print("info string error option name %s min %lld max %lld\n", name, chessEngine.optionSpin[idx].min, chessEngine.optionSpin[idx].max);
            }
          } else if (type == String) {
            strncpy(chessEngine.optionString[idx].value, value, MAX_UCI_OPTION_STRING_LEN);
            if ((strncmp(name, "SyzygyPath", 10) == 0) && ! tb_init_done) {
              tb_init(chessEngine.optionString[idx].value);
              if (TB_LARGEST == 0) {
                  log_file("info string error unable to initialize tablebase; no tablebase files found in %s\n", chessEngine.optionString[idx].value);
                  print("info string error unable to initialize tablebase; no tablebase files found in %s\n", chessEngine.optionString[idx].value);
              } else {
                tb_init_done = true;
                log_file("info string successfully initialized tablebases in %s. Max number of pieces %d\n", chessEngine.optionString[idx].value, TB_LARGEST);
                print("info string successfully initialized tablebases in %s. Max number of pieces %d\n", chessEngine.optionString[idx].value, TB_LARGEST);
              }
            }
          }
          else if (type == Button) chessEngine.optionButton[idx].value = strncmp(value, "true", 4) == 0 ? true : false;         
        }
      }
    }
  }
  
  void handleIsReady(void) {
      print("readyok\n");
      log_file("readyok\n");
  }
  
  void handleNewGame() {
      strtofen(board->fen, startPos);
      fentoboard(board->fen, board);
      getHash(board->zh, board);
  }
  
  void handlePosition(char * command) {
      char * token;
      char tempFen[MAX_FEN_STRING_LEN];
      struct Move move;
      token = strtok(command, " ");
      if (strncmp(token, "position", 8) != 0) {
          exit(-1);
      }
      token = strtok(NULL, " ");
      if (strcmp(token, "startpos") == 0) {
          strtofen(board->fen, startPos);
          fentoboard(board->fen, board);
          getHash(board->zh, board);
          token = strtok(NULL, " ");
          if (token != NULL && strcmp(token, "moves") == 0) {
              while ((token = strtok(NULL, " ")) != NULL) {
                  if (initMove(&move, board, token)) {
                      log_file("handlePosition() error: invalid move %u%s%s (%s); FEN %s\n",
                              move.chessBoard->fen->moveNumber,
                              move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ",
                              move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
                      exit(-1);
                  }
                  makeMove(&move);
                  if (updateHash(board, &move)) {
                      log_file("handlePosition() error: updateHash() returned non-zero value\n");
                      exit(-1);
                  }
              }
          }
      }
      else if (strcmp(token, "fen") == 0) {
          tempFen[0] = '\0';
          while ((token = strtok(NULL, " ")) != NULL) {
              if (strcmp(token, "moves") == 0) {
                  break;
              }
              if (strlen(tempFen) + strlen(token) + 2 <= MAX_FEN_STRING_LEN) {
                  strcat(tempFen, token);
                  strcat(tempFen, " ");
              } else {
                  log_file("handlePosition() error: FEN string is too long, max length is %d\n",
                          MAX_FEN_STRING_LEN - 1);
                  exit(-1);
              }
          }
          if (strtofen(board->fen, tempFen)) {
            log_file("handlePosition() error: strtofen() returned non-zero code, fen %s\n", tempFen);
            exit(-1);
          }
          if (fentoboard(board->fen, board)) {
            log_file("handlePosition() error: fentoboard() returned non-zero code, fen %s\n", board->fen->fenString);
            exit(-1);
          }
          getHash(board->zh, board);
          if (token != NULL && strcmp(token, "moves") == 0) {
              while ((token = strtok(NULL, " ")) != NULL) {
                  if (initMove(&move, board, token)) {
                      log_file("handlePosition() error: invalid move %u%s%s (%s); FEN %s\n",
                              move.chessBoard->fen->moveNumber,
                              move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ",
                              move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
                      exit(-1);
                  }
                  makeMove(&move);
                  if (updateHash(board, &move)) {
                      log_file("handlePosition() error: updateHash() returned non-zero value\n");
                      exit(-1);
                  }
              }
          }
      }
  }
  
  void handleGo(char * command) {
      char * token;
      if (searchFlag) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [] { return !searchFlag; }); // Wait for previous search to stop
      }
      token = strtok(command, " ");
      while ((token = strtok(NULL, " ")) != NULL) {
          if (strcmp(token, "wtime") == 0) {
              token = strtok(NULL, " ");
              chessEngine.wtime = atoi(token); //ms
          }
          else if (strcmp(token, "btime") == 0) {
              token = strtok(NULL, " ");
              chessEngine.btime = atoi(token); //ms
          }
          else if (strcmp(token, "winc") == 0) {
              token = strtok(NULL, " ");
              chessEngine.winc = atoi(token); //ms
          }
          else if (strcmp(token, "binc") == 0) {
              token = strtok(NULL, " ");
              chessEngine.binc = atoi(token); //ms
          }
          else if (strcmp(token, "movestogo") == 0) {
              token = strtok(NULL, " ");
              chessEngine.movestogo = atoi(token);
          }
          else if (strcmp(token, "movetime") == 0) {
              token = strtok(NULL, " ");
              chessEngine.movetime = atoi(token); //ms
          }
          else if (strcmp(token, "depth") == 0) {
              token = strtok(NULL, " ");
              chessEngine.depth = atoi(token); //ms
              timeAllocated = 1e9;
          }
          else if (strcmp(token, "nodes") == 0) {
              token = strtok(NULL, " ");
              chessEngine.nodes = atoi(token); //ms
              timeAllocated = 1e9;
          }
          else if (strcmp(token, "infinite") == 0) {
              chessEngine.infinite = true;
          }
      }
  
      if (chessEngine.movetime > 0) {
          timeAllocated = chessEngine.movetime * 0.95;
      }
      else if (chessEngine.infinite) {
          timeAllocated = 1e9;
      }
      else {
          int remainingTime = board->fen->sideToMove == ColorWhite ? chessEngine.wtime : chessEngine.btime;
          int increment = board->fen->sideToMove == ColorWhite ? chessEngine.winc : chessEngine.binc;
          int movesLeft = chessEngine.movestogo ? chessEngine.movestogo : MIN_MOVES_REMAINING; //40
          if (strstr(board->fen->fenString, "rnbqkbnr") != NULL) {
              movesLeft = MAX_MOVES_REMAINING;
          }
          if (remainingTime > TIME_SAFETY_BUFFER) { //5000 ms
              remainingTime -= TIME_SAFETY_BUFFER;
          }
          timeAllocated = (double)remainingTime / movesLeft + increment * 0.7;
          if (board->fen->moveNumber > 10) {
              timeAllocated *= CRITICAL_TIME_FACTOR; //1.5
          }
          if (remainingTime < MIN_TIME_THRESHOLD) { //10000 ms
              timeAllocated = remainingTime * 0.5;
          }
          if (timeAllocated < 100) {
              timeAllocated = 100;
          }
      }
  
      if (bitCount(board->occupations[PieceNameAny]) > TB_LARGEST) {
          std::lock_guard<std::mutex> lock(mtx);
          searchFlag = true;
          stopFlag = false;
          cv.notify_one(); // Start search
      } else {
        best_move[0] = '\0';
        unsigned int ep = lsBit(board->fen->enPassantLegalBit);
        unsigned int result = tb_probe_root(board->occupations[PieceNameWhite], board->occupations[PieceNameBlack], 
            board->occupations[WhiteKing] | board->occupations[BlackKing],
            board->occupations[WhiteQueen] | board->occupations[BlackQueen], board->occupations[WhiteRook] | board->occupations[BlackRook], board->occupations[WhiteBishop] | board->occupations[BlackBishop], board->occupations[WhiteKnight] | board->occupations[BlackKnight], board->occupations[WhitePawn] | board->occupations[BlackPawn],
            board->fen->halfmoveClock, 0, ep == 64 ? 0 : ep, board->opponentColor == ColorBlack ? 1 : 0, NULL);
        if (result == TB_RESULT_FAILED) {
            log_file("handleGo() error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, occupations %u, fen %s\n", TB_LARGEST, bitCount(board->occupations[PieceNameAny]), board->fen->fenString);
            exit(-1);
        }
        unsigned int wdl      = TB_GET_WDL(result); //0 - loss, 4 - win, 1..3 - draw
        int scorecp = 0;
        if (wdl == 4) scorecp = MATE_SCORE;
        else if (wdl == 0) scorecp = -MATE_SCORE;
        unsigned int from     = TB_GET_FROM(result);
        unsigned int to       = TB_GET_TO(result);
        unsigned int promotes = TB_GET_PROMOTES(result);
        strncat(best_move, squareName[from], 2);
        strncat(best_move, squareName[to], 2);
        best_move[4] = uciPromoLetter[6 - promotes];
        best_move[5] = '\0';
        print("info depth 1 seldepth 1 multipv 1 score cp %d nodes 1 nps 1 hashfull 0 tbhits 1 time 0 pv %s\nbestmove %s\n", scorecp, best_move, best_move);
        log_file("info depth 1 seldepth 1 multipv 1 score cp %d nodes 1 nps 1 hashfull 0 tbhits 1 time 0 pv %s\nbestmove %s\n", scorecp, best_move, best_move);
      }          
  }
  
  void handleStop() {
    std::unique_lock<std::mutex> lock(mtx);
    stopFlag = true;
    cv.wait(lock, [] { return !searchFlag; }); // Wait for search to stop
  }
  
  void handleQuit(void) {
    log_file("quit\n");    
    std::lock_guard<std::mutex> lock(mtx);
    quitFlag = true;
    cv.notify_one();
  }
}

void setEngineOptions() {
    strcpy(chessEngine.id, "Creatica Chess Engine 1.0");
    strcpy(chessEngine.authors, "Creatica");
    chessEngine.numberOfCheckOptions = 0;
	  chessEngine.numberOfComboOptions = 0;
	  chessEngine.numberOfSpinOptions = 8;
		chessEngine.numberOfStringOptions = 1;
		chessEngine.numberOfButtonOptions = 0;
	  strcpy(chessEngine.optionString[SyzygyPath].name, "SyzygyPath");
	  strcpy(chessEngine.optionString[SyzygyPath].defaultValue, SYZYGY_PATH_DEFAULT);
	  strcpy(chessEngine.optionString[SyzygyPath].value, SYZYGY_PATH);
	  if (chessEngine.optionString[SyzygyPath].value[0]) {
      tb_init(chessEngine.optionString[SyzygyPath].value);
      if (TB_LARGEST == 0) {
          log_file("info string error unable to initialize tablebase; no tablebase files found in %s\n", chessEngine.optionString[SyzygyPath].value);
          print("info string error unable to initialize tablebase; no tablebase files found in %s\n", chessEngine.optionString[SyzygyPath].value);
      } else {
        tb_init_done = true;
        log_file("info string successfully initialized tablebases in %s. Max number of pieces %d\n", chessEngine.optionString[SyzygyPath].value, TB_LARGEST);
        print("info string successfully initialized tablebases in %s. Max number of pieces %d\n", chessEngine.optionString[SyzygyPath].value, TB_LARGEST);
      }
	  } else print("setEngineOptions() error: syzygy path is 0\n");
	  strcpy(chessEngine.optionSpin[Hash].name, "Hash");
	  chessEngine.optionSpin[Hash].defaultValue = HASH;
	  chessEngine.optionSpin[Hash].value = chessEngine.optionSpin[Hash].defaultValue;
	  chessEngine.optionSpin[Hash].min = 128;
	  chessEngine.optionSpin[Hash].max = 4096;
	  strcpy(chessEngine.optionSpin[Threads].name, "Threads");
	  chessEngine.optionSpin[Threads].defaultValue = THREADS;
	  chessEngine.optionSpin[Threads].value = chessEngine.optionSpin[Threads].defaultValue;
	  chessEngine.optionSpin[Threads].min = 1;
	  chessEngine.optionSpin[Threads].max = 8;
	  strcpy(chessEngine.optionSpin[MultiPV].name, "MultiPV");
	  chessEngine.optionSpin[MultiPV].defaultValue = MULTI_PV;
	  chessEngine.optionSpin[MultiPV].value = chessEngine.optionSpin[MultiPV].defaultValue;
	  chessEngine.optionSpin[MultiPV].min = 1;
	  chessEngine.optionSpin[MultiPV].max = 8;
	  strcpy(chessEngine.optionSpin[ProbabilityMass].name, "ProbabilityMass");
	  chessEngine.optionSpin[ProbabilityMass].defaultValue = PROBABILITY_MASS;
	  chessEngine.optionSpin[ProbabilityMass].value = chessEngine.optionSpin[ProbabilityMass].defaultValue;
	  chessEngine.optionSpin[ProbabilityMass].min = 1;
	  chessEngine.optionSpin[ProbabilityMass].max = 100;
	  strcpy(chessEngine.optionSpin[ExplorationConstant].name, "ExplorationConstant");
	  chessEngine.optionSpin[ExplorationConstant].defaultValue = EXPLORATION_CONSTANT;
	  chessEngine.optionSpin[ExplorationConstant].value = chessEngine.optionSpin[ExplorationConstant].defaultValue;
	  chessEngine.optionSpin[ExplorationConstant].min = 0;
	  chessEngine.optionSpin[ExplorationConstant].max = 200;
	  /*
	  strcpy(chessEngine.optionSpin[DirichletAlpha].name, "DirichletAlpha");
	  chessEngine.optionSpin[DirichletAlpha].defaultValue = DIRICHLET_ALPHA;
	  chessEngine.optionSpin[DirichletAlpha].value = chessEngine.optionSpin[DirichletAlpha].defaultValue;
	  chessEngine.optionSpin[DirichletAlpha].min = 0;
	  chessEngine.optionSpin[DirichletAlpha].max = 10;
	  strcpy(chessEngine.optionSpin[DirichletEpsilon].name, "DirichletEpsilon");
	  chessEngine.optionSpin[DirichletEpsilon].defaultValue = DIRICHLET_EPSILON;
	  chessEngine.optionSpin[DirichletEpsilon].value = chessEngine.optionSpin[DirichletEpsilon].defaultValue;
	  chessEngine.optionSpin[DirichletEpsilon].min = 0;
	  chessEngine.optionSpin[DirichletEpsilon].max = 50;
	  */
	  strcpy(chessEngine.optionSpin[Noise].name, "Noise");
	  chessEngine.optionSpin[Noise].defaultValue = NOISE;
	  chessEngine.optionSpin[Noise].value = chessEngine.optionSpin[Noise].defaultValue;
	  chessEngine.optionSpin[Noise].min = 1;
	  chessEngine.optionSpin[Noise].max = 30;
	  strcpy(chessEngine.optionSpin[VirtualLoss].name, "VirtualLoss");
	  chessEngine.optionSpin[VirtualLoss].defaultValue = VIRTUAL_LOSS;
	  chessEngine.optionSpin[VirtualLoss].value = chessEngine.optionSpin[VirtualLoss].defaultValue;
	  chessEngine.optionSpin[VirtualLoss].min = 0;
	  chessEngine.optionSpin[VirtualLoss].max = 10;
	  strcpy(chessEngine.optionSpin[PVPlies].name, "PVPlies");
	  chessEngine.optionSpin[PVPlies].defaultValue = PV_PLIES;
	  chessEngine.optionSpin[PVPlies].value = chessEngine.optionSpin[PVPlies].defaultValue;
	  chessEngine.optionSpin[PVPlies].min = 1;
	  chessEngine.optionSpin[PVPlies].max = 32;
	  chessEngine.seldepth = 0;
	  chessEngine.currentDepth = 0;
    chessEngine.wtime = 1e9;
    chessEngine.btime = 1e9;
    chessEngine.winc = 0;
    chessEngine.binc = 0;
    chessEngine.movestogo = 0;
    chessEngine.movetime = 0;
    chessEngine.depth = 0;
    chessEngine.nodes = 0;
    chessEngine.infinite = false;
}

int main(int argc, char **argv) {
    TB_LARGEST = 0;
    struct Fen fen;
    struct ZobristHash zh;
    struct Board chess_board;
    chess_board.fen = &fen;
    zobristHash(&zh);
    chess_board.zh = &zh;
    board = &chess_board;
    logfile = fopen("uci.log", "w");
    srand(time(NULL)); 
    init_magic_bitboards();
    init_nnue("nn-1c0000000000.nnue", "nn-37f18f62d772.nnue");
    setEngineOptions();

    // Start the search thread
    std::thread search_thread(search_thread_func);
    uciLoop();
    search_thread.join();
    
    cleanup_nnue();
    cleanup_magic_bitboards();
    fclose(logfile);
    return 0;
}