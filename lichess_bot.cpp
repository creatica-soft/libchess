//c++ -std=c++20 -O3 -flto -I /Users/ap/libchess -Wl,-lcurl lichess_bot.cpp -o lichess_bot

#include <functional>
#include <iostream>
#include <string>
#include <sstream>
#include <cstdio>
#include <thread>
#include <chrono>
#include <unordered_set>
#include <curl/curl.h>
#include <sys/select.h> // For non-blocking pipe reading
#include <unistd.h>     // For pipe file descriptors
#include <fcntl.h>
#include <random>       // For RNG in GetAndProcessBots
#include "json.hpp"     // https://github.com/nlohmann/json
#include "libchess.h"

#define ENGINE_PATH "/Users/ap/libchess/creatica"
#define BOT_USERNAME "creaticachessbot"  // Lowercase, as per API IDs
#define ESTIMATED_OVERALL_MOVES 100
#define DRAW_CP 30 //accept draw if score cp is less than this value in centipawns
#define MIN_ELO 1700
#define MAX_ELO 2000
#define CLOCK_LIMIT 180 //seconds
#define CLOCK_INCREMENT 3 //seconds
#define NUMBER_OF_BOTS 50 //number of online bots to return from the list

const std::string token = "fake_token"; // Replace with real token
std::string current_game_id = "";
bool game_in_progress = false;
bool reconnect = true;
int nb = NUMBER_OF_BOTS;
using json = nlohmann::json;
std::mt19937 rng;
std::string outgoing_challenge_id = "";
std::chrono::steady_clock::time_point challenge_sent_time;

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

// Callback for curl to write response data incrementally
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
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

// Function to send a command to engine without reading response (for setup)
void SendCommand(FILE* engine_pipe, const std::string& command) {
    fprintf(engine_pipe, "%s\n", command.c_str());
    fflush(engine_pipe);
    std::cout << "SendCommand() debug: " << command << std::endl;
}

// Function to wait for "readyok" after "isready"
/*
std::string WaitForReady(FILE* engine_pipe) {
    char buffer[1024];
    std::string response;
    while (fgets(buffer, sizeof(buffer), engine_pipe)) {
        response += buffer;
        if (response.find("readyok") != std::string::npos) break;
    }
    std::cout << "Engine response: " << response << std::endl;  // Debug
    return response;
}*/

bool WaitForReady(FILE* engine_pipe, int timeout_ms = 1000) {
    char buffer[1024];
    std::string response;
    int fd = fileno(engine_pipe);
    if (fd == -1) {
        std::cerr << "WaitForReady() error: failed to get pipe file descriptor" << std::endl;
        return false;
    }

    auto start = std::chrono::steady_clock::now();
    while (true) {
        fd_set read_fds; //file descriptor set
        FD_ZERO(&read_fds); //zero the set
        FD_SET(fd, &read_fds); //add engine's fd to the set
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;  // 100ms per check

        int ready = select(fd + 1, &read_fds, nullptr, nullptr, &tv); //returns the number of fds that are ready for read
        if (ready > 0) {
            if (fgets(buffer, sizeof(buffer), engine_pipe) == nullptr) {
                if (feof(engine_pipe)) {
                    std::cerr << "WaitForReady() error: EOF reached (engine crashed)" << std::endl;
                } else if (ferror(engine_pipe)) {
                    std::cerr << "WaitForReady() error: reading from the pipe" << std::endl;
                }
                return false;
            }
            response += buffer;
            if (response.find("readyok") != std::string::npos) {
                std::cout << "WaitForReady() debug: engine response: " << response << std::endl;
                return true;
            }
        } else if (ready == 0) {
            // Timeout on this iteration; check total time
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeout_ms) {
                std::cerr << "WaitForReady() error: timeout waiting for readyok" << std::endl;
                return false;
            }
        } else {
            std::cerr << "WaitForReady() error in select()" << std::endl;
            return false;
        }
    }
}

FILE* StartEngine() {
    FILE* engine_pipe = popen(ENGINE_PATH, "r+");
    if (!engine_pipe) {
        std::cerr << "StartEngine() error: failed to start engine" << std::endl;
        return nullptr;
    }
    // Initialize engine (same as in HandleGame)
    SendCommand(engine_pipe, "setoption name MultiPV value 1");
    SendCommand(engine_pipe, "setoption name PVPlies value 8");
    SendCommand(engine_pipe, "setoption name Hash value 2048");
    SendCommand(engine_pipe, "isready");
    if (!WaitForReady(engine_pipe)) {  // If initial readyok fails
        pclose(engine_pipe);
        return nullptr;
    }
    return engine_pipe;
}

// Function to clear pipe after "stop" if pondering, using non-blocking read
void ClearPipeIfPondering(FILE* engine_pipe, bool& is_pondering) {
    if (!is_pondering) {
        std::cout << "ClearPipeIfPondering() debug: not pondering; skipping pipe clear" << std::endl;
        return;
    }

    SendCommand(engine_pipe, "stop");
    is_pondering = false;

    // Get file descriptor for the pipe
    int fd = fileno(engine_pipe);
    if (fd == -1) {
        std::cerr << "ClearPipeIfPondering() error: failed to get pipe file descriptor" << std::endl;
        return;
    }
    //fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    char buffer[4096];
    std::string response;
    bool found_bestmove = false;
    // Loop for up to 1000ms total (10 iterations of 100ms)
    for (int iter = 0; iter < 11; ++iter) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000; // 100ms per iteration

        int ready = select(fd + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            std::cerr << "ClearPipeIfPondering() select(): no data; continue to next iteration" << std::endl;
            continue;
        }

        // Read all available data in this iteration
        while (fgets(buffer, sizeof(buffer), engine_pipe)) {
        //size_t bytes_read = 0;
        //while ((bytes_read = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
            //response.append(buffer, bytes_read);
            response += buffer;
            if (response.find("bestmove") != std::string::npos) {
                found_bestmove = true;
                break;
            }
        }
        if (found_bestmove) break;
    }
    if (feof(engine_pipe) || ferror(engine_pipe) || !found_bestmove) {
        std::cerr << "ClearPipeIfPondering() error: engine error or crash; restarting..." << std::endl;
        pclose(engine_pipe);
        engine_pipe = StartEngine();
        if (!engine_pipe) {
            std::cerr << "ClearPipeIfPondering() error: failed to restart engine" << std::endl;
            return;
        }
    }
    std::cout << "ClearPipeIfPondering() debug: cleared pipe after stop (bestmove found: " << found_bestmove << "): " << response << std::endl;
}

bool IsEngineAlive(FILE* engine_pipe, bool& is_pondering) {
    ClearPipeIfPondering(engine_pipe, is_pondering);  // Stop if pondering
    SendCommand(engine_pipe, "isready");
    return WaitForReady(engine_pipe);
}

// Function to get bestmove after "go"
std::string GetBestMove(FILE* engine_pipe, const std::string& go_cmd) {
    SendCommand(engine_pipe, go_cmd);
    char buffer[4096];
    std::string response;
    while (fgets(buffer, sizeof(buffer), engine_pipe)) {
        response += buffer;
        if (response.find("bestmove") != std::string::npos) break;
    }
    if (feof(engine_pipe) || ferror(engine_pipe) || response.find("bestmove") == std::string::npos) {
        std::cerr << "GetBestMove() error: engine error or crash; restarting..." << std::endl;
        pclose(engine_pipe);
        engine_pipe = StartEngine();
        if (!engine_pipe) {
            std::cerr << "GetBestMove() error: failed to restart engine" << std::endl;
            return "";
        }
    }
    std::cout << "GetBestMove() debug: " << response << std::endl;  // Debug
    return response;
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
    if (game_in_progress) {
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
        std::cout << "CreateChallenge(): Challenge sent to " << opponent << " successfully" << std::endl;
        //we need to wait here for acceptance or expiration of the challenge before returning success or failure
        //get challenge id from response to query for challenge status
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
        const std::string challenge_url = "https://lichess.org/api/challenge/" + challengeId + "/show";
        std::string status = "created";
        auto start = std::chrono::steady_clock::now();
        double elapsed = 0;
        while (elapsed < 20) { //challenges created without keepAliveStream=true expire in 20 seconds
          response.clear();
          success = HttpRequest("GET", challenge_url, "", &response, "application/json", true);
          if (success) {
              std::cout << "CreateChallenge(): request challenge status sent successfully" << std::endl;
              try {
                json data = json::parse(response);
                if (data.contains("status")) status = data.value("status", "");
              } catch (const std::exception& e) {
                  std::cerr << "CreateChallenge(): JSON parse error: " << e.what() << " - data: " << response << std::endl;
                  return false;
              }
              if (status == "accepted") {
                std::cout << "CreateChallenge(): challenge accepted" << std::endl;
                return true;
              } else if (status != "created") {
                std::cout << "CreateChallenge(): challenge status " << status << std::endl;
                return false;
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(1000));
              elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
          } else return false;
        }
        if (status != "accepted") return false;
    } else {
        std::cerr << "CreateChallenge(): Failed to send challenge to " << opponent << std::endl;
    }
    return success;
}
// Refactored CreateChallenge to be non-blocking
/*
bool CreateChallenge(const std::string& opponent, bool rated, int time_sec, int inc_sec, std::string& out_challenge_id, const std::string& color = "random", const std::string& variant = "standard") {
    if (game_in_progress) {
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
    std::string response;
    if (HttpRequest("POST", url, fields.str(), &response)) {
        try {
            json data = json::parse(response);
            if (data.contains("id")) { // Fallback for older API behavior
                out_challenge_id = data["id"];
                std::cout << "CreateChallenge(): Challenge sent to " << opponent << " with ID: " << out_challenge_id << std::endl;
                return true;
            } else {
                 std::cerr << "CreateChallenge(): Challenge response did not contain an ID. Response: " << response << std::endl;
                 return false;
            }
        } catch (const std::exception& e) {
            std::cerr << "CreateChallenge(): JSON parse error: " << e.what() << " - data: " << response << std::endl;
            return false;
        }
    }    
    std::cerr << "CreateChallenge(): Failed to send challenge to " << opponent << std::endl;
    return false;
}*/

// Function to get and process online bots, filter by ELO, and challenge a random one
bool GetAndProcessBots(int nb) {
    bool success = false;
    const std::string bot_url = "https://lichess.org/api/bot/online";
    std::vector<Bot> bots;
    bots.reserve(nb);

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
            std::this_thread::sleep_for(std::chrono::milliseconds(10000));
        }
    }
    auto numberOfBots = bots.size();
    std::cout << "numberOfBots " << numberOfBots << std::endl;
    std::uniform_int_distribution<int> uniform(0, numberOfBots - 1);
    //randomly challenge a bot from the list
    bool res = false;
    std::unordered_set<int> tried_bots;
    while(!res) {    
        int i = uniform(rng);
        if (tried_bots.empty()) tried_bots.emplace(i);
        else {
          if (tried_bots.contains(i)) {
            if (tried_bots.size() == numberOfBots) {
              bots.clear();
              tried_bots.clear();
              return false;
            } else continue;
          } else tried_bots.emplace(i);
        }
        std::string botname = bots[i].botname;
        std::cout << "Bot " << i << " name " << botname << ". Elo " << bots[i].elo << " +/- " << bots[i].rd << " after " << bots[i].games << " games." << std::endl; 
        //res = CreateChallenge(botname, true, 180, 3);        
        std::string new_challenge_id;
        res = CreateChallenge(botname, true, CLOCK_LIMIT, CLOCK_INCREMENT, new_challenge_id);
        if (res) {
          outgoing_challenge_id = new_challenge_id;
          challenge_sent_time = std::chrono::steady_clock::now();
        }
    }
    bots.clear();
    tried_bots.clear();
    return true;
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
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 180L);  // 5min timeout

    CURLcode res = curl_easy_perform(curl); //this blocks until curl times out or receives some ndjson data 
    if (res != CURLE_OK) {
        std::cerr << "StreamAndProcess() error: " << curl_easy_strerror(res) << std::endl;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

// Helper to compute and post move if it's our turn
void ComputeAndPostMove(FILE* engine_pipe, const std::string& game_id, std::string status, const int num_plies, const bool is_white, const long wtime, const long btime, const long winc, const long binc, const bool wdraw, const bool bdraw, bool& is_pondering, struct Board& board) {
    // Calculate turn
    bool white_turn = (num_plies % 2 == 0);
    bool our_turn = ((is_white && white_turn) || (!is_white && !white_turn));

    std::cout << "ComputeAndPostMove() debug: turn check - num plies: " << num_plies << ", white turn: " << white_turn << ", our turn: " << our_turn << std::endl;

    if (status == "started") {
        // Update position (always set before pondering or computing)
        const std::string fen(board.fen->fenString);
        std::string pos_cmd = (num_plies == 0 ? "position startpos" : "position fen " + fen);
        if (our_turn) {
            // Stop pondering if active, clear pipe, set position, compute move
            //ClearPipeIfPondering(engine_pipe, is_pondering);
            if (!IsEngineAlive(engine_pipe, is_pondering)) {
                std::cerr << "ComputeAndPostMove() error: engine unresponsive; restarting..." << std::endl;
                pclose(engine_pipe);
                engine_pipe = StartEngine();
                if (!engine_pipe) {
                    std::cerr << "ComputeAndPostMove() error: failed to restart engine; aborting game" << std::endl;
                    // Optionally post resign or handle error
                    return;
                }
            }
            SendCommand(engine_pipe, pos_cmd);
            bool draw_offer = is_white ? bdraw : wdraw;
            long remaining_time = is_white ? wtime : btime;
            long inc = is_white ? winc : binc;
            long movetime = 100L;  // Default emergency time
            if (remaining_time > 0) {
                remaining_time = static_cast<long>(remaining_time * 0.60);
                int estimated_moves_left = ESTIMATED_OVERALL_MOVES - num_plies;
                if (estimated_moves_left < 10) {
                  if (inc > 0)
                    estimated_moves_left = 10;
                  else estimated_moves_left = 20;
                } 
                long base_time = remaining_time * 0.60 / estimated_moves_left;
                movetime = base_time + static_cast<long>(inc * 0.5);
                // Clamp: Min 100ms, max 10% of remaining to avoid overthinking
                long max_time = remaining_time / 10L;
                if (movetime > max_time) movetime = max_time;
                if (movetime < 500L) movetime = 100L;
            }
            std::cout << "ComputeAndPostMove() debug: calculated movetime: " << movetime << "ms (remaining: " << remaining_time << ", inc: " << inc << ")" << std::endl;

            std::string go_cmd = "go movetime " + std::to_string(movetime);
            std::string response = GetBestMove(engine_pipe, go_cmd);
            if (draw_offer) {
 getScoreCP:             
              size_t pos = response.rfind("score cp ");
              if (pos != std::string::npos) {
                  std::string scorecp = response.substr(pos + 9);
                  scorecp = scorecp.substr(0, scorecp.find_first_of(" \n"));  // Trim to UCI move
                  if (stoi(scorecp) < DRAW_CP) {
                    std::string draw_url = "https://lichess.org/api/bot/game/" + game_id + "/draw/yes";
                    if (!HttpRequest("POST", draw_url)) {
                      std::cerr << "ComputeAndPostMove() error: failed to post request to " << draw_url << std::endl;
                    } else return;            
                  } else {
                    std::string draw_url = "https://lichess.org/api/bot/game/" + game_id + "/draw/no";
                    if (!HttpRequest("POST", draw_url))
                      std::cerr << "ComputeAndPostMove() error: failed to post request to " << draw_url << std::endl;
                  }
              } else {
                //resend position and go commands
                SendCommand(engine_pipe, pos_cmd);
                response = GetBestMove(engine_pipe, go_cmd);
                goto getScoreCP;
              }
            } //end of if (draw_offer)
getBestMove:
            size_t pos = response.find("bestmove ");
            if (pos != std::string::npos) {
                std::string move = response.substr(pos + 9);
                move = move.substr(0, move.find_first_of(" \n"));  // Trim to UCI move
                std::string move_url = "https://lichess.org/api/bot/game/" + game_id + "/move/" + move;
                if (!HttpRequest("POST", move_url))
                  std::cerr << "ComputeAndPostMove() error: failed to post request to " << move_url << std::endl;                
            } else {
                //resend position and go commands
                SendCommand(engine_pipe, pos_cmd);
                response = GetBestMove(engine_pipe, go_cmd);
                goto getBestMove;
            }
        } else { // Not our turn and piece number is more than 7
            // Set position and start pondering
            int pieceCount = bitCount(board.occupations[PieceNameAny]);
            if (pieceCount > 7) {
              SendCommand(engine_pipe, pos_cmd);
              SendCommand(engine_pipe, "go infinite");
              is_pondering = true;
            }
        }
    }
}

// Function to handle a single game
void HandleGame(const std::string& game_id) {
    game_in_progress = true;
    bool is_white = false;  // To be set in gameFull
    bool is_pondering = false;  // Track if engine is pondering
    std::string initial_fen;  // To be set in gameFull and used in all position commands
    struct Board board;
    struct Fen fen;
    int num_plies = 0;
    std::string uci_move = "";
    std::string prev_move = "";

    // Start engine
    FILE* engine_pipe = StartEngine();
    if (!engine_pipe) {
        std::cerr << "HandleGame() error: failed to start engine" << std::endl;
        game_in_progress = false;
        return;
    }
    init_magic_bitboards();

    // Stream game state
    std::string stream_url = "https://lichess.org/api/bot/game/stream/" + game_id;
    StreamAndProcess(stream_url, [engine_pipe, game_id, &is_white, &initial_fen, &is_pondering, &board, &fen, &num_plies, &uci_move, &prev_move](const json& state) {
        struct Move move;
        if (state.contains("type") && state["type"] == "gameFull") {
            // Determine color (use value() for safety if key missing)
            std::string white_id = state["white"].value("id", "");
            std::string black_id = state["black"].value("id", "");
            is_white = (white_id == BOT_USERNAME);
            std::cout << "HandleGame() debug: our color " << (is_white ? "white" : "black") << std::endl;

            // Initial position from initialFen (not state.fen)
            initial_fen = state.value("initialFen", "startpos");
            std::cout << "HandleGame() debug: initial FEN " << initial_fen << std::endl;
            if (initial_fen == "startpos") strtofen(&fen, startPos);
            else strtofen(&fen, initial_fen.c_str());
            std::cout << "fen " << fen.fenString << std::endl; 
            fentoboard(&fen, &board);
            std::cout << "HandleGame() debug: board fen " << board.fen->fenString << std::endl; 
            
            // Get moves and time controls from state["state"]
            std::string moves = state["state"].value("moves", "");
            std::string status = state["state"].value("status", "");
            long wtime = state["state"].value("wtime", 0L);
            long btime = state["state"].value("btime", 0L);
            long winc = state["state"].value("winc", 0L);
            long binc = state["state"].value("binc", 0L);
            bool wdraw = state["state"].value("wdraw", false);
            bool bdraw = state["state"].value("bdraw", false);
            std::cout << "HandleGame() debug: game state - status: " << status << ", moves: " << moves << std::endl;
            if (reconnect) { //first time and on reconnect, we need to process all moves
              std::istringstream uci_moves(moves);
              while (uci_moves >> uci_move) {                
                //std::cout << "uci_move " << uci_move << std::endl; 
                initMove(&move, &board, uci_move.c_str());
                //std::cout << "move uci_move " << move.uciMove << std::endl; 
                makeMove(&move);
                //std::cout << "fen after move " << uci_move << ": " << board.fen->fenString << std::endl;
                num_plies++;
                prev_move = uci_move;
              }
              reconnect = false;
            } else { //otherwise, only the last move
              size_t last_space = moves.find_last_of(' ');
              if (last_space == std::string::npos) uci_move = moves;
              else uci_move = moves.substr(last_space + 1);
              if (uci_move != "" && uci_move != prev_move) {
                //std::cout << "uci_move " << uci_move << std::endl; 
                initMove(&move, &board, uci_move.c_str());
                //std::cout << "move uci_move " << move.uciMove << std::endl; 
                makeMove(&move);
                //std::cout << "fen after move " << uci_move << ": " << board.fen->fenString << std::endl;
                num_plies++;
                prev_move = uci_move;
              }
            }
            //std::cout << "num_plies " << num_plies << std::endl;
            // Check if our turn and compute move (or ponder)
            ComputeAndPostMove(engine_pipe, game_id, status, num_plies, is_white, wtime, btime, winc, binc, wdraw, bdraw, is_pondering, board);
        } else if (state.contains("type") && state["type"] == "gameState") {
            std::string status = state.value("status", "");
            std::string moves = state.value("moves", "");
            long wtime = state.value("wtime", 0L);
            long btime = state.value("btime", 0L);
            long winc = state.value("winc", 0L);
            long binc = state.value("binc", 0L);
            bool wdraw = state.value("wdraw", false);
            bool bdraw = state.value("bdraw", false);
            std::cout << "HandleGame() debug: game state - status: " << status << ", moves: " << moves << std::endl;
            if (reconnect) { //first time and on reconnect, we need to process all moves
              std::istringstream uci_moves(moves);
              while (uci_moves >> uci_move) {
                //std::cout << "uci_move " << uci_move << std::endl; 
                initMove(&move, &board, uci_move.c_str());
                //std::cout << "move uci_move " << move.uciMove << std::endl; 
                makeMove(&move);
                //std::cout << "fen after move " << uci_move << ": " << board.fen->fenString << std::endl;
                num_plies++;
                prev_move = uci_move;
              }
              reconnect = false;
            } else { //otherwise, only the last move
              size_t last_space = moves.find_last_of(' ');
              if (last_space == std::string::npos) uci_move = moves;
              else uci_move = moves.substr(last_space + 1);
              if (uci_move != "" && uci_move != prev_move) {
                //std::cout << "uci_move " << uci_move << std::endl; 
                initMove(&move, &board, uci_move.c_str());
                //std::cout << "move uci_move " << move.uciMove << std::endl; 
                makeMove(&move);
                //std::cout << "fen after move " << uci_move << ": " << board.fen->fenString << std::endl;
                num_plies++;
                prev_move = uci_move;
              }
            }
            //std::cout << "num_plies " << num_plies << std::endl;
            // Check turn and compute move (or ponder)
            ComputeAndPostMove(engine_pipe, game_id, status, num_plies, is_white, wtime, btime, winc, binc, wdraw, bdraw, is_pondering, board);
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
                    } else {
                        std::cout << "HandleGame() error: failed to claim the victory" << std::endl;
                    }
                }
            }
        }
    });
    game_in_progress = false;
    current_game_id = "";
    SendCommand(engine_pipe, "stop");
    SendCommand(engine_pipe, "quit");
    pclose(engine_pipe);
    cleanup_magic_bitboards();
}

// Example processor for event stream
void ProcessEvent(const json& event) {
    if (event.contains("type") && (event["type"] == "challenge" || event["type"] == "challengeCanceled" || event["type"] == "challengeDeclined")) {
        std::string type = event["type"];
        std::string challenge_id = event["challenge"]["id"];
        std::string challenger_id = event["challenge"]["challenger"]["id"];
        std::string variant = event["challenge"]["variant"]["key"];
        std::string speed = event["challenge"]["speed"];
        std::string status = event["challenge"]["status"];
        // Accept (customize logic, e.g., only standard variant)
        if (challenger_id == BOT_USERNAME) {
            if (type == "challengeDeclined") { //let's try to challenge someone else
              if (!game_in_progress) GetAndProcessBots(nb);
            }
            std::cout << "ProcessEvent() debug: our challenge " << challenge_id << " status " << status << std::endl;
            return; // This is our challenge
        }
        std::cout << "ProcessEvent() debug: received challenge " << challenge_id << " with status " << status << " from " << challenger_id << std::endl;
        if ((variant == "standard" || variant == "chess960") &&
            (speed == "blitz" || speed == "rapid" || speed == "classical") && !game_in_progress && status == "created") {
            std::string accept_url = "https://lichess.org/api/challenge/" + challenge_id + "/accept";
            if (HttpRequest("POST", accept_url)) {
                std::cout << "ProcessEvent() debug: challenge accepted successfully" << std::endl;
            } else {
                std::cout << "ProcessEvent() error: failed to accept challenge" << std::endl;
            }
        } else if (status == "created") {
            std::string decline_url = "https://lichess.org/api/challenge/" + challenge_id + "/decline";
            std::string reason = "reason=";
            if (game_in_progress) reason += "later";
            else if (variant != "standard" && variant != "chess960") reason += "variant";
            else if (speed != "blitz" && speed != "rapid" && speed != "classical") reason += "timeControl";
            else reason += "generic";
            if (HttpRequest("POST", decline_url, reason)) {
                std::cout << "ProcessEvent() debug: challenge declined successfully" << std::endl;
            } else {
                std::cout << "ProcessEvent() error: failed to decline challenge" << std::endl;
            }
        }
    } else if (event.contains("type") && event["type"] == "gameStart") {
        std::string game_id = event["game"]["gameId"];
        if (game_id == current_game_id) {
            std::cout << "ProcessEvent() debug: ignoring duplicate gameStart for ongoing game: " << game_id << std::endl;
            return;  // Skip to avoid restarting engine
        }
        current_game_id = game_id;
        std::cout << "ProcessEvent() debug: game " << game_id << " (" << event["game"]["fullId"] << ") started" << std::endl;
        HandleGame(game_id);
    } else if (event.contains("type") && event["type"] == "gameFinish") {
        std::string game_id = event["game"]["gameId"];
        std::cout << "ProcessEvent() debug: game " << game_id << " (" << event["game"]["fullId"] << ") finished with " << event["game"]["status"]["name"] << std::endl;
        game_in_progress = false;
        current_game_id = "";
        //std::this_thread::sleep_for(std::chrono::seconds(3));
        //GetAndProcessBots(nb);
    }
}

int main() {
    rng.seed(static_cast<unsigned int>(std::random_device{}()));
    curl_global_init(CURL_GLOBAL_DEFAULT);
    //before calling GetAndProcessBots() need to check that there is no game in progress!
    GetAndProcessBots(nb);
    while (true) {  // Main loop: Keep streaming events
        std::string event_url = "https://lichess.org/api/stream/event";
        StreamAndProcess(event_url, ProcessEvent);
        std::cerr << "main() debug: stream ended; reconnecting in 1s..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
        if (!game_in_progress) GetAndProcessBots(nb);
    }
    curl_global_cleanup();
    return 0;
}


