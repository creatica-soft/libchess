#include <algorithm>
#include "creatica.hpp"

extern std::mutex mtx, log_mtx, print_mtx, pool_mutex, search_done_mtx;//, nnue_cache_mutex;
//extern std::shared_mutex map_mutex;
extern std::condition_variable cv, pool_cv, pool_done_cv, cv_search_done;
extern std::atomic<bool> searchFlag;
extern std::atomic<bool> stopFlag;
extern std::atomic<bool> quitFlag;
extern std::atomic<bool> ponderHit;
extern std::atomic<bool> pool_quit;
extern std::atomic<int> pool_generation;
extern std::atomic<int> active_workers;
extern std::atomic<bool> search_done;
extern std::atomic<unsigned long long> total_children;
extern std::atomic<unsigned long long> tbhits;
extern std::atomic<int> generation;
extern std::atomic<int> hash_full;
extern std::atomic<int> depth;
extern std::atomic<int> seldepth;

extern FILE * logfile;
extern char best_move[6];
extern bool tb_init_done;
extern double timeAllocated; //ms
extern double exploration_min;
extern double exploration_max;
extern double exploration_depth_decay;
//extern double probability_mass;
//extern double virtual_loss;
extern double eval_scale;
extern double temperature;
//extern std::unordered_map<uint64_t, double> nnue_cache; //too much waste because of contention

extern std::string last_move;
extern std::unordered_set<unsigned long long> position_history;
extern Board board;
extern ZobristHash zh;
extern Zobrist z;
extern Engine chessEngine;
//extern MCTSSearch search;
extern std::vector<std::thread> pool_threads;
extern std::vector<ThreadParams *> pool_params;
using json = nlohmann::json;

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
    std::string * response = static_cast<std::string*>(userp);
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
          for (int i = 0; i < chessEngine.optionSpin[Threads].value; i++) {
            cleanup(i);
          }
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
        } else if (strcmp(line, "eval") == 0) {
          handleEval();
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
      int idx;
      OptionType type;
      for (int i = Button; i <= String; i++) {
        idx = nametoindex(chessEngine, name, (OptionType)i);
        if (idx >= 0) {
          type = (OptionType)i;
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
          if (v >= chessEngine.optionSpin[idx].min && v <= chessEngine.optionSpin[idx].max) {
            chessEngine.optionSpin[idx].value = v;
            switch(idx) {
              case ExplorationMin:
                exploration_min = static_cast<double>(chessEngine.optionSpin[ExplorationMin].value) * 0.01;
                break;
              case ExplorationMax:
                exploration_max = static_cast<double>(chessEngine.optionSpin[ExplorationMax].value) * 0.01;
                break;
              case ExplorationDepthDecay:
                exploration_depth_decay = static_cast<double>(chessEngine.optionSpin[ExplorationDepthDecay].value) * 0.01;
                break;
              //case ProbabilityMass:
              //  probability_mass = static_cast<double>(chessEngine.optionSpin[ProbabilityMass].value) * 0.01;
              //  break;
              case Threads:
                shutdown_thread_pool();
                init_thread_pool(chessEngine.optionSpin[Threads].value);
                break;
              case EvalScale:
                eval_scale = static_cast<double>(chessEngine.optionSpin[EvalScale].value) * 0.1;
                break;
              case Temperature:
                temperature = static_cast<double>(chessEngine.optionSpin[Temperature].value) * 0.01; //used in calculating probabilities for moves in                 
                break;
            }
          }
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
  numberOfPieces = bitCount(board.side[ColorWhite] | board.side[ColorBlack]);
  print("%d\n", numberOfPieces);
  log_file("%d\n", numberOfPieces);
}

void handleEval(void) {
  std::string eval = nnue_eval(board);
  print("%s\n", eval.c_str());
}

void handleNewGame() {
  for (int i = 0; i < chessEngine.optionSpin[Threads].value; i++) {
    cleanup(i);
  }
  position_history.clear();
  last_move.clear();
}

void handlePosition(char * command) {
  std::string position(command);
  last_move.clear();
  std::string fen_string;
  std::string uci_moves;
  std::vector<std::string> moves;
  size_t moves_pos = position.find("moves", 18);
  if (moves_pos != std::string::npos) uci_moves = position.substr(moves_pos + 6);
  if (position.find("startpos", 9) != std::string::npos) fen_string.assign(startPos);
  else {
    size_t fen_pos = position.find("fen", 9);
    if (fen_pos != std::string::npos) {
      if (moves_pos != std::string::npos) fen_string = position.substr(fen_pos + 4, moves_pos - fen_pos - 5);
      else fen_string = position.substr(fen_pos + 4);
    } else {
      log_file("handlePosition() error: neither startpos no fen keyword is present in position command\n");
      return;
    }
  }
  if (fen_string.compare(startPos) == 0 && uci_moves.empty()) position_history.clear();
  while (!uci_moves.empty() && std::isspace(uci_moves.back())) uci_moves.pop_back();
  if (uci_moves.size() >= 4) { //we have at least one move
    std::istringstream moves_stream(uci_moves);
    std::string some_move;
    while (moves_stream >> some_move) moves.push_back(some_move);
  }
  fen2board(board, fen_string.c_str());
  getHash(zh, board, z);
  if (!moves.empty()) {
    size_t num_moves = moves.size() - 1; //last move should wait till go
    for (int i = 0; i < num_moves; i++) {
      Move move = {};
      uci2move_idx(moves[i].c_str(), move);
      updateHash(zh, board, move, ff_move(board, move), z);
      position_history.insert(zh.hash);
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
      Move move = {};
      uci2move_idx(last_move.c_str(), move);
      updateHash(zh, board, move, ff_move(board, move), z);
      position_history.insert(zh.hash);
      last_move.clear();
    }

    if (chessEngine.movetime > 0) timeAllocated = chessEngine.movetime * 0.99;
    else if (chessEngine.infinite) timeAllocated = 1e9;
    else {
        int remainingTime = board.sideToMove == ColorWhite ? chessEngine.wtime : chessEngine.btime;
        int increment = board.sideToMove == ColorWhite ? chessEngine.winc : chessEngine.binc;
        int movesLeft = chessEngine.movestogo ? chessEngine.movestogo : MAX_MOVES_REMAINING - board.moveNumber;
        if (movesLeft < MIN_MOVES_REMAINING) movesLeft = MIN_MOVES_REMAINING;
        if (remainingTime > TIME_SAFETY_BUFFER) { //5000 ms
            remainingTime -= TIME_SAFETY_BUFFER;
        }
        timeAllocated = (double)remainingTime / movesLeft + increment * 0.5;
        if (board.moveNumber > 10) {
            timeAllocated *= CRITICAL_TIME_FACTOR; //1.5
        }
        if (remainingTime < MIN_TIME_THRESHOLD) { //10000 ms
            //CAP, do not assign. Assigning made the allocation non-monotonic in the clock:
            //at 14999 ms remaining it handed out 5000 ms - a third of the whole clock on a
            //single move - while at 15001 ms it handed out 100 ms. More time bought LESS
            //thinking, at 30 separate points across the clock sweep. std::min can only ever
            //lower the figure, so this cannot spend more time than before on any input.
            timeAllocated = std::min(timeAllocated, remainingTime * 0.5);
        }
        if (timeAllocated < 3000) {
            timeAllocated = 100;
        }
    }
    int numberOfPieces = bitCount(board.side[ColorWhite] | board.side[ColorBlack]);
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
        char fenString[MAX_FEN_STRING_LEN];
        std::string fen_string(board2fen(board, fenString));
        while (!fen_string.empty() && std::isspace(fen_string.back())) {
          fen_string.pop_back(); //trim whitespaces at the end
        }
        size_t pos = fen_string.rfind(" ");
        while (pos != std::string::npos) {
          fen_string.replace(pos, 1, "_");
          pos = fen_string.rfind(" ");
        }
        std::string syzygy_tb_url = "http://tablebase.lichess.ovh/standard?fen=" + fen_string;
        int score_cp = 0;
        std::string uci_move = "";
        if (sendGetRequest(syzygy_tb_url, score_cp, uci_move)) {
            strncpy(best_move, uci_move.c_str(), 6);
            print("info depth 1 seldepth 1 multipv 1 score cp %d nodes 1 nps 1 hashfull 0 tbhits 1 time 0 pv %s\nbestmove %s\n", score_cp, best_move, best_move);
            log_file("info depth 1 seldepth 1 multipv 1 score cp %d nodes 1 nps 1 hashfull 0 tbhits 1 time 0 pv %s\nbestmove %s\n", score_cp, best_move, best_move);
            Move move = {};
            uci2move_idx(best_move, move);
            updateHash(zh, board, move, ff_move(board, move), z);
            position_history.insert(zh.hash);
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
      unsigned int ep = legalEnPassantMove(board);     
      unsigned int result = tb_probe_root(board.side[ColorWhite], board.side[ColorBlack], board.pieceTypes[King - 1], board.pieceTypes[Queen - 1], board.pieceTypes[Rook - 1], board.pieceTypes[Bishop - 1], board.pieceTypes[Knight - 1], board.pieceTypes[Pawn - 1], board.halfmoveClock, 0, ep == SquareNone ? 0 : ep, (board.sideToMove ^ 1) == ColorBlack ? 1 : 0, NULL);
      if (result == TB_RESULT_FAILED) {
          log_file("handleGo() error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, numberOfPieces %u\n", TB_LARGEST, numberOfPieces);
          //A failed ROOT probe is not fatal. tb_probe_root is stricter than tb_probe_wdl,
          //so one missing, unreadable or rejected table used to kill the whole process
          //mid-game - on lichess that is a forfeit. Fall back to the normal search, the
          //same way the numberOfPieces > 7 branch above starts it.
          std::lock_guard<std::mutex> lock(mtx);
          searchFlag.store(true);
          stopFlag.store(false);
          cv.notify_all(); // Start search
          return;
      }
      unsigned int wdl = TB_GET_WDL(result); //0 - loss, 4 - win, 1..3 - draw
      int scorecp = 0;
      if (wdl == 4) scorecp = MATE_SCORE;
      else if (wdl == 0) scorecp = -MATE_SCORE;
      unsigned int from = TB_GET_FROM(result);
      unsigned int to = TB_GET_TO(result);
      unsigned int promotes = TB_GET_PROMOTES(result);
      strncat(best_move, square[from], 2);
      strncat(best_move, square[to], 2);
      best_move[4] = uciPromoLetter[6 - promotes];
      best_move[5] = '\0';
      print("info depth 1 seldepth 1 multipv 1 score cp %d nodes 1 nps 1 hashfull 0 tbhits 1 time 0 pv %s\nbestmove %s\n", scorecp, best_move, best_move);
      log_file("info depth 1 seldepth 1 multipv 1 score cp %d nodes 1 nps 1 hashfull 0 tbhits 1 time 0 pv %s\nbestmove %s\n", scorecp, best_move, best_move);
      Move move = {};
      uci2move_idx(best_move, move);
      updateHash(zh, board, move, ff_move(board, move), z);
      position_history.insert(zh.hash);
    }          
}

void handlePonderhit(char * command) { //stop pondering, execute last_move and continue searching normally
  log_file("handlePonderhit(): stop\n");
  handleStop();
  if (!last_move.empty()) {
    Move move = {};
    uci2move_idx(last_move.c_str(), move);
    updateHash(zh, board, move, ff_move(board, move), z);
    position_history.insert(zh.hash);
    char fenString[MAX_FEN_STRING_LEN];
    log_file("position fen %s\n", board2fen(board, fenString));
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

void setEngineOptions() {
    strcpy(chessEngine.id, "Creatica (MCT Parallel Roots)");
    strcpy(chessEngine.authors, "Arkadi Poliakevitch");
    chessEngine.numberOfCheckOptions = 3;
	  chessEngine.numberOfComboOptions = 0;
	  chessEngine.numberOfSpinOptions = 9;
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
	  //strcpy(chessEngine.optionSpin[ProbabilityMass].name, "ProbabilityMass");
	  //chessEngine.optionSpin[ProbabilityMass].defaultValue = PROBABILITY_MASS;
	  //chessEngine.optionSpin[ProbabilityMass].value = chessEngine.optionSpin[ProbabilityMass].defaultValue;
	  //chessEngine.optionSpin[ProbabilityMass].min = 1;
	  //chessEngine.optionSpin[ProbabilityMass].max = 100;
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
	  //strcpy(chessEngine.optionSpin[VirtualLoss].name, "VirtualLoss");
	  //chessEngine.optionSpin[VirtualLoss].defaultValue = VIRTUAL_LOSS;
	  //chessEngine.optionSpin[VirtualLoss].value = chessEngine.optionSpin[VirtualLoss].defaultValue;
	  //chessEngine.optionSpin[VirtualLoss].min = 0;
	  //chessEngine.optionSpin[VirtualLoss].max = 100;
	  strcpy(chessEngine.optionSpin[PVPlies].name, "PVPlies");
	  chessEngine.optionSpin[PVPlies].defaultValue = PV_PLIES;
	  chessEngine.optionSpin[PVPlies].value = chessEngine.optionSpin[PVPlies].defaultValue;
	  chessEngine.optionSpin[PVPlies].min = 1;
	  chessEngine.optionSpin[PVPlies].max = 32;
	  strcpy(chessEngine.optionSpin[EvalScale].name, "EvalScale");
	  chessEngine.optionSpin[EvalScale].defaultValue = EVAL_SCALE;
	  chessEngine.optionSpin[EvalScale].value = chessEngine.optionSpin[EvalScale].defaultValue;
	  chessEngine.optionSpin[EvalScale].min = 10;
	  chessEngine.optionSpin[EvalScale].max = 100;
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
    //probability_mass = static_cast<double>(chessEngine.optionSpin[ProbabilityMass].value) * 0.01;
    //virtual_loss = static_cast<double>(chessEngine.optionSpin[VirtualLoss].value) * 0.1;
    eval_scale = static_cast<double>(chessEngine.optionSpin[EvalScale].value) * 0.1;
    temperature = static_cast<double>(chessEngine.optionSpin[Temperature].value) * 0.01; //used in calculating probabilities for moves in softmax exp((eval - max_eval)/temperature) / eval_sum
                          //can be tuned so that values < 1.0 sharpen the distribution and values > 1.0 flatten it
}

void shutdown_thread_pool() {
    pool_quit.store(true);
    pool_cv.notify_all(); // Wake everyone up so they see the quit flag
    int thread_id = 0;
    for (auto& t : pool_threads) {
        if (t.joinable()) t.join();
        delete pool_params[thread_id++];
    }
    pool_params.clear();
    pool_threads.clear();
}

// Search thread function
void persistent_worker_func(int thread_id) {
    int local_generation = 0;
    NNUEContext ctx;
    init_nnue_context(ctx);

    while (true) {
        // 2. WAIT PHASE
        std::unique_lock<std::mutex> lock(pool_mutex);
        pool_cv.wait(lock, [&] { 
            // Wake up if there is a new search generation OR we need to quit
            return pool_generation.load() > local_generation || pool_quit.load(); 
        });
        lock.unlock(); // Release lock so other threads can wake up
        if (pool_quit.load()) break;
        if (pool_generation.load() == local_generation) continue;
        local_generation = pool_generation.load();
        ThreadParams * params = pool_params[thread_id];
        auto iter_start = std::chrono::steady_clock::now();
        double elapsed = 0.0;
                
        while (depth.load(std::memory_order_relaxed) < chessEngine.depth &&
              elapsed < (params->time_alloc * 0.001) &&
              !stopFlag.load(std::memory_order_relaxed) &&
              hash_full.load(std::memory_order_relaxed) < 1000) {
      
            mcts_search(thread_id, ctx); 
            int expected = seldepth.load(std::memory_order_relaxed);
            while (params->seldepth > expected && !seldepth.compare_exchange_strong(expected, params->seldepth, std::memory_order_relaxed)) expected = seldepth.load(std::memory_order_relaxed);
            if ((chessEngine.depth && depth.load(std::memory_order_relaxed) >= chessEngine.depth) || 
                (chessEngine.nodes && params->nodes[0].N >= chessEngine.nodes)) {
                 break;
            }
            elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
        }
        if (active_workers.fetch_sub(1) == 1) pool_done_cv.notify_one();
    }
    free_nnue_context(ctx);
}

void init_thread_pool(const int num_threads) {
    // Stop existing threads if any
    shutdown_thread_pool();

    pool_quit.store(false);
    pool_generation.store(0);

    for (int i = 0; i < num_threads; ++i) {
      ThreadParams * params = new ThreadParams();
      params->nodes.reserve(MAX_NODES);
      params->nnue_cache.reserve(1000000);
      pool_params.push_back(params);
      pool_threads.emplace_back(persistent_worker_func, i);
    }
}

int main(int argc, char **argv) {
    TB_LARGEST = 0;
    zobristHash(z);
    logfile = fopen("uci.log", "a"); //was "w"
    srand(time(NULL)); 
    init_magic_bitboards();
    init_nnue("nn-1c0000000000.nnue", "nn-37f18f62d772.nnue");
    //init_nnue("nn-1111cefa1111.nnue", "nn-37f18f62d772.nnue");
    //nnue_cache.reserve(1000000);
    setEngineOptions();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    init_thread_pool(chessEngine.optionSpin[Threads].value);

    std::thread search_thread(search_thread_func);
 
    if (argc == 2 && std::string(argv[1]) == "bench") {
      char pos[18] = "position startpos";
      handlePosition(pos);
      char go[18] = "go movetime 30000";
      handleGo(go);
      if (searchFlag.load()) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [] { return !searchFlag.load(); }); // Wait for search to stop
      }      
      handleQuit();
    } else {
      uciLoop();
    }
    search_thread.join();
    
    shutdown_thread_pool();
    curl_global_cleanup();
    cleanup_nnue();
    cleanup_magic_bitboards();
    fclose(logfile);
    return 0;
}