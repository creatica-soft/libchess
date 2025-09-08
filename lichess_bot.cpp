//c++ -std=c++20 -O3 -flto -I /Users/ap/libchess -Wl,-lcurl lichess_bot.cpp -o lichess_bot

#include <functional>
#include <iostream>
#include <string>
#include <sstream>
#include <cstdio>
#include <thread>
#include <chrono>
#include <curl/curl.h>
#include <sys/select.h> // For non-blocking pipe reading
#include <unistd.h>     // For pipe file descriptors
#include "json.hpp"     // https://github.com/nlohmann/json

#define ENGINE_PATH "/Users/ap/libchess/creatica"
#define BOT_USERNAME "creaticachessbot"  // Lowercase, as per API IDs
#define ESTIMATED_OVERALL_MOVES 100
#define DRAW_CP 30 //accept draw if score cp is less than this value in centipawns

const std::string token = "faked_token"; // Replace with real token
bool game_in_progress = false;
using json = nlohmann::json;

std::string current_game_id = "";  // Track ongoing game to avoid duplicate HandleGame on reconnect

// Struct for WriteCallback state (to handle partial lines across calls)
struct StreamState {
    std::string partial_line;
    std::function<void(const json&)> process_line;
};

// Callback for curl to write response data incrementally
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    StreamState* state = static_cast<StreamState*>(userp);
    std::string data((char*)contents, size * nmemb);
    //std::cout << "Received chunk: " << data << std::endl;  // Debug: Log raw chunks

    size_t pos = 0;
    while ((pos = data.find('\n', pos)) != std::string::npos) {
        std::string line = state->partial_line + data.substr(0, pos);
        data.erase(0, pos + 1);
        state->partial_line.clear();

        if (!line.empty()) {
            try {
                json j = json::parse(line);
                std::cout << "Parsed line: " << j.dump() << std::endl;  // Debug: Log parsed JSON
                state->process_line(j);
            } catch (const std::exception& e) {
                std::cerr << "JSON parse error: " << e.what() << " - Line: " << line << std::endl;
            }
        } else {
            //std::cout << "Keep-alive empty line received" << std::endl;  // Debug
        }
    }
    state->partial_line += data;  // Save any remaining partial line
    return size * nmemb;
}

// Function to send a command to engine without reading response (for setup)
void SendCommand(FILE* engine_pipe, const std::string& command) {
    fprintf(engine_pipe, "%s\n", command.c_str());
    fflush(engine_pipe);
    std::cout << "Sent command: " << command << std::endl;  // Debug
}

// Function to wait for "readyok" after "isready"
std::string WaitForReady(FILE* engine_pipe) {
    char buffer[1024];
    std::string response;
    while (fgets(buffer, sizeof(buffer), engine_pipe)) {
        response += buffer;
        if (response.find("readyok") != std::string::npos) break;
    }
    std::cout << "Engine response: " << response << std::endl;  // Debug
    return response;
}

// Function to clear pipe after "stop" if pondering, using non-blocking read
void ClearPipeIfPondering(FILE* engine_pipe, bool& is_pondering) {
    if (!is_pondering) {
        std::cout << "Not pondering; skipping pipe clear" << std::endl;
        return;
    }

    SendCommand(engine_pipe, "stop");
    is_pondering = false;

    // Get file descriptor for the pipe
    int fd = fileno(engine_pipe);
    if (fd == -1) {
        std::cerr << "Failed to get pipe file descriptor" << std::endl;
        return;
    }

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
        timeout.tv_usec = 100000; // 100ms per iteration

        int ready = select(fd + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            // No data; continue to next iteration
            continue;
        }

        // Read all available data in this iteration
        while (fgets(buffer, sizeof(buffer), engine_pipe)) {
            response += buffer;
            if (response.find("bestmove") != std::string::npos) {
                found_bestmove = true;
                break;
            }
        }
        if (found_bestmove) break;
    }
    std::cout << "Cleared pipe after stop (bestmove found: " << found_bestmove << "): " << response << std::endl;  // Debug
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
    std::cout << "Bestmove response: " << response << std::endl;  // Debug
    return response;
}

// Function to perform a POST request and log response/code
bool PostRequest(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    //std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    //curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    //curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    std::cout << "POST to " << url << " - HTTP code: " << http_code << std::endl;  // Debug

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "Curl error: " << curl_easy_strerror(res) << std::endl;
        return false;
    }
    return http_code == 200;
}

// Updated StreamAndProcess for incremental processing
void StreamAndProcess(const std::string& url, const std::string& token, std::function<void(const json&)> process_line) {
    CURL* curl = curl_easy_init();
    if (!curl) return;

    StreamState state;
    state.process_line = process_line;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
    headers = curl_slist_append(headers, "Accept: application/x-ndjson");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);

    // Keep connection alive longer for correspondence games
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 3600L);  // 1 hour timeout

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "Stream error: " << curl_easy_strerror(res) << std::endl;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

// Helper to compute and post move if it's our turn
void ComputeAndPostMove(FILE* engine_pipe, const std::string& game_id, const std::string& status, const std::string& moves, bool is_white, long wtime, long btime, long winc, long binc, bool wdraw, bool bdraw, bool& is_pondering) {
    // Calculate turn
    std::istringstream iss(moves);
    std::string move_token;
    size_t num_moves = 0;
    while (iss >> move_token) ++num_moves;
    bool white_turn = (num_moves % 2 == 0);
    bool our_turn = ((is_white && white_turn) || (!is_white && !white_turn));

    std::cout << "Turn check - Num moves: " << num_moves << ", White turn: " << white_turn << ", Our turn: " << our_turn << std::endl;

    if (status == "started") {
        // Update position (always set before pondering or computing)
        std::string pos_cmd = (moves.empty() ? "position startpos" : "position startpos moves " + moves);
        if (our_turn) {
            // Stop pondering if active, clear pipe, set position, compute move
            ClearPipeIfPondering(engine_pipe, is_pondering);
            SendCommand(engine_pipe, pos_cmd);
            bool draw_offer = is_white ? bdraw : wdraw;
            long remaining_time = is_white ? wtime : btime;
            long inc = is_white ? winc : binc;
            long movetime = 100L;  // Default emergency time
            if (remaining_time > 0) {
                int estimated_moves_left = ESTIMATED_OVERALL_MOVES - num_moves;
                if (estimated_moves_left < 10) {
                  if (inc > 0)
                    estimated_moves_left = 5;
                  else estimated_moves_left = 10;
                } 
                long base_time = remaining_time / estimated_moves_left;
                movetime = base_time + static_cast<long>(inc * 0.8);
                // Clamp: Min 100ms, max 10% of remaining to avoid overthinking
                long max_time = remaining_time / 10L;
                if (movetime > max_time) movetime = max_time;
                if (movetime < 100L) movetime = 100L;
            }
            std::cout << "Calculated movetime: " << movetime << "ms (remaining: " << remaining_time << ", inc: " << inc << ")" << std::endl;

            std::string go_cmd = "go movetime " + std::to_string(movetime);
            std::string response = GetBestMove(engine_pipe, go_cmd);
            if (draw_offer) {
              size_t pos = response.rfind("score cp ");
              if (pos != std::string::npos) {
                  std::string scorecp = response.substr(pos + 9);
                  scorecp = scorecp.substr(0, scorecp.find_first_of(" \n"));  // Trim to UCI move
                  if (stoi(scorecp) < DRAW_CP) {
                    std::string draw_url = "https://lichess.org/api/bot/game/" + game_id + "/draw/yes";
                    if (!PostRequest(draw_url)) {
                      std::cerr << "Failed to post request to " << draw_url << std::endl;
                    } else return;            
                  } else {
                    std::string draw_url = "https://lichess.org/api/bot/game/" + game_id + "/draw/no";
                    if (!PostRequest(draw_url))
                      std::cerr << "Failed to post request to " << draw_url << std::endl;
                  }
              }
            }
            size_t pos = response.find("bestmove ");
            if (pos != std::string::npos) {
                std::string move = response.substr(pos + 9);
                move = move.substr(0, move.find_first_of(" \n"));  // Trim to UCI move
                std::string move_url = "https://lichess.org/api/bot/game/" + game_id + "/move/" + move;
                if (!PostRequest(move_url))
                  std::cerr << "Failed to post request to " << move_url << std::endl;                
            }
        } else { // Not our turn
            // Set position and start pondering
            SendCommand(engine_pipe, pos_cmd);
            SendCommand(engine_pipe, "go infinite");
            is_pondering = true;
        }
    }
}

// Function to handle a single game
void HandleGame(const std::string& game_id) {
    game_in_progress = true;
    bool is_white = false;  // To be set in gameFull
    bool is_pondering = false;  // Track if engine is pondering
    std::string initial_fen;  // To be set in gameFull and used in all position commands

    // Start engine
    FILE* engine_pipe = popen(ENGINE_PATH, "r+");
    if (!engine_pipe) {
        std::cerr << "Failed to start engine" << std::endl;
        game_in_progress = false;
        return;
    }
    //SendCommand(engine_pipe, "uci");
    SendCommand(engine_pipe, "setoption name MultiPV value 1");
    SendCommand(engine_pipe, "isready");
    WaitForReady(engine_pipe);

    // Stream game state
    std::string stream_url = "https://lichess.org/api/bot/game/stream/" + game_id;
    StreamAndProcess(stream_url, token, [engine_pipe, game_id, &is_white, &initial_fen, &is_pondering](const json& state) {
        if (state.contains("type") && state["type"] == "gameFull") {
            // Determine color (use value() for safety if key missing)
            std::string white_id = state["white"].value("id", "");
            std::string black_id = state["black"].value("id", "");
            is_white = (white_id == BOT_USERNAME);
            std::cout << "Our color " << (is_white ? "white" : "black") << std::endl;

            // Initial position from initialFen (not state.fen)
            initial_fen = state.value("initialFen", "startpos");
            std::cout << "Initial FEN " << initial_fen << std::endl;

            // Get moves and time controls from state["state"]
            std::string moves = state["state"].value("moves", "");
            std::string status = state["state"].value("status", "");
            long wtime = state["state"].value("wtime", 0L);
            long btime = state["state"].value("btime", 0L);
            long winc = state["state"].value("winc", 0L);
            long binc = state["state"].value("binc", 0L);
            bool wdraw = state["state"].value("wdraw", false);
            bool bdraw = state["state"].value("bdraw", false);

            // Check if our turn and compute move (or ponder)
            ComputeAndPostMove(engine_pipe, game_id, status, moves, is_white, wtime, btime, winc, binc, wdraw, bdraw, is_pondering);
        } else if (state.contains("type") && state["type"] == "gameState") {
            std::string status = state.value("status", "");
            std::string moves = state.value("moves", "");
            long wtime = state.value("wtime", 0L);
            long btime = state.value("btime", 0L);
            long winc = state.value("winc", 0L);
            long binc = state.value("binc", 0L);
            bool wdraw = state.value("wdraw", false);
            bool bdraw = state.value("bdraw", false);
            std::cout << "Game state - Status: " << status << ", Moves: " << moves << std::endl;

            // Check turn and compute move (or ponder)
            ComputeAndPostMove(engine_pipe, game_id, status, moves, is_white, wtime, btime, winc, binc, wdraw, bdraw, is_pondering);
        } else if (state.contains("type") && state["type"] == "opponentGone") {
            bool gone = state["gone"];
            int claimWinInSeconds = state["claimWinInSeconds"];
            if (gone) {
                std::cout << "Game " << game_id << " state: opponentGone. Victory can be claimed in " << claimWinInSeconds << " sec" << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (claimWinInSeconds == 0) {
                    std::string claim_victory_url = "https://lichess.org/api/bot/game/" + game_id + "/claim-victory";
                    if (PostRequest(claim_victory_url)) {
                        std::cout << "Victory was claimed successfully" << std::endl;
                    } else {
                        std::cout << "Failed to claim the victory" << std::endl;
                    }
                }
            }
        }
    });
    SendCommand(engine_pipe, "stop");
    SendCommand(engine_pipe, "quit");
    pclose(engine_pipe);
    game_in_progress = false;
}

// Example processor for event stream
void ProcessEvent(const json& event) {
    if (event.contains("type") && (event["type"] == "challenge" || event["type"] == "challengeCanceled" || event["type"] == "challengeDeclined")) {
        std::string challenge_id = event["challenge"]["id"];
        std::string challenger_id = event["challenge"]["challenger"]["id"];
        std::string status = event["challenge"]["status"];
        // Accept (customize logic, e.g., only standard variant)
        if (challenger_id == BOT_USERNAME) {
            std::cout << "Our challenge " << challenge_id << " status " << status << std::endl;
            return; // This is our challenge
        }
        std::cout << "Received challenge " << challenge_id << " with status " << status << " from " << challenger_id << std::endl;
        if ((event["challenge"]["variant"]["key"] == "standard" || event["challenge"]["variant"]["key"] == "chess960") &&
            ((event["challenge"]["speed"] == "blitz") || (event["challenge"]["speed"] == "rapid") || (event["challenge"]["speed"] == "classical")) && !game_in_progress && status == "created") {
            std::string accept_url = "https://lichess.org/api/challenge/" + challenge_id + "/accept";
            if (PostRequest(accept_url)) {
                std::cout << "Challenge accepted successfully" << std::endl;
            } else {
                std::cout << "Failed to accept challenge" << std::endl;
            }
        } else if (status == "created") {
            std::string decline_url = "https://lichess.org/api/challenge/" + challenge_id + "/decline";
            if (PostRequest(decline_url)) {
                std::cout << "Challenge declined successfully" << std::endl;
            } else {
                std::cout << "Failed to decline challenge" << std::endl;
            }
        }
    } else if (event.contains("type") && event["type"] == "gameStart") {
        std::string game_id = event["game"]["gameId"];
        if (game_id == current_game_id) {
            std::cout << "Ignoring duplicate gameStart for ongoing game: " << game_id << std::endl;
            return;  // Skip to avoid restarting engine
        }
        current_game_id = game_id;
        std::cout << "Game " << game_id << " (" << event["game"]["fullId"] << ") started" << std::endl;
        HandleGame(game_id);
    } else if (event.contains("type") && event["type"] == "gameFinish") {
        std::string game_id = event["game"]["gameId"];
        std::cout << "Game " << game_id << " (" << event["game"]["fullId"] << ") finished with " << event["game"]["status"]["name"] << std::endl;
        game_in_progress = false;
        current_game_id = "";
    }
}

int main() {
    std::string event_url = "https://lichess.org/api/stream/event";
    curl_global_init(CURL_GLOBAL_DEFAULT);
    while (true) {  // Main loop: Keep streaming events
        StreamAndProcess(event_url, token, ProcessEvent);
        std::cerr << "Stream ended; reconnecting..." << std::endl;
    }
    curl_global_cleanup();
    return 0;
}