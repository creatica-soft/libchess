#include <torch/torch.h>
#include <torch/script.h> // Necessary for tracing
#include <torch/csrc/jit/frontend/tracer.h> // REQUIRED for tracing in C++
#ifdef _MSC_VER
#include <mutex>
#endif

#ifdef __GNUC__ // g++ on Alpine Linux
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <condition_variable>
#include <queue>
#include <cstdarg>
#endif
#include <cassert>
#include <vector>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <random>
#include <chrono>
#include <algorithm>
#include <math.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#include <pthread/qos.h>
#include <pthread.h>
#include <dispatch/dispatch.h>
#include "tbprobe.h"
#include "json.hpp"
#include <curl/curl.h>
#include "nnue/bitboard.h"
#include "libchess.h"

#define THREADS 4
#define MULTI_PV 5
#define SYZYGY_PATH_DEFAULT "<empty>"
#define SYZYGY_PATH "/Users/ap/syzygy"
#define HASH 2048 //default, GUI may set it via Hash option (once full, expansion won't happen!)
#define EXPLORATION_MIN 70 // used in formular for exploration constant decay with depth
#define EXPLORATION_MAX 160 //smaller value favor exploitation, i.e. deeper tree vs wider tree
#define EXPLORATION_DEPTH_DECAY 5 //linear decay of EXPLORATION CONSTANT with depth using formula:
                      // C * 100 = max(EXPLORATION_MIN, (EXPLORATION_MAX - seldepth * EXPLORATION_DEPTH_DECAY))
#define PROBABILITY_MASS 99 //% - cumulative probability - how many moves we consider
// In your #defines or initialisation:
const double VIRTUAL_LOSS = 35;   // Penalty in centipawns – tuneable, start with 5
//double virtual_loss = std::tanh(VIRTUAL_LOSS / eval_scale);
#define EVAL_SCALE 600 //This is a divisor in W = tanh(eval/eval_scale) where eval is NNUE evaluation in pawns. 
                     //W is a fundamental value in Monte Carlo tree node along with N (number of visits) 
                     //and P (prior move probability), though P belongs to edges (same as move) but W and N to nodes.
#define TEMPERATURE 7 //used in calculating probabilities for moves in get_prob() using softmax:
                        // exp((eval - max_eval)/(temperature/100)) / eval_sum
                        //can be tuned so that values < 100 sharpen the distribution and values > 100 flatten it
#define BATCH_SIZE 2 * 8192
#define TIMEOUT_US 10 //2000
#define MAX_INFLIGHT 4 * 16384 // tuneable – start with 4× target batch
#define PV_PLIES 16
#define PONDER false
#define DISPLAY_INTERMITTENT_INFO_LINES true
#define DISPLAY_FINAL_INFO_LINES true
#define MAX_DEPTH 10000
extern double virtual_loss;   //NOT const: uci-kan.cpp:274/:669 set it from the VirtualLoss UCI option
extern const double eval_scale;
extern double temperature;    //NOT const: uci-kan.cpp:280/:671 set it from the Temperature UCI option
void log_file(const char * message, ...);
extern std::atomic<uint64_t> total_evals_completed;
extern std::atomic<uint64_t> total_batches;
extern std::atomic<int> inflight_count;

struct ChebyKANLayerImpl : torch::nn::Module {
    int in_features, out_features, degree;
    torch::Tensor weights;

    ChebyKANLayerImpl(int in, int out, int deg) 
        : in_features(in), out_features(out), degree(deg) {
        
        // 1. Create the tensor first
        auto w_tensor = torch::empty({out, in, deg + 1});
    
        // 2. Initialize it. 
        // Since this is 3D, we can't use xavier_uniform_ directly.
        // We use a small scaled distribution which is safer for KANs.
        float scale = std::sqrt(1.0 / (in * (deg + 1)));
        torch::nn::init::uniform_(w_tensor, -scale, scale);
    
        // 3. Register it as a parameter
        weights = register_parameter("weights", w_tensor);
    }

    torch::Tensor forward(torch::Tensor x) {
        int64_t batch_size = x.size(0);
        // x shape: [batch, in_features]
        // 1. Precompute the Chebyshev basis T_n(x)
        // We use a vector of tensors for the recurrence
        std::vector<torch::Tensor> T;
        T.push_back(torch::ones_like(x)); // T0 = 1
        T.push_back(x);                  // T1 = x

        for (int n = 2; n <= degree; ++n) {
            T.push_back(2.0 * x * T[n-1] - T[n-2]);
        }

        // 2. Stack basis into [batch, in_features, degree + 1]
        auto basis = torch::stack(T, -1);

        // 3. Compute the KAN output using einsum
        // 'bid' = batch, in_features, degree
        // 'oid' = out_features, in_features, degree
        // result 'bo' = batch, out_features
        //return torch::einsum("bid,oid->bo", {basis, weights});
        // Instead of einsum("bid,oid->bo", {basis, weights})
        auto flattened_basis = basis.view({batch_size, -1}); // [B, In * (Deg+1)]
        //Derive the width from the LOADED tensor, not from the constructor. torch::load()
        //resizes weights to whatever the checkpoint holds while out_features keeps the
        //constructed value, so a config/checkpoint mismatch used to reinterpret the data
        //into garbage shapes instead of failing: simple_bkan.pt is hidden=128 while the
        //engine constructs hidden=256, which produced "mat1 and mat2 shapes cannot be
        //multiplied (20x2880 and 1440x256)" from deep inside matmul.
        auto flattened_weights = weights.view({weights.size(0), -1}).transpose(0, 1); // [In * (Deg+1), Out]
        return torch::matmul(flattened_basis, flattened_weights);
    }
};
TORCH_MODULE(ChebyKANLayer);

struct ChessChebyKANImpl : torch::nn::Module {
    ChebyKANLayer layer1{nullptr}, layer2{nullptr};
    torch::nn::Linear value_head{nullptr};

    ChessChebyKANImpl(int in_features = 576, int hidden = 256, int degree = 4) {
        layer1 = register_module("layer1", ChebyKANLayer(in_features, hidden, degree));
        layer2 = register_module("layer2", ChebyKANLayer(hidden, 32, degree));
        
        // Final mapping to a single scalar
        value_head = register_module("value_head", torch::nn::Linear(32, 1));
    }

    torch::Tensor forward(torch::Tensor x) {
        x = x.view({x.size(0), -1}); 
        
        x = layer1->forward(x);
        x = layer2->forward(x);
        
        // Final value output
        auto out = value_head->forward(x);
        
        // Return tanh to keep it in the [-1, 1] range for your centipawn conversion
        return torch::tanh(out);
    }
};
TORCH_MODULE(ChessChebyKAN);
extern ChessChebyKAN model;

struct ThreadParams {
    int thread_id;
    unsigned long long time_alloc;
    int seldepth;
    std::mt19937 mt;
};
struct Edge;
struct MCTSNode {
    std::atomic<uint64_t> hash{0};
    std::atomic<uint64_t> N{0};  // Atomic for lock-free updates
    std::atomic<double> W{0};
    std::atomic<int> cp {0}; //position evaluation in centipawns 
    std::atomic<int> num_children{0};
    std::atomic<int> pending_evals{0};
    std::atomic<int> generation{0};
    std::atomic<int> terminal{0}; //0 (pending), 1 (mate), 2 (stalemate), 3 (repetition), 4 (evaluated)
    std::shared_mutex mutex;  // For protecting children expansion
    std::atomic<Edge *> children {nullptr}; //array of moves and priors leading to next nodes
    //std::atomic<MCTSNode *> parent {nullptr};
    //std::atomic<int> in_flight{0}; // number of pending evals in this subtree
};
// Custom hasher that uses the key directly
struct NoOpHash {
    std::size_t operator()(unsigned long long key) const noexcept {
        return key; // Directly use the key as the hash
    }
};
struct MCTSSearch {
    MCTSNode * root = nullptr;
    std::unordered_map<unsigned long long, MCTSNode *, NoOpHash> tree; //Zobrist hash and node 
};
extern MCTSSearch search;

struct Edge {
    std::atomic<int> move {0};             // The move that leads to the child position
    std::atomic<double> P {0.0};            // Prior probability - model move_probs for a given move in the node
    std::atomic<struct MCTSNode *> child {nullptr}; // Pointer to the child node
};

struct PositionDedup {
    static constexpr size_t BUCKETS = 256;
    
    struct Bucket {
        alignas(64) std::mutex mtx;
        std::unordered_set<uint64_t> hashes;
    } buckets[BUCKETS];
    
    // Returns true if newly inserted (not a duplicate)
    bool insert(uint64_t hash) {
        auto& bucket = buckets[hash >> 56]; // top 8 bits
        std::lock_guard<std::mutex> lock(bucket.mtx);
        return bucket.hashes.insert(hash).second;
    }
    
    void remove(uint64_t hash) {
        auto& bucket = buckets[hash >> 56];
        std::lock_guard<std::mutex> lock(bucket.mtx);
        bucket.hashes.erase(hash);
    }
    
    void clear() {
        for (auto& bucket : buckets) {
            std::lock_guard<std::mutex> lock(bucket.mtx);
            bucket.hashes.clear();
        }
    }
};

extern PositionDedup pos_dedup;

// Ensure the Slot is aligned to avoid performance degradation
struct alignas(128) Slot {
    // Sequence acts as a 'Ready' flag and a wrap-around protector
    // Even = Empty/Processing, Odd = Ready to Read (or vice-versa)
    std::atomic<size_t> sequence{0}; 
    MCTSNode* node = nullptr;
    Board board;
    std::vector<MCTSNode *> path;
};

template<size_t Size>
struct EvalQueue {
    EvalQueue() {
        for (size_t i = 0; i < Size; ++i) {
            buffer[i].sequence.store(i, std::memory_order_relaxed);
        }
    }
    // Size must be a power of 2 for the bitwise mask (Size - 1)
    static_assert((Size & (Size - 1)) == 0, "Buffer size must be a power of 2");

    Slot buffer[Size];
    const size_t mask = Size - 1;
    
    alignas(128) std::atomic<size_t> head{0}; // Producer index
    alignas(128) std::atomic<size_t> tail{0}; // Consumer index

    // Search Threads: Push the Board state reached at the leaf
    void push(MCTSNode* node, const Board& board, const std::vector<MCTSNode*>& path) {
        size_t h = head.load(std::memory_order_relaxed);
        int spin_count = 0; 
        
        while (true) {
            auto& slot = buffer[h & mask];
            size_t seq = slot.sequence.load(std::memory_order_acquire);
            
            if (seq == h) {
                if (head.compare_exchange_weak(h, h + 1, std::memory_order_relaxed)) {
                    slot.node = node;
                    slot.board = board; 
                    slot.path = path;
                    slot.sequence.store(h + 1, std::memory_order_release);
                    return; // Void return type, push now guarantees success
                }
            } else if (seq < h) {
                // Buffer is full (Producer is outrunning prediction server)
                if (spin_count < 16) {
                    __builtin_arm_yield(); // Hardware yield (keep core active but polite)
                } else {
                    std::this_thread::yield(); // OS yield (give up time slice)
                }
                spin_count++;
                
                // Refresh head before retrying
                h = head.load(std::memory_order_relaxed); 
            } else {
                // Another thread claimed the slot
                h = head.load(std::memory_order_relaxed);
            }
        }
    }
    // Prediction Server: Pop as many as possible into a batch
    size_t pop_batch(std::vector<MCTSNode*>& nodes, std::vector<Board>& boards, std::vector<std::vector<MCTSNode *>>& paths, size_t max_batch) {
        size_t t = tail.load(std::memory_order_relaxed);
        size_t count = 0;

        while (count < max_batch) {
            auto& slot = buffer[t & mask];
            size_t seq = slot.sequence.load(std::memory_order_acquire);
            
            // If sequence == t + 1, it means a producer has finished writing
            if (seq == t + 1) {
                if (tail.compare_exchange_weak(t, t + 1, std::memory_order_relaxed)) {
                    nodes.push_back(slot.node);
                    boards.push_back(slot.board);
                    paths.push_back(slot.path);
                    
                    // Set sequence to t + Size to mark it ready for the NEXT wrap-around producer
                    slot.sequence.store(t + Size, std::memory_order_release);
                    t++;
                    count++;
                }
            } else {
                // No more ready slots
                break;
            }
        }
        return count;
    }
};


class PredictionServer {
public:
    const size_t batch_size = BATCH_SIZE;
    const int timeout = TIMEOUT_US; 

    PredictionServer(EvalQueue<65536>& q, ChessChebyKAN m) 
        : queue(q), model(m) {}

    void quit() {
      running.store(false, std::memory_order_relaxed);
    }
    
    void run() {
        maximize_performance(); 
        set_affinity(2); 

        // Allocate CPU tensor [Batch, Channels, H, W]
        auto options = torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCPU);
        torch::Tensor cpu_tensor = torch::empty({(long)batch_size, 9, 8, 8}, options);

        std::vector<MCTSNode*> nodes;
        std::vector<Board> boards;
        std::vector<std::vector<MCTSNode*>> paths;
        nodes.reserve(batch_size);
        boards.reserve(batch_size);
        paths.reserve(batch_size);

        while (running.load(std::memory_order_relaxed)) {
            nodes.clear();
            boards.clear();
            paths.clear();

            auto start_time = std::chrono::steady_clock::now();
            int spin_count = 0;
            
            while (nodes.size() < batch_size) {
                size_t popped = queue.pop_batch(nodes, boards, paths, batch_size - nodes.size());
                
                if (popped == 0) {
                    if (!nodes.empty()) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start_time).count();
                        if (elapsed > timeout) break; // Time's up, process what we have
                    }
                    
                    // Wait politely instead of burning CPU
                    if (spin_count < 16) {
                        __builtin_arm_yield();
                    } else {
                        std::this_thread::yield();
                    }
                    spin_count++;
                } else {
                    spin_count = 0; // Reset spin count on successful pop
                }
            }    

            // 1. Parallel Flatten directly into the CPU tensor memory
            torch::Tensor current_batch_tensor = cpu_tensor.slice(0, 0, (long)nodes.size());
            parallel_flatten(boards, current_batch_tensor);


            {   //Report the ACTUAL batch size reached. TIMEOUT_US gates collection, so the
                //configured BATCH_SIZE is an upper bound the server may never approach -
                //and per-position model cost depends almost entirely on this number.
                uint64_t b = total_batches.fetch_add(1) + 1;
                if ((b & 0x3F) == 0)
                    fprintf(stderr, "info string batches %llu mean_batch %.1f\n",
                            (unsigned long long)b, (double)total_evals_completed.load() / (double)b);
            }
            //printf("Batch %llu: size %zu, queue depth est %zu\n", total_batches.load(), nodes.size(), (queue.head.load() - queue.tail.load()) & queue.mask);

            // 2. GPU Inference (MPS)
            torch::NoGradGuard no_grad;
            auto gpu_input = current_batch_tensor.to(torch::kMPS).to(torch::kHalf); 
            torch::Tensor output = model->forward(gpu_input).to(torch::kFloat32); // forward returns IValue
            
            auto cpu_output = output.to(torch::kCPU);
            float* results_ptr = cpu_output.data_ptr<float>();

            // 3. Update the MCTS Tree
            size_t num_nodes = nodes.size();
            for (size_t i = 0; i < num_nodes; ++i) {
                MCTSNode* leaf = nodes[i];
                std::vector<MCTSNode*> path = paths[i];
                
                // KAN output is usually tanh [-1, 1]. Map to centipawns.
                double eval = std::clamp(results_ptr[i], -0.999f, 0.999f);
                int cp = static_cast<int>(std::atanh(eval) * eval_scale);
                //printf("cp %d\n", cp);
                leaf->cp.store(cp, std::memory_order_relaxed);
                
                if (!path.empty()) {
                    auto* parent = path.back();
                    if (parent->pending_evals.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        finalize_priors(parent); 
                    }
                }
                path.push_back(leaf);
                backpropagate(eval, path); 
                inflight_count.fetch_sub(1, std::memory_order_release);
                leaf->terminal.store(4, std::memory_order_release);
                pos_dedup.remove(leaf->hash.load(std::memory_order_relaxed));
            }
        }
    }

private:
    EvalQueue<65536>& queue;
    ChessChebyKAN model;
    std::atomic<bool> running{true};

    void parallel_flatten(const std::vector<Board>& batch, torch::Tensor& out_tensor) {
        size_t batch_size = batch.size();
        float* base_tensor_ptr = out_tensor.data_ptr<float>();
        dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0);

        dispatch_apply(batch_size, q, ^(size_t i) {
            Board& chess_board = const_cast<Board&>(batch[i]);// Use index i directly
            //Board chess_board = batch[i]; //just for testing
            float* board_ptr = base_tensor_ptr + (i * 576);
            std::memset(board_ptr, 0, 576 * sizeof(float));
            bool is_black = (chess_board.sideToMove == ColorBlack);
        	  auto [king_moves, pinned, pinning, checkers, kingSquare] = kingMoves(chess_board);
            uint64_t all_legal_moves = king_moves;
            int num_checkers = checkers ? bitCount(checkers) : 0;
            auto [check_mask, ep_mask] = checkers ? checkMask(chess_board, kingSquare, checkers) : std::make_pair(0xffffffffffffffffULL, 0ULL);
        
            if (num_checkers <= 1) {
              // Loop through friendly pieces to gather their moves
              for (PieceType pt = Pawn; pt <= Queen; ++pt) {
                uint64_t occupations = chess_board.side[is_black ? ColorBlack : ColorWhite] & chess_board.pieceTypes[pt - 1]; 
                while (occupations) {
                  const Square sq = popLSB(occupations);
                  all_legal_moves |= piece_moves(chess_board, pt, sq, kingSquare, pinned, pinning, check_mask, ep_mask);
                }
              }
            }       
        
            uint64_t any = occupations(chess_board);
            const float weights[] = {0.0f, 1.0f, 3.0f, 3.0f, 5.0f, 9.0f, 10.0f};
            // 3. Temporary accumulators for attacks
            float net_tension[64] = {0.0f};
            float total_volume[64] = {0.0f};
            
            for(int sq = 0; sq < 64; ++sq) {
                int input_sq = is_black ? (sq ^ 56) : sq;
                Piece pc = static_cast<Piece>(chess_board.piecesOnSquares[sq]);
        
                if (pc != PieceNone) {
                    Color color = PC_COLOR(pc);
                    PieceType type = PC_TYPE(pc);
                    
                    // Bipolar Pieces: +1 for Friendly, -1 for Enemy
                    float val = (color == chess_board.sideToMove) ? 1.0f : -1.0f;
                    int pieceChannel = type - 1; 
                    board_ptr[(pieceChannel << 6) | input_sq] = val;
        
                    // Attack Logic
                    uint64_t occ = any;
                    if (type >= Bishop && type <= Queen) {
                        occ ^= (chess_board.side[1 - color] & chess_board.pieceTypes[King - 1]);
                    }
        
                    uint64_t pieceAttacks = Stockfish::attacks_bb(static_cast<Stockfish::Piece>(PC(color,type)), static_cast<Stockfish::Square>(sq), occ);
        
                    float w = weights[type];
                    while (pieceAttacks) {
                        int atk_sq = popLSB(pieceAttacks);
                        if (color == chess_board.sideToMove) {
                            net_tension[atk_sq] += w;
                        } else {
                            net_tension[atk_sq] -= w;
                        }
                        total_volume[atk_sq] += w;
                    }
                }
            }
        
            // 4. Finalize Attack Planes with Normalization
            for(int sq = 0; sq < 64; ++sq) {
                int input_sq = is_black ? (sq ^ 56) : sq;
                
                // Net Tension: tanh(sum/5.0) -> distinguishes 1-2 attackers clearly
                board_ptr[(6 << 6) | input_sq] = std::tanh(net_tension[sq] / 5.0f);
                
                // Total Volume: tanh(sum/10.0) -> captures overall chaos/tension
                board_ptr[(7 << 6) | input_sq] = std::tanh(total_volume[sq] / 10.0f);
        
                // Legal Moves
                if ((all_legal_moves >> sq) & 1) {
                    board_ptr[(8 << 6) | input_sq] = 1.0f;
                }
            }
        });
    }
    
    void finalize_priors(MCTSNode* parent) {
        const size_t n = parent->num_children.load(std::memory_order_relaxed);
        if (n == 0) return;

        double evals[256]; 
        double max_val = -std::numeric_limits<double>::infinity();

        // Edge array is stored as a pointer in parent
        auto* edges = parent->children.load(std::memory_order_acquire);

        for (size_t i = 0; i < n; ++i) {
            MCTSNode* child = edges[i].child.load(std::memory_order_relaxed);
            double e = -static_cast<double>(child->cp.load(std::memory_order_relaxed)) * 0.01;
            evals[i] = e;
            if (e > max_val) max_val = e;
        }

        double total = 0.0;
        for (size_t i = 0; i < n; ++i) {
            evals[i] = std::exp((evals[i] - max_val) / temperature);
            total += evals[i];
        }

        double inv_total = (total > 0.0) ? (1.0 / total) : 0.0;
        
        for (size_t i = 0; i < n; ++i) {
            double prob = (inv_total == 0.0) ? (1.0f / n) : (double)(evals[i] * inv_total);
            edges[i].P.store(prob, std::memory_order_release);
        }
        parent->terminal.store(4, std::memory_order_release);
    }
    
    void backpropagate(double eval, const std::vector<MCTSNode*>& path) {
        for (auto n = path.rbegin(); n != path.rend(); ++n) {
            auto curr = *n;
            curr->W.fetch_add(virtual_loss + eval, std::memory_order_relaxed);
            eval = -eval;
        }
        search.root->W.fetch_sub(virtual_loss, std::memory_order_release);
        search.root->N.fetch_add(1, std::memory_order_release);
        total_evals_completed.fetch_add(1);
    }    

    void set_affinity(int tag) {
        thread_affinity_policy_data_t policy = { tag };
        thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_AFFINITY_POLICY, (thread_policy_t)&policy, 1);
    }

    void maximize_performance() {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    }
};

void runMCTS(EvalQueue<65536>& queue);
void cleanup(); //free Hash tree
void mcts_search(ThreadParams& params, EvalQueue<65536>& queue);
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
void handleEval(void); //NNUE eval trace
void init_thread_pool(int num_threads, EvalQueue<65536>& queue);
void log_file(const char * message, ...);
void print(const char * message, ...);
bool sendGetRequest(const std::string& url, int& scorecp, std::string& uci_move);
int expand(MCTSNode * node, Board& chess_board, const ZobristHash& board_hash, const std::unordered_set<uint64_t>& pos_history, EvalQueue<65536>& queue, const std::vector<MCTSNode*>& path);


