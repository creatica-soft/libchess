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
//MAX_INFLIGHT throttles queue ITEMS, but an item means different things in the two prior
//modes: roughly 29 items per expansion with child evaluations, exactly ONE with policy
//priors. Left at 65536 the policy path runs 65536 expansions ahead of any value coming
//back -- 29x child mode's lookahead. Measured consequence: a backlog of 47,000 items that
//would not drain in 5 seconds, the engine silent long enough for the tournament driver to
//declare it dead, and a search expanding blind because every child still sits at its
//first-play-urgency value. Throttle pending EXPANSIONS instead, matching what child mode
//already allows (65536 / ~29).
#define MAX_INFLIGHT_POLICY 2048
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
        // Bound the input before building the Chebyshev basis -- MUST match the trainer.
        // Without it T_8 of layer 1's unbounded output reaches ~8e6 and fp16 inference
        // overflows outright. See dual_head_kan_train.cpp for the full reasoning.
        x = torch::tanh(x);

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

#ifndef MAX_TRAIN_PVS
#define MAX_TRAIN_PVS MAX_PVS
#endif

#ifndef POLICY_TEMP
#define POLICY_TEMP 1.5
#endif

// MUST match dual_head_kan_train.cpp. Width is also derived from the loaded tensor in
// ChebyKANLayerImpl, but DEGREE is not -- it comes from the constructor and decides how
// many basis terms are built, so a mismatch reshapes the weights into garbage.
// -DCONV_POLICY=1 replaces the bilinear from/to table with an AlphaZero-style directional
// head: a move is (from-square, plane) where the 73 planes are 8 directions x 7 distances,
// 8 knight moves and 9 underpromotions. Two advantages over the bilinear table. It is
// cheaper -- 118k MACs/position against 494k, once the trunk context is projected down
// before being broadcast to every square. And it shares weights across the board: a rook
// going e1-e8 and one going a1-a8 are the same "north 7", where the bilinear table has to
// learn them as two unrelated entries in a 64x64 grid.
#ifndef CONV_POLICY
#define CONV_POLICY 0
#endif
// Width the 64-wide trunk context is projected to before per-square broadcast. Broadcasting
// all 64 is what makes the conv head only 31% cheaper instead of 4x.
#ifndef POLICY_CTX
#define POLICY_CTX 16
#endif

#if CONV_POLICY
static constexpr int POLICY_PLANES = 73;
static constexpr int POLICY_OUT    = 64 * POLICY_PLANES;   // 4672
#else
static constexpr int POLICY_OUT    = 64 * 64;              // 4096
#endif

// Plane index for a move in the SIDE-TO-MOVE-ORIENTED frame; -1 if it has no queen/knight
// shape. Verified exhaustively: 1792 shaped moves over all 64 squares, no collisions.
static inline int move_plane(int from_i, int to_i, int promo) {
    const int dr = (to_i >> 3) - (from_i >> 3);
    const int df = (to_i & 7)  - (from_i & 7);
    if (promo == Knight || promo == Bishop || promo == Rook) {   // queen promo uses a queen plane
        const int dirp = (df == 0) ? 0 : (df < 0 ? 1 : 2);
        const int pcp  = (promo == Knight) ? 0 : (promo == Bishop ? 1 : 2);
        return 64 + pcp * 3 + dirp;
    }
    static const int kn[8][2] = {{2,1},{1,2},{-1,2},{-2,1},{-2,-1},{-1,-2},{1,-2},{2,-1}};
    for (int i = 0; i < 8; ++i) if (dr == kn[i][0] && df == kn[i][1]) return 56 + i;
    static const int dir[8][2] = {{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1}};
    for (int i = 0; i < 8; ++i)
        for (int d = 1; d <= 7; ++d)
            if (dr == dir[i][0]*d && df == dir[i][1]*d) return i*7 + (d-1);
    return -1;
}

// Index of a move in the policy output, oriented squares in, -1 if unrepresentable.
static inline int policy_index(int from_i, int to_i, int promo) {
#if CONV_POLICY
    const int pl = move_plane(from_i, to_i, promo);
    return pl < 0 ? -1 : from_i * POLICY_PLANES + pl;
#else
    (void)promo;
    return from_i * 64 + to_i;
#endif
}

#ifndef KAN_HIDDEN
#define KAN_HIDDEN 256
#endif
#ifndef KAN_DEGREE
#define KAN_DEGREE 8
#endif

#ifndef POLICY_DIM
#define POLICY_DIM 32
#endif
// Width of the attention layer's per-square representation; follows POLICY_DIM unless set.
#ifndef ATTN_DIM
#define ATTN_DIM POLICY_DIM
#endif

// Self-attention over the 64 squares. DEFAULT ON: it is the only large effect measured
// here -- Top-6 67.5% with it against 58.6% without, on the same shard, against a noise
// floor of 0.8 points. Costs 14,176 parameters and about 23 microseconds per position.
// Build with -DUSE_ATTENTION=0 for the ablation.
#ifndef USE_ATTENTION
#define USE_ATTENTION 1
#endif

#if USE_ATTENTION
// One self-attention layer over the 64 squares, so a square can fold in what is on the
// other 63 before the bilinear head scores pairs. Without it each square sees only its
// own 9 planes plus a shared position summary, so the head cannot know that the squares
// between a rook and its target are empty, or that a bishop covers the destination.
//
// Chess is a 64-token sequence, which is why this is affordable: the 64x64 attention
// matrix that makes transformers expensive on language is trivial here.

struct SquareAttentionImpl : torch::nn::Module {
    torch::Tensor pos_emb, Win, bin_, Wq, Wk, Wv, Wo, W1, b1, W2, b2;
    int d, hidden;
    SquareAttentionImpl(int in_feats, int d_ = 32, int hidden_ = 64) : d(d_), hidden(hidden_) {
        auto u = [](std::initializer_list<int64_t> shp, int fan) {
            float sc = std::sqrt(1.0f / fan);
            return torch::empty(shp).uniform_(-sc, sc);
        };
        // a1 and h8 are not interchangeable, so each square gets a learned identity
        pos_emb = register_parameter("pos_emb", 0.02f * torch::randn({64, d_}));
        Win  = register_parameter("Win",  u({in_feats, d_}, in_feats));
        bin_ = register_parameter("bin",  torch::zeros({d_}));
        Wq   = register_parameter("Wq",   u({d_, d_}, d_));
        Wk   = register_parameter("Wk",   u({d_, d_}, d_));
        Wv   = register_parameter("Wv",   u({d_, d_}, d_));
        Wo   = register_parameter("Wo",   u({d_, d_}, d_));
        W1   = register_parameter("W1",   u({d_, hidden_}, d_));
        b1   = register_parameter("b1",   torch::zeros({hidden_}));
        W2   = register_parameter("W2",   u({hidden_, d_}, hidden_));
        b2   = register_parameter("b2",   torch::zeros({d_}));
    }
    // x [B,64,in_feats] -> [B,64,d]
    torch::Tensor forward(torch::Tensor x) {
        auto z = torch::matmul(x, Win) + bin_ + pos_emb;
        z = torch::layer_norm(z, {d});
        auto q = torch::matmul(z, Wq);
        auto k = torch::matmul(z, Wk);
        auto v = torch::matmul(z, Wv);
        auto a = torch::softmax(torch::matmul(q, k.transpose(1, 2))
                                / std::sqrt(static_cast<float>(d)), -1);
        z = z + torch::matmul(torch::matmul(a, v), Wo);
        z = torch::layer_norm(z, {d});
        z = z + torch::matmul(torch::relu(torch::matmul(z, W1) + b1), W2) + b2;
        return torch::layer_norm(z, {d});
    }
};
TORCH_MODULE(SquareAttention);
#endif   // USE_ATTENTION

#if CONV_POLICY
// A 1x1 convolution over the 8x8 board IS a per-square linear map, so this is a matmul --
// same arithmetic, none of the conv-kernel overhead.
struct ConvPolicyHeadImpl : torch::nn::Module {
    torch::Tensor Wc, bc, Wp, bp;
    ConvPolicyHeadImpl(int plane_feats, int ctx, int ctx_out) {
        auto u = [](std::initializer_list<int64_t> shp, int fan) {
            float sc = std::sqrt(1.0f / fan);
            return torch::empty(shp).uniform_(-sc, sc);
        };
        Wc = register_parameter("Wc", u({ctx, ctx_out}, ctx));           // squeeze the context once
        bc = register_parameter("bc", torch::zeros({ctx_out}));
        const int in = plane_feats + ctx_out;
        Wp = register_parameter("Wp", u({in, POLICY_PLANES}, in));       // per-square -> 73 planes
        bp = register_parameter("bp", torch::zeros({POLICY_PLANES}));
    }
    // x_sq [B,64,plane_feats], ctx [B,ctx] -> [B, 64*73]
    torch::Tensor forward(torch::Tensor x_sq, torch::Tensor ctx) {
        const int64_t B = x_sq.size(0);
        auto c = torch::tanh(torch::matmul(torch::tanh(ctx), Wc) + bc);  // [B,ctx_out]
        auto h = torch::cat({x_sq, c.unsqueeze(1).expand({B, 64, c.size(1)})}, 2);
        auto s = torch::matmul(h, Wp) + bp;                              // [B,64,73]
        return s.reshape({B, 64 * POLICY_PLANES});
    }
};
TORCH_MODULE(ConvPolicyHead);
#endif

struct BilinearPolicyHeadImpl : torch::nn::Module {
    torch::Tensor Wf, bf, Wt, bt, Wb;
    int d;
    BilinearPolicyHeadImpl(int plane_feats = 9, int ctx = 64, int d_ = 32) : d(d_) {
        const int in = plane_feats + ctx;
        float sc = std::sqrt(1.0f / in);
        Wf = register_parameter("Wf", torch::empty({in, d}).uniform_(-sc, sc));
        bf = register_parameter("bf", torch::zeros({d}));
        Wt = register_parameter("Wt", torch::empty({in, d}).uniform_(-sc, sc));
        bt = register_parameter("bt", torch::zeros({d}));
        // start near identity so the head begins as a plain dot product between the
        // two square embeddings rather than as noise
        Wb = register_parameter("Wb", torch::eye(d) + 0.01f * torch::randn({d, d}));
    }
    // x_sq [B,64,plane_feats], ctx [B,64] -> [B,4096] logits, index = from*64 + to
    torch::Tensor forward(torch::Tensor x_sq, torch::Tensor ctx) {
        const int64_t B = x_sq.size(0);
        // The trunk's output is an unbounded matmul, so squash it before the head reads
        // it, and scale the bilinear product by 1/sqrt(d) the way attention does. Both
        // keep the head in a sane starting range.
        auto c = torch::tanh(ctx).unsqueeze(1).expand({B, 64, ctx.size(1)});
        auto h = torch::cat({x_sq, c}, 2);                     // [B,64,in]
        auto U = torch::matmul(h, Wf) + bf;                    // [B,64,d]
        auto V = torch::matmul(h, Wt) + bt;                    // [B,64,d]
        auto S = torch::matmul(torch::matmul(U, Wb), V.transpose(1, 2))
                 / std::sqrt(static_cast<float>(d));           // [B,64,64]
        return S.reshape({B, 64 * 64});
    }
};
TORCH_MODULE(BilinearPolicyHead);

struct ChessChebyKANImpl : torch::nn::Module {
    ChebyKANLayer layer1{nullptr}, layer2{nullptr};
    torch::nn::Linear value_head{nullptr};
#if CONV_POLICY
    ConvPolicyHead policy{nullptr};
#else
    BilinearPolicyHead policy{nullptr};
#endif
#if USE_ATTENTION
    SquareAttention attn{nullptr};
#endif

    ChessChebyKANImpl(int in_features = 576, int hidden = KAN_HIDDEN, int degree = KAN_DEGREE) {
        layer1     = register_module("layer1", ChebyKANLayer(in_features, hidden, degree));
        layer2     = register_module("layer2", ChebyKANLayer(hidden, 64, degree));
        value_head = register_module("value_head", torch::nn::Linear(64, 1));
#if USE_ATTENTION
        // attention consumes the 9 planes plus the broadcast context and emits 32/square
        attn       = register_module("attn",   SquareAttention(9 + 64, ATTN_DIM));
        policy     = register_module("policy", BilinearPolicyHead(ATTN_DIM, 64, POLICY_DIM));
#else
#if CONV_POLICY
        policy     = register_module("policy", ConvPolicyHead(9, 64, POLICY_CTX));
#else
        policy     = register_module("policy", BilinearPolicyHead(9, 64, POLICY_DIM));
#endif
#endif
    }

    // x [B,576] -> { value [B,1] (pre-tanh), policy logits [B,4096] }
    std::pair<torch::Tensor, torch::Tensor> forward(torch::Tensor x) {
        const int64_t B = x.size(0);
        auto flat = x.view({B, -1});
        auto ctx  = layer2->forward(layer1->forward(flat));    // [B,64] position context
        auto value = value_head->forward(ctx);                 // [B,1]
        // The 576 inputs are already 9 planes x 64 squares -- data[(plane << 6) | sq] --
        // so per-square features come free with a reshape, no convolution needed.
        auto x_sq = flat.view({B, 9, 64}).transpose(1, 2).contiguous();  // [B,64,9]
#if USE_ATTENTION
        auto ctx_b = torch::tanh(ctx).unsqueeze(1).expand({B, 64, ctx.size(1)});
        x_sq = attn->forward(torch::cat({x_sq, ctx_b}, 2));              // [B,64,32]
#endif
        return {value, policy->forward(x_sq, ctx)};
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
    //terminal is the terminal TYPE only: 0 (not terminal), 1 (mate), 2 (stalemate).
    //It used to double as an "evaluated" flag by taking the value 4, which meant a
    //mate node (terminal == 1) could never satisfy a `>= 4` test and was treated as
    //permanently unevaluated - so mates were skipped in PV reporting and the root
    //wait loop would spin forever on a terminal root. Those are separate facts now.
    std::atomic<int> terminal{0};
    std::atomic<bool> evaluated{false}; //a usable value/priors have been stored
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
//When true, expand() pushes the PARENT once and priors come from the policy head instead
//of from one network evaluation per child. Both paths live in the same binary so the A/B
//is a single option flip.
extern bool use_policy_priors;

// Ensure the Slot is aligned to avoid performance degradation
struct alignas(128) Slot {
    // Sequence acts as a 'Ready' flag and a wrap-around protector
    // Even = Empty/Processing, Odd = Ready to Read (or vice-versa)
    std::atomic<size_t> sequence{0}; 
    MCTSNode* node = nullptr;
    Board board;
    std::vector<MCTSNode *> path;
    //Which protocol this item was pushed under. Must be carried per-item, not read from
    //a global at consume time: the option is written unsynchronised from the UCI thread
    //and nothing drains the queue on stop, so an item pushed as a child could otherwise
    //be consumed as a parent -- different path length, different write-back.
    bool policy = false;
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
    void push(MCTSNode* node, const Board& board, const std::vector<MCTSNode*>& path,
              bool policy = false) {
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
                    slot.policy = policy;          //before the release store below
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
    size_t pop_batch(std::vector<MCTSNode*>& nodes, std::vector<Board>& boards,
                     std::vector<std::vector<MCTSNode *>>& paths,
                     std::vector<uint8_t>& kinds, size_t max_batch) {
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
                    kinds.push_back(slot.policy ? 1 : 0);   //after the acquire load above
                    
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
        std::vector<uint8_t> kinds;   //1 = policy item (a parent), 0 = child
        kinds.reserve(batch_size);

        while (running.load(std::memory_order_relaxed)) {
            nodes.clear();
            boards.clear();
            paths.clear();
            kinds.clear();

            auto start_time = std::chrono::steady_clock::now();
            int spin_count = 0;
            
            //`running` must be tested HERE too, not only by the outer loop: the timeout
            //break below sits inside `if (!nodes.empty())`, so an idle server - queue
            //empty and nothing buffered, which is exactly the state at shutdown - spun
            //in here forever and never returned to re-read `running`. quit() then never
            //took effect and server_thread.join() blocked for good.
            while (nodes.size() < batch_size && running.load(std::memory_order_relaxed)) {
                size_t popped = queue.pop_batch(nodes, boards, paths, kinds, batch_size - nodes.size());
                
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

            //Collection can now end with nothing buffered - quit() breaks the inner loop
            //immediately. Feeding an empty batch to the model throws
            //"cannot reshape tensor of 0 elements into shape [0, -1]".
            if (nodes.empty()) continue;

            // 1. Parallel Flatten directly into the CPU tensor memory
            torch::Tensor current_batch_tensor = cpu_tensor.slice(0, 0, (long)nodes.size());
            parallel_flatten(boards, current_batch_tensor);


            {   //Report the ACTUAL batch size reached. TIMEOUT_US gates collection, so the
                //configured BATCH_SIZE is an upper bound the server may never approach -
                //and per-position model cost depends almost entirely on this number.
                uint64_t b = total_batches.fetch_add(1) + 1;
                //Opt-in: BATCH_STATS=1. The configured BATCH_SIZE is only an upper bound -- what
                //matters is the batch the server actually assembles, and that depends on how fast
                //workers produce items. Policy priors emit ONE item per expansion where child
                //evaluations emit ~29, so the achieved batch can be far smaller, and per-position
                //GPU cost depends almost entirely on it.
                static const bool stats = std::getenv("BATCH_STATS") != nullptr;
                if (stats && (b & 0x3F) == 0)
                    fprintf(stderr, "info string batches %llu mean_batch %.1f\n",
                            (unsigned long long)b, (double)total_evals_completed.load() / (double)b);
            }
            //printf("Batch %llu: size %zu, queue depth est %zu\n", total_batches.load(), nodes.size(), (queue.head.load() - queue.tail.load()) & queue.mask);

            // 2. GPU Inference (MPS)
            torch::NoGradGuard no_grad;
            auto gpu_input = current_batch_tensor.to(torch::kMPS).to(torch::kHalf); 
            // The dual-head model returns { value (PRE-tanh), policy logits [B,4096] }.
            // The old single-head model applied tanh inside forward(); everything downstream
            // expects the bounded value, so apply it at this boundary instead.
            auto [value_raw, policy_logits] = model->forward(gpu_input);
            torch::Tensor output = torch::tanh(value_raw).to(torch::kFloat32);
            //Only pay the [B,4096] copy when the batch actually holds a policy item:
            //16 KB per position, 32 MB at B=2048. Masking cannot be done on-device here --
            //inference runs in kHalf and -1e9 is infinity in fp16 -- so the softmax over
            //legal moves happens on the CPU in policy_priors().
            bool any_policy = false;
            for (uint8_t k : kinds) if (k) { any_policy = true; break; }
            torch::Tensor cpu_policy;
            const float* pol = nullptr;
            if (any_policy) {
                cpu_policy = policy_logits.to(torch::kFloat32).to(torch::kCPU).contiguous();
                pol = cpu_policy.data_ptr<float>();
            }
            
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
                //Never overwrite a child proven terminal at expansion time - its score
                //is exact and the network's guess is not.
                if (leaf->terminal.load(std::memory_order_acquire) == 0)
                    leaf->cp.store(cp, std::memory_order_relaxed);
                
                if (i < kinds.size() && kinds[i]) {
                  //POLICY ITEM: `leaf` is the node that was expanded, not one of its children, and
                  //`path` ALREADY ends with it (mcts_search pushes it at creatica-kan.cpp:516 before
                  //calling expand). Do not push it again: backpropagate walks in reverse flipping
                  //sign each step, so a duplicated leaf inverts the sign for EVERY ancestor and
                  //leaks a second virtual loss into W on every simulation -- silent, and the engine
                  //would play the moves it thinks are worst.
                  if (pol) policy_priors(leaf, boards[i].sideToMove == ColorBlack,
                                         pol + (size_t)i * 4096u);
                  //Priors BEFORE the gate: runMCTS spins on root->evaluated and releases every
                  //worker the instant it clears. Open it on zero priors and select_best_child
                  //returns child 0 forever.
                  leaf->evaluated.store(true, std::memory_order_release);
                  backpropagate(eval, path);
                  inflight_count.fetch_sub(1, std::memory_order_release);
                  //pending_evals untouched - there are no per-child completions. pos_dedup keeps
                  //this hash: the node is expanded now and must not be handed back for a second
                  //expansion. The set is cleared per search.
                } else {
                  if (!path.empty()) {
                      auto* parent = path.back();
                      if (parent->pending_evals.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                          finalize_priors(parent); 
                      }
                  }
                  path.push_back(leaf);
                  backpropagate(eval, path); 
                  leaf->evaluated.store(true, std::memory_order_release);
                  pos_dedup.remove(leaf->hash.load(std::memory_order_relaxed));
                  //inflight_count is the ONLY thing runMCTS waits on before cleanup()/gc() free every
                  //node and Edge array. It must therefore be decremented LAST: dropping it before the
                  //two lines above let the drain return while the server was still writing into `leaf`,
                  //so those writes landed in freed memory.
                  inflight_count.fetch_sub(1, std::memory_order_release);
                }
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
    
//Fill the edge priors from ONE policy row instead of from the children's own values.
//
//Index mapping: an edge move is (promoType << 12) | (src << 6) | dst, and PieceTypeNone
//is 7, so a non-promotion carries 0x7000 in the high bits -- the policy index is the LOW
//12 BITS ONLY. The trainer orients the board for the side to move (square ^ 56 when Black
//is to move, dual_head_kan_train.cpp), so for Black both squares must be flipped:
//src ^ 56 lives in bits 6-11 (^0xE00) and dst ^ 56 in bits 0-5 (^0x38), hence ^0xE38.
//Getting this flip wrong does not crash -- it yields a mirrored policy, which plays like
//a mediocre engine rather than a broken one.
//
//Promotions: four edges share one (src,dst) pair and therefore one policy entry. The
//trainer ignored promotion type entirely, so what the model learned is "this from/to pair
//is good", which in practice means the queen promotion. Splitting evenly would hand three
//quarters of that mass to under-promotions, so they take a fixed logit penalty instead --
//still reachable, but not preferred.
static inline void policy_priors(MCTSNode* parent, bool is_black, const float* row) {
    const size_t n = parent->num_children.load(std::memory_order_acquire);
    if (n == 0) return;
    Edge* edges = parent->children.load(std::memory_order_acquire);
    if (!edges) return;

    double logits[256];
    double max_val = -std::numeric_limits<double>::infinity();
    const size_t count = n < 256 ? n : 256;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t m = edges[i].move.load(std::memory_order_relaxed);
        //Oriented squares: the trainer flips both when Black is to move.
        int src = (m >> 6) & 63, dst = m & 63;
        if (is_black) { src ^= 56; dst ^= 56; }
        const uint32_t promo = (m >> 12) & 7u;
        const int pi = policy_index(src, dst, (int)promo);
        if (pi < 0) { logits[i] = -1e9; continue; }   //shape with no plane: unreachable
        double v = static_cast<double>(row[pi]);
        #if !CONV_POLICY
        //Bilinear only: four promotions share one (from,to) entry, so bias against the
        //under-promotions. The directional head gives them their own planes and needs no
        //such fudge.
        if (promo != PieceTypeNone && promo != Queen) v -= 4.0;
        #endif
        logits[i] = v;
        if (v > max_val) max_val = v;
    }
    if (!std::isfinite(max_val)) {          //nothing mappable: fall back to uniform
        for (size_t i = 0; i < count; ++i)
            edges[i].P.store(1.0 / (double)count, std::memory_order_release);
        return;
    }
    double total = 0.0;
    for (size_t i = 0; i < count; ++i) { logits[i] = std::exp(logits[i] - max_val); total += logits[i]; }
    const double inv = (total > 0.0) ? 1.0 / total : 0.0;
    for (size_t i = 0; i < count; ++i)
        edges[i].P.store(inv == 0.0 ? 1.0 / (double)count : logits[i] * inv,
                         std::memory_order_release);
    //Caller sets parent->evaluated AFTER this returns -- never before, or workers descend
    //on a node whose priors are still 0 and select_best_child returns child 0 forever.
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
        parent->evaluated.store(true, std::memory_order_release);
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


