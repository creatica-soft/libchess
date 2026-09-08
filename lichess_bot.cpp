// c++ -Wno-writable-strings -std=c++20 -O3 -flto -I /Users/ap/libchess -L /Users/ap/libchess -Wl,-lcurl,-lchess,-rpath,/Users/ap/libchess lichess_bot.cpp -o lichess_bot

#include <functional>
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <unordered_set>
#include <curl/curl.h>
#include <fcntl.h>
#include <csignal>
#include <random>       // For RNG in GetAndProcessBots
#include "json.hpp"     // https://github.com/nlohmann/json
#include "nnue/bitboard.h"
#include "libchess.h"

#define INTERMITTENT_INFO_LINES false
#define FINAL_INFO_LINES false
//The engine with the learned policy head blended into the move priors. Measured at
//13.0/20 against creatica-shared-root at 2 s/move -- about +108 Elo, p ~ 0.03 -- so the bot
//had been playing rated games with the weaker of the two engines available to it.
//
//NOTE: the engine loads its policy net from the RELATIVE path "nnue_policy.bin", so it must
//be started with /Users/ap/libchess as the working directory. If the net cannot be found it
//does not fail: it prints an info string and falls back to the eval-derived prior, which is
//sound but is exactly the ~108 Elo this change is meant to gain. Check the engine's startup
//line says "policy head nnue_policy.bin loaded".
#define CREATICA_PATH "/Users/ap/libchess/creatica"
#define DEPTH 0
#define MOVETIME 0
//Back to 2048 after a bad call on my part. MCTSNode did shrink 216 -> 56 bytes, but with
//edges the cost per node only went 535 -> 375, i.e. 1.43x cheaper - while dropping Hash
//from 2048 to 512 cut the budget 4x. Net effect was 4.01M nodes -> 1.43M, about a THIRD
//of the tree the engine had when it was rated 2300-2400. At 2048 the smaller node is a
//real gain instead: 5.72M nodes for the same memory the bot was already using.
#define HASH 2048
//4, not 8. Measured on this machine: 4 threads searched about 36% more nodes per second on
//four performance cores, and won a 24-game match 14-10. Both engine headers default to 4; this
//file was still overriding that with the value the measurement rejected, so the bot has been
//running the worse setting. It also halves the number of NNUE contexts, which is fixed memory.
#define THREADS 4
#define SYZYGY_PATH "/Users/ap/syzygy"
#define BOT_USERNAME "creaticachessbot"  // Lowercase, as per API IDs
#define DRAW_CP 30 //accept draw if score cp is less than this value in centipawns
#define MIN_ELO 2100
#define MAX_ELO 2800
#define ELO_CREATICA 2300
#define CLOCK_LIMIT 180 //seconds
#define CLOCK_INCREMENT 3 //seconds
#define NUMBER_OF_BOTS 50 //number of online bots to return from the list
#define MULTI_PV 1 //number of PVs
#define PV_PLIES 2 //number of plies in PV
#define EXPLORATION_MIN 65 // used in formular for exploration constant decay with depth
#define EXPLORATION_MAX 160 //smaller value favor exploitation, i.e. deeper tree vs wider tree
#define EXPLORATION_DEPTH_DECAY 5 //linear decay of EXPLORATION CONSTANT with depth using formula:
                      // C * 100 = max(EXPLORATION_MIN, (EXPLORATION_MAX - seldepth * EXPLORATION_DEPTH_DECAY))
//#define PROBABILITY_MASS 100 //% - cumulative probability - how many moves we consider
#define VIRTUAL_LOSS 36 //this is used primarily for performance in MT to avoid threads working on the same tree nodes
//EvalScale is NOT a UCI option on creatica -- it is a compile-time constant, EVAL_SCALE 61 in
//creatica_search.hpp, the same 61 this file used to try to send. Setting it here did nothing
//except print a warning at every engine start, so it is gone. Change it in the header if you
//ever want a different value, and remember it also has to match the model training constant
//(eval_scale = 600.0f, i.e. 600 centipawns) or the engine and the net are in different units.
                     //W is a fundamental value in Monte Carlo tree node along with N (number of visits) 
                     //and P (prior move probability), though P belongs to edges (same as move) but W and N to nodes.
#define TEMPERATURE 58 //used in calculating probabilities for moves in get_prob() using softmax:
                        // exp((eval - max_eval)/(temperature/100)) / eval_sum
                        //can be tuned so that values < 100 sharpen the distribution and values > 100 flatten it
                        //another words, the cooler the temperature, the more distant move probabilities, and vice versa
//Default ON, measured: 44/64 against the same engine with pondering off, about +137 Elo at
//5.2 standard errors, +27 =34 -3. It was off because before tree reuse existed a pondered
//tree was destroyed by cleanup() at the next search, so pondering burned the opponent's time
//for nothing -- hence the older "avoid using Ponder" note in the search. Reuse is what made it
//worth anything. Override per instance with CREATICA_PONDER=0.
#define PONDER true

//The API token is read from the environment so that it never lives in the source tree.
//Keep it outside the repository and export it before starting the bot, e.g.
//  export LICHESS_TOKEN="$(cat ~/.config/creatica/lichess_token)"
//Per-instance settings, overridable from the environment.
//
//Two bots can then run from one binary and differ only in what is being tested -- which is
//what a bot-vs-bot experiment needs, since the two must differ in exactly one thing. The
//token already worked this way; these follow it. The #defines above remain the defaults, so
//running the bot with no environment set behaves exactly as before.
//
//  LICHESS_TOKEN     the API token (already supported)
//  LICHESS_USERNAME  this account's name, lowercase
//  CREATICA_ENGINE   path to the engine binary
//  CREATICA_THREADS  search threads
//  CREATICA_HASH     MB for the MCTS tree
//  CREATICA_PONDER   1 or 0
static std::string env_str(const char * name, const char * dflt) {
  const char * v = std::getenv(name);
  return (v && *v) ? std::string(v) : std::string(dflt);
}
static int env_int(const char * name, int dflt) {
  const char * v = std::getenv(name);
  return (v && *v) ? (int)strtol(v, nullptr, 10) : dflt;
}
static bool env_bool(const char * name, bool dflt) {
  const char * v = std::getenv(name);
  if (!v || !*v) return dflt;
  return !(v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N');
}

static std::string load_lichess_token() {
    //Do NOT exit from here: this runs as a global initializer, before main(), so bailing
    //out made even --help impossible without a token. main() checks it after parsing args.
    const char * t = std::getenv("LICHESS_TOKEN");
    return (t && *t) ? std::string(t) : std::string();
}
const std::string token = load_lichess_token();
const std::string bot_username = env_str("LICHESS_USERNAME", BOT_USERNAME);
const std::string engine_path  = env_str("CREATICA_ENGINE",  CREATICA_PATH);
const int         bot_threads  = env_int("CREATICA_THREADS", THREADS);
const int         bot_hash     = env_int("CREATICA_HASH",    HASH);
const bool        bot_ponder   = env_bool("CREATICA_PONDER", PONDER);
//Time control of challenges this bot SENDS. Only the challenging side's values are used --
//the accepting side plays whatever it is offered -- so setting these on the --no-challenge
//instance has no effect.
const int         bot_clock     = env_int("CREATICA_CLOCK", CLOCK_LIMIT);
const int         bot_increment = env_int("CREATICA_INC",   CLOCK_INCREMENT);
//Variant this bot plays. "standard" or "chess960"; set it on BOTH bots for a 960 match,
//because it decides what is CHALLENGED and what is ACCEPTED. Chess960 was declined outright
//until the library was actually tested against Stockfish -- see perft_suite_960*.txt, which
//found three real defects, two in move generation/unmake and one in FEN parsing.
//
//For a 960 match no opening book is wanted or possible: lichess randomises the start position
//for every game, which gives more opening variety than a book does, and a book FEN would be
//sent as variant=fromPosition and override the 960 request anyway.
const std::string bot_variant = env_str("CREATICA_VARIANT", "standard");
const std::string book_path   = env_str("CREATICA_BOOK", "");
//Opponent rating band. Overridable because the right band depends on what the games are FOR.
//
//For measuring strength you want opponents near creatica's own rating -- games against much
//weaker or much stronger bots carry little information per game.
//
//For collecting TRAINING data the opposite is true: games against stronger opponents contain
//more of creatica's mistakes, and a mistake is the only thing a distillation target can teach.
//An easy win produces a long sequence of positions where the search agrees with the prior and
//there is nothing to learn.
const int         min_elo       = env_int("CREATICA_MIN_ELO", MIN_ELO);
const int         max_elo       = env_int("CREATICA_MAX_ELO", MAX_ELO);
//Per instance, so two bots running side by side do not interleave into one file.
std::mutex results_mutex;
const std::string results_path = env_str("CREATICA_RESULTS", ("results_" + bot_username + ".csv").c_str());
//Where the engine appends its root visit distribution after each search. Empty disables it.
//Per instance, so two bots do not interleave into one dataset.
const std::string visits_path = env_str("CREATICA_VISITS", "");

//--- opening book -----------------------------------------------------------------------------
//
//Two bots running the same engine and settings play very similar games from the start position,
//so a night of self-play revisits a handful of lines. That wastes the games twice: as an Elo
//measurement the effective sample is far smaller than the game count, and as training data the
//positions are mostly duplicates, each of which cost a full search to produce.
//
//CREATICA_BOOK points at the file make_book.py writes from eco.pgn: "fen<TAB>eco<TAB>name<TAB>plies".
//Only the FEN is used here.
//
//Each opening is played TWICE, colours swapped. Paired sampling like this removes whatever
//advantage the position itself carries, which is the largest single source of variance in a
//short match -- without it, an unbalanced opening dealt to one side is indistinguishable from
//that side being stronger.
std::vector<std::string> opening_book;
std::atomic<long> games_started{0};   //advanced only when a game actually STARTS
//The opponent of the first leg of a pair, so the second leg can go to the SAME bot.
//
//Without this, a randomly chosen opponent differs between the two legs, and the colour swap
//cancels nothing -- you have changed the position's owner AND the opponent at once. Pairing is
//only meaningful against a fixed opponent, which --challenge=<user> gives for free and random
//selection does not.
std::string pair_opponent;
std::mutex  pair_mutex;

static void load_opening_book(const std::string& path) {
  if (path.empty()) return;
  std::ifstream f(path);
  if (!f) { std::cerr << "opening book: cannot read " << path << std::endl; return; }
  std::string line;
  while (std::getline(f, line)) {
    const size_t tab = line.find('\t');
    std::string fen = (tab == std::string::npos) ? line : line.substr(0, tab);
    if (fen.size() > 10) opening_book.push_back(fen);
  }
  //Shuffled once, then walked in order. Picking at random each time would revisit some
  //openings and never reach others; walking a shuffled list covers the book evenly while still
  //differing between runs.
  std::srand((unsigned)time(nullptr));
  for (size_t i = opening_book.size(); i > 1; --i)
    std::swap(opening_book[i - 1], opening_book[(size_t)rand() % i]);
  std::cout << "opening book: " << opening_book.size() << " positions from " << path
            << " (shuffled; each played twice, colours swapped)" << std::endl;
}
#define RESULTS_FILE results_path.c_str()
std::string current_game_id = "";
std::atomic<bool> game_in_progress {false};
std::atomic<bool> challenge_accepted {false};
std::atomic<bool> challenge_declined {false};
//"Am I committed to a game?" has TWO parts and only one was tracked. game_in_progress
//covers a game that has started; this covers a challenge WE sent that has not yet been
//answered. Without it the bot could send a challenge, accept someone else's while
//waiting, then have its own accepted too - and it has resources for exactly one game,
//so the extra ones were abandoned and lost on time. Bounded by the 5 s wait plus the
//cancel below, so it cannot wedge the bot shut.
std::atomic<bool> challenge_outstanding {false};
//When that challenge was sent. lichess expires a challenge in ~20 s and we cancel after
//5 s, so anything older than this is certainly gone - whatever happened to the events we
//were waiting for. A flag guarding an asynchronous external event must not depend on
//having enumerated every clear path correctly: if one is ever missed the bot would stop
//challenging anyone, permanently and silently. This makes that failure self-heal.
#define CHALLENGE_OUTSTANDING_TIMEOUT_MS 30000
std::atomic<long long> challenge_sent_ms {0};
static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void markChallengeSent() {
    challenge_sent_ms.store(nowMs());
    challenge_outstanding.store(true);
}
static void clearChallengeOutstanding() {
    challenge_outstanding.store(false);
}
//True only while a challenge we sent is still plausibly live.
static bool challengeStillOutstanding() {
    if (!challenge_outstanding.load()) return false;
    const long long age = nowMs() - challenge_sent_ms.load();
    if (age > CHALLENGE_OUTSTANDING_TIMEOUT_MS) {
        std::cerr << "challengeStillOutstanding(): our challenge has been outstanding for "
                  << age << " ms with no accept, decline or cancel - clearing it" << std::endl;
        clearChallengeOutstanding();
        return false;
    }
    return true;
}
//Empty means "accept anyone" (subject to the variant and speed checks). Populated by
//--accept-only=<username>, which replaces the opponent names that used to be hardcoded
//into the accept condition and needed a rebuild to change.
std::unordered_set<std::string> accept_only;
//--challenge=<user> names the opponent to challenge, instead of picking a random bot from
//lichess's online list. --accept-only does NOT constrain this: it gates only INCOMING
//challenges, so a bot left free to challenge will go and play whoever it finds, which is not
//what you want when the point is to play one specific opponent under controlled conditions.
std::string challenge_target;
//--casual sends unrated challenges. Rated is right for measuring strength on the ladder;
//casual is right for an A/B between two of your own bots, which would otherwise drag both
//ratings around for a result that has nothing to do with the ladder.
bool challenge_rated = true;
static bool challengerAllowed(const std::string& challenger_id) {
    return accept_only.empty() || accept_only.count(challenger_id) > 0;
}
std::atomic<bool> playing {true};
std::mutex mutex;
std::mutex playing_mutex;
std::mutex challenge_mutex;
std::condition_variable game_cv, challenge_cv;
int nb = NUMBER_OF_BOTS;
using json = nlohmann::json;
std::mt19937 rng;
struct Engine creatica;
struct Evaluation * evaluations[MULTI_PV] = { nullptr };
//enum GameStateStatus { created, started, aborted, mate, resign, stalemate, timeout, draw, outoftime, cheat, noStart, unknownFinish, insufficientMaterialClaim, variantEnd };
enum GameStateStatus { unknown, created, started, aborted, mate, resign, stalemate, timeout, draw, outoftime, cheat, noStart, unknownFinish, insufficientMaterialClaim, variantEnd };
std::vector<std::string> gameSS = { "unknown", "created", "started", "aborted", "mate", "resign", "stalemate", "timeout", "draw", "outoftime", "cheat", "noStart", "unknownFinish", "insufficientMaterialClaim", "variantEnd" };
int game_states = 15;
//std::string gameStateStatus = "";
std::atomic<int> gameStateStatus {0};

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
    //When bytes last arrived, including a bare keep-alive newline. CURLOPT_LOW_SPEED_LIMIT
    //cannot express this: lichess sends roughly one newline every 6 s, i.e. ~0.17 bytes/s,
    //so ANY integer limit >= 1 aborts a perfectly healthy idle stream. Track liveness
    //ourselves instead and let the progress callback decide.
    std::chrono::steady_clock::time_point last_activity = std::chrono::steady_clock::now();
};

#define STREAM_SILENCE_TIMEOUT_S 60
//Aborts a transfer that has gone genuinely silent. curl calls this about once a second
//once CURLOPT_NOPROGRESS is off, and returning non-zero ends the transfer.
static int StreamProgress(void * clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    if (!playing.load()) return 1;
    StreamState * st = static_cast<StreamState *>(clientp);
    if (!st) return 0;
    auto quiet = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::steady_clock::now() - st->last_activity).count();
    if (quiet >= STREAM_SILENCE_TIMEOUT_S) {
        std::cerr << "StreamAndProcess() error: no data for " << quiet
                  << "s (keep-alive is ~6s) - treating the connection as dead" << std::endl;
        return 1;
    }
    return 0;
}

//Every option is set BY NAME. This used to index creatica.optionSpin[] with the
//EngineSpinOptions enum, but getOptions() fills that array in the order the engine advertises
//its options, so the enum addressed the right slot only by coincidence. EVAL_SCALE went to
//index 10, past the nine options creatica advertises, so setOptions() never sent
//it and the engine ran its own default; against creatica, which advertises fourteen, the same
//index is PolicyBlend and 61 would have set the blend to 0.61 instead of 0.45.
//
//A name the engine does not advertise is now reported rather than silently written elsewhere.
//Warnings are printed once, since setEngineOptions() is called again after every engine restart.
void setEngineOptions() {
	static bool complained = false;
	struct Spin { const char * name; int64_t value; };
	static const Spin spins[] = {
		{"MultiPV",               MULTI_PV},
		{"PVPlies",               PV_PLIES},
		{"ExplorationMin",        EXPLORATION_MIN},
		{"ExplorationMax",        EXPLORATION_MAX},
		{"ExplorationDepthDecay", EXPLORATION_DEPTH_DECAY},
		{"VirtualLoss",           VIRTUAL_LOSS},
		{"Temperature",           TEMPERATURE},
	};
	struct Check { const char * name; bool value; };
	static const Check checks[] = {
		{"FinalInfoLines",        FINAL_INFO_LINES},
		{"IntermittentInfoLines", INTERMITTENT_INFO_LINES},
		{"Ponder",                bot_ponder},
	};

	//setEngineSpin()/setEngineCheck() print their own warning naming the engine and the option.
	for (const Spin& o : spins)
		if (!setEngineSpin(creatica, o.name, o.value) && !complained)
			fprintf(stderr, "  (%s = %lld therefore has no effect)\n", o.name, (long long)o.value);
	if (!visits_path.empty()) setEngineStringOption(creatica, "VisitDumpFile", visits_path.c_str());
	for (const Check& o : checks)
		if (!setEngineCheck(creatica, o.name, o.value) && !complained)
			fprintf(stderr, "  (%s = %s therefore has no effect)\n", o.name, o.value ? "true" : "false");
	complained = true;
	setOptions(creatica);
}

// Callback for curl to write response data incrementally
size_t WriteCallback(void * contents, size_t size, size_t nmemb, void * userp) {
    if (!playing.load()) return 0; //exit streaming if ctrl-c is pressed - common way to abort streaming is to return 0
    StreamState * state = static_cast<StreamState *>(userp);
    state->last_activity = std::chrono::steady_clock::now(); //keep-alive newlines count
    std::string data((char *)contents, size * nmemb);
    //std::cout << "WriteCallback() debug: received chunk: " << data << std::endl;
    size_t pos = 0;
    //ndjson - new line delimited json
    while ((pos = data.find('\n', pos)) != std::string::npos) {
        std::string line = state->partial_line + data.substr(0, pos); //complete the line with a chunk that is terminated with '\n'
        data.erase(0, pos + 1); //delete this chunk once it is consumed
        state->partial_line.clear(); //empty partial_line

        if (!line.empty()) {
            try {
                int gss = gameStateStatus.load();
                json j = json::parse(line); //pass the complete line to json parse
                //std::cout << "WriteCallback() debug: parsed line: " << j.dump() << std::endl;
                state->process_line(j); //pass parsed json object to event or game state processing function
                if (game_in_progress.load() && gss != created && gss != started && gss != 0) {
                  std::cout << "WriteCallback() debug: gameStateStatus " << gameSS[gss] << std::endl;
                  gameStateStatus.store(0); //reset gameStateStatus
                  return 0;
                }
            } catch (const std::exception& e) {
                std::cerr << "WriteCallback() error: JSON parse error: " << e.what() << " - Line: " << line << std::endl;
            }
        } else {
            //lichess sends an empty line every 7 seconds to keep the connection alive
            //std::cout << "WriteCallback() debug: keep-alive empty line received" << std::endl;
        }
    } //end of while("a line is terminated with new line char '\n'")
    state->partial_line += data;  // Save any remaining partial line, not terminated with new line delimiter '\n'
    return size * nmemb;
}

// A classic C-style callback function that libcurl will call to write data.
static size_t WriteCallback2(void * contents, size_t size, size_t nmemb, void * userp) {
    // userp is the pointer to our std::string object.
    // Append the data received from libcurl to our string.
    size_t total_size = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char *>(contents), total_size);
    return total_size;
}

// Combined function for HTTP requests (GET or POST) with retries
bool HttpRequest(const std::string& method, const std::string& url, const std::string& postfields = "", std::string * response_out = nullptr, const std::string& accept = "", bool auth_required = true) {
    const int max_retries = 5;
    int retry_count = 0;
    bool success = false;

    while (!success && retry_count <= max_retries) {
        CURL * curl = curl_easy_init();
        if (!curl) {
          std::cerr << "HttpRequest() error: curl_easy_init() failed. " << method << " to " << url << std::endl;
          break;
        }
        std::string auth_header;
        std::string accept_header;
        struct curl_slist * headers = nullptr;
        char errbuf[CURL_ERROR_SIZE] = "";
        if (auth_required) {
            auth_header = "Authorization: Bearer " + token;
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        if (!accept.empty()) {
            accept_header = "Accept: " + accept; 
            headers = curl_slist_append(headers, accept_header.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        //Without these a half-open connection blocks this request forever - and a move
        //POST that never returns means the move is never sent and the game flags.
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); //curl is used from several threads here
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 65536L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
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

        long http_code = 0;
        CURLcode res = curl_easy_perform(curl);
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
            retry_count++;
            std::cerr << "HttpRequest() error: retryable error: " << curl_easy_strerror(res) << ": " << errbuf << " HTTP: " << http_code << "Retry count " << retry_count << ". Max retries " << max_retries << std::endl;
            if (retry_count <= max_retries) {
                std::this_thread::sleep_for(std::chrono::seconds(retry_count));  // Linear delay (1s, 2s, 3s...)
            }
        } else {
            // Non-retryable error (e.g., 4xx client errors)
            std::cerr << "HttpRequest() error: non-retryable error: " << curl_easy_strerror(res) << ": " << errbuf << " HTTP: " << http_code << std::endl;
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return false;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    } //end of while (!success && retry_count <= max_retries)

    return success;
}

// Function to create a challenge to another bot

bool CreateChallenge(const std::string& opponent, bool rated, int time_sec, int inc_sec, std::string& challengeId, const std::string& color = "random", const std::string& variant = "standard", bool dry_run = false) {
    if (!dry_run && game_in_progress.load()) {
        std::cout << "CreateChallenge(): Skipping - game already in progress" << std::endl;
        return false;
    }
    if (!dry_run && challengeStillOutstanding()) {
        std::cout << "CreateChallenge(): Skipping - a challenge we sent "
                  << (nowMs() - challenge_sent_ms.load()) << " ms ago is still outstanding" << std::endl;
        return false;
    }
    std::string url = "https://lichess.org/api/challenge/" + opponent;
    std::stringstream fields;
    //Pick the opening and the colour together. Each book position is played TWICE with the
    //colours swapped, so the position's own bias cancels instead of being credited to whichever
    //side happened to receive it -- the largest single source of variance in a short match.
    std::string use_colour = color, use_fen;
    //Alternate colours even with no book. The book path already does this, because playing each
    //position twice with the colours swapped cancels its bias; with lichess randomising the 960
    //start position there is no position to pair up, but the COLOUR bias is still worth removing
    //from an overnight match, and alternating costs nothing.
    if ((opening_book.empty() || bot_variant != "standard") && color == "random")
      use_colour = (games_started.load(std::memory_order_relaxed) % 2 == 0) ? "white" : "black";
    //A book FEN goes out as variant=fromPosition, which would SILENTLY OVERRIDE a chess960
    //request: the challenge would become a standard game from a standard book position. In
    //960 a book is meaningless anyway, because lichess randomises the start position itself.
    if (!opening_book.empty() && bot_variant == "standard") {
      //Derived from the number of games STARTED, not from a counter this function advances.
      //
      //An earlier version flipped a leg counter on every challenge SENT, so a challenge that was
      //declined, expired or failed silently consumed a leg -- after which the pairing was
      //desynchronised (opening X as White, opening Y as Black) with nothing to show it. Deriving
      //both values from a counter that only moves when a game really begins makes a failed
      //challenge cost nothing: the next attempt asks for exactly the same position and colour.
      const long g = games_started.load(std::memory_order_relaxed);
      use_fen    = opening_book[(size_t)((g / 2) % (long)opening_book.size())];
      use_colour = (g % 2 == 0) ? "white" : "black";
    }

    fields << "rated=" << (rated ? "true" : "false")
           << "&clock.limit=" << time_sec
           << "&clock.increment=" << inc_sec
           << "&color=" << use_colour;
    if (!use_fen.empty()) {
      //A FEN contains spaces and slashes, so it must be percent-encoded or the POST body is
      //truncated at the first space and lichess rejects the challenge.
      char * esc = curl_easy_escape(nullptr, use_fen.c_str(), 0);
      fields << "&variant=fromPosition&fen=" << (esc ? esc : "");
      if (esc) curl_free(esc);
    } else {
      fields << "&variant=" << variant;
    }
    std::string postfields = fields.str();
    //Logged because a challenge that goes out as the wrong variant is otherwise invisible until
    //the games have already been played: the FEN branch above sets variant=fromPosition, and a
    //silently overridden 960 request would just look like a normal match.
    std::cout << "CreateChallenge(): POST " << url << " " << postfields << std::endl;
    //--print-challenge stops here: the body is what a misconfiguration corrupts, and finding
    //that out from the log after a night of games is expensive.
    if (dry_run) return true;

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
      markChallengeSent(); //committed until accepted, declined, cancelled or stale
      std::cout << "CreateChallenge(): Challenge " <<  challengeId << " sent to " << opponent << " successfully" << std::endl;      
    } else {
      std::string error;
      try {
        json data = json::parse(response);
        if (data.contains("error")) error = data.value("error", "");
      } catch (const std::exception& e) {
          std::cerr << "CreateChallenge(): JSON parse error: " << e.what() << " - data: " << response << std::endl;
          return false;
      }
      std::cerr << "CreateChallenge(): Failed to send challenge to " << opponent << "Error: " << error << std::endl;
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
      std::this_thread::sleep_for(std::chrono::seconds(15)); //just to give time to kill the bot if needed
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
                          if (bot_username == data.value("username", "")) continue; //don't challenge itself
                          if (data.contains("perfs") && data["perfs"].contains("blitz")) {
                              int rating = data["perfs"]["blitz"].value("rating", 0);
                              if ((rating > min_elo && rating < max_elo) || data.value("username", "") == "creaticachessbot2") {
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
          //A named opponent replaces the list entirely, so the retry loop below is unchanged --
          //it just has one candidate to try.
          if (!challenge_target.empty()) {
            bots.clear();
            Bot only{};
            only.botname = challenge_target;
            bots.push_back(only);
          }
          if (bots.empty()) continue;
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
              //Second leg of a pair: re-challenge whoever played the first, so the only thing
              //that changed between the two games is which side of the position we hold.
              if (!opening_book.empty()) {
                std::lock_guard<std::mutex> lk(pair_mutex);
                const long g = games_started.load(std::memory_order_relaxed);
                if ((g % 2) == 1 && !pair_opponent.empty()) botname = pair_opponent;
                else pair_opponent = botname;
              }
              std::cout << "Bot " << i << " name " << botname << ". Elo " << bots[i].elo << " +/- " << bots[i].rd << " after " << bots[i].games << " games." << std::endl; 
              std::string challengeId;
              res = CreateChallenge(botname, challenge_rated, bot_clock, bot_increment, challengeId,
                                    "random", bot_variant);
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
                        clearChallengeOutstanding(); //cancelled - free to accept again
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
    CURL * curl = curl_easy_init();
    if (!curl) return;
    char errbuf[CURL_ERROR_SIZE] = "";

    StreamState state;
    state.process_line = process_line;
    struct curl_slist * headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
    headers = curl_slist_append(headers, "Accept: application/x-ndjson");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback); //WriteCallback() may be called multiple times
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state); // state is where received data is processed
                                                       // it is passed as last arg to WriteCallback()

    // Keep connection alive longer for correspondence games - this may not be needed - we terminate curl_easy_perform() with
    // WriteCallback() returning 0 when we press Ctrl-C, which sets playng to false. 
    // WriteCallback() is called at least every 7 seconds with empty line as a keep alive if no other events occur
    //These were commented out, so a silently dead socket (sleep/wake, Wi-Fi handover, a
    //NAT table eviction) left curl_easy_perform blocked indefinitely - macOS only starts
    //probing after 2 hours - and the bot never saw another event. The comment above says
    //a keep-alive newline arrives at least every 7 s, so 30 s of silence means it is dead.
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, StreamProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl); //this blocks until curl times out or errors out
    if (res != CURLE_OK) {
        std::cerr << "StreamAndProcess() error: " << curl_easy_strerror(res) << ": " << errbuf << std::endl;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

// Helper to compute and post move if it's our turn
//Side to move from a FEN: the second space-separated field, "w" or "b".
static bool fenSideToMoveIsWhite(const std::string& fen) {
  const size_t sp = fen.find(' ');
  if (sp == std::string::npos || sp + 1 >= fen.size()) return true;   //malformed: assume white
  return fen[sp + 1] != 'b';
}

static const char * startPosFen =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

//Any legal move, for the case where the engine gave us nothing usable. Not a good move -- a
//legal one. The alternative is to post nothing and let the clock run out, which loses the game
//with certainty rather than merely probably.
//
//Standard generation loop from the library: king moves first (they are always legal once
//kingMoves() has filtered them), then the rest under the check mask.
static bool firstLegalMove(Board& board, char * out) {
  auto [king_moves, pinned, pinning, checkers, kingSquare] = kingMoves(board);
  if (king_moves) {
    const Square to = (Square)lsBit(king_moves);
    std::snprintf(out, 6, "%s%s", square[kingSquare], square[to]);
    return true;
  }
  if (bitCount(checkers) > 1) return false;          // double check: only the king may move
  auto [check_mask, ep_mask] = checkers ? checkMask(board, kingSquare, checkers)
                                        : std::make_pair(0xffffffffffffffffULL, 0ULL);
  for (PieceType pt = Queen; pt >= Pawn; --pt) {
    uint64_t occ = board.side[board.sideToMove] & board.pieceTypes[pt - 1];
    while (occ) {
      const Square from = (Square)popLSB(occ);
      uint64_t mv = piece_moves(board, pt, from, kingSquare, pinned, pinning, check_mask, ep_mask);
      if (!mv) continue;
      const Square to = (Square)lsBit(mv);
      Move probe; probe.src = from; probe.dst = to; probe.promoType = PieceTypeNone;
      //A pawn reaching the last rank must carry a promotion letter or the move is not legal
      //notation; queen is as good as any when we are only trying to stay in the game.
      const bool promo = (pt == Pawn) && promoMove(board, probe);
      std::snprintf(out, 6, "%s%s%s", square[from], square[to], promo ? "q" : "");
      return true;
    }
  }
  return false;
}

void ComputeAndPostMove(const std::string& game_id, const bool draw_offer, const bool our_turn, const std::string& initial_fen, const std::string& moves, const long long wtime, const long long btime, const long long winc, const long long binc) {
    int gss = gameStateStatus.load();
    std::cout << "ComputeAndPostMove() debug: game state - status: " << gameSS[gss] << ", moves: " << moves << std::endl;
    if (gss != started && gss != created) {
      std::cout << "ComputeAndPostMove() warning: game state - status: " << gameSS[gss] << ", returning..." << std::endl;      
      gameStateStatus.store(0);
      return;
    }
    if (gss == started) {
        if (creatica.ponder) {
          creatica.infinite = false;
          creatica.ponder = false;
          //std::cout << "ComputeAndPostMove() debug: stopping pondering..." << std::endl; 
          stop(creatica);
          //std::cout << "ComputeAndPostMove() debug: and getting PV..." << std::endl; 
          if (getPV(creatica, evaluations, MULTI_PV)) {
            std::cerr << "ComputeAndPostMove() error: getPV(creatica, evaluations, MULTI_PV) returned non-zero code, restarting..." << std::endl;
            releaseChessEngine(creatica);
            //exit(-1); //temp exit for debugging
            initChessEngine(creatica, engine_path.c_str(), MOVETIME, DEPTH, bot_hash, bot_threads, SYZYGY_PATH, MULTI_PV, false, false, ELO_CREATICA);
            setEngineOptions();
          } //end of if (getPV())
        } //end of if (ponder)
        //Whether a predicted reply was actually appended to the move list below.
        //
        //The engine DEFERS the last move of a "go ponder" position: it ponders the position
        //BEFORE it, building a tree over all of the opponent's replies rather than betting on
        //one prediction. That only works when the move it defers really is the predicted reply.
        //With no ponder move to append, the move it defers is OUR OWN, so it would ponder the
        //position before we moved -- wrong side to move, and an entire search wasted on a tree
        //the next search cannot use.
        bool ponder_move_sent = false;
        //std::cout << "ComputeAndPostMove() debug: and getting PV..." << std::endl; 
        strncpy(creatica.position, initial_fen.c_str(), MAX_FEN_STRING_LEN);
        if (!moves.empty()) {
          strncpy(creatica.moves, moves.c_str(), MAX_UCI_MOVES_LEN);
          //uci GUIs use last move as a ponder move for "go ponder" command
          //lichess last move in position command has already been played, hence we need to play it, 
          //otherwise, the engine will ponder on it!
          //actually, it is easier to just append our ponder move to lichess moves - the engine does not care what it is anyway
          if (bot_ponder && !our_turn && strcmp(evaluations[0]->ponder, "") != 0) {
            strcat(creatica.moves, " ");
            strcat(creatica.moves, evaluations[0]->ponder);
            ponder_move_sent = true;
          }
        } else creatica.moves[0] = '\0';
int pos_retries = 0;
try_pos: if (!position(creatica)) {
          fprintf(stderr, "ComputeAndPostMove() error: position() returned false, fen %s\n", creatica.position);
          //position() returns isReady(), which is false only when the pipe hits EOF -
          //i.e. the engine child has died. exit(-1) here killed the WHOLE bot from a
          //detached thread inside a curl callback: the rated game was forfeited, no
          //'quit' was sent and neither fifo was removed - which is how /tmp filled with
          //orphaned pipes. It also made the restart code immediately below unreachable.
          if (++pos_retries > 3) {
            fprintf(stderr, "ComputeAndPostMove() error: engine did not come back after %d attempts; abandoning this move\n", pos_retries);
            return;
          }
          initChessEngine(creatica, engine_path.c_str(), MOVETIME, DEPTH, bot_hash, bot_threads, SYZYGY_PATH, MULTI_PV, false, false, ELO_CREATICA);
          setEngineOptions();
          strncpy(creatica.position, initial_fen.c_str(), MAX_FEN_STRING_LEN);
          goto try_pos;
        }
        int numberOfPieces = pieces(creatica);
        creatica.wtime = wtime;
        creatica.btime = btime;
        creatica.winc = winc;
        creatica.binc = binc;
        if (our_turn) {
try_again:  if (go(creatica, evaluations)) {
              std::cerr << "ComputeAndPostMove() error: go(creatica, evaluations) returned non-zero code, restarting..." << std::endl;
              releaseChessEngine(creatica);
              initChessEngine(creatica, engine_path.c_str(), MOVETIME, DEPTH, bot_hash, bot_threads, SYZYGY_PATH, MULTI_PV, false, false, ELO_CREATICA);
              setEngineOptions();
              strncpy(creatica.position, initial_fen.c_str(), MAX_FEN_STRING_LEN);
              strncpy(creatica.moves, moves.c_str(), MAX_UCI_MOVES_LEN);
              if (!position(creatica)) {
                fprintf(stderr, "ComputeAndPostMove() error: position() returned false after restart, fen %s\n", creatica.position);
                return; //lose this move, not the whole bot
              }
              creatica.wtime = wtime;
              creatica.btime = btime;
              creatica.winc = winc;
              creatica.binc = binc;
              goto try_again;
            } //end of if (go())
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

            //Never POST an empty or malformed move.
            //
            //When go() fails -- the engine hung and engineFgets() declared it dead, say --
            //bestmove can be empty or stale, and the URL then ends in "/move/" with nothing after
            //it. Lichess answers 404 "No such command", the bot logs a failure and carries on as
            //though it had moved, and the clock keeps running until it flags. Falling back to any
            //LEGAL move is strictly better than that: a poor move loses a game, a lost connection
            //to our own clock loses it anyway and with no chance of the opponent erring.
            auto looks_like_uci = [](const std::string& m) {
              if (m.size() < 4 || m.size() > 5) return false;
              if (m[0] < 'a' || m[0] > 'h' || m[2] < 'a' || m[2] > 'h') return false;
              if (m[1] < '1' || m[1] > '8' || m[3] < '1' || m[3] > '8') return false;
              if (m.size() == 5 && !strchr("qrbn", m[4])) return false;
              return true;
            };
            if (!looks_like_uci(new_move)) {
              std::cerr << "ComputeAndPostMove() error: engine returned no usable move ('"
                        << new_move << "'); falling back to a legal one" << std::endl;
              Board b = Board{};
              if (fen2board(b, initial_fen == "startpos" ? startPosFen : initial_fen.c_str()) == 0) {
                //Replay the game's moves onto the board, then take any legal move.
                std::istringstream ms(moves);
                std::string mv;
                while (ms >> mv) {
                  Move m2;
                  uci2move_idx(mv.c_str(), m2);
                  StateInfo st = {};
                  do_move(b, m2, st);
                }
                char cand[6] = "";
                if (firstLegalMove(b, cand)) new_move = cand;
              }
              if (!looks_like_uci(new_move)) {
                std::cerr << "ComputeAndPostMove() error: no legal fallback either; not posting"
                          << std::endl;
                return;   //better to post nothing than to post nonsense
              }
            }

            std::string move_url = "https://lichess.org/api/bot/game/" + game_id + "/move/" + new_move;
            std::cout << "ComputeAndPostMove() debug: submitting the move " << new_move << "..." << std::endl;
            if (!HttpRequest("POST", move_url))
              std::cerr << "ComputeAndPostMove() error: failed to post request to " << move_url << std::endl;
            std::cout << "ComputeAndPostMove() debug: submitting the move " << new_move << "... done" << std::endl;
        } else { // Not our turn
            if (numberOfPieces > 7 && bot_ponder && ponder_move_sent) {
              creatica.infinite = true;
              creatica.ponder = true;
              go(creatica, evaluations);
            }
        }
    } //end of if (gameStateStatus == started)
}

// Function to handle a single game in a detached thread
void HandleGame(const std::string& game_id) {
    newGame(creatica);
    bool is_white = false;  // To be set in gameFull
    std::string initial_fen;  // To be set in gameFull
    creatica.ponder = false;
    creatica.infinite = false;
    
    // Stream game state - game-specific stream, which is separate from the main events one
    // It has its own ProcessEvent() lambda function used by libcurl once the game state (lines) is parsed by nlohmann json library into json state object 
    std::string stream_url = "https://lichess.org/api/bot/game/stream/" + game_id;
    int gss = gameStateStatus.load();
    //Reconnect until the game actually FINISHES. The old condition was
    //(gss == created || gss != started), which reduces to just `gss != started` - so the
    //loop exited exactly when the game was still LIVE and the stream had merely dropped,
    //abandoning a rated game to its clock, and kept looping once the game was over.
    //In GameStateStatus everything from `aborted` (3) upwards is terminal; unknown(0),
    //created(1) and started(2) all mean "still going, keep streaming".
    int stream_attempts = 0;
    while (game_in_progress.load() && gss < aborted) {
        if (stream_attempts++) {
          std::cout << "HandleGame() debug: game " << game_id << " stream ended with status "
                    << gameSS[gss] << " but the game is not over - reconnecting (attempt "
                    << stream_attempts << ")" << std::endl;
          std::this_thread::sleep_for(std::chrono::seconds(2)); //do not hammer lichess
        }
        StreamAndProcess(stream_url, [game_id, &is_white, &initial_fen, &gss](const json& state) { //this is process_line() function for game-specific events
            if (state.contains("type") && state["type"] == "gameFull") {
                // Determine color (use value() for safety if key missing)
                gameStateStatus.store(0);
                for (int i = 0; i < game_states; i++) {
                  if (gameSS[i] == state["state"].value("status", "unknown")) {
                    gameStateStatus.store(i);
                    gss = i;
                    break;
                  }
                }
                if (gss != created && gss != started) return; //from process_line() function in StreamAndProcess()
                std::string white_id = state["white"].value("id", "");
                std::string black_id = state["black"].value("id", "");
                is_white = (white_id == bot_username);
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
                //Whose move it is depends on the INITIAL POSITION, not just the ply count.
                //
                //In a standard game ply 0 is White's, so `num_plies % 2 == 0` works. In a
                //fromPosition game -- which is how an opening book is delivered -- the starting
                //FEN carries its own side to move, and about half of any book's positions have
                //Black to move. Reading the parity alone made the bot believe it was on move
                //when it was not: it searched, got a legal move for the OTHER side, and posted
                //it, which lichess rejects with "Not your turn, or game already over".
                const bool starts_white = (initial_fen == "startpos") || fenSideToMoveIsWhite(initial_fen);
                bool white_turn = starts_white ? (num_plies % 2 == 0) : (num_plies % 2 == 1);
                bool our_turn = ((is_white && white_turn) || (!is_white && !white_turn));
                std::cout << "HandleGame() debug: turn check - num plies: " << num_plies << ", white turn: " << white_turn << ", our turn: " << our_turn << std::endl;
                long long wtime = state["state"].value("wtime", 0);
                long long btime = state["state"].value("btime", 0);
                long long winc = state["state"].value("winc", 0);
                long long binc = state["state"].value("binc", 0);
                
                bool wdraw = state["state"].value("wdraw", false);
                bool bdraw = state["state"].value("bdraw", false);
                bool draw_offer = is_white ? bdraw : wdraw;
                ComputeAndPostMove(game_id, draw_offer, our_turn, initial_fen, moves, wtime, btime, winc, binc);
              } else if (state.contains("type") && state["type"] == "gameState") {
                gameStateStatus.store(0);
                for (int i = 0; i < game_states; i++) {
                  if (gameSS[i] == state.value("status", "unknown")) {
                    gameStateStatus.store(i);
                    gss = i;
                    break;
                  }
                }
                //gameStateStatus = state.value("status", "");
                if (gss != created && gss != started) return;                
                std::string moves = state.value("moves", "");
                int num_plies = 0;
                std::string uci_move = "";
                std::istringstream uci_moves(moves);
                while (uci_moves >> uci_move) num_plies++;
                // Calculate turn
                //Whose move it is depends on the INITIAL POSITION, not just the ply count.
                //
                //In a standard game ply 0 is White's, so `num_plies % 2 == 0` works. In a
                //fromPosition game -- which is how an opening book is delivered -- the starting
                //FEN carries its own side to move, and about half of any book's positions have
                //Black to move. Reading the parity alone made the bot believe it was on move
                //when it was not: it searched, got a legal move for the OTHER side, and posted
                //it, which lichess rejects with "Not your turn, or game already over".
                const bool starts_white = (initial_fen == "startpos") || fenSideToMoveIsWhite(initial_fen);
                bool white_turn = starts_white ? (num_plies % 2 == 0) : (num_plies % 2 == 1);
                bool our_turn = ((is_white && white_turn) || (!is_white && !white_turn));
                std::cout << "HandleGame() debug: turn check - num plies: " << num_plies << ", white turn: " << white_turn << ", our turn: " << our_turn << std::endl;
                long long wtime = state.value("wtime", 0);
                long long btime = state.value("btime", 0);
                long long winc = state.value("winc", 0);
                long long binc = state.value("binc", 0);
                bool wdraw = state.value("wdraw", false);
                bool bdraw = state.value("bdraw", false);
                bool draw_offer = is_white ? bdraw : wdraw;
                ComputeAndPostMove(game_id, draw_offer, our_turn, initial_fen, moves, wtime, btime, winc, binc);
            } else if (state.contains("type") && state["type"] == "opponentGone") {
                //lichess omits claimWinInSeconds entirely when gone is false, and a bare
                //operator[] on a const json& asserts and abort()s - which killed the bot
                //every time an opponent disconnected and came back.
                bool gone = state.value("gone", false);
                int claimWinInSeconds = state.value("claimWinInSeconds", -1);
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
        std::cout << "HandleGame() debug: StreamAndProcess() returned. Stopping the engine..." << std::endl;
    } //end of while (game_in_progress && (gameStateStatus == "created" || gameStateStatus != "started"))
    gameStateStatus.store(0);
    stop(creatica);
}

// processor for event stream used by libcurl once an event (lines) is parsed by nlohmann json library into json event object
void ProcessEvent(const json& event) {
    if (!playing.load()) return; //do not process any events if ctrl-c is pressed
    if (event.contains("type") && (event["type"] == "challenge" || event["type"] == "challengeCanceled" || event["type"] == "challengeDeclined")) {
        std::string type = event["type"];
        std::string challenge_id = event["challenge"]["id"];
        bool rated = event["challenge"]["rated"];
        std::string challenger_id = event["challenge"]["challenger"]["id"];
        std::string title = event["challenge"]["challenger"].value("title", "");
        std::string variant = event["challenge"]["variant"]["key"];
        std::string speed = event["challenge"]["speed"];
        std::string status = event["challenge"]["status"];
        // Accept (customize logic, e.g., only standard variant)
        if (challenger_id == bot_username) {
            std::cout << "ProcessEvent() debug: our challenge " << challenge_id << " status " << status << std::endl;
            if (status == "accepted") {
              {
                std::lock_guard<std::mutex> lk(challenge_mutex);
                challenge_accepted.store(true);
                clearChallengeOutstanding();
              }
              challenge_cv.notify_one();
            }
            else if (status == "declined") {
              {
                std::lock_guard<std::mutex> lk(challenge_mutex);
                challenge_declined.store(true);
                clearChallengeOutstanding();
              }
              challenge_cv.notify_one();
            }
            return; // This is our challenge
        }
        std::cout << "ProcessEvent() debug: received challenge " << challenge_id << " with status " << status << " from " << challenger_id << std::endl;
        if (status == "created") {
            //fromPosition is accepted because that is how an opening book is delivered: a
            //challenge carrying a FEN arrives as this variant, not as "standard". It was
            //commented out here, so with CREATICA_BOOK set on one bot the other declined every
            //challenge with reason=variant and no games were played at all.
            //
            //chess960 is accepted only when this bot is CONFIGURED for it (CREATICA_VARIANT),
            //so a passing stranger cannot pull a standard-configured bot into a 960 game.
            //
            //It was declined outright for a while, because nothing had ever tested 960. That has
            //now been done properly -- perft against Stockfish under UCI_Chess960 over all 960
            //start positions plus 900 generated midgame positions in which castling is legal --
            //and it found three real defects: castling into a discovered check when the castling
            //rook was shielding an enemy slider, undo_move() erasing the rook from the mailbox
            //when its castling destination was its own square, and fen2board() being unable to
            //read X-FEN at all, which is the notation lichess actually sends.
            //
            //Safe: the gameFull event carries initialFen, which flows into creatica.position,
            //and position() sends "position fen <...>" for anything longer than 25 characters
            //and "position startpos" otherwise -- so both a custom position and a normal game
            //are handled by the same path.
            const bool variant_ok = (variant == "standard" || variant == "fromPosition" || variant == bot_variant);
            if ((!game_in_progress.load() && !challengeStillOutstanding() && variant_ok) &&
                (speed == "blitz" || speed == "rapid" || speed == "classical") && challengerAllowed(challenger_id) /*&& title != "BOT"*/) {
                std::string accept_url = "https://lichess.org/api/challenge/" + challenge_id + "/accept";
                if (HttpRequest("POST", accept_url)) {
                    std::cout << "ProcessEvent() debug: challenge accepted successfully" << std::endl;
                } else {
                    std::cout << "ProcessEvent() error: failed to accept challenge" << std::endl;
                }
            } else {
                std::string decline_url = "https://lichess.org/api/challenge/" + challenge_id + "/decline";
                std::string reason = "reason=";
                if (game_in_progress.load() || challengeStillOutstanding()) reason += "later";
                else if (!variant_ok) reason += "variant";
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
        games_started.fetch_add(1, std::memory_order_relaxed);
        //Tag the visit records with this game, so a dataset row can be joined to the result the
        //gameFinish handler writes. Without it the dump is usable for policy training but not for
        //anything that needs the outcome -- calibrating the value mapping, for instance.
        if (!visits_path.empty()) {
          std::string tag = game_id;
          setOption(creatica, "GameTag", String, (void *)tag.c_str());
        }
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
            clearChallengeOutstanding(); //the game supersedes any outstanding challenge
          }
          game_cv.notify_one();
          //start a detached thread to play a single game
          gameStateStatus.store(1); //created
          std::thread play(HandleGame, game_id);
          play.detach();
        }
    } else if (event.contains("type") && event["type"] == "gameFinish") {
        std::string game_id = event["game"]["gameId"];
        std::cout << "ProcessEvent() debug: game " << game_id << " (" << event["game"]["fullId"] << ") finished with " << event["game"]["status"]["name"] << std::endl;
        //Record the result to a file. Printing it to stdout leaves an overnight experiment as
        //terminal scrollback, and casual games do not move a rating either, so without this
        //there is nowhere a score actually accumulates.
        //
        //The settings are written on every line deliberately: a results file that does not say
        //what produced it is worth very little a week later, and this is the file someone will
        //be reading when they want to know what the configuration was.
        try {
          const std::string our_colour = event["game"].value("color", "");
          const std::string winner     = event["game"].value("winner", "");     //absent on a draw
          const std::string status     = event["game"]["status"].value("name", "");
          const double score = winner.empty() ? 0.5 : (winner == our_colour ? 1.0 : 0.0);
          std::lock_guard<std::mutex> rlk(results_mutex);
          bool fresh = true;
          if (FILE * probe = fopen(RESULTS_FILE, "r")) { fresh = false; fclose(probe); }
          if (FILE * rf = fopen(RESULTS_FILE, "a")) {
            //`speed` comes from the game and is true for both sides. `offered_clock` is this bot's OWN
            //challenge setting -- correct on the side that challenged, meaningless on the side that
            //accepted, since the accepter plays whatever it was offered. Named so nobody reads it as
            //the game's time control.
            if (fresh) fprintf(rf, "utc,game_id,bot,colour,winner,score,status,speed,ponder,threads,hash,offered_clock\n");
            const std::time_t now = std::time(nullptr);
            char ts[32] = "";
            std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
            const std::string speed = event["game"].value("speed", "");
            fprintf(rf, "%s,%s,%s,%s,%s,%.1f,%s,%s,%d,%d,%d,%d+%d\n",
                    ts, game_id.c_str(), bot_username.c_str(), our_colour.c_str(),
                    winner.empty() ? "draw" : winner.c_str(), score, status.c_str(), speed.c_str(),
                    bot_ponder ? 1 : 0, bot_threads, bot_hash, bot_clock, bot_increment);
            fclose(rf);
          }
          std::cout << "ProcessEvent(): result " << score << " as " << our_colour
                    << " (" << (winner.empty() ? "draw" : winner) << ", " << status << ")" << std::endl;
        } catch (const std::exception& e) {
          std::cerr << "ProcessEvent() warning: could not record result: " << e.what() << std::endl;
        }
        gameStateStatus.store(0);
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
    {
      std::lock_guard<std::mutex> lk(playing_mutex);
      game_in_progress.store(false);
    }
    game_cv.notify_one();
}

int main(int argc, char ** argv) {
  //Reject an unknown variant here rather than discovering it from declined challenges hours
  //later. Only these two are supported: everything else is untested against libchess.
  if (bot_variant != "standard" && bot_variant != "chess960") {
    fprintf(stderr, "CREATICA_VARIANT=%s is not supported; use standard or chess960\n",
            bot_variant.c_str());
    return 1;
  }
  if (bot_variant != "standard" && !book_path.empty())
    fprintf(stderr, "warning: CREATICA_BOOK is ignored with CREATICA_VARIANT=%s "
                    "(lichess randomises the start position itself)\n", bot_variant.c_str());
  //--no-challenge lets the bot answer incoming challenges without hunting for opponents,
  //which is what you want while verifying it by challenging it yourself. Previously this
  //meant commenting out the thread and rebuilding.
  bool no_challenge = false;
  bool print_challenge = false;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--no-challenge") no_challenge = true;
    else if (arg == "--print-challenge") print_challenge = true;
    else if (arg == "--casual") challenge_rated = false;
    else if (arg.rfind("--challenge=", 0) == 0) {
      challenge_target = arg.substr(12);
      if (challenge_target.empty()) {
        fprintf(stderr, "%s: --challenge= needs a username\n", argv[0]); return 1;
      }
    }
    else if (arg.rfind("--accept-only=", 0) == 0) {
      //Comma-separated list; the flag may also be repeated.
      std::stringstream names(arg.substr(14));
      std::string who;
      int added = 0;
      while (std::getline(names, who, ',')) {
        //tolerate stray spaces around a name
        const size_t b = who.find_first_not_of(" \t");
        const size_t e = who.find_last_not_of(" \t");
        if (b == std::string::npos) continue;
        accept_only.insert(who.substr(b, e - b + 1));
        added++;
      }
      if (!added) { fprintf(stderr, "%s: --accept-only= needs at least one username\n", argv[0]); return 1; }
    }
    else if (arg == "-h" || arg == "--help") {
      printf("usage: %s [--no-challenge] [--challenge=<user>] [--casual]\n"
             "            [--accept-only=<user>[,<user>...]]\n"
             "  --no-challenge         do not challenge other bots; only respond to incoming challenges\n"
             "  --challenge=<user>     challenge this opponent instead of a random online bot\n"
             "  --casual               send unrated challenges\n"
             "  --accept-only=<a>[,<b>...]  only accept challenges from these users.\n"
             "                         May be repeated. Omit entirely to accept anyone.\n", argv[0]);
      return 0;
    } else {
      fprintf(stderr, "%s: unknown argument '%s' (try --help)\n", argv[0], argv[i]);
      return 1;
    }
  }
  //Report the configuration we parsed before doing anything that can fail, so a
  //mistyped filter is obvious rather than silently meaning "accept anyone".
  if (accept_only.empty()) std::cout << "main(): accepting challenges from ANYONE" << std::endl;
  else {
    std::cout << "main(): accepting challenges only from:";
    for (const auto& w : accept_only) std::cout << " " << w;
    std::cout << std::endl;
  }
  //Give this instance's engine its own log. The child inherits our environment, so setting it
  //here is enough. Without it two bots share creatica.log and the interleaved result cannot be
  //attributed to either engine.
  if (!getenv("CREATICA_LOG")) {
    const std::string lg = "creatica_" + bot_username + ".log";
    setenv("CREATICA_LOG", lg.c_str(), 1);
    std::cout << "main(): engine log -> " << lg << std::endl;
  }
  load_opening_book(book_path);
  if (no_challenge) std::cout << "main(): --no-challenge, so we will not challenge anyone" << std::endl;
  else if (!challenge_target.empty())
    std::cout << "main(): challenging only " << challenge_target
              << (challenge_rated ? " (rated)" : " (casual)") << std::endl;
  else std::cout << "main(): will challenge a RANDOM online bot" << std::endl;

  //--print-challenge builds the challenge exactly as the challenge thread would and prints the
  //POST body, then exits. No token and no network. It answers the one question that a night of
  //games cannot easily be un-wasted over: is this configuration actually going to ask for the
  //variant, clock and colour I think it is? A book FEN, for instance, silently rewrites the
  //variant to fromPosition.
  if (print_challenge) {
    if (!book_path.empty() && bot_variant == "standard") load_opening_book(book_path);
    std::string id;
    CreateChallenge(challenge_target.empty() ? "OPPONENT" : challenge_target,
                    challenge_rated, bot_clock, bot_increment, id, "random", bot_variant, true);
    return 0;
  }

  if (token.empty()) {
    fprintf(stderr, "lichess_bot: LICHESS_TOKEN is not set.\n"
                    "  export LICHESS_TOKEN=\"$(cat ~/.config/creatica/lichess_token)\"\n");
    return 1;
  }
  const int multiPV = MULTI_PV;
  std::signal(SIGINT, signal_handler);  // Set up Ctrl-C handler
  //A write to a dead engine's pipe raises SIGPIPE, whose default action terminates the
  //process. Ignore it so the write returns EPIPE and the restart path can run instead.
  std::signal(SIGPIPE, SIG_IGN);
  //init_magic_bitboards();
  Stockfish::Bitboards::init();
  rng.seed(static_cast<unsigned int>(std::random_device{}()));

  //start chess engine process and communicate with it over stdin, stdout redirected to named pipes internally
  initChessEngine(creatica, engine_path.c_str(), MOVETIME, DEPTH, bot_hash, bot_threads, SYZYGY_PATH, MULTI_PV, false, false, ELO_CREATICA);
  //else fprintf(stderr, "initilized chess engine %s for creatica\n", creatica.id);
  setEngineOptions();
  for (int i = 0; i < multiPV; i++) {
    evaluations[i] = new Evaluation;
    evaluations[i]->maxPlies = 1; //we just need the next move  
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::thread challenge;
  if (!no_challenge) challenge = std::thread(GetAndProcessBots, nb);
  std::string event_url = "https://lichess.org/api/stream/event";
  while (playing.load()) {  // Main loop: Keep streaming events
      StreamAndProcess(event_url, ProcessEvent);
      std::cerr << "main() debug: stream ended; reconnecting in 3s..." << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(3));
  }
  if (challenge.joinable()) challenge.join();
  quit(creatica);
  releaseChessEngine(creatica);
  for (int i = 0; i < multiPV; i++) delete evaluations[i];
  curl_global_cleanup();
  //cleanup_magic_bitboards();
  return 0;
}


