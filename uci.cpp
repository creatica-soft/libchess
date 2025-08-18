#include "nnue/types.h"
#include "nnue/position.h"
#include "nnue/evaluate.h"
#include "nnue/nnue/nnue_common.h"
#include "nnue/nnue/network.h"
#include "nnue/nnue/nnue_accumulator.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <random>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "uthash.h"
#include "tbprobe.h"
#include "libchess.h"

// Shared variables
std::mutex mtx;
std::condition_variable cv;
bool searchFlag = false;
bool stopFlag = false;
bool quitFlag = false;
int logfile;
double timeAllocated = 0.0; //ms
struct Board * board = nullptr;
char best_move[6] = "";

//#define TB_MAX_PIECES 5


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
  
  //extern void runNNUE(struct Board * board, char * uciMove, struct NNUEContext * ctx);
  extern void cleanup();

// Search thread function
void search_thread_func() {
    while (!quitFlag) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [] { return searchFlag || quitFlag; }); // Wait for "go"
        lock.unlock();
        if (quitFlag) break;
        if (board) {
            runMCTS();
        }
        if (!stopFlag) {
          printf("bestmove %s\n", best_move);
          fflush(stdout);
          dprintf(logfile, "bestmove %s\n", best_move);    
        }
        lock.lock();
        searchFlag = false;
        stopFlag = false;
        cv.notify_one(); // Signal search is done
    }
}
  
  void uciLoop() {
      char line[2048];
      printf("id name %s\n", chessEngine.id);
      dprintf(logfile, "id name %s\n", chessEngine.id);
      while (!quitFlag) {
          if (fgets(line, sizeof(line), stdin) == NULL) {
              dprintf(logfile, "uciloop() error: fgets() returned NULL\n");
              fprintf(stderr, "uciloop() error: fgets() returned NULL\n");
              exit(-1);
          }
          line[strcspn(line, "\n")] = 0;
  
          if (strncmp(line, "uci", 3) == 0) {
              dprintf(logfile, "uci\n");
              handleUCI();
          }
          else if (strncmp(line, "setoption", 9) == 0) {
              dprintf(logfile, "%s\n", line);
              handleOption(line);
          }
          else if (strncmp(line, "isready", 7) == 0) {
              dprintf(logfile, "isready\n");
              handleIsReady();
          }
          else if (strncmp(line, "ucinewgame", 10) == 0) {
              dprintf(logfile, "ucinewgame\n");
              handleNewGame();
          }
          else if (strncmp(line, "position", 8) == 0) {
              dprintf(logfile, "%s\n", line);
              handlePosition(line);
          }
          else if (strncmp(line, "go", 2) == 0) {
              dprintf(logfile, "%s\n", line);
              handleGo(line);
          }
          else if (strncmp(line, "stop", 4) == 0) {
              handleStop();
          }
          else if (strncmp(line, "quit", 4) == 0) {
              handleQuit();
              break;
          }
          fflush(stdout);
      }
  }
  
  void handleUCI(void) {
      printf("id name %s\n", chessEngine.id);
      dprintf(logfile, "id name %s\n", chessEngine.id);
      printf("id author %s\n\n", chessEngine.authors);
      dprintf(logfile, "id author %s\n\n", chessEngine.authors);
      for (int i = 0; i < 5; i++) {
        int num = 0;
        if (i == Check) num = chessEngine.numberOfCheckOptions;
        else if (i == Combo) num = chessEngine.numberOfComboOptions;
        else if (i == Spin) num = chessEngine.numberOfSpinOptions;
        else if (i == String) num = chessEngine.numberOfStringOptions;
        else if (i == Button) num = chessEngine.numberOfButtonOptions;
        for (int j = 0; j < num; j++) {
          if (i == Check) {
            printf("option name %s type check default %s\n", chessEngine.optionCheck[j].name, chessEngine.optionCheck[j].defaultValue ? "true" : "false");
            dprintf(logfile, "option name %s type check default %s\n", chessEngine.optionCheck[j].name, chessEngine.optionCheck[j].defaultValue ? "true" : "false");            
          } else if (i == Combo) {
          } else if (i == Spin) {
            printf("option name %s type spin default %ld min %ld max %ld\n", chessEngine.optionSpin[j].name, chessEngine.optionSpin[j].defaultValue, chessEngine.optionSpin[j].min, chessEngine.optionSpin[j].max);
            dprintf(logfile, "option name %s type spin default %ld min %ld max %ld\n", chessEngine.optionSpin[j].name, chessEngine.optionSpin[j].defaultValue, chessEngine.optionSpin[j].min, chessEngine.optionSpin[j].max);
          } else if (i == String) {
            printf("option name %s type string default %s\n", chessEngine.optionString[j].name, chessEngine.optionString[j].defaultValue);
            dprintf(logfile, "option name %s type string default %s\n", chessEngine.optionString[j].name, chessEngine.optionString[j].defaultValue);      
          } else if (i == Button) { 
            printf("option name %s type button\n", chessEngine.optionButton[j].name);
            dprintf(logfile, "option name %s type button\n", chessEngine.optionButton[j].name);                  
          }
        }
      }
      printf("uciok\n");
      dprintf(logfile, "uciok\n");
      fflush(stdout);
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
            dprintf(logfile, "info string error unknown option name %s\n", name);
            printf("info string error unknown option name %s\n", name);
        } else {
          if (type == Check) chessEngine.optionCheck[idx].value = strncmp(value, "true", 4) == 0 ? true : false;
          else if (type == Combo) strncpy(chessEngine.optionCombo[idx].value, value, MAX_UCI_OPTION_STRING_LEN);
          else if (type == Spin) chessEngine.optionSpin[idx].value = atoi(value);
          else if (type == String) {
            strncpy(chessEngine.optionString[idx].value, value, MAX_UCI_OPTION_STRING_LEN);
            if (strncmp(name, "SyzygyPath", 10) == 0) {
              tb_init(chessEngine.optionString[idx].value);
              if (TB_LARGEST == 0) {
                  dprintf(logfile, "info string error unable to initialize tablebase; no tablebase files found in %s\n", chessEngine.optionString[idx].value);
                  printf("info string error unable to initialize tablebase; no tablebase files found in %s\n", chessEngine.optionString[idx].value);
              } else {
                tb_init_done = true;
                dprintf(logfile, "info string successfully initialized tablebases in %s. Max number of pieces %d\n", chessEngine.optionString[idx].value, TB_LARGEST);
                printf("info string successfully initialized tablebases in %s. Max number of pieces %d\n", chessEngine.optionString[idx].value, TB_LARGEST);
              }
            }
          }
          else if (type == Button) chessEngine.optionButton[idx].value = strncmp(value, "true", 4) == 0 ? true : false;         
        }
      }
    }
    fflush(stdout);
  }
  
  void handleIsReady(void) {
      printf("readyok\n");
      dprintf(logfile, "readyok\n");
      fflush(stdout);
  }
  
  void handleNewGame() {
      cleanup();
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
                      fprintf(stderr, "handlePosition() error: invalid move %u%s%s (%s); FEN %s\n",
                              move.chessBoard->fen->moveNumber,
                              move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ",
                              move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
                      dprintf(logfile, "handlePosition() error: invalid move %u%s%s (%s); FEN %s\n",
                              move.chessBoard->fen->moveNumber,
                              move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ",
                              move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
                      exit(-1);
                  }
                  makeMove(&move);
                  if (updateHash(board, &move)) {
                      fprintf(stderr, "handlePosition() error: updateHash() returned non-zero value\n");
                      dprintf(logfile, "handlePosition() error: updateHash() returned non-zero value\n");
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
                  fprintf(stderr, "handlePosition() error: FEN string is too long, max length is %d\n",
                          MAX_FEN_STRING_LEN - 1);
                  dprintf(logfile, "handlePosition() error: FEN string is too long, max length is %d\n",
                          MAX_FEN_STRING_LEN - 1);
                  exit(-1);
              }
          }
          strtofen(board->fen, tempFen);
          fentoboard(board->fen, board);
          getHash(board->zh, board);
          if (token != NULL && strcmp(token, "moves") == 0) {
              while ((token = strtok(NULL, " ")) != NULL) {
                  if (initMove(&move, board, token)) {
                      fprintf(stderr, "handlePosition() error: invalid move %u%s%s (%s); FEN %s\n",
                              move.chessBoard->fen->moveNumber,
                              move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ",
                              move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
                      dprintf(logfile, "handlePosition() error: invalid move %u%s%s (%s); FEN %s\n",
                              move.chessBoard->fen->moveNumber,
                              move.chessBoard->fen->sideToMove == ColorWhite ? ". " : "... ",
                              move.sanMove, move.uciMove, move.chessBoard->fen->fenString);
                      exit(-1);
                  }
                  makeMove(&move);
                  if (updateHash(board, &move)) {
                      fprintf(stderr, "handlePosition() error: updateHash() returned non-zero value\n");
                      dprintf(logfile, "handlePosition() error: updateHash() returned non-zero value\n");
                      exit(-1);
                  }
              }
          }
      }
  }
  
  void handleGo(char * command) {
      char * token;
            
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
  
      int res = 0;
      if (__builtin_popcountl(board->occupations[PieceNameAny]) > TB_LARGEST) {
          std::lock_guard<std::mutex> lock(mtx);
          searchFlag = true;
          stopFlag = false;
          cv.notify_one(); // Start search
      } else {
        best_move[0] = '\0';
        unsigned int ep = __builtin_ctzl(board->fen->enPassantLegalBit);
        unsigned int result = tb_probe_root(board->occupations[PieceNameWhite], board->occupations[PieceNameBlack], 
        board->occupations[WhiteKing] | board->occupations[BlackKing],
            board->occupations[WhiteQueen] | board->occupations[BlackQueen], board->occupations[WhiteRook] | board->occupations[BlackRook], board->occupations[WhiteBishop] | board->occupations[BlackBishop], board->occupations[WhiteKnight] | board->occupations[BlackKnight], board->occupations[WhitePawn] | board->occupations[BlackPawn],
            board->fen->halfmoveClock, 0, ep == 64 ? 0 : ep, board->opponentColor == ColorBlack ? 1 : 0, NULL);
        if (result == TB_RESULT_FAILED) {
            fprintf(stderr, "error: unable to probe tablebase; position invalid, illegal or not in tablebase\n");
            dprintf(logfile, "error: unable to probe tablebase; position invalid, illegal or not in tablebase\n");
            exit(-1);
        }
        unsigned int from     = TB_GET_FROM(result);
        unsigned int to       = TB_GET_TO(result);
        unsigned int promotes = TB_GET_PROMOTES(result);
        strncat(best_move, squareName[from], 2);
        strncat(best_move, squareName[to], 2);
        switch (promotes) {
          case 0:
            break;
          case 1:
            best_move[4] = 'q';
            best_move[5] = '\0';
            break;
          case 2:
            best_move[4] = 'r';
            best_move[5] = '\0';
            break;
          case 3:
            best_move[4] = 'b';
            best_move[5] = '\0';
            break;
          case 4:
            best_move[4] = 'n';
            best_move[5] = '\0';
            break;
        }
        printf("bestmove %s\n", best_move);
        fflush(stdout);
        dprintf(logfile, "bestmove %s\n", best_move);
      }          
  }
  
  void handleStop() {
    std::unique_lock<std::mutex> lock(mtx);
    stopFlag = true;
    cv.wait(lock, [] { return !searchFlag; }); // Wait for search to stop
    printf("bestmove %s\n", best_move);
    fflush(stdout);
    dprintf(logfile, "bestmove %s\nstop\n", best_move);
  }
  
  void handleQuit(void) {
    dprintf(logfile, "quit\n");    
    std::unique_lock<std::mutex> lock(mtx);
    quitFlag = true;
    cv.notify_all();
  }
}


int main(int argc, char **argv) {
    struct Fen fen;
    struct ZobristHash zh;
    struct Board chess_board;
    chess_board.fen = &fen;
    zobristHash(&zh);
    chess_board.zh = &zh;
    board = &chess_board;
    logfile = open("uci.log", O_RDWR | O_APPEND | O_CREAT | O_TRUNC);
    fchmod(logfile, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    srand(time(NULL)); 
    init_magic_bitboards();
    init_nnue("nn-1c0000000000.nnue", "nn-37f18f62d772.nnue");

    strcpy(chessEngine.id, "Creatica Chess Engine 1.0");
    strcpy(chessEngine.authors, "Creatica");
    chessEngine.numberOfCheckOptions = 0;
	  chessEngine.numberOfComboOptions = 0;
	  chessEngine.numberOfSpinOptions = 3;
		chessEngine.numberOfStringOptions = 1;
		chessEngine.numberOfButtonOptions = 0;
	  strcpy(chessEngine.optionString[0].name, "SyzygyPath");
	  strcpy(chessEngine.optionString[0].defaultValue, "<empty>");
	  strcpy(chessEngine.optionSpin[0].name, "Hash");
	  chessEngine.optionSpin[0].defaultValue = 1024;
	  chessEngine.optionSpin[0].value = chessEngine.optionSpin[0].defaultValue;
	  chessEngine.optionSpin[0].min = 1024;
	  chessEngine.optionSpin[0].max = 4096;
	  strcpy(chessEngine.optionSpin[1].name, "Threads");
	  chessEngine.optionSpin[1].defaultValue = 1;
	  chessEngine.optionSpin[1].value = chessEngine.optionSpin[1].defaultValue;
	  chessEngine.optionSpin[1].min = 1;
	  chessEngine.optionSpin[1].max = 8;
	  strcpy(chessEngine.optionSpin[2].name, "MultiPV");
	  chessEngine.optionSpin[2].defaultValue = 1;
	  chessEngine.optionSpin[2].value = chessEngine.optionSpin[2].defaultValue;
	  chessEngine.optionSpin[2].min = 1;
	  chessEngine.optionSpin[2].max = 8;
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
    chessEngine.currentNodes = 0;
    chessEngine.infinite = false;

    // Start the search thread
    std::thread search_thread(search_thread_func);
    uciLoop();
    search_thread.join();
    
    cleanup_nnue();
    cleanup();
    cleanup_magic_bitboards();
    close(logfile);
    return 0;
}