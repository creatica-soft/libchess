//For MacOS using clang
// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -Wno-vla-cxx-extension -O3 -flto -I /Users/ap/libchess  -L /Users/ap/libchess -Wl,-lchess,-lcurl,-rpath,/Users/ap/libchess creatica_search.cpp creatica.cpp uci_frontend.cpp tbcore.c tbprobe.c -o creatica

// For linux or Windows using mingw
//g++ -std=c++20 -mpopcnt -Wno-deprecated -Wno-write-strings -Wno-deprecated-declarations -Wno-stringop-overflow -O3 -I /home/ap/libchess -L /home/ap/libchess creatica_search.cpp creatica.cpp uci_frontend.cpp tbcore.c tbprobe.c -o creatica -lchess -lcurl

// creatica is creatica's search driven by the shared UCI layer
// (uci_engine.h / uci_options.h / uci_frontend.cpp) instead of its own copy of the
// protocol. The search itself -- creatica_search.cpp -- is linked unchanged.
//
// The point of the move is the settings. uci-nnue-policy.cpp read eight CREATICA_*
// environment variables once at startup, so every one of them needed a process restart and
// none of them was discoverable from a GUI. All eight are now ordinary UCI options,
// declared in declare_options() below alongside the ones the old engine already
// advertised. The advertised numbers of the pre-existing options are unchanged, so a GUI
// or script that drove creatica drives this identically.
//
//   old (process start)                  new (any time, via setoption)
//   CREATICA_POLICY=...                  PolicyWeights   string
//   CREATICA_POLICY_MODE=prior           PolicyMode      spin 0..2   (0 off 1 prior 2 full)
//   CREATICA_POLICY_BLEND=0.45           PolicyBlend     spin 0..100  /100
//   CREATICA_POLICY_TEMP=0.75            PolicyTemp      spin 0..300  /100
//   CREATICA_BLEND_SCALE=1.15            BlendScale      spin 0..300  /100
//   CREATICA_FPU=0.20                    FpuReduction    spin 0..100  /100
//   CREATICA_SEED_CHILDREN=1             SeedChildren    check
//   CREATICA_NODE_MINIMAX=1              NodeMinimax     check
//
// The scaled-integer options (Temperature 58 meaning 0.58 and friends) keep advertising
// the same integers, but the /100 that used to sit at every point of use is now done once,
// by Options::real(), so the engine's globals hold the real value directly.
//
// One non-protocol command is added: "settings" prints every option's current value. What
// a match was actually run with is otherwise unrecoverable from the log.

#include <algorithm>
#include <cstdarg>
#include <ctime>
#include "nnue/bitboard.h"
#include "creatica_search.hpp"
#include "policy_net.h"
#include "uci_engine.h"
#include <sys/stat.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

//--- state owned by creatica_search.cpp ------------------------------------------
extern PolicyNet policy_net;
extern bool      policy_enabled;
extern int       policy_mode;
extern double    policy_temperature;
extern double    policy_blend;
extern double    policy_blend_scale;
extern double    fpu_reduction;
extern bool      seed_children;
extern bool      node_minimax;
extern double    probability_mass;
extern bool      reuse_tree;
extern bool      post_move_collect;
extern int64_t   gc_threshold;
void gc_join();
extern std::string visit_dump_path;
extern std::string game_tag;
extern bool      validate_tree_enabled;

extern std::mutex mtx, log_mtx, print_mtx, pool_mutex, search_done_mtx, probe_mutex;
extern std::shared_mutex map_mutex;
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
extern double virtual_loss;
extern double eval_scale;
extern double temperature;

extern std::string last_move;
extern std::unordered_set<unsigned long long> position_history;
extern Board board;
extern ZobristHash zh;
extern Zobrist z;
extern Engine chessEngine;
extern MCTSSearch search;
extern std::vector<std::thread> pool_threads;
extern std::vector<ThreadParams> pool_params;
using json = nlohmann::json;

//--- output ----------------------------------------------------------------------------

// uci_frontend.cpp's writer. Declared rather than reimplemented so that everything this
// process prints -- the frontend's "uciok"/"readyok", the search's info lines, and the
// bestmove -- goes out under ONE mutex. Two independent output locks would let a
// "readyok" land in the middle of an info line that another thread is emitting.
namespace uci { void out(const char * fmt, ...); }

void log_file(const char * message, ...) {
  if (!logfile) return;
  std::lock_guard<std::mutex> lock(log_mtx);
  va_list args;
  va_start(args, message);
  vfprintf(logfile, message, args);
  va_end(args);
  fflush(logfile);
}

// The search calls print() for everything it emits, including the NNUE eval trace, which
// runs to several kilobytes -- hence the heap fallback rather than a fixed buffer.
void print(const char * message, ...) {
  char stackbuf[4096];
  va_list args;
  va_start(args, message);
  va_list copy;
  va_copy(copy, args);
  int n = vsnprintf(stackbuf, sizeof stackbuf, message, args);
  va_end(args);
  if (n < 0) { va_end(copy); return; }
  if ((size_t)n < sizeof stackbuf) {
    va_end(copy);
    uci::out("%s", stackbuf);
  } else {
    std::vector<char> big((size_t)n + 1);
    vsnprintf(big.data(), big.size(), message, copy);
    va_end(copy);
    uci::out("%s", big.data());
  }
}

//--- online tablebase lookup (unchanged from uci-nnue-policy.cpp) ----------------------

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    std::string * response = static_cast<std::string*>(userp);
    size_t total_size = size * nmemb;
    response->append(static_cast<char*>(contents), total_size); // Append, don't overwrite
    return total_size;
}

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

//--- search thread and worker pool (unchanged from uci-nnue-policy.cpp) ----------------

void search_thread_func() {
  NNUEContext ctx;
  init_nnue_context(ctx);
  while (!quitFlag.load()) {
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [] { return searchFlag.load() || quitFlag.load(); }); // Wait for "go" or "quit"
    lock.unlock();
    if (quitFlag.load()) {
      cleanup();
      position_history.clear();
      break;
    }
    if (searchFlag.load()) {
      accumulator_stack_reset(ctx);
      runMCTS(ctx);
    }
    else continue;
    lock.lock();
    searchFlag.store(false);
    stopFlag.store(false);
    cv.notify_all(); // Signal search is done
  }
  free_nnue_context(ctx);
}

void shutdown_thread_pool() {
  {
    //Same reason as the other pool_cv signal: set the flag the waiters test while holding the
    //mutex they wait on, or a worker can block just after testing it and never see the quit.
    std::lock_guard<std::mutex> lk(pool_mutex);
    pool_quit.store(true);
  }
  pool_cv.notify_all(); // Wake everyone up so they see the quit flag
  for (auto& t : pool_threads) {
    if (t.joinable()) t.join();
  }
  pool_threads.clear();
}

//Which cores the search threads are allowed to run on.
//
//This is an Apple M1: 4 performance cores and 4 efficiency cores. macOS places a thread by
//its QoS class, and a plain std::thread inherits a default that permits the efficiency
//cores -- so at Threads 8 roughly half the search can land on cores that are three to four
//times slower.
//
//That is not simply "some threads are slow". A shared-tree MCTS applies virtual loss when a
//thread starts visiting a node and removes it when the visit completes, to steer the other
//threads elsewhere. A thread that takes four times as long leaves its virtual loss in place
//four times as long, distorting selection for every other thread while it runs. The slow
//thread pays a full share of the synchronisation cost while contributing a quarter of the
//nodes.
//
//USER_INTERACTIVE is the highest non-realtime class and biases the scheduler strongly
//toward performance cores. It is a hint, not a guarantee -- macOS gives no way to pin a
//thread to a core -- so this makes 4 threads *likely* to get the 4 fast cores, not certain.
//Set by the thread on itself, which is the only way pthread QoS works.
std::atomic<bool> use_performance_cores{false};

void persistent_worker_func(int thread_id) {
  if (use_performance_cores.load(std::memory_order_relaxed)) {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  }
  int local_generation = 0;
  NNUEContext ctx;
  init_nnue_context(ctx);
  while (true) {
    std::unique_lock<std::mutex> lock(pool_mutex);
    pool_cv.wait(lock, [&] {
      // Wake up if there is a new search generation OR we need to quit
      return pool_generation.load() > local_generation || pool_quit.load();
    });
    lock.unlock(); // Release lock so other threads can wake up
    if (pool_quit.load()) break;
    if (pool_generation.load() == local_generation) continue;
    local_generation = pool_generation.load();
    ThreadParams params = pool_params[thread_id];
    auto iter_start = std::chrono::steady_clock::now();
    double elapsed = 0.0;
    unsigned occupancy_tick = 0;

    while (depth.load(std::memory_order_relaxed) < chessEngine.depth &&
      elapsed < (params.time_alloc * 0.001) &&
      !stopFlag.load(std::memory_order_relaxed)) {
      //hash_full is NOT a termination condition. A full tree gates expansion inside
      //mcts_search(); ending the loop here made the engine stop searching altogether the
      //moment the tree filled, which with tree reuse happens routinely in a long game.

      accumulator_stack_reset(ctx);
      mcts_search(params, ctx);
      //Keep the expansion guard current during the search, not just between searches. Cheap:
      //tree_occupancy() reads two atomics. Thread 0 only, every 4096 simulations, so the cost
      //is negligible and the guard reflects a tree that is still growing.
      if (params.thread_id == 0 && (++occupancy_tick & 0xFFF) == 0) {
        int hf = tree_occupancy();
        if (hf > 1000) hf = 1000;
        hash_full.store(hf, std::memory_order_relaxed);
      }
      int expected = seldepth.load(std::memory_order_relaxed);
      while (params.seldepth > expected && !seldepth.compare_exchange_strong(expected, params.seldepth, std::memory_order_relaxed)) expected = seldepth.load(std::memory_order_relaxed);
      if ((chessEngine.depth && depth.load(std::memory_order_relaxed) >= chessEngine.depth) ||
          (chessEngine.nodes && search.root->N.load(std::memory_order_relaxed) >= chessEngine.nodes)) {
        break;
      }
      elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - iter_start).count();
    }
    //Decrement under pool_mutex, notify after releasing it. runMCTS() waits on
    //pool_done_cv with the predicate active_workers == 0, and evaluates that predicate while
    //holding the lock; a decrement-and-notify from outside the lock can land in the window
    //between its check and its block, losing the wake and hanging the search forever.
    bool last_worker;
    {
      std::lock_guard<std::mutex> lk(pool_mutex);
      last_worker = (active_workers.fetch_sub(1) == 1);
    }
    if (last_worker) pool_done_cv.notify_one();
  }
  free_nnue_context(ctx);
}

void init_thread_pool(int num_threads) {
  // Stop existing threads if any
  shutdown_thread_pool();

  pool_quit.store(false);
  pool_generation.store(0);
  pool_params.resize(num_threads);

  for (int i = 0; i < num_threads; ++i)
    pool_threads.emplace_back(persistent_worker_func, i);
}

//--- the engine ------------------------------------------------------------------------

class CreaticaEngine : public uci::SearchEngine {
public:
    const char * id_name()   const override { return "Creatica (Shared MCTS Root, NNUE policy)"; }
    const char * id_author() const override { return "Arkadi Poliakevitch"; }

    void declare_options(uci::Options& o) override {
        opts_ = &o;

        // --- what creatica already advertised, with the same numbers -------
        // The spin slots bind straight into chessEngine, because that is where the search
        // reads them; the real() slots bind to the search's own doubles, which used to be
        // recomputed as value*0.01 (or *0.1) inside handleOption.
        o.spin("Hash",    &chessEngine.optionSpin[Hash].value,    HASH,     128, 4096);
        o.spin("Threads", &chessEngine.optionSpin[Threads].value, THREADS,  1,   8);
        //Rebuilds the pool, because QoS is set by each thread on itself at startup and
        //cannot be changed for a thread already running.
        o.check("PerformanceCores", &performance_cores_opt_, false,
                [this] { apply_performance_cores(); });
        o.spin("MultiPV", &chessEngine.optionSpin[MultiPV].value, MULTI_PV, 1,   8);
        // advertises 65 / 0..100, engine sees 0.65
        o.real("ExplorationMin", &exploration_min, EXPLORATION_MIN / 100.0, 0.0, 1.0, 100);
        // advertises 160 / 0..200, engine sees 1.60
        o.real("ExplorationMax", &exploration_max, EXPLORATION_MAX / 100.0, 0.0, 2.0, 100);
        // advertises 5 / 0..10, engine sees 0.05
        o.real("ExplorationDepthDecay", &exploration_depth_decay,
               EXPLORATION_DEPTH_DECAY / 100.0, 0.0, 0.10, 100);
        o.spin("PVPlies", &chessEngine.optionSpin[PVPlies].value, PV_PLIES, 1, 32);
        // advertises 58 / 1..200, engine sees 0.58
        o.real("Temperature", &temperature, TEMPERATURE / 100.0, 0.01, 2.0, 100);
        // advertises 36 / 0..100, engine sees 3.6.
        // NOT 0.36: uci-nnue-policy.cpp scaled VirtualLoss by 0.1, not 0.01, in both
        // setEngineOptions() and handleOption(). ENGINE_SETTINGS.md says 1/100 and is
        // wrong about it. Scale 10 here keeps the advertised integer AND the value the
        // running engine actually searches with; changing it to 0.36 would be a silent
        // ten-fold change to virtual loss, i.e. a different engine.
        o.real("VirtualLoss", &virtual_loss, VIRTUAL_LOSS / 10.0, 0.0, 10.0, 10);
        // Advertised default is <empty>, as before; init() then applies the compiled-in
        // SYZYGY_PATH, which is what setEngineOptions() did.
        o.string("SyzygyPath", &syzygy_path_, "", [this] { on_syzygy_path(); });
        o.check("Ponder",                &chessEngine.optionCheck[Ponder].value,                PONDER);
        o.check("FinalInfoLines",        &chessEngine.optionCheck[FinalInfoLines].value,        DISPLAY_FINAL_INFO_LINES);
        o.check("IntermittentInfoLines", &chessEngine.optionCheck[IntermittentInfoLines].value, DISPLAY_INTERMITTENT_INFO_LINES);

        // --- the eight former environment variables -----------------------------------
        o.string("PolicyWeights", &policy_weights_, POLICY_WEIGHTS_DEFAULT,
                 [this] { on_policy_weights(); });
        // 0 = off (the incumbent eval-derived prior), 1 = prior, 2 = full. The old
        // spelling was the words "off"/"prior"/"full"; UCI has no enum type short of a
        // combo, which uci_options.h does not implement, so this is the integer the
        // PolicyMode enum already used.
        o.spin("PolicyMode", &policy_mode_opt_, 1, 0, 2, [this] { apply_policy_mode(); });
        o.real("PolicyBlend",  &policy_blend,       POLICY_BLEND,       0.0, 1.0, 100);
        o.real("PolicyTemp",   &policy_temperature, 0.75,               0.0, 3.0, 100);
        o.real("BlendScale",   &policy_blend_scale, POLICY_BLEND_SCALE, 0.0, 3.0, 100);
        o.real("FpuReduction", &fpu_reduction,      FPU_REDUCTION,      0.0, 1.0, 100);
        o.check("SeedChildren", &seed_children, true);
        o.check("NodeMinimax",  &node_minimax,  true);
        // Per-mille, not percent: the interesting settings are near the top of the range
        // (0.99 and 0.999 behave very differently) and a percent scale cannot express them.
        // 1000 = keep every move, which is the default and an exact no-op.
        o.real("ProbabilityMass", &probability_mass, 1.0, 0.90, 1.0, 1000);
        // Diagnostic, not a tunable. Walks the whole tree after every collection and reports
        // any broken invariant on stderr and in the log. Costs a full map walk per move, so it
        // is for runs that are asking whether tree reuse is sound, not for playing.
        // Keep the tree between moves rather than rebuilding it. Ponder implies this; this
        // does not imply Ponder. Default off until it has been measured, not merely validated.
        o.check("ReuseTree", &reuse_tree, true);
        // Turn OFF when the engine's chosen move is not what comes next -- replaying a game,
        // or searching a position list. See the note in creatica_search.cpp.
        o.check("PostMoveCollect", &post_move_collect, true);
        // Per-mille of Hash at which the tree is collected. Collection is O(tree) and only
        // reclaims memory, so running it every move paid a growing cost for nothing.
        o.spin("GcThreshold", &gc_threshold, 700, 0, 1000);
        // Where to append the root visit distribution after each search, and a tag written on
        // every line so records can be joined back to a game and its result. Empty = off.
        o.string("VisitDumpFile", &visit_dump_path, "");
        o.string("GameTag",       &game_tag,        "");
        o.check("ValidateTree", &validate_tree_enabled, false);
    }

    void init() override {
        TB_LARGEST = 0;
        zobristHash(z);
        // Its own log. Sharing uci-mcts-nnue-policy.log with a running
        // creatica would interleave two engines' lines in one file.
        //CREATICA_LOG, not a UCI option, because the log has to exist before the protocol does:
        //init() writes the settings line, and an option cannot arrive until after the handshake.
        //
        //Per instance matters. Two engines appending to one file interleave, and the result is
        //actively misleading rather than merely untidy -- two bots playing each other produce
        //apparently duplicated "bestmove" lines, because creatica ponders the position where the
        //OPPONENT is to move, which is the same position the opponent is searching for real. That
        //artefact was read as an engine emitting two bestmoves for one go, and a driver change
        //was made on the strength of it.
        {
            const char * lp = std::getenv("CREATICA_LOG");
            logfile = fopen((lp && *lp) ? lp : "creatica.log", "a");
        }
        srand(time(NULL));
        Stockfish::Bitboards::init();
        init_nnue();

        // EvalScale is not a UCI option here for the same reason it was not one before:
        // uci-nnue-policy.cpp advertised only the first ten spin slots and EvalScale sits
        // at index ten, so it was never settable. setEngineOptions() still initialised the
        // divisor, and so does this line -- W = tanh(eval / 6.1).
        eval_scale = EVAL_SCALE * 0.1;

        snprintf(chessEngine.id, sizeof chessEngine.id, "%s", id_name());
        snprintf(chessEngine.authors, sizeof chessEngine.authors, "%s", id_author());

        if (syzygy_path_.empty()) syzygy_path_ = SYZYGY_PATH;
        init_tablebases();

        load_policy_net(policy_weights_.c_str(), /*startup=*/true);
        apply_policy_mode();

        curl_global_init(CURL_GLOBAL_DEFAULT);
        init_thread_pool((int)chessEngine.optionSpin[Threads].value);
        search_thread_ = std::thread(search_thread_func);

        print("info string settings: %s\n", opts_->summary().c_str());
        log_file("info string settings: %s\n", opts_->summary().c_str());
    }

    void new_game() override {
        //Quiesce the search BEFORE freeing the tree. cleanup() deletes every MCTSNode and every
        //Edge array and nulls search.root; doing that while worker threads are still descending
        //the tree is a use-after-free. It never bit with Ponder off, because then a search is
        //only ever running between "go" and its own "bestmove" and no command arrives in
        //between -- but pondering means the engine IS searching when ucinewgame arrives, which
        //is exactly the configuration tree reuse needs.
        //
        //stop() is the established idiom here: set stopFlag and wait until searchFlag clears.
        //It returns immediately when nothing is running, and run_go() clears stopFlag for the
        //next search, so calling it unconditionally is safe.
        stop();
        cleanup();
        position_history.clear();
        last_move.clear();
    }

    // The last move is deliberately NOT played here. It is usually the opponent's move,
    // and in ponder mode the engine has to think about the position BEFORE it -- so the
    // decision to apply it belongs to "go", exactly as in handlePosition().
    void set_position(const std::string& fen, const std::vector<std::string>& moves) override {
        //Same reason as new_game(): this rewrites board, zh and position_history, all of which a
        //running search reads on every simulation. Wait for it to finish first.
        stop();
        last_move.clear();
        if (fen == startPos && moves.empty()) position_history.clear();
        fen2board(board, fen.c_str());
        getHash(zh, board, z);
        if (!moves.empty()) {
            for (size_t i = 0; i + 1 < moves.size(); ++i) play(moves[i]);
            last_move = moves.back();
        }
    }

    void go(const uci::Limits& lim) override {
        last_limits_ = lim;
        have_limits_ = true;
        run_go(lim);
    }

    void stop() override {
        std::unique_lock<std::mutex> lock(mtx);
        stopFlag.store(true);
        cv.wait(lock, [] { return !searchFlag.load(); }); // Wait for search to stop
    }

    // Stop pondering without emitting a bestmove, play the move that was pondered on, and
    // re-run the same "go" with ponder cleared.
    void ponderhit() override {
        if (!have_limits_) return;
        ponderHit.store(true);
        log_file("ponderhit: stop\n");
        stop();
        if (!last_move.empty()) {
            play(last_move);
            char fenString[MAX_FEN_STRING_LEN];
            log_file("position fen %s\n", board2fen(board, fenString));
            last_move.clear();
        }
        chessEngine.ponder = false;
        ponderHit.store(false);
        uci::Limits lim = last_limits_;
        lim.ponder = false;
        run_go(lim);
    }

    void quit() override {
        //A joinable std::thread destroyed at exit calls std::terminate.
        gc_join();
        {
            std::lock_guard<std::mutex> lock(mtx);
            quitFlag.store(true);
        }
        cv.notify_all();
        if (search_thread_.joinable()) search_thread_.join();
        shutdown_thread_pool();
        curl_global_cleanup();
        cleanup_nnue();
        if (logfile) { fclose(logfile); logfile = nullptr; }
    }

    bool custom(const std::string& line) override {
        if (line == "pieces") {
            print("%d\n", bitCount(board.side[ColorWhite] | board.side[ColorBlack]));
            return true;
        }
        if (line == "eval") {
            print("%s\n", nnue_eval(board).c_str());
            return true;
        }
        if (line == "settings") {
            print_settings();
            return true;
        }
        return false;
    }

private:
    uci::Options * opts_ = nullptr;
    std::string    syzygy_path_;
    std::string    policy_weights_;
    std::string    policy_loaded_;      // what policy_net actually holds
    int64_t        policy_mode_opt_ = 1;
    bool           performance_cores_opt_ = false;
    std::thread    search_thread_;
    uci::Limits    last_limits_;
    bool           have_limits_ = false;

    void play(const std::string& uci_move) {
        Move move = {};
        uci2move_idx(uci_move.c_str(), move);
        updateHash(zh, board, move, ff_move(board, move), z);
        position_history.insert(zh.hash);
    }

    void init_tablebases() {
        if (syzygy_path_.empty()) return;
        //A wrong path and an empty directory used to produce the same message, so a typo
        //looked like missing files. Distinguish them before tb_init() gets a chance to.
        struct stat st = {};
        const bool exists = (stat(syzygy_path_.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
        if (!exists) {
            const char * msg = "info string error SyzygyPath %s is not a directory; "
                               "endgames of 7 pieces or fewer will be queried from the "
                               "online lichess tablebase instead -- slow and rate limited\n";
            print(msg, syzygy_path_.c_str());
            log_file(msg, syzygy_path_.c_str());
            return;
        }
        tb_init(syzygy_path_.c_str());
        if (TB_LARGEST == 0) {
            //Worth spelling out what the fallback costs. Losing the tables does not merely
            //remove the instant probes: TB_LARGEST drops to 0, and the root handler sends
            //EVERY position of 7 pieces or fewer to lichess rather than only those above
            //the local limit. A missing path therefore increases network traffic, in the
            //phase of the game where it hurts most.
            const char * msg = "info string error no tablebase files found in %s; endgames "
                               "of 7 pieces or fewer will be queried from the online lichess "
                               "tablebase instead -- slow and rate limited\n";
            print(msg, syzygy_path_.c_str());
            log_file(msg, syzygy_path_.c_str());
        } else {
            tb_init_done = true;
            print("info string successfully initialized tablebases in %s. Max number of pieces %d\n", syzygy_path_.c_str(), TB_LARGEST);
            log_file("info string successfully initialized tablebases in %s. Max number of pieces %d\n", syzygy_path_.c_str(), TB_LARGEST);
        }
    }

    // Same guard the old handleOption() had: tb_init() is not re-entrant here, so the
    // first successful initialisation wins for the life of the process.
    void on_syzygy_path() {
        if (tb_init_done) {
            print("info string SyzygyPath: tablebases already initialised; ignoring\n");
            return;
        }
        init_tablebases();
    }

    // A failed load keeps the net that is already there. Falling back to the eval-derived
    // prior is sound but slower and measurably weaker; playing on with a half-read net
    // would be neither. policy_net_load() reports rather than throws, and it writes into a
    // scratch net, so policy_net is only replaced once the whole file has been read.
    bool load_policy_net(const char * path, bool startup) {
        PolicyNet fresh;
        char err[512] = "";
        if (!policy_net_load(fresh, path, err, sizeof err)) {
            if (startup) {
                print("info string policy head disabled (%s); using NNUE child evaluations\n", err);
                log_file("info string policy head disabled (%s); using NNUE child evaluations\n", err);
            } else {
                print("info string PolicyWeights: %s -- keeping %s\n", err,
                      policy_loaded_.empty() ? "the NNUE child evaluations" : policy_loaded_.c_str());
                policy_weights_ = policy_loaded_;   // the option must name what is loaded
            }
            return false;
        }
        policy_net = std::move(fresh);
        policy_loaded_ = path;
        print("info string policy head %s loaded: %d -> %d -> %d -> %d\n",
              path, policy_net.in, policy_net.h1, policy_net.h2, policy_net.out);
        log_file("info string policy head %s loaded: %d -> %d -> %d -> %d\n",
                 path, policy_net.in, policy_net.h1, policy_net.h2, policy_net.out);
        return true;
    }

    // Reloading swaps vectors that the worker threads read without a lock, so it cannot
    // happen underneath a live search. Refusing is safe; racing is a crash mid-game.
    void on_policy_weights() {
        if (policy_weights_ == policy_loaded_) return;
        if (searchFlag.load()) {
            print("info string PolicyWeights: cannot reload while searching; keeping %s\n",
                  policy_loaded_.empty() ? "<none>" : policy_loaded_.c_str());
            policy_weights_ = policy_loaded_;
            return;
        }
        // Only on a successful swap: a failed load changes nothing, and echoing the
        // mode line there reads as if it had.
        if (load_policy_net(policy_weights_.c_str(), /*startup=*/false)) apply_policy_mode();
    }

    //Changing the QoS of a running thread is not possible, so the pool is rebuilt. Refused
    //during a search for the same reason a weights reload is: the workers are live.
    void apply_performance_cores() {
        if (searchFlag.load()) {
            uci::out("info string PerformanceCores: ignored during a search\n");
            performance_cores_opt_ = use_performance_cores.load();
            return;
        }
        use_performance_cores.store(performance_cores_opt_);
        init_thread_pool((int)chessEngine.optionSpin[Threads].value);
        uci::out("info string search threads restarted on %s cores\n",
                 performance_cores_opt_ ? "performance" : "any");
    }

    void apply_policy_mode() {
        policy_mode    = (int)policy_mode_opt_;
        policy_enabled = policy_net.loaded && policy_mode != 0;
        print("info string policy mode %s, temp %.3f, blend %.2f, scale %.2f, fpu %.2f, seed %d, minimax %d\n",
              policy_mode == 2 ? "full" : policy_mode == 1 ? "prior" : "off",
              policy_temperature, policy_blend, policy_blend_scale, fpu_reduction,
              (int)seed_children, (int)node_minimax);
    }

    void print_settings() {
        char buf[256];
        for (const uci::Options::Entry& e : opts_->entries()) {
            if (e.type == uci::Options::Button) continue;
            if (e.type == uci::Options::Check)
                snprintf(buf, sizeof buf, "%-24s %s", e.name.c_str(), *e.b_slot ? "true" : "false");
            else if (e.type == uci::Options::String)
                snprintf(buf, sizeof buf, "%-24s %s", e.name.c_str(),
                         e.s_slot->empty() ? "<empty>" : e.s_slot->c_str());
            else if (e.d_slot)
                snprintf(buf, sizeof buf, "%-24s %g   (uci %lld)", e.name.c_str(), *e.d_slot,
                         (long long)llround(*e.d_slot * e.scale));
            else
                snprintf(buf, sizeof buf, "%-24s %lld", e.name.c_str(), (long long)*e.i_slot);
            print("%s\n", buf);
        }
        print("%-24s %g   (compile-time)\n", "EvalScale", eval_scale);
        print("%-24s %s\n", "policy net loaded",
              policy_loaded_.empty() ? "<none>" : policy_loaded_.c_str());
    }

    //--- go --------------------------------------------------------------------------
    //
    // A straight port of handleGo(). The only difference is where the numbers come from:
    // uci::Limits instead of strtok over the command line. Limits cannot distinguish an
    // absent "wtime" from "wtime 0", so a zero clock is read as absent and becomes the
    // same 1e9 default handleGo() started from -- no GUI sends wtime 0.
    void run_go(const uci::Limits& lim) {
        chessEngine.wtime     = lim.wtime ? lim.wtime : (int64_t)1e9;
        chessEngine.btime     = lim.btime ? lim.btime : (int64_t)1e9;
        chessEngine.winc      = lim.winc;
        chessEngine.binc      = lim.binc;
        chessEngine.movestogo = lim.movestogo;
        chessEngine.movetime  = lim.movetime_ms;
        chessEngine.depth     = lim.depth;      // runMCTS() raises 0 to MAX_DEPTH, so this
        chessEngine.nodes     = lim.nodes;      // must be reset on every go
        chessEngine.infinite  = lim.infinite;
        chessEngine.ponder    = lim.ponder;

        if (searchFlag.load()) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [] { return !searchFlag.load(); }); // Wait for previous search to stop
        }

        if (!chessEngine.ponder && !last_move.empty()) {
            //last move in ponder mode (go ponder) should only be executed on ponderhit.
            //It is usually the opponent's move2 in "bestmove move1 ponder move2".
            play(last_move);
            last_move.clear();
        }

        //A ponder search must NEVER stop on its own. UCI is explicit that after "go ponder" the
        //engine may not send bestmove until it receives "stop" or "ponderhit" -- an unsolicited
        //bestmove desynchronises the GUI, which is not reading for one yet.
        //
        //This used to depend on the GUI also sending "infinite", which our own bot happens to do.
        //A standard GUI sends "go ponder wtime ... btime ..." with no infinite, and the engine
        //would then compute an ordinary allocation and terminate itself. Worse, movetime was
        //tested FIRST, so even with infinite present a non-zero movetime beat it.
        //
        //The real allocation is computed on ponderhit, which re-runs this with ponder cleared.
        if (chessEngine.ponder || chessEngine.infinite) timeAllocated = 1e9;
        else if (chessEngine.movetime > 0) timeAllocated = chessEngine.movetime * 0.99;
        else {
            int64_t remainingTime = board.sideToMove == ColorWhite ? chessEngine.wtime : chessEngine.btime;
            int64_t increment     = board.sideToMove == ColorWhite ? chessEngine.winc  : chessEngine.binc;
            int movesLeft = chessEngine.movestogo ? chessEngine.movestogo : MAX_MOVES_REMAINING - board.moveNumber;
            if (movesLeft < MIN_MOVES_REMAINING) movesLeft = MIN_MOVES_REMAINING;
            if (remainingTime > TIME_SAFETY_BUFFER) remainingTime -= TIME_SAFETY_BUFFER;
            timeAllocated = (double)remainingTime / movesLeft + increment * 0.5;
            if (board.moveNumber > 10) timeAllocated *= CRITICAL_TIME_FACTOR;
            if (remainingTime < MIN_TIME_THRESHOLD) {
                //CAP, do not assign. Assigning made the allocation non-monotonic in the
                //clock: at 14999 ms remaining it handed out 5000 ms while at 15001 ms it
                //handed out 100 ms. std::min can only lower the figure.
                timeAllocated = std::min(timeAllocated, remainingTime * 0.5);
            }
            if (timeAllocated < 3000) timeAllocated = 100;
        }

        const int numberOfPieces = bitCount(board.side[ColorWhite] | board.side[ColorBlack]);
        if (numberOfPieces > 7) {
            start_search();
        } else if ((unsigned)numberOfPieces > TB_LARGEST) {
            // Between the local tablebases and 7 pieces, ask lichess.
            if (drop_ponder()) return;   // no need to query tablebases for the opponent
            char fenString[MAX_FEN_STRING_LEN];
            std::string fen_string(board2fen(board, fenString));
            while (!fen_string.empty() && std::isspace(fen_string.back())) fen_string.pop_back();
            for (size_t pos = fen_string.rfind(" "); pos != std::string::npos; pos = fen_string.rfind(" "))
                fen_string.replace(pos, 1, "_");
            std::string syzygy_tb_url = "http://tablebase.lichess.ovh/standard?fen=" + fen_string;
            int score_cp = 0;
            std::string uci_move = "";
            if (sendGetRequest(syzygy_tb_url, score_cp, uci_move)) {
                strncpy(best_move, uci_move.c_str(), 6);
                report_tb_move(score_cp);
            } else {
                log_file("run_go() error: failed to send Syzygy TB request to lichess\n");
                start_search();
            }
        } else {
            if (drop_ponder()) return;
            best_move[0] = '\0';
            unsigned int ep = legalEnPassantMove(board);
            unsigned int result = tb_probe_root(board.side[ColorWhite], board.side[ColorBlack],
                                                board.pieceTypes[King - 1], board.pieceTypes[Queen - 1],
                                                board.pieceTypes[Rook - 1], board.pieceTypes[Bishop - 1],
                                                board.pieceTypes[Knight - 1], board.pieceTypes[Pawn - 1],
                                                board.halfmoveClock, 0, ep == SquareNone ? 0 : ep,
                                                (board.sideToMove ^ 1) == ColorBlack ? 1 : 0, NULL);
            if (result == TB_RESULT_FAILED) {
                log_file("run_go() error: unable to probe tablebase; position invalid, illegal or not in tablebase, TB_LARGEST %d, numberOfPieces %d\n", TB_LARGEST, numberOfPieces);
                //A failed ROOT probe is not fatal. tb_probe_root is stricter than
                //tb_probe_wdl, so one missing, unreadable or rejected table used to kill
                //the whole process mid-game -- on lichess that is a forfeit.
                start_search();
                return;
            }
            unsigned int wdl = TB_GET_WDL(result); //0 - loss, 4 - win, 1..3 - draw
            int scorecp = 0;
            if (wdl == 4) scorecp = MATE_SCORE;
            else if (wdl == 0) scorecp = -MATE_SCORE;
            unsigned int from = TB_GET_FROM(result);
            unsigned int to   = TB_GET_TO(result);
            unsigned int promotes = TB_GET_PROMOTES(result);
            strncat(best_move, square[from], 2);
            strncat(best_move, square[to], 2);
            best_move[4] = uciPromoLetter[6 - promotes];
            best_move[5] = '\0';
            report_tb_move(scorecp);
        }
    }

    void start_search() {
        std::lock_guard<std::mutex> lock(mtx);
        searchFlag.store(true);
        stopFlag.store(false);
        cv.notify_all(); // Start search
    }

    bool drop_ponder() {
        if (!chessEngine.ponder) return false;
        if (chessEngine.infinite) chessEngine.infinite = false;
        chessEngine.ponder = false;
        return true;
    }

    void report_tb_move(int scorecp) {
        print("info depth 1 seldepth 1 multipv 1 score cp %d nodes 1 nps 1 hashfull 0 tbhits 1 time 0 pv %s\nbestmove %s\n", scorecp, best_move, best_move);
        log_file("info depth 1 seldepth 1 multipv 1 score cp %d nodes 1 nps 1 hashfull 0 tbhits 1 time 0 pv %s\nbestmove %s\n", scorecp, best_move, best_move);
        play(best_move);
    }
};

int main(int argc, char ** argv) {
    CreaticaEngine engine;
    return uci::uci_main(engine, argc, argv);
}
