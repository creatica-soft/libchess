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
#include <unordered_set>
#include <condition_variable>
#include <curl/curl.h>
#include "json.hpp"
#include "tbprobe.h"
#include "libchess.h"

#define THREADS 8
#define MULTI_PV 5
#define SYZYGY_PATH_DEFAULT "<empty>"
#define SYZYGY_PATH "/Users/ap/syzygy"
#define HASH 2048 //default, GUI may set it via Hash option (once full, expansion won't happen!)
#define EXPLORATION_MIN 40 // used in formular for exploration constant decay with depth
#define EXPLORATION_MAX 120 //smaller value favor exploitation, i.e. deeper tree vs wider tree
#define EXPLORATION_DEPTH_DECAY 6 //linear decay of EXPLORATION CONSTANT with depth using formula:
                      // C * 100 = max(EXPLORATION_MIN, (EXPLORATION_MAX - seldepth * EXPLORATION_DEPTH_DECAY))
#define PROBABILITY_MASS 100 //% - cumulative probability - how many moves we consider
//#define MAX_NOISE 3 //% - default noise applied to move NNUE evaluations relative to their values, ie eval += eval * noise, not used because not effective
                     // where noise is sampled randomly from a uniform distribution [-0.3..0.3]
#define VIRTUAL_LOSS 4 //this is used primarily for performance in MT to avoid threads working on the same tree nodes
#define EVAL_SCALE 6 //This is a divisor in W = tanh(eval/eval_scale) where eval is NNUE evaluation in pawns. 
                     //W is a fundamental value in Monte Carlo tree node along with N (number of visits) 
                     //and P (prior move probability), though P belongs to edges (same as move) but W and N to nodes.
#define TEMPERATURE 60 //used in calculating probabilities for moves in get_prob() using softmax:
                        // exp((eval - max_eval)/(temperature/100)) / eval_sum
                        //can be tuned so that values < 1.0 sharpen the distribution and values > 1.0 flatten it
#define PV_PLIES 16
#define PONDER false
#define DISPLAY_INTERMITTENT_INFO_LINES true
#define DISPLAY_FINAL_INFO_LINES true

// Shared variables
std::mutex mtx, log_mtx, print_mtx;
std::condition_variable cv;
std::atomic<bool> searchFlag {false};
std::atomic<bool> stopFlag {false};
std::atomic<bool> quitFlag {false};
std::atomic<bool> ponderHit {false};
FILE * logfile = nullptr;
double timeAllocated = 0.0; //ms
char best_move[6] = "";
using json = nlohmann::json;
std::string last_move;

std::unordered_set<unsigned long long> position_history;

extern "C" {  
  struct Board * board = nullptr;
  struct Engine chessEngine;
  bool tb_init_done = false;
  double exploration_min;
  double exploration_max;
  double exploration_depth_decay;
  double probability_mass;
  //double noise;
  double virtual_loss;
  double eval_scale;
  double temperature;
  void init_nnue(const char * nnue_file_big, const char * nnue_file_small);
  void cleanup_nnue();
  extern void runMCTS();
  extern void cleanup(); //free Hash tree
  
  //UCI protocol in uci.c
  void uciLoop();
  void handleUCI(void);
  void handleIsReady(void);
  void handleNewGame();
  void handlePosition(char * command);
  void handleOption(char * command);
  void handleGo(char * command);
  void handlePonderhit(char * command);
  void handleStop();
  void handleQuit(void);
  void handlePieces(void); //non-standard UCI commnand "pieces" - returns the number of pieces on board

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

// WriteCallback to append data to the response string
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    std::string* response = static_cast<std::string*>(userp);
    size_t total_size = size * nmemb;
    response->append(static_cast<char*>(contents), total_size); // Append, don't overwrite
    return total_size;
}

// Function to perform a GET request and log response/code
//it appears that lichess allows no more than 10 requests per minute or so, then it will reply with 429 code - too many requests
//therefore, it can't be used during search
bool sendGetRequest(const std::string& url, int& scorecp, std::string& uci_move) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    char errbuf[CURL_ERROR_SIZE] = "";
    std::string response = "";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L); // in sec
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L); // in sec
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);    

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    //log_file("sendGetRequest() debug: GET %s, HTTP code %ld, Response: %s\n", url.c_str(),  http_code, response.c_str());
 
    bool success = (res == CURLE_OK && http_code == 200);
    if (success && !response.empty()) {
        try {
            json data = json::parse(response);
            if (data.contains("category")) {
                std::string category = data["category"];
                if (category == "win" || category == "syzygy-win" || category == "maybe-win") {
                    scorecp = MATE_SCORE;
                } else if (category == "loss" || category == "syzygy-loss" || category == "maybe-loss") {
                    scorecp = -MATE_SCORE;
                } else {
                    scorecp = 0;
                }
            }
            if (data.contains("moves") && data["moves"].is_array() && !data["moves"].empty()) {
                if (data["moves"][0].contains("uci")) {
                    uci_move = data["moves"][0].value("uci", "");
                }
            }
        } catch (const json::parse_error& e) {
            log_file("sendGetRequest() error: JSON parse error: %s for response %s\n", e.what(), response.c_str());
            success = false;
        }
    } else {
        log_file("sendGetRequest() error: curl error: %s (%s) or HTTP code %ld or empty response \"%s\"\n", curl_easy_strerror(res), errbuf, http_code, response.c_str());
    }
         
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        log_file("sendGetRequest() error: curl error: %s\n", curl_easy_strerror(res));
        return false;
    }
    return success;
}
  
// Search thread function
void search_thread_func() {
    while (!quitFlag.load()) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [] { return searchFlag.load() || quitFlag.load(); }); // Wait for "go" or "quit"
        lock.unlock();
        if (quitFlag.load()) {
          cleanup();
          position_history.clear();
          break;
        }
        if (searchFlag.load()) runMCTS();
        else continue;
        lock.lock();
        searchFlag.store(false);
        stopFlag.store(false);
        cv.notify_all(); // Signal search is done
    }
}
  
  void uciLoop() {
      char line[4096] = "";
      char go_line[4096] = "";
      print("id name %s\n", chessEngine.id);
      log_file("id name %s\n", chessEngine.id);
      while (!quitFlag.load()) {
          if (fgets(line, sizeof(line), stdin) == NULL) {
              log_file("uciloop() error: fgets() returned NULL\n");
              exit(-1);
          }
          log_file("%s", line);
          line[strcspn(line, "\n")] = 0;
  
          if (strcmp(line, "ucinewgame") == 0) {
              handleNewGame();
          } else if (strcmp(line, "uci") == 0) {
              handleUCI();
          } else if (strncmp(line, "setoption", 9) == 0) {
              handleOption(line);
          } else if (strcmp(line, "isready") == 0) {
              handleIsReady();
          } else if (strncmp(line, "position", 8) == 0) {
              handlePosition(line);
          } else if (strncmp(line, "go", 2) == 0) {
              char * ponder = strstr(line, "ponder");
              if (ponder) { //remove "ponder" from  go_line for ponderhit
                strncpy(go_line, line, ponder - line - 1);
                go_line[ponder - line - 1] = 0;
                strcat(go_line, ponder + 6);
              } else strcpy(go_line, line);
              handleGo(line);
          } else if (strcmp(line, "ponderhit") == 0) { //engine should not output bestmove prior to executing ponder move
              if (go_line[0] != '\0') {
                ponderHit.store(true);
                handlePonderhit(go_line);
              }
          } else if (strcmp(line, "pieces") == 0) {
            handlePieces();
          } else if (strcmp(line, "stop") == 0) {
              handleStop();
          } else if (strcmp(line, "quit") == 0) {
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
      if (strncmp(token, "value", 5) == 0) { //Button option does not have value and its name may have space; for example, Clear Hash
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
  
  void handlePieces(void) {
    int numberOfPieces = 0;
    numberOfPieces = bitCount(board->occupations[PieceNameAny]);
    print("%d\n", numberOfPieces);
    log_file("%d\n", numberOfPieces);
  }
  
  void handleNewGame() {
    cleanup();
    position_history.clear();
    last_move.clear();
  }
  
  void handlePosition(char * command) {
    struct Move move;
    std::string position(command);
    last_move.clear();
    std::string fen;
    std::string uci_moves;
    std::vector<std::string> moves;
    size_t moves_pos = position.find("moves", 18);
    if (moves_pos != std::string::npos) uci_moves = position.substr(moves_pos + 6);
    if (position.find("startpos", 9) != std::string::npos) fen.assign(startPos);
    else {
      size_t fen_pos = position.find("fen", 9);
      if (fen_pos != std::string::npos) {
        if (moves_pos != std::string::npos) fen = position.substr(fen_pos + 4, moves_pos - fen_pos - 5);
        else fen = position.substr(fen_pos + 4);
      } else {
        log_file("handlePosition() error: neither startpos no fen keyword is present in position command\n");
        return;
      }
    }
    if (fen.compare(startPos) == 0 && uci_moves.empty()) position_history.clear();
    while (!uci_moves.empty() && std::isspace(uci_moves.back())) uci_moves.pop_back();
    if (uci_moves.size() >= 4) { //we have at least one move
      std::istringstream moves_stream(uci_moves);
      std::string some_move;
      while (moves_stream >> some_move) moves.push_back(some_move);
    }
    strtofen(board->fen, fen.c_str());
    fentoboard(board->fen, board);
    getHash(board->zh, board);
    if (!moves.empty()) {
      size_t num_moves = moves.size() - 1; //last move should wait till go
      for (int i = 0; i < num_moves; i++) {
        int src = 0, dst = 0, promo = 0;
        move_to_idx(moves[i].c_str(), &src, &dst, &promo);
        struct Move move;
        ff_move(board, &move, src, dst, promo);
        updateFen(board);
        updateHash(board, &move);
        position_history.insert(board->zh->hash);
      }
      last_move = moves.back();
    }
  }
  
  void handleGo(char * command) {
      chessEngine.wtime = 1e9;
      chessEngine.btime = 1e9;
      chessEngine.winc = 0;
      chessEngine.binc = 0;
      chessEngine.movestogo = 0;
      chessEngine.movetime = 0;
      chessEngine.depth = 0;
      chessEngine.nodes = 0;
      chessEngine.infinite = false;
      chessEngine.ponder = false;

      char * token;
      if (searchFlag.load()) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [] { return !searchFlag.load(); }); // Wait for previous search to stop
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
          else if (strcmp(token, "ponder") == 0) {
              chessEngine.ponder = true;
          }
      }
      if (!chessEngine.ponder && !last_move.empty()) { //execute last move
        //last move in ponder mode (go ponder) should only be executed on ponderhit
        //It is usually opponent's move2 in engine's output "bestmove move1 ponder move2"
        //In any case a decision to exec last move should be suspended until go command!
        //If it's without ponder, then execute; otherwise, ponder on this move 
        int src = 0, dst = 0, promo = 0;
        move_to_idx(last_move.c_str(), &src, &dst, &promo);
        struct Move move;
        ff_move(board, &move, src, dst, promo);
        updateHash(board, &move);
        position_history.insert(board->zh->hash);
        last_move.clear();
      }
  
      if (chessEngine.movetime > 0) timeAllocated = chessEngine.movetime * 0.99;
      else if (chessEngine.infinite) timeAllocated = 1e9;
      else {
          int remainingTime = board->fen->sideToMove == ColorWhite ? chessEngine.wtime : chessEngine.btime;
          int increment = board->fen->sideToMove == ColorWhite ? chessEngine.winc : chessEngine.binc;
          int movesLeft = chessEngine.movestogo ? chessEngine.movestogo : MAX_MOVES_REMAINING - board->fen->moveNumber;
          if (movesLeft < MIN_MOVES_REMAINING) movesLeft = MIN_MOVES_REMAINING;
          if (remainingTime > TIME_SAFETY_BUFFER) { //5000 ms
              remainingTime -= TIME_SAFETY_BUFFER;
          }
          timeAllocated = (double)remainingTime / movesLeft + increment * 0.5;
          if (board->fen->moveNumber > 10) {
              timeAllocated *= CRITICAL_TIME_FACTOR; //1.5
          }
          if (remainingTime < MIN_TIME_THRESHOLD) { //10000 ms
              timeAllocated = remainingTime * 0.5;
          }
          if (timeAllocated < 3000) {
              timeAllocated = 100;
          }
      }
      int numberOfPieces = bitCount(board->occupations[PieceNameAny]);
      if (numberOfPieces > 7) {
          std::lock_guard<std::mutex> lock(mtx);
          searchFlag.store(true);
          stopFlag.store(false);
          cv.notify_all(); // Start search
      } else if (numberOfPieces > TB_LARGEST) {
          if (chessEngine.ponder) {
            if (chessEngine.infinite) chessEngine.infinite = false;
            chessEngine.ponder = false;
            return; //no need to query table bases for the opponent
          }
          updateFen(board);
          std::string fen(board->fen->fenString);
          while (!fen.empty() && std::isspace(fen.back())) {
            fen.pop_back(); //trim whitespaces at the end
          }
          size_t pos = fen.rfind(" ");
          while (pos != std::string::npos) {
            fen.replace(pos, 1, "_");
            pos = fen.rfind(" ");
          }
          std::string syzygy_tb_url = "http://tablebase.lichess.ovh/standard?fen=" + fen;
          int score_cp = 0;
          std::string uci_move = "";
          if (sendGetRequest(syzygy_tb_url, score_cp, uci_move)) {
              strncpy(best_move, uci_move.c_str(), 6);
              print("info depth 1 seldepth 1 multipv 1 score cp %d nodes 1 nps 1 hashfull 0 tbhits 1 time 0 pv %s\nbestmove %s\n", score_cp, best_move, best_move);
              log_file("info depth 1 seldepth 1 multipv 1 score cp %d nodes 1 nps 1 hashfull 0 tbhits 1 time 0 pv %s\nbestmove %s\n", score_cp, best_move, best_move);
              int src = 0, dst = 0, promo = 0;
              move_to_idx(best_move, &src, &dst, &promo);
              struct Move move;
              ff_move(board, &move, src, dst, promo);
              updateHash(board, &move);
              position_history.insert(board->zh->hash);
          } else {
              log_file("HandleGo() error: failed to send Syzygy TB request to lichess\n");
              std::lock_guard<std::mutex> lock(mtx);
              searchFlag.store(true);
              stopFlag.store(false);
              cv.notify_all(); // Start search
          }        
      } else {
        if (chessEngine.ponder) {
          if (chessEngine.infinite) chessEngine.infinite = false;
          chessEngine.ponder = false;
          return; //no need to query table bases for the opponent
        }        
        best_move[0] = '\0';
        unsigned int ep = 64; 
        if (board->fen->enPassant != FileNone) ep = lsBit(enPassantLegalBit(board));     
        unsigned int result = tb_probe_root(board->occupations[PieceNameWhite], board->occupations[PieceNameBlack], 
            board->occupations[WhiteKing] | board->occupations[BlackKing],
            board->occupations[WhiteQueen] | board->occupations[BlackQueen], board->occupations[WhiteRook] | board->occupations[BlackRook], board->occupations[WhiteBishop] | board->occupations[BlackBishop], board->occupations[WhiteKnight] | board->occupations[BlackKnight], board->occupations[WhitePawn] | board->occupations[BlackPawn],
            board->fen->halfmoveClock, 0, ep == 64 ? 0 : ep, OPP_COLOR(board->fen->sideToMove) == ColorBlack ? 1 : 0, NULL);
        if (result == TB_RESULT_FAILED) {
            log_file("handleGo() error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, occupations %u\n", TB_LARGEST, bitCount(board->occupations[PieceNameAny]));
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
        int src = 0, dst = 0, promo = 0;
        move_to_idx(best_move, &src, &dst, &promo);
        struct Move move;
        ff_move(board, &move, src, dst, promo);
        updateHash(board, &move);
        position_history.insert(board->zh->hash);
      }          
  }
  
  void handlePonderhit(char * command) { //stop pondering, execute last_move and continue searching normally
    log_file("handlePonderhit(): stop\n");
    handleStop();
    if (!last_move.empty()) {
      int src = 0, dst = 0, promo = 0;
      move_to_idx(last_move.c_str(), &src, &dst, &promo);
      struct Move move;
      ff_move(board, &move, src, dst, promo);
      updateHash(board, &move);
      position_history.insert(board->zh->hash);
      updateFen(board);
      log_file("position fen %s\n", board->fen->fenString);
      last_move.clear();
    }    
    chessEngine.ponder = false;
    ponderHit.store(false);
    log_file("%s\n", command);
    handleGo(command);
  }
  
  void handleStop() {
    std::unique_lock<std::mutex> lock(mtx);
    stopFlag.store(true);
    cv.wait(lock, [] { return !searchFlag.load(); }); // Wait for search to stop
  }
  
  void handleQuit(void) {
    {
      std::lock_guard<std::mutex> lock(mtx);
      quitFlag.store(true);
    }
    cv.notify_all();
  }
}

void setEngineOptions() {
    strcpy(chessEngine.id, "Creatica Chess Engine 1.0");
    strcpy(chessEngine.authors, "Creatica");
    chessEngine.numberOfCheckOptions = 3;
	  chessEngine.numberOfComboOptions = 0;
	  chessEngine.numberOfSpinOptions = 11;
		chessEngine.numberOfStringOptions = 1;
		chessEngine.numberOfButtonOptions = 0;
	  strcpy(chessEngine.optionCheck[Ponder].name, "Ponder");
	  chessEngine.optionCheck[Ponder].defaultValue = PONDER;
	  chessEngine.optionCheck[Ponder].value = chessEngine.optionCheck[Ponder].defaultValue;
	  strcpy(chessEngine.optionCheck[FinalInfoLines].name, "FinalInfoLines");
	  chessEngine.optionCheck[FinalInfoLines].defaultValue = DISPLAY_FINAL_INFO_LINES;
	  chessEngine.optionCheck[FinalInfoLines].value = chessEngine.optionCheck[FinalInfoLines].defaultValue;
	  strcpy(chessEngine.optionCheck[IntermittentInfoLines].name, "IntermittentInfoLines");
	  chessEngine.optionCheck[IntermittentInfoLines].defaultValue = DISPLAY_INTERMITTENT_INFO_LINES;
	  chessEngine.optionCheck[IntermittentInfoLines].value = chessEngine.optionCheck[IntermittentInfoLines].defaultValue;
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
	  }
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
	  strcpy(chessEngine.optionSpin[ExplorationMax].name, "ExplorationMax");
	  chessEngine.optionSpin[ExplorationMax].defaultValue = EXPLORATION_MAX;
	  chessEngine.optionSpin[ExplorationMax].value = chessEngine.optionSpin[ExplorationMax].defaultValue;
	  chessEngine.optionSpin[ExplorationMax].min = 0;
	  chessEngine.optionSpin[ExplorationMax].max = 200;
	  strcpy(chessEngine.optionSpin[ExplorationMin].name, "ExplorationMin");
	  chessEngine.optionSpin[ExplorationMin].defaultValue = EXPLORATION_MIN;
	  chessEngine.optionSpin[ExplorationMin].value = chessEngine.optionSpin[ExplorationMin].defaultValue;
	  chessEngine.optionSpin[ExplorationMin].min = 0;
	  chessEngine.optionSpin[ExplorationMin].max = 100;
	  strcpy(chessEngine.optionSpin[ExplorationDepthDecay].name, "ExplorationDepthDecay");
	  chessEngine.optionSpin[ExplorationDepthDecay].defaultValue = EXPLORATION_DEPTH_DECAY;
	  chessEngine.optionSpin[ExplorationDepthDecay].value = chessEngine.optionSpin[ExplorationDepthDecay].defaultValue;
	  chessEngine.optionSpin[ExplorationDepthDecay].min = 0;
	  chessEngine.optionSpin[ExplorationDepthDecay].max = 10;
	  //strcpy(chessEngine.optionSpin[Noise].name, "Noise");
	  //chessEngine.optionSpin[Noise].defaultValue = MAX_NOISE;
	  //chessEngine.optionSpin[Noise].value = chessEngine.optionSpin[Noise].defaultValue;
	  //chessEngine.optionSpin[Noise].min = 1;
	  //chessEngine.optionSpin[Noise].max = 30;
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
	  strcpy(chessEngine.optionSpin[EvalScale].name, "EvalScale");
	  chessEngine.optionSpin[EvalScale].defaultValue = EVAL_SCALE;
	  chessEngine.optionSpin[EvalScale].value = chessEngine.optionSpin[EvalScale].defaultValue;
	  chessEngine.optionSpin[EvalScale].min = 1;
	  chessEngine.optionSpin[EvalScale].max = 16;
	  strcpy(chessEngine.optionSpin[Temperature].name, "Temperature");
	  chessEngine.optionSpin[Temperature].defaultValue = TEMPERATURE;
	  chessEngine.optionSpin[Temperature].value = chessEngine.optionSpin[Temperature].defaultValue;
	  chessEngine.optionSpin[Temperature].min = 1;
	  chessEngine.optionSpin[Temperature].max = 200;
    chessEngine.wtime = 1e9;
    chessEngine.btime = 1e9;
    chessEngine.winc = 0;
    chessEngine.binc = 0;
    chessEngine.movestogo = 0;
    chessEngine.movetime = 0;
    chessEngine.depth = 0;
    chessEngine.nodes = 0;
    chessEngine.infinite = false;
    chessEngine.ponder = false;
    exploration_min = static_cast<double>(chessEngine.optionSpin[ExplorationMin].value) * 0.01;
    exploration_max = static_cast<double>(chessEngine.optionSpin[ExplorationMax].value) * 0.01;
    exploration_depth_decay = static_cast<double>(chessEngine.optionSpin[ExplorationDepthDecay].value) * 0.01;
    probability_mass = static_cast<double>(chessEngine.optionSpin[ProbabilityMass].value) * 0.01;
    //noise = static_cast<double>(chessEngine.optionSpin[Noise].value) * 0.01;
    virtual_loss = static_cast<double>(chessEngine.optionSpin[VirtualLoss].value);
    eval_scale = static_cast<double>(chessEngine.optionSpin[EvalScale].value);
    temperature = static_cast<double>(chessEngine.optionSpin[Temperature].value); //used in calculating probabilities for moves in softmax exp((eval - max_eval)/temperature) / eval_sum
                          //can be tuned so that values < 1.0 sharpen the distribution and values > 1.0 flatten it
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
    logfile = fopen("uci.log", "a"); //was "w"
    srand(time(NULL)); 
    init_magic_bitboards();
    init_nnue("nn-1c0000000000.nnue", "nn-37f18f62d772.nnue");
    setEngineOptions();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    // Start the search thread
    std::thread search_thread(search_thread_func);
    uciLoop();
    search_thread.join();
    
    curl_global_cleanup();
    cleanup_nnue();
    cleanup_magic_bitboards();
    fclose(logfile);
    return 0;
}