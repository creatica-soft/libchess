/*
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
*/
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "chess_cnn.h"
#include <torch/torch.h>
#include <torch/script.h>
#include <torch/serialize.h>
#include <iostream>
#include <fstream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <string>
#include <filesystem>
#include <chrono>
#include <random>
#include "uthash.h"
#include "tbprobe.h"
#include "libchess.h"

#define TB_MAX_PIECES 5
#define TB_PATH "/Users/ap/syzygy"

torch::Device device(torch::kCPU);
ChessCNN model;
volatile int stopFlag = 0;
int logfile;

extern "C" {  
  //UCI protocol in uci.c
  void uciLoop(struct Fen * fen, struct Board * board, struct ZobristHash * zh);
  void handleUCI(void);
  void handleIsReady(void);
  void handleNewGame(struct Fen * fen, struct Board * board, struct ZobristHash * zh);
  int handlePosition(struct Fen * fen, struct Board * board, struct ZobristHash * zh, char * command);
  void handleGo(struct Board * board, char * command);
  void handleStop(struct Board * board);
  void handleQuit(void);
  
  extern int neuralEvaluate(struct Board * board, char * uciMove);
  extern int neuralEvaluateDirect(struct Board * board, char * uciMove);
  extern int runMCTS(struct Board * board, double maxTime, char * uciMove);
  extern void cleanup();
  
  void uciLoop(struct Fen *fen, struct Board *board, struct ZobristHash *zh) {
      char line[2048];
      printf("id name Creatica Chess Engine 1.0\n");
      dprintf(logfile, "id name Creatica Chess Engine 1.0\n");
      while (1) {
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
          else if (strncmp(line, "isready", 7) == 0) {
              dprintf(logfile, "isready\n");
              handleIsReady();
          }
          else if (strncmp(line, "ucinewgame", 10) == 0) {
              dprintf(logfile, "ucinewgame\n");
              handleNewGame(fen, board, zh);
          }
          else if (strncmp(line, "position", 8) == 0) {
              handlePosition(fen, board, zh, line);
          }
          else if (strncmp(line, "go", 2) == 0) {
              handleGo(board, line);
          }
          else if (strncmp(line, "stop", 4) == 0) {
              dprintf(logfile, "stop\n");
              handleStop(board);
          }
          else if (strncmp(line, "quit", 4) == 0) {
              dprintf(logfile, "quit\n");
              handleQuit();
              break;
          }
          fflush(stdout);
      }
  }
  
  void handleUCI(void) {
      printf("id name Creatica Chess Engine 1.0\n");
      dprintf(logfile, "id name Creatica Chess Engine 1.0\n");
      printf("id author Creatica\n");
      dprintf(logfile, "id author Creatica\n");
      printf("uciok\n");
      dprintf(logfile, "uciok\n");
  }
  
  void handleIsReady(void) {
      printf("readyok\n");
      dprintf(logfile, "readyok\n");
  }
  
  void handleNewGame(struct Fen * fen, struct Board * board, struct ZobristHash * zh) {
      cleanup();
      strtofen(fen, startPos);
      fentoboard(fen, board);
      getHash(zh, board);
  }
  
  int handlePosition(struct Fen * fen, struct Board * board, struct ZobristHash * zh, char * command) {
      char *token;
      char tempFen[MAX_FEN_STRING_LEN];
      struct Move move;
      dprintf(logfile, "%s\n", command);
      token = strtok(command, " ");
      if (strncmp(token, "position", 8) != 0) {
          return -1;
      }
      token = strtok(NULL, " ");
      if (strcmp(token, "startpos") == 0) {
          strtofen(fen, startPos);
          fentoboard(fen, board);
          getHash(zh, board);
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
                      return -2;
                  }
                  makeMove(&move);
                  if (!updateHash(board->zh, board, &move)) {
                      board->hash = board->zh->hash;
                  } else {
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
          strtofen(fen, tempFen);
          fentoboard(fen, board);
          getHash(zh, board);
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
                      return -2;
                  }
                  makeMove(&move);
                  if (updateHash(zh, board, &move)) {
                      fprintf(stderr, "handlePosition() error: updateHash() returned non-zero value\n");
                      dprintf(logfile, "handlePosition() error: updateHash() returned non-zero value\n");
                      exit(-1);
                  }
              }
          }
      }
      return 0;
  }
  
  void handleGo(struct Board * board, char * command) {
      char *token;
      int wtime = 0, btime = 0, winc = 0, binc = 0, movestogo = 0;
      int movetime = 0, infinite = 0;
      double timeAllocated = 0.0;
      char uciMove[6] = "";
      dprintf(logfile, "%s\n", command);
      clock_t start_time = clock();
      
      token = strtok(command, " ");
      while ((token = strtok(NULL, " ")) != NULL) {
          if (strcmp(token, "wtime") == 0) {
              token = strtok(NULL, " ");
              wtime = atoi(token);
          }
          else if (strcmp(token, "btime") == 0) {
              token = strtok(NULL, " ");
              btime = atoi(token);
          }
          else if (strcmp(token, "winc") == 0) {
              token = strtok(NULL, " ");
              winc = atoi(token);
          }
          else if (strcmp(token, "binc") == 0) {
              token = strtok(NULL, " ");
              binc = atoi(token);
          }
          else if (strcmp(token, "movestogo") == 0) {
              token = strtok(NULL, " ");
              movestogo = atoi(token);
          }
          else if (strcmp(token, "movetime") == 0) {
              token = strtok(NULL, " ");
              movetime = atoi(token);
          }
          else if (strcmp(token, "infinite") == 0) {
              infinite = 1;
          }
      }
  
      if (movetime > 0) {
          timeAllocated = movetime * 0.95;
      }
      else if (infinite) {
          timeAllocated = 1e9;
      }
      else {
          int remainingTime = board->fen->sideToMove == ColorWhite ? wtime : btime;
          int increment = board->fen->sideToMove == ColorWhite ? winc : binc;
          int movesLeft = movestogo ? movestogo : MIN_MOVES_REMAINING;
          if (strstr(board->fen->fenString, "rnbqkbnr") != NULL) {
              movesLeft = MAX_MOVES_REMAINING;
          }
          if (remainingTime > TIME_SAFETY_BUFFER) {
              remainingTime -= TIME_SAFETY_BUFFER;
          }
          timeAllocated = (double)remainingTime / movesLeft + increment * 0.7;
          if (board->fen->moveNumber > 10) {
              timeAllocated *= CRITICAL_TIME_FACTOR;
          }
          if (remainingTime < MIN_TIME_THRESHOLD) {
              timeAllocated = remainingTime * 0.5;
          }
          if (timeAllocated < 100) {
              timeAllocated = 100;
          }
      }
  
      int res = 0;
      enum GameStage stage = getStage(board);
      if (stage != EndGame) {
          res = neuralEvaluateDirect(board, uciMove);
          // Respect movetime if neural evaluation is too fast
          if (movetime > 0) {
              double elapsed = ((double)(clock() - start_time)) / CLOCKS_PER_SEC * 1000;
              if (elapsed < timeAllocated) {
                	struct timespec delay;
                  delay.tv_sec = 0;
                  delay.tv_nsec = (timeAllocated - elapsed) * 1000000;
                  //usleep((unsigned int)((timeAllocated - elapsed) * 1000)); // Sleep to match movetime
                  nanosleep(&delay, NULL);
              }
          }
      } else {
          if (__builtin_popcountl(board->occupations[PieceNameAny]) > TB_MAX_PIECES) 
            res = runMCTS(board, timeAllocated, uciMove);
          else {
            //unsigned results[TB_MAX_MOVES];
            unsigned int ep = __builtin_ctzl(board->fen->enPassantLegalBit);
            //fprintf(stderr, "white %lu, black %lu, kings %lu, queens %lu, rooks %lu, bishops %lu, knights %lu, pawns %lu, rule50 %u, castling %u, ep %u, turn %d\n", board->occupations[PieceNameWhite], board->occupations[PieceNameBlack], board->occupations[WhiteKing] | board->occupations[BlackKing], board->occupations[WhiteQueen] | board->occupations[BlackQueen], board->occupations[WhiteRook] | board->occupations[BlackRook], board->occupations[WhiteBishop] | board->occupations[BlackBishop], board->occupations[WhiteKnight] | board->occupations[BlackKnight], board->occupations[WhitePawn] | board->occupations[BlackPawn], board->fen->halfmoveClock, 0, ep == 64 ? 0 : ep, board->opponentColor == ColorBlack ? 1 : 0);

            unsigned int result = tb_probe_root(board->occupations[PieceNameWhite], board->occupations[PieceNameBlack], 
            board->occupations[WhiteKing] | board->occupations[BlackKing],
                board->occupations[WhiteQueen] | board->occupations[BlackQueen], board->occupations[WhiteRook] | board->occupations[BlackRook], board->occupations[WhiteBishop] | board->occupations[BlackBishop], board->occupations[WhiteKnight] | board->occupations[BlackKnight], board->occupations[WhitePawn] | board->occupations[BlackPawn],
                board->fen->halfmoveClock, 0, ep == 64 ? 0 : ep, board->opponentColor == ColorBlack ? 1 : 0, NULL);
            if (result == TB_RESULT_FAILED)
            {
                fprintf(stderr, "error: unable to probe tablebase; position invalid, illegal or not in tablebase\n");
                dprintf(logfile, "error: unable to probe tablebase; position invalid, illegal or not in tablebase\n");
                exit(-1);
            }
            /*
            unsigned wdl = TB_GET_WDL(result);
            const char * wdl_to_name_str[5] =
            {
                "Loss",
                "BlessedLoss",
                "Draw",
                "CursedWin",
                "Win"
            };
             
            if (result == TB_RESULT_CHECKMATE) {
                fprintf(stderr, "# %s\n", (board->opponentColor == ColorBlack ? "0-1" : "1-0"));
                dprintf(logfile, "# %s\n", (board->opponentColor == ColorBlack ? "0-1" : "1-0"));
                return;
            }
            if (board->fen->halfmoveClock >= 50 || result == TB_RESULT_STALEMATE) { 
                fprintf(stderr, " 1/2-1/2\n");
                dprintf(logfile, " 1/2-1/2\n");
                return;
            }*/
            unsigned int from     = TB_GET_FROM(result);
            unsigned int to       = TB_GET_TO(result);
            unsigned int promotes = TB_GET_PROMOTES(result);
            //div_t move = div(move_idx, 64);
            //enum SquareName source_square = (enum SquareName)move.quot;
            //enum SquareName destination_square = (enum SquareName)move.rem;
            strncat(uciMove, squareName[from], 2);
            strncat(uciMove, squareName[to], 2);
            switch (promotes) {
              case 0:
                break;
              case 1:
                uciMove[4] = 'q';
                uciMove[5] = '\0';
                break;
              case 2:
                uciMove[4] = 'r';
                uciMove[5] = '\0';
                break;
              case 3:
                uciMove[4] = 'b';
                uciMove[5] = '\0';
                break;
              case 4:
                uciMove[4] = 'n';
                uciMove[5] = '\0';
                break;
            }
          }
      }
      if (res) {
          fprintf(stderr, "handleGo() error: evaluation returned %d\n", res);
          dprintf(logfile, "handleGo() error: evaluation returned %d\n", res);
          exit(-1);
      }
      printf("bestmove %s\n", uciMove);
      dprintf(logfile, "bestmove %s\n", uciMove);
      fflush(stdout);
  }
  
  void handleStop(struct Board *board) {
      stopFlag = 1;
  }
  
  void handleQuit(void) {
      stopFlag = 1;
      cleanup_magic_bitboards();
      cleanup();
      close(logfile);
      exit(0);
  }
}

int main(int argc, char **argv) {
    const std::string weights_file = "chessCNN4.pt";
    struct Fen fen;
    struct Board board;
    struct ZobristHash zh;
    zobristHash(&zh);
    logfile = open("uci.log", O_RDWR | O_APPEND | O_CREAT | O_TRUNC);
    fchmod(logfile, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    init_magic_bitboards();
    tb_init(TB_PATH);
    if (TB_LARGEST == 0) {
        fprintf(stderr, "error: unable to initialize tablebase; no tablebase files found\n");
        exit(-1);
    }

    // Device selection
    if (torch::cuda::is_available()) {
        device = torch::Device(torch::kCUDA);
        dprintf(logfile, "Using CUDA device\n");
        std::cerr << "Using CUDA device" << std::endl;
    } else if (torch::hasMPS()) {
        device = torch::Device(torch::kMPS);
        dprintf(logfile, "Using MPS device\n");
        std::cerr << "Using MPS device" << std::endl;
    } else {
        dprintf(logfile, "Using CPU device\n");
        std::cerr << "Using CPU device" << std::endl;
    }

    model->eval();
    torch::NoGradGuard no_grad;

    if (std::filesystem::exists(weights_file)) {
        try {
            dprintf(logfile, "Loading weights from %s...\n", weights_file.c_str());
            std::cerr << "Loading weights from " << weights_file << "..." << std::endl;
            // Load traced model
            auto traced_module = torch::jit::load(weights_file, device);
            // Copy parameters from traced module to model
            auto traced_params = traced_module.named_parameters();
            auto model_params = model->named_parameters();
            for (const auto& pair : traced_params) {
                std::string key = pair.name;
                // Detach the source tensor to avoid gradient issues
                torch::Tensor value = pair.value.detach().to(device);
                auto param = model_params.find(key);
                if (param != nullptr) {
                    // Disable requires_grad temporarily
                    bool orig_requires_grad = param->requires_grad();
                    param->set_requires_grad(false);
                    param->copy_(value);
                    param->set_requires_grad(orig_requires_grad);
                } else {
                    dprintf(logfile, "Error: Parameter %s not found in model. Exiting...\n", key.c_str());
                    std::cerr << "Error: Parameter " << key << " not found in model. Exiting..." << std::endl;
                    goto exit;
                }
            }
            dprintf(logfile, "Loaded weights from %s\n", weights_file.c_str());
            std::cerr << "Loaded weights from " << weights_file << std::endl;
        } catch (const c10::Error& e) {
            dprintf(logfile, "Error loading weights: %s. Exiting...\n", e.what());
            std::cerr << "Error loading weights: " << e.what() << std::endl;
            std::cerr << "Exiting..." << std::endl;
            goto exit;
        } catch (const std::runtime_error& e) {
            dprintf(logfile, "Error loading weights: %s. Exiting...\n", e.what());
            std::cerr << "Error loading weights: " << e.what() << std::endl;
            std::cerr << "Exiting..." << std::endl;
            goto exit;
        }
    } else {
        dprintf(logfile, "Weights file %s not found. Exiting...\n", weights_file.c_str());
        std::cerr << "Weights file " << weights_file << " not found, exiting..." << std::endl;
        goto exit;
    }
            
    model->to(device);

    uciLoop(&fen, &board, &zh);
exit:
    cleanup_magic_bitboards();
    close(logfile);
    return 0;
}