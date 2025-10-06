//c++ -Wno-writable-strings -std=c++20 -O3 -flto -I /Users/ap/libchess -L /Users/ap/libchess -Wl,-lcurl,-lchess,-rpath,/Users/ap/libchess lichess_bot2.cpp -o lichess_bot2

#include <functional>
#include <iostream>
#include <string>
#include <sstream>
#include <cstdio>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <unordered_set>
#include <curl/curl.h>
#include <fcntl.h>
#include <csignal>
#include <random>       // For RNG in GetAndProcessBots
#include "json.hpp"     // https://github.com/nlohmann/json
#include "libchess.h"

#define CREATICA_PATH "/Users/ap/libchess/creatica"
#define DEPTH 0
#define MOVETIME 0
#define HASH 2048
#define THREADS 8
#define SYZYGY_PATH "/Users/ap/syzygy"
#define BOT_USERNAME "creaticachessbot"  // Lowercase, as per API IDs
#define DRAW_CP 30 //accept draw if score cp is less than this value in centipawns
#define MIN_ELO 1800
#define MAX_ELO 2100
#define ELO_CREATICA 2000
#define CLOCK_LIMIT 180 //seconds
#define CLOCK_INCREMENT 3 //seconds
#define NUMBER_OF_BOTS 50 //number of online bots to return from the list
#define MULTI_PV 2
#define EXPLORATION_CONSTANT 100 //smaller value favor exploitation, i.e. deeper tree vs wider tree - varies per thread
#define PROBABILITY_MASS 90 //% - cumulative probability - how many moves we consider - varies per thread [0.5..0.99]
#define MAX_NOISE 1 //% - default noise applied to move NNUE evaluations relative to their values, ie eval += eval * noise
                     // where noise is sampled randomly from a uniform distribution [-0.2..0.2]
#define VIRTUAL_LOSS 3 //this is used primarily for performance in MT to avoid threads working on the same tree nodes
#define EVAL_SCALE 6 //This is a divisor in W = tanh(eval/eval_scale) where eval is NNUE evaluation in pawns. 
                     //W is a fundamental value in Monte Carlo tree node along with N (number of visits) 
                     //and P (prior move probability), though P belongs to edges (same as move) but W and N to nodes.
#define TEMPERATURE 110 //used in calculating probabilities for moves in get_prob() using softmax:
                        // exp((eval - max_eval)/(temperature/100)) / eval_sum
                        //can be tuned so that values < 1.0 sharpen the distribution and values > 1.0 flatten it

const std::string token = "faked_token"; // Replace with real token
std::string current_game_id = "";
std::atomic<bool> game_in_progress {false};
std::atomic<bool> challenge_accepted {false};
std::atomic<bool> challenge_declined {false};
std::atomic<bool> playing {true};
std::mutex mutex;
std::mutex playing_mutex;
std::mutex challenge_mutex;
std::condition_variable game_cv, challenge_cv;
int nb = NUMBER_OF_BOTS;
using json = nlohmann::json;
std::mt19937 rng;
struct Engine * creatica = nullptr;
struct Evaluation * evaluations[MULTI_PV] = { nullptr };
//enum GameStateStatus { created, started, aborted, mate, resign, stalemate, timeout, draw, outoftime, cheat, noStart, unknownFinish, insufficientMaterialClaim, variantEnd };
std::string gameStateStatus = "";

struct Bot {
    std::string botname;
    int games;
    int elo;
    int rd;
    int prog;
};

// Struct for WriteCallback state (to handle partial lines across calls)
struct StreamState {
    std::string partial_line;
    std::function<void(const json&)> process_line;
};

void setEngineOptions() {
	  creatica->optionSpin[ProbabilityMass].value = PROBABILITY_MASS;
	  creatica->optionSpin[ExplorationConstant].value = EXPLORATION_CONSTANT;
	  creatica->optionSpin[Noise].value = MAX_NOISE;
	  creatica->optionSpin[VirtualLoss].value = VIRTUAL_LOSS;
	  creatica->optionSpin[EvalScale].value = EVAL_SCALE;
	  creatica->optionSpin[Temperature].value = TEMPERATURE;
}

// Callback for curl to write response data incrementally
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    if (!playing.load()) return 0; //exit streaming if ctrl-c is pressed
    StreamState* state = static_cast<StreamState*>(userp);
    std::string data((char*)contents, size * nmemb);
    //std::cout << "WriteCallback() debug: received chunk: " << data << std::endl;
    size_t pos = 0;
    while ((pos = data.find('\n', pos)) != std::string::npos) {
        std::string line = state->partial_line + data.substr(0, pos);
        data.erase(0, pos + 1);
        state->partial_line.clear();

        if (!line.empty()) {
            try {
                json j = json::parse(line);
                //std::cout << "WriteCallback() debug: parsed line: " << j.dump() << std::endl;
                state->process_line(j);
                if (game_in_progress.load() && gameStateStatus != "created" && gameStateStatus != "started" && gameStateStatus != "") {
                  std::cout << "WriteCallback() debug: gameStateStatus " << gameStateStatus << std::endl;
                  gameStateStatus = ""; //reset gameStateStatus
                  return 0;
                }
            } catch (const std::exception& e) {
                std::cerr << "WriteCallback() error: JSON parse error: " << e.what() << " - Line: " << line << std::endl;
            }
        } else {
            //std::cout << "WriteCallback() debug: keep-alive empty line received" << std::endl;
        }
    }
    state->partial_line += data;  // Save any remaining partial line
    return size * nmemb;
}

// A classic C-style callback function that libcurl will call to write data.
static size_t WriteCallback2(void* contents, size_t size, size_t nmemb, void* userp) {
    // userp is the pointer to our std::string object.
    // Append the data received from libcurl to our string.
    size_t total_size = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total_size);
    return total_size;
}

// Combined function for HTTP requests (GET or POST) with retries
bool HttpRequest(const std::string& method, const std::string& url, const std::string& postfields = "", std::string* response_out = nullptr, const std::string& accept = "", bool auth_required = true) {
    const int max_retries = 3;
    int retry_count = 0;
    bool success = false;

    while (!success && retry_count < max_retries) {
        CURL* curl = curl_easy_init();
        if (!curl) break;
        std::string auth_header;
        std::string accept_header;
        struct curl_slist* headers = nullptr;
        if (auth_required) {
            auth_header = "Authorization: Bearer " + token;
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        if (!accept.empty()) {
            accept_header = "Accept: " + accept; 
            headers = curl_slist_append(headers, accept_header.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L); //for better performance?
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 65536L);
        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            if (!postfields.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postfields.c_str());
            } else {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
            }
        } else if (method == "GET") {
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        } else {
            std::cerr << "HttpRequest() error: Unsupported method: " << method << std::endl;
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return false;
        }
        if (response_out) {
            response_out->clear();
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback2);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_out);
        }

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        std::cout << "HttpRequest() debug: " << method << " to " << url << " - HTTP code: " << http_code << std::endl;  // Debug
        /*
        if (response_out) {
            std::cout << "HttpRequest() debug: response body: " << *response_out << std::endl;
        }*/

        if (res == CURLE_OK && http_code >= 200 && http_code < 300) {
            success = true;
        } else if (res != CURLE_OK || http_code == 429 || http_code >= 500) {
            // Retryable error
            std::cerr << "HttpRequest() error: retryable error - CURL: " << res << " HTTP: " << http_code << std::endl;
            retry_count++;
            if (retry_count < max_retries) {
                std::this_thread::sleep_for(std::chrono::seconds(retry_count));  // Exponential backoff (1s, 2s, ...)
            }
        } else {
            // Non-retryable error (e.g., 4xx client errors)
            std::cerr << "HttpRequest() error: non-retryable error - CURL: " << res << " HTTP: " << http_code << std::endl;
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return false;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    return success;
}

// Function to create a challenge to another bot

bool CreateChallenge(const std::string& opponent, bool rated, int time_sec, int inc_sec, std::string& challengeId, const std::string& color = "random", const std::string& variant = "standard") {
    if (game_in_progress.load()) {
        std::cout << "CreateChallenge(): Skipping - game already in progress" << std::endl;
        return false;
    }
    std::string url = "https://lichess.org/api/challenge/" + opponent;
    std::stringstream fields;
    fields << "rated=" << (rated ? "true" : "false")
           << "&clock.limit=" << time_sec
           << "&clock.increment=" << inc_sec
           << "&color=" << color
           << "&variant=" << variant;
    std::string postfields = fields.str();

    std::string response;
    bool success = HttpRequest("POST", url, postfields, &response);
    if (success) {
      try {
        json data = json::parse(response);
        if (data.contains("id")) challengeId = data.value("id", "");
      } catch (const std::exception& e) {
          std::cerr << "CreateChallenge(): JSON parse error: " << e.what() << " - data: " << response << std::endl;
          return false;
      }
      if (challengeId.empty()) {
        std::cerr << "CreateChallenge(): challengeId is empty" << std::endl;
        return false;
      }
      std::cout << "CreateChallenge(): Challenge " <<  challengeId << " sent to " << opponent << " successfully" << std::endl;      
    } else {
      std::cerr << "CreateChallenge(): Failed to send challenge to " << opponent << std::endl;
    }
    return success;
}

// Function to get and process online bots, filter by ELO, and challenge a random one
void GetAndProcessBots(int nb) {
    const std::string bot_url = "https://lichess.org/api/bot/online";
    std::vector<Bot> bots;
    bots.reserve(nb);

    while (playing.load()) {      
      std::unique_lock<std::mutex> lock(playing_mutex);
      game_cv.wait(lock, [] { return !game_in_progress.load(); }); // Wait for the end of the game
      lock.unlock();
      if (!playing.load()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::seconds(5)); //just to give time to kill the bot is needed
      if (!game_in_progress.load()) {
          while (bots.empty()) {
              std::string query = bot_url + "?nb=" + std::to_string(nb);
              std::string response;
              HttpRequest("GET", query, "", &response, "application/x-ndjson", false);
              if (!response.empty()) {
                  std::istringstream iss(response);
                  std::string line;
                  while (std::getline(iss, line)) {
                      if (line.empty()) continue;
                      try {
                          json data = json::parse(line);
                          if (BOT_USERNAME == data.value("username", "")) continue; //don't challenge itself
                          if (data.contains("perfs") && data["perfs"].contains("blitz")) {
                              int rating = data["perfs"]["blitz"].value("rating", 0);
                              if (rating > MIN_ELO && rating < MAX_ELO) {
                                  Bot bot;
                                  bot.botname = data.value("username", "");
                                  bot.games = data["perfs"]["blitz"].value("games", 0);
                                  bot.elo = rating;
                                  bot.rd = data["perfs"]["blitz"].value("rd", 0);
                                  bot.prog = data["perfs"]["blitz"].value("prog", 0);
                                  bots.push_back(bot);
                                  std::cout << "Botname " << bot.botname << ". Elo " << bot.elo << " +/- " << bot.rd << " after " << bot.games << " games." << std::endl;
                              }
                          }
                      } catch (const std::exception& e) {
                          std::cerr << "GetAndProcessBots(): JSON parse error: " << e.what() << " - Line: " << line << std::endl;
                      }
                  }
              } else {
                  std::cout << "GetAndProcessBots() debug: no bots have been found" << std::endl;
              }  
              if (bots.empty()) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(10000)); //sleep for 10s and try to create a list of bots again
              }
          } //end of while (bots.empty)
          auto numberOfBots = bots.size();
          std::cout << "numberOfBots " << numberOfBots << std::endl;
          std::uniform_int_distribution<int> uniform(0, numberOfBots - 1);
          //randomly challenge a bot from the list
          bool res = false;
          std::unordered_set<int> tried_bots;
          while(playing.load() && (!challenge_accepted.load() || challenge_declined.load())) {
              if (challenge_declined.load()) challenge_declined.store(false); //reset challenge_declined
              int i = uniform(rng);
              if (tried_bots.empty()) tried_bots.emplace(i);
              else {
                if (tried_bots.contains(i)) {
                  if (tried_bots.size() == numberOfBots) {
                    bots.clear();
                    tried_bots.clear();
                    break;
                  } else continue;
                } else tried_bots.emplace(i);
              }
              std::string botname = bots[i].botname;
              std::cout << "Bot " << i << " name " << botname << ". Elo " << bots[i].elo << " +/- " << bots[i].rd << " after " << bots[i].games << " games." << std::endl; 
              std::string challengeId;
              res = CreateChallenge(botname, true, CLOCK_LIMIT, CLOCK_INCREMENT, challengeId);
              if (res) {
                while (!challenge_accepted.load() && !challenge_declined.load()) {
                  std::cout << "GetAndProcessBots() debug: challenge_accepted " << challenge_accepted.load() << ", challenge_declined " << challenge_declined.load() << std::endl;
                  std::unique_lock<std::mutex> lck(challenge_mutex);
                  using namespace std::chrono_literals;
                  int seconds = 5;
                  bool wait_result = challenge_cv.wait_for(lck, seconds*1000ms, [] { return challenge_accepted.load() || challenge_declined.load(); });
                  lck.unlock();
                  if (wait_result) {// Wait until challenge is accepted for 5 s, challenge would expire in 20s
                    if (challenge_accepted.load() || challenge_declined.load()) break;
                    else continue; //spurious exit in wait_for()
                  } else { //cancel the challenge if it was not declined and try another bot
                    //if (! challenge_declined.load()) { //perhaps, this check is not needed
                        std::string cancel_url = "https://lichess.org/api/challenge/" + challengeId + "/cancel";
                        if (HttpRequest("POST", cancel_url, "", nullptr, "application/json")) {
                          std::cout << "GetAndProcessBots() debug: our challenge " << challengeId << " was canceled successfully" << std::endl;
                        } else {
                          std::cout << "GetAndProcessBots() error: failed to cancel our challenge " << challengeId << std::endl;
                        }
                        break;
                    //}
                  }
                } //end of while (!challenge_accepted && !challenge_declined)
              } //end of if (res)
          } //end of while(playing && !challenge_accepted)
          bots.clear();
          tried_bots.clear();    
      } //end of if (!game_in_progress)
      else continue;
    } //end of while(playing)
}

// Updated StreamAndProcess for incremental processing
void StreamAndProcess(const std::string& url, std::function<void(const json&)> process_line) {
    CURL* curl = curl_easy_init();
    if (!curl) return;

    StreamState state;
    state.process_line = process_line;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
    headers = curl_slist_append(headers, "Accept: application/x-ndjson");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback); //WriteCallback() may be called multiple times
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state); //state is where received data is written, passed as last arg to WriteCallback

    // Keep connection alive longer for correspondence games
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 180L);  // 1min timeout

    CURLcode res = curl_easy_perform(curl); //this blocks until curl times out or receives some ndjson data 
    if (res != CURLE_OK) {
        std::cerr << "StreamAndProcess() error: " << curl_easy_strerror(res) << std::endl;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

// Helper to compute and post move if it's our turn
void ComputeAndPostMove(const std::string& game_id, const bool draw_offer, const bool our_turn, bool& is_pondering, const std::string& initial_fen, const std::string& moves, const long long wtime, const long long btime, const long long winc, const long long binc) {
    std::cout << "ComputeAndPostMove() debug: game state - status: " << gameStateStatus << ", moves: " << moves << std::endl;
    if (gameStateStatus != "started" && gameStateStatus != "created") {
      gameStateStatus = "";
      return;
    }
    if (gameStateStatus == "started") {
        if (is_pondering) {
          is_pondering = false;
          stop(creatica);
          if (getPV(creatica, evaluations, MULTI_PV)) {
            std::cerr << "ComputeAndPostMove() error: getPV(creatica, evaluations, MULTI_PV) returned non-zero code, restarting..." << std::endl;
            releaseChessEngine(creatica);
            creatica = initChessEngine(CREATICA_PATH, MOVETIME, DEPTH, HASH, THREADS, SYZYGY_PATH, MULTI_PV, false, false, ELO_CREATICA);
            if (!creatica) {
              std::cerr << "ComputeAndPostMove() error: failed to restart chessEngine" << std::endl;
              exit(-1);
            }
            //else fprintf(stderr, "initilized chess engine %s for creatica\n", creatica->id);
            setEngineOptions();
          }
        }
        strncpy(creatica->position, initial_fen.c_str(), MAX_FEN_STRING_LEN);
        strncpy(creatica->moves, moves.c_str(), MAX_UCI_MOVES_LEN);
        if (!position(creatica)) {
          fprintf(stderr, "ComputeAndPostMove() error: position() returned false, fen %s\n", creatica->position);
          exit(-1);
        }
        creatica->wtime = wtime;
        creatica->btime = btime;
        creatica->winc = winc;
        creatica->binc = binc;
        if (our_turn) {
            creatica->infinite = false;
try_again:  if (go(creatica, evaluations)) {
              std::cerr << "ComputeAndPostMove() error: go(creatica, evaluations) returned non-zero code, restarting..." << std::endl;
              releaseChessEngine(creatica);
              creatica = initChessEngine(CREATICA_PATH, MOVETIME, DEPTH, HASH, THREADS, SYZYGY_PATH, MULTI_PV, false, false, ELO_CREATICA);
              if (!creatica) {
                std::cerr << "ComputeAndPostMove() error: failed to restart chessEngine" << std::endl;
                exit(-1);
              }
              setEngineOptions();
              strncpy(creatica->position, initial_fen.c_str(), MAX_FEN_STRING_LEN);
              strncpy(creatica->moves, moves.c_str(), MAX_UCI_MOVES_LEN);
              if (!position(creatica)) {
                fprintf(stderr, "ComputeAndPostMove() error: position() returned false, fen %s\n", creatica->position);
                exit(-1);
              }
              creatica->wtime = wtime;
              creatica->btime = btime;
              creatica->winc = winc;
              creatica->binc = binc;
              goto try_again;
            }
            if (draw_offer) {
                if (evaluations[0]->scorecp < DRAW_CP) {
                  std::string draw_url = "https://lichess.org/api/bot/game/" + game_id + "/draw/yes";
                  if (!HttpRequest("POST", draw_url)) {
                    std::cerr << "ComputeAndPostMove() error: failed to post request to " << draw_url << std::endl;
                  } else return;            
                } else {
                  std::string draw_url = "https://lichess.org/api/bot/game/" + game_id + "/draw/no";
                  if (!HttpRequest("POST", draw_url))
                    std::cerr << "ComputeAndPostMove() error: failed to post request to " << draw_url << std::endl;
                }
            } //end of if (draw_offer)
            std::string new_move = evaluations[0]->bestmove;
            std::string move_url = "https://lichess.org/api/bot/game/" + game_id + "/move/" + new_move;
            if (!HttpRequest("POST", move_url))
              std::cerr << "ComputeAndPostMove() error: failed to post request to " << move_url << std::endl;
        } else { // Not our turn
              creatica->infinite = true;
              is_pondering = true;
              go(creatica, evaluations);
        }
    } //end of if (gameStateStatus == "started")
}

// Function to handle a single game
void HandleGame(const std::string& game_id) {
    newGame(creatica);
    bool is_white = false;  // To be set in gameFull
    bool is_pondering = false;
    std::string initial_fen;  // To be set in gameFull

    // Stream game state
    while (game_in_progress.load() && (gameStateStatus == "created" || gameStateStatus != "started")) {
        std::string stream_url = "https://lichess.org/api/bot/game/stream/" + game_id;
        StreamAndProcess(stream_url, [game_id, &is_white, &initial_fen, &is_pondering](const json& state) {
            if (state.contains("type") && state["type"] == "gameFull") {
                // Determine color (use value() for safety if key missing)
                gameStateStatus = state["state"].value("status", "");
                if (gameStateStatus != "created" && gameStateStatus != "started") return;
                std::string white_id = state["white"].value("id", "");
                std::string black_id = state["black"].value("id", "");
                is_white = (white_id == BOT_USERNAME);
                std::cout << "HandleGame() debug: our color " << (is_white ? "white" : "black") << std::endl;
    
                // Initial position from initialFen (not state.fen)
                initial_fen = state.value("initialFen", "startpos");
                std::cout << "HandleGame() debug: initial FEN " << initial_fen << std::endl;
                std::string moves = state["state"].value("moves", "");
                int num_plies = 0;
                std::string uci_move = "";
                std::istringstream uci_moves(moves);
                while (uci_moves >> uci_move) num_plies++;
                // Calculate turn
                bool white_turn = (num_plies % 2 == 0);
                bool our_turn = ((is_white && white_turn) || (!is_white && !white_turn));
                std::cout << "HandleGame() debug: turn check - num plies: " << num_plies << ", white turn: " << white_turn << ", our turn: " << our_turn << std::endl;
                long long wtime = state["state"].value("wtime", 0);
                long long btime = state["state"].value("btime", 0);
                long long winc = state["state"].value("winc", 0);
                long long binc = state["state"].value("binc", 0);
                
                bool wdraw = state["state"].value("wdraw", false);
                bool bdraw = state["state"].value("bdraw", false);
                bool draw_offer = is_white ? bdraw : wdraw;
                ComputeAndPostMove(game_id, draw_offer, our_turn, is_pondering, initial_fen, moves, wtime, btime, winc, binc);
              } else if (state.contains("type") && state["type"] == "gameState") {
                gameStateStatus = state.value("status", "");
                if (gameStateStatus != "created" && gameStateStatus != "started") return;                
                std::string moves = state.value("moves", "");
                int num_plies = 0;
                std::string uci_move = "";
                std::istringstream uci_moves(moves);
                while (uci_moves >> uci_move) num_plies++;
                // Calculate turn
                bool white_turn = (num_plies % 2 == 0);
                bool our_turn = ((is_white && white_turn) || (!is_white && !white_turn));
                std::cout << "HandleGame() debug: turn check - num plies: " << num_plies << ", white turn: " << white_turn << ", our turn: " << our_turn << std::endl;
                long long wtime = state.value("wtime", 0);
                long long btime = state.value("btime", 0);
                long long winc = state.value("winc", 0);
                long long binc = state.value("binc", 0);
                bool wdraw = state.value("wdraw", false);
                bool bdraw = state.value("bdraw", false);
                bool draw_offer = is_white ? bdraw : wdraw;
                ComputeAndPostMove(game_id, draw_offer, our_turn, is_pondering, initial_fen, moves, wtime, btime, winc, binc);
            } else if (state.contains("type") && state["type"] == "opponentGone") {
                bool gone = state["gone"];
                int claimWinInSeconds = state["claimWinInSeconds"];
                if (gone) {
                    std::cout << "HandleGame() debug: game " << game_id << " state: opponentGone. Victory can be claimed in " << claimWinInSeconds << " sec" << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    if (claimWinInSeconds == 0) {
                        std::string claim_victory_url = "https://lichess.org/api/bot/game/" + game_id + "/claim-victory";
                        if (HttpRequest("POST", claim_victory_url)) {
                            std::cout << "HandleGame() debug: victory was claimed successfully" << std::endl;
                            //game_in_progress.store(false);
                        } else {
                            std::cout << "HandleGame() error: failed to claim the victory" << std::endl;
                        }
                    }
                }
            }
        });
        std::cout << "HandleGame() debug: StreamAndProcess() returned. Reconnecting..." << std::endl;
    } //end of while (game_in_progress)
    gameStateStatus = "";
    stop(creatica);
}

// Example processor for event stream
void ProcessEvent(const json& event) {
    if (!playing.load()) return; //do not process any events if ctrl-c is pressed
    if (event.contains("type") && (event["type"] == "challenge" || event["type"] == "challengeCanceled" || event["type"] == "challengeDeclined")) {
        std::string type = event["type"];
        std::string challenge_id = event["challenge"]["id"];
        std::string challenger_id = event["challenge"]["challenger"]["id"];
        std::string variant = event["challenge"]["variant"]["key"];
        std::string speed = event["challenge"]["speed"];
        std::string status = event["challenge"]["status"];
        // Accept (customize logic, e.g., only standard variant)
        if (challenger_id == BOT_USERNAME) {
            std::cout << "ProcessEvent() debug: our challenge " << challenge_id << " status " << status << std::endl;
            if (status == "accepted") {
              {
                std::lock_guard<std::mutex> lk(challenge_mutex);
                challenge_accepted.store(true);
              }
              challenge_cv.notify_one();
            }
            else if (status == "declined") {
              {
                std::lock_guard<std::mutex> lk(challenge_mutex);
                challenge_declined.store(true);
              }
              challenge_cv.notify_one();
            }
            return; // This is our challenge
        }
        std::cout << "ProcessEvent() debug: received challenge " << challenge_id << " with status " << status << " from " << challenger_id << std::endl;
        if (status == "created") {
            if ((!game_in_progress.load() && variant == "standard" || variant == "chess960") &&
                (speed == "blitz" || speed == "rapid" || speed == "classical") && challenger_id != "poliakevitch") {
                std::string accept_url = "https://lichess.org/api/challenge/" + challenge_id + "/accept";
                if (HttpRequest("POST", accept_url)) {
                    std::cout << "ProcessEvent() debug: challenge accepted successfully" << std::endl;
                } else {
                    std::cout << "ProcessEvent() error: failed to accept challenge" << std::endl;
                }
            } else {
                std::string decline_url = "https://lichess.org/api/challenge/" + challenge_id + "/decline";
                std::string reason = "reason=";
                if (game_in_progress.load()) reason += "later";
                else if (variant != "standard" && variant != "chess960") reason += "variant";
                else if (speed != "blitz" && speed != "rapid" && speed != "classical") reason += "timeControl";
                else reason += "generic";
                if (HttpRequest("POST", decline_url, reason)) {
                    std::cout << "ProcessEvent() debug: challenge declined successfully with " << reason << std::endl;
                } else {
                    std::cout << "ProcessEvent() error: failed to decline challenge with " << reason << std::endl;
                }
            }
        }
    } else if (event.contains("type") && event["type"] == "gameStart") {
        std::string game_id = event["game"]["gameId"];
        std::unique_lock<std::mutex> lock(mutex);
        if (game_id == current_game_id) {
            std::cout << "ProcessEvent() debug: ignoring duplicate gameStart for ongoing game: " << game_id << std::endl;
            lock.unlock();
            return;  // Skip to avoid restarting engine
        }
        current_game_id = game_id;
        lock.unlock();
        std::cout << "ProcessEvent() debug: game " << game_id << " (" << event["game"]["fullId"] << ") started" << std::endl;
        if (!challenge_accepted.load()) {
          {  
            std::lock_guard<std::mutex> lk(challenge_mutex);
            challenge_accepted.store(true);
          }  
          challenge_cv.notify_one();
        }
        if (!game_in_progress.load()) {
          {
            std::lock_guard<std::mutex> lc(playing_mutex);
            game_in_progress.store(true);
          }
          game_cv.notify_one();
          std::thread play(HandleGame, game_id);
          play.detach();
        }
    } else if (event.contains("type") && event["type"] == "gameFinish") {
        std::string game_id = event["game"]["gameId"];
        std::cout << "ProcessEvent() debug: game " << game_id << " (" << event["game"]["fullId"] << ") finished with " << event["game"]["status"]["name"] << std::endl;
        std::unique_lock<std::mutex> lock(mutex);
        current_game_id = "";
        lock.unlock();
        {
          std::lock_guard<std::mutex> lk(playing_mutex);
          game_in_progress.store(false);
        }
        game_cv.notify_one();
        {
          std::lock_guard<std::mutex> lk(challenge_mutex);
          challenge_accepted.store(false);
        }
        challenge_cv.notify_one();
    }
}

void signal_handler(int sig) {
    std::cout << "Received Ctrl-C (SIGINT). Shutting down gracefully..." << std::endl;
    playing.store(false);
}

int main() {
  int multiPV = MULTI_PV;
  std::signal(SIGINT, signal_handler);  // Set up Ctrl-C handler
  init_magic_bitboards();
  rng.seed(static_cast<unsigned int>(std::random_device{}()));

  creatica = initChessEngine(CREATICA_PATH, MOVETIME, DEPTH, HASH, THREADS, SYZYGY_PATH, MULTI_PV, false, false, ELO_CREATICA);
  if (!creatica) {
    fprintf(stderr, "tournament main() error: failed to init chessEngine %s for creatica\n", CREATICA_PATH);
    return -1;
  }
  //else fprintf(stderr, "initilized chess engine %s for creatica\n", creatica->id);
  setEngineOptions();
  for (int i = 0; i < multiPV; i++) {
    evaluations[i] = new Evaluation;
    evaluations[i]->maxPlies = 1; //we just need the next move  
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::thread challenge(GetAndProcessBots, nb);
  while (playing.load()) {  // Main loop: Keep streaming events. Need to add keyboard CTR-C handling to break from this loop gracefully
      std::string event_url = "https://lichess.org/api/stream/event";
      StreamAndProcess(event_url, ProcessEvent);
      std::cerr << "main() debug: stream ended; reconnecting in 3s..." << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(3));
  }
  challenge.join();
  quit(creatica);
  releaseChessEngine(creatica);
  for (int i = 0; i < multiPV; i++) delete evaluations[i];
  curl_global_cleanup();
  cleanup_magic_bitboards();
  return 0;
}


