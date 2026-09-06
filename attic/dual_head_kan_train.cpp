// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -O3 -flto -I /Users/ap/Downloads/libtorch2/include -I /Users/ap/Downloads/libtorch2/include/torch/csrc/api/include -L /Users/ap/Downloads/libtorch2/lib -L /Users/ap/libchess -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/Users/ap/Downloads/libtorch2/lib,-rpath,/Users/ap/libchess -o dual_head_kan_train dual_head_kan_train.cpp

#include <torch/torch.h>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <cstdint>
#include <iomanip>
#include <random>
#include <chrono>
#include "nnue/bitboard.h"
#include "nnue/nnue/nnue_accumulator.h"
#include "libchess.h"

const float eval_scale = 600.0f; // conversion scale from cp to cnn target and back to cp from cnn output
//target = tanh(cp / eval_scale);
//cp = eval_scale * atanh(cnn_output);

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
        // Chebyshev polynomials are bounded only on [-1,1]; outside it T_8 grows like
        // 128x^8. layer1 emits a raw matmul over a 5184-wide basis and reaches |x|~4 at
        // init, so without this the layer-2 basis hits ~1e6 before the first gradient
        // step. This is why lr had to be 2e-5. Must stay identical to the inference
        // copy in creatica-kan.hpp or trained weights are meaningless there.
// Bound the input before building the Chebyshev basis. NOT optional, and it used to be
        // a -DKAN_TANH flag until the evidence came in one-sided:
        //   * degree 8 means T_8 ~ 128x^8, so layer 1's unbounded output (already ~N(0,1) at
        //     init, reaching |x| ~ 4 in the tails) puts the layer-2 basis near 8e6 before the
        //     first gradient step;
        //   * validation runs in half precision, which tops out at 65504, so the unbounded
        //     basis overflows and every reported metric comes back NaN;
        //   * without it the learning rate has to drop about tenfold for stability -- the
        //     original file ran at 2e-5 where the bounded version runs at 2e-4.
        // The single-head model that trained without it used degree 4, where T_4 peaks around
        // 2e3 rather than 8e6 -- roughly four thousand times tamer at the same input.
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
        auto flattened_weights = weights.view({out_features, -1}).transpose(0, 1); // [In * (Deg+1), Out]
        return torch::matmul(flattened_basis, flattened_weights);
    }
};
TORCH_MODULE(ChebyKANLayer);

// A move is a PAIR of squares. The old head produced 64 "from" logits and 64 "to"
// logits and scored them with two independent cross-entropies, which models
// P(move) = P(from) * P(to). That is a rank-1 approximation of the 64x64 move table:
// if e2e4 and d2d4 are both good, mass on {e2,d2} x {e4,d4} also endorses e2d4 and
// d2e4, and no parameter exists that can separate them.
//
// This head builds the same 64x64 table as U W V^T, where U and V hold one small
// vector per square and W is d x d. The table's rank is then d instead of 1, and it
// costs FEWER parameters than the head it replaces -- a flat 4096-wide output layer
// off the 64-wide trunk would need 266,240, this needs 5,760 at d=32.
//
// Raw matmul rather than nn::Linear on purpose: nn::Linear on 3-D inputs has a
// pathological slow path on MPS for some shapes.
// Rank of the 64x64 move-score table the policy head builds. d = 1 is the old broken
// factorized head; d = 64 makes the table fully general. Cost is negligible at every
// setting (head params = 2*in*d + d^2, against 1.3M in the trunk), so this is purely an
// empirical question -- sweep it:
//     for d in 8 16 32 64; do c++ ... -DPOLICY_DIM=$d -o train_d$d dual_head_kan_train.cpp; done
// Temperature of the softmax over PV centipawn scores that forms the policy TARGET.
// This decides what the model is taught. At the original 1.5 the target is nearly
// one-hot: measured over 300,000 records, PV1 holds 0.764 of the mass, PV2-3 hold
// 0.196, and everything beyond PV3 holds 0.040 -- an effective 1.88 moves. That is
// why raising the PV cap from 3 to 16 did not move Top-4 or Top-6 at all.
// Raise it to spread mass down the list and teach ranking rather than just picking.
// -DSEED=n pins weight init and batch order so a config can be re-run exactly, and so
// the same config run at two seeds measures the noise floor -- which is what tells you
// whether a sub-point difference between configs is real. Undefined leaves the run
// nondeterministic, as before.
// -DPOLICY_WEIGHT=x scales the policy term. The two weights normalise each loss by its
// random baseline (value MSE 0.33, policy CE 3.37) so both start near 1.0. They do not
// stay balanced: value falls to ~0.10 (weighted 0.30) while policy sits at ~2.45
// (weighted 0.73), so the policy objective ends up pulling the shared trunk 2.4x harder.
// POLICY_WEIGHT=0 trains value only, which isolates task interference from the tanh.
#ifndef POLICY_WEIGHT
#define POLICY_WEIGHT 1.0f
#endif

// -DMAX_TRAIN_PVS=1 trains against PV1 only, discarding the rest. Pair it with a default
// run to measure what tail supervision is actually worth: same positions, same difficulty,
// only the number of labelled moves differs. Splitting the TEST set by PV count cannot
// answer this -- Stockfish emits more PVs when moves are close, so PV count measures
// difficulty first: measured Top-6 was 65.86% at 1 PV and 61.67% at 4+.
#ifndef MAX_TRAIN_PVS
#define MAX_TRAIN_PVS MAX_PVS
#endif

#ifndef POLICY_TEMP
#define POLICY_TEMP 1.5
#endif

// Chebyshev degree of the KAN trunk. Each input becomes degree+1 basis terms, so this
// scales layer-1 parameters AND compute directly. The engine's previous net used 4; this
// trainer defaulted to 8.
// Width of the KAN trunk. NOTE: the deployed engine has been running hidden=128, not the
// 256 this trainer defaults to -- creatica-kan.hpp derives layer width from the LOADED
// tensor rather than the constructor, and simple_bkan.pt is a half-width net. Measured
// cost at hidden=256, degree=4, no attention is 12.1 us/position against the old engine's
// ~1.9, so matching the engine's budget means matching its width too.
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
// ~12,700 parameters at d=32, against 1.3M in the KAN trunk.
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
#endif

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

// MUST MIRROR lichess_evals_bin_writer.cpp. The count is stored in PV_COUNT_BITS bits
// and the writer caps at MAX_PVS; if these drift apart the reader consumes the wrong
// number of PV entries and every later record in the file is read at the wrong offset.
static constexpr int PV_COUNT_BITS = 4;
static constexpr int MAX_PVS       = 1 << PV_COUNT_BITS;   // 16

struct CompressedPosition {
    uint8_t piecesOnSquares[64];
    int8_t side_to_move;    // 0=White, 1=Black
    int8_t castling_rights; // Bitmask
    int8_t ep_file;         // 0-7, 8=None
    int16_t eval_cp;         // Centipawns
    int8_t pvs; //number of PVs - 1 (0, 1, 2)
    int8_t from_sq[MAX_PVS];
    int8_t to_sq[MAX_PVS];
    int8_t promo[MAX_PVS]; //writer: 0=none, 1=N, 2=B, 3=Q (unused here)
    int16_t cp[MAX_PVS]; //PV eval
};

class BitReader {
private:
    std::ifstream& in;
    uint64_t accumulator;
    int bits_in_buffer;

public:
    BitReader(std::ifstream& i) : in(i), accumulator(0), bits_in_buffer(0) {}

    // Read n bits
    uint32_t read(int n_bits) {
        while (bits_in_buffer < n_bits) {
            // We need more bits, grab a byte from the file
            if (in.peek() == EOF) return 0; // Should handle gracefully
            
            char c;
            in.get(c);
            uint8_t byte = static_cast<uint8_t>(c);
            
            accumulator |= (static_cast<uint64_t>(byte) << bits_in_buffer);
            bits_in_buffer += 8;
        }

        uint32_t val = accumulator & ((1ULL << n_bits) - 1);
        accumulator >>= n_bits;
        bits_in_buffer -= n_bits;
        return val;
    }

    // Align to the next byte boundary (skip padding bits)
    void align() {
        accumulator = 0;
        bits_in_buffer = 0;
    }
    
    bool eof() {
        return in.peek() == EOF && bits_in_buffer == 0;
    }
};

bool read_next_position(BitReader& reader, CompressedPosition& pos) {
    if (reader.eof()) return false;

    // 1. Clear the board
    std::fill(std::begin(pos.piecesOnSquares), std::end(pos.piecesOnSquares), 7); //PieceNone

    // 2. Read Number of Pieces (5 bits); we subtracted 1 during encoding to allow 32 pieces using 5 bits, here we need to add 1
    int num_pieces = reader.read(5) + 1;

    // 3. Read Each Piece (10 bits: 6 sq + 1 col + 3 type)
    for (int i = 0; i < num_pieces; ++i) {
        int square = reader.read(6);
        int color = reader.read(1);
        int type = reader.read(3);
        pos.piecesOnSquares[square] = (color << 3) | type;
    }

    // 4. Global State
    pos.side_to_move = reader.read(1);
    pos.castling_rights = reader.read(4);
    pos.ep_file = reader.read(4);

    // 5. Eval (16 bits)
    // We must cast to int16_t to interpret the bits as a signed number
    uint16_t raw_eval = static_cast<uint16_t>(reader.read(16));
    pos.eval_cp = static_cast<int16_t>(raw_eval); //eval is from white point of view
    pos.pvs = reader.read(PV_COUNT_BITS);
    for (int i = 0; i <= pos.pvs; ++i) {
      pos.from_sq[i] = reader.read(6);
      pos.to_sq[i] = reader.read(6);
      pos.promo[i] = reader.read(2);
      if (i == 0) pos.cp[i] = pos.eval_cp;
      else {
        raw_eval = static_cast<uint16_t>(reader.read(16));
        pos.cp[i] = static_cast<int16_t>(raw_eval); 
      }
    }
    // 6. Align reader (discard padding)
    reader.align();
    return true;
}

// Padding width for the per-sample legal-move index list. A legal chess position
// tops out near 218 moves in constructed cases; real positions from the shards sit
// under 100. Overflow is dropped, which can only remove a move from training.
static constexpr int MAX_LEGAL = 200;

// --- 1. Define the Custom Dataset ---
class ChessDataset : public torch::data::datasets::Dataset<ChessDataset> {
private:
  std::vector<CompressedPosition> samples;
public:
  ChessDataset(const std::string& filepath, size_t max_samples = 0) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Could not open file!");

    BitReader reader(file);
    CompressedPosition pos;
    int count = 0;
    while (read_next_position(reader, pos)) {
        samples.push_back(pos);
        count++;
        //if (count % 1000000 == 0) 
            //std::cout << "Loaded " << count << " positions.\n" << std::flush;
        if (max_samples > 0 && samples.size() >= max_samples) break; // early exit
    }
    std::cout << "Finished loading " << samples.size() << " positions.\n";
  }

  // The "Hot" Path: Converts 1 struct into Tensors (Input + Label)
  torch::data::Example<> get(size_t index) override {
    constexpr double policy_temperature = POLICY_TEMP;
    const CompressedPosition& pos = samples[index];
    Board board = {};
    board.sideToMove = static_cast<Color>(pos.side_to_move);
    board.enPassant = static_cast<File>(pos.ep_file);
    board.castlingRights = pos.castling_rights;

    for(int sq = 0; sq < 64; ++sq) {
      Piece pc = static_cast<Piece>(pos.piecesOnSquares[sq]);
      board.piecesOnSquares[sq] = pc;
      PieceType type = PC_TYPE(pc);
      Color color = PC_COLOR(pc);
      if (type != PieceTypeNone) {
        board.side[color] |= (1ULL << sq);
        board.pieceTypes[type - 1] |= (1ULL << sq);
      }
    }
    // A record can carry a castling right with no rook (or no king) to back it. msBit()
    // and lsBit() assert on an empty bitboard, which aborts the whole training run on one
    // bad record out of hundreds of millions. Clear any right not backed by a king on the
    // e-file and a rook on the matching corner, exactly as the writer's own checks require.
    {
      const bool wk = board.piecesOnSquares[SquareE1] == WhiteKing;
      const bool bk = board.piecesOnSquares[SquareE8] == BlackKing;
      if (!(wk && board.piecesOnSquares[SquareH1] == WhiteRook)) board.castlingRights &= ~1;
      if (!(wk && board.piecesOnSquares[SquareA1] == WhiteRook)) board.castlingRights &= ~2;
      if (!(bk && board.piecesOnSquares[SquareH8] == BlackRook)) board.castlingRights &= ~4;
      if (!(bk && board.piecesOnSquares[SquareA8] == BlackRook)) board.castlingRights &= ~8;
    }
    if (board.castlingRights & 1) board.castlingRooks |= SQ_BIT(SquareH1);
    if (board.castlingRights & 2) board.castlingRooks |= SQ_BIT(SquareA1);
    if (board.castlingRights & 4) board.castlingRooks |= SQ_BIT(SquareH8);
    if (board.castlingRights & 8) board.castlingRooks |= SQ_BIT(SquareA8);
    
    //torch::Tensor legal_mask = torch::zeros({4096}, torch::kFloat32);
    //float* mask_data = legal_mask.data_ptr<float>();
    // A move is a (from,to) PAIR, so legality is a mask over pairs, not two masks over
    // squares. A dense [4096] mask per sample would be 16 KB; keep the indices instead
    // and let the training loop scatter them onto the device.
    int legal_idx[MAX_LEGAL];
    int n_legal = 0;

    bool is_black = (board.sideToMove == ColorBlack);
    // 1. Determine which bitboard belongs to 'Friendly' (side to move)
    //board is oriented for the side to move perspective to help the model learn it only once
    auto [king_moves, pinned, pinning, checkers, kingSquare] = kingMoves(board);
    uint64_t all_legal_moves = king_moves;
    int num_checkers = checkers ? bitCount(checkers) : 0;
    auto [check_mask, ep_mask] = checkers ? checkMask(board, kingSquare, checkers) : std::make_pair(0xffffffffffffffffULL, 0ULL);

    auto add_legal = [&](int from_i, int to_i) {
      const int pi = policy_index(from_i, to_i, PieceTypeNone);
      if (pi >= 0 && n_legal < MAX_LEGAL) legal_idx[n_legal++] = pi;
    };

    int king_from = is_black ? (kingSquare ^ 56) : kingSquare;
    while (king_moves) {
      const Square to_sq = popLSB(king_moves);
      add_legal(king_from, is_black ? (to_sq ^ 56) : to_sq);
    }

    if (num_checkers <= 1) {
      // Loop through friendly pieces to gather their moves
      for (PieceType pt = Pawn; pt <= Queen; ++pt) {
        uint64_t occupations = board.side[is_black ? ColorBlack : ColorWhite] & board.pieceTypes[pt - 1];
        while (occupations) {
          const Square sq = popLSB(occupations);
          int from_i = is_black ? (sq ^ 56) : sq;
          uint64_t moves = piece_moves(board, pt, sq, kingSquare, pinned, pinning, check_mask, ep_mask);
          all_legal_moves |= moves;
          while (moves) {
            const Square to_sq = popLSB(moves);
            add_legal(from_i, is_black ? (to_sq ^ 56) : to_sq);
          }
        }
      }
    }

    torch::Tensor input = torch::zeros({9, 8, 8}, torch::kFloat32);
    // Pointers to raw data for fast access
    float* data = input.data_ptr<float>();        
    uint64_t any = occupations(board);
    const float weights[] = {0.0f, 1.0f, 3.0f, 3.0f, 5.0f, 9.0f, 10.0f};
    // 3. Temporary accumulators for attacks
    float net_tension[64] = {0.0f};
    float total_volume[64] = {0.0f};
    
    for(int sq = 0; sq < 64; ++sq) {
        int input_sq = is_black ? (sq ^ 56) : sq;
        Piece pc = static_cast<Piece>(board.piecesOnSquares[sq]);

        if (pc != PieceNone) {
            Color color = PC_COLOR(pc);
            PieceType type = PC_TYPE(pc);
            
            // Bipolar Pieces: +1 for Friendly, -1 for Enemy
            float val = (color == board.sideToMove) ? 1.0f : -1.0f;
            int pieceChannel = type - 1; 
            data[(pieceChannel << 6) | input_sq] = val;

            // Attack Logic
            uint64_t occ = any;
            if (type >= Bishop && type <= Queen) {
                occ ^= (board.side[1 - color] & board.pieceTypes[King - 1]);
            }

            uint64_t pieceAttacks = Stockfish::attacks_bb(static_cast<Stockfish::Piece>(PC(color,type)), static_cast<Stockfish::Square>(sq), occ);

            float w = weights[type];
            while (pieceAttacks) {
                int atk_sq = popLSB(pieceAttacks);
                if (color == board.sideToMove) {
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
        data[(6 << 6) | input_sq] = std::tanh(net_tension[sq] / 5.0f);
        
        // Total Volume: tanh(sum/10.0) -> captures overall chaos/tension
        data[(7 << 6) | input_sq] = std::tanh(total_volume[sq] / 10.0f);

        // Legal Moves
        if ((all_legal_moves >> sq) & 1) {
            data[(8 << 6) | input_sq] = 1.0f;
        }
    }
    
    // 4. Create Target Tensor
    // Normalize centipawns (e.g., clamp between -1000 and 1000, then divide)
    //float normalized_score = std::max(-1000.0f, std::min(1000.0f, (float)pos.eval_cp));
    //normalized_score /= 1000.0f; // Range -1.0 to 1.0; negative score - white's loosing, positive - white's winning 
    //instead of linear clamping, use sigmoid
    float normalized_score = tanh(static_cast<float>(pos.eval_cp) / eval_scale);
    
    // Important: If side_to_move is Black, but eval is from White's perspective, flip it!
    // NNUE usually trains on "Eval from Side-to-Move's perspective", so we do the same for our CNN
    if (is_black) normalized_score = -normalized_score;
    torch::Tensor value_target = torch::tensor({normalized_score}, torch::kFloat32);

    // Policy target, now JOINT. Each PV is one (from,to) index into the 64x64 table
    // rather than two separate square marginals, so the model is asked for the move
    // itself. This also removes the shared-square collision the marginal version had:
    // two PVs from the same piece no longer land on the same target entry.
    //
    // Promotion type is deliberately ignored -- all four promotions share a (from,to)
    // pair. That also sidesteps the writer/reader promotion-code mismatch, since the
    // promo field is never read here.
    float pv_idx[MAX_PVS], pv_prob[MAX_PVS];
    for (int i = 0; i < MAX_PVS; ++i) { pv_idx[i] = -1.0f; pv_prob[i] = 0.0f; }

    //Clamp regardless: a corrupt or wrong-format record must never index past the
    //arrays. The writer emits 1..MAX_PVS.
    int num_pvs = std::min<int>(pos.pvs + 1, std::min(MAX_PVS, MAX_TRAIN_PVS)); // stored as count - 1
    if (num_pvs > 0 && pos.from_sq[0] >= 0) {
      float scores[MAX_PVS];
      for (int i = 0; i < num_pvs; ++i) {
        int from_sq = is_black ? (pos.from_sq[i] ^ 56) : pos.from_sq[i];
        int to_sq   = is_black ? (pos.to_sq[i]   ^ 56) : pos.to_sq[i];
        static const int PROMO_PT[4] = {PieceTypeNone, Knight, Bishop, Queen};
        const int pi = policy_index(from_sq, to_sq, PROMO_PT[pos.promo[i] & 3]);
        pv_idx[i] = (pi >= 0) ? static_cast<float>(pi) : -1.0f;
        //cp is WHITE-relative (see the value target above, which flips it for
        //Black). The squares were being flipped here but the scores were not, so
        //for a Black position PV1 - lichess orders PVs best-first for the side to
        //move - had the LOWEST cp, max_score below subtracted a minimum, and the
        //softmax put its weight on Black's WORST moves. Inverted on half the data.
        float cp_i = is_black ? -static_cast<float>(pos.cp[i]) : static_cast<float>(pos.cp[i]);
        float cp_clamped = std::max(-1000.0f, std::min(1000.0f, cp_i));
        scores[i] = cp_clamped / (100.0f * policy_temperature);
      }
      if (num_pvs == 1) {
        pv_prob[0] = 1.0f;
      } else {
        float max_score = scores[0];   // PV1 is best by construction
        float sum = 0.0f;
        for (int i = 0; i < num_pvs; ++i) { scores[i] = std::exp(scores[i] - max_score); sum += scores[i]; }
        for (int i = 0; i < num_pvs; ++i) pv_prob[i] = scores[i] / sum;
      }
    } else {
      num_pvs = 0;
    }

    // Compact target: [value, n_pv, pv_idx x3, pv_prob x3, n_legal, legal_idx x MAX_LEGAL]
    // [value, n_pv, pv_idx x MAX_PVS, pv_prob x MAX_PVS, n_legal, legal_idx x MAX_LEGAL]
    const int META = 2 + 2 * MAX_PVS + 1 + MAX_LEGAL;
    torch::Tensor meta = torch::empty({META}, torch::kFloat32);
    float* m = meta.data_ptr<float>();
    m[0] = normalized_score;
    m[1] = static_cast<float>(num_pvs);
    for (int i = 0; i < MAX_PVS; ++i) { m[2 + i] = pv_idx[i]; m[2 + MAX_PVS + i] = pv_prob[i]; }
    m[2 + 2 * MAX_PVS] = static_cast<float>(n_legal);
    float* lg = m + 3 + 2 * MAX_PVS;
    for (int i = 0; i < MAX_LEGAL; ++i) lg[i] = (i < n_legal) ? static_cast<float>(legal_idx[i]) : -1.0f;

    return {input, meta};
  }

  torch::optional<size_t> size() const override {
    return samples.size();
  }
};

// Helper to find all split files matching pattern "lichess_db_eval(_\d+)?\.bin"
std::vector<std::string> get_data_files(const std::string& base_path) {
  namespace fs = std::filesystem;
  std::vector<std::string> files;
  
  // Check if base file exists
  if (fs::exists(base_path)) files.push_back(base_path);

  // Check for numbered splits: base_1.bin, base_2.bin...
  // We assume the extension is .bin and the prefix is everything before .bin
  std::string path_no_ext = base_path.substr(0, base_path.find_last_of("."));
  
  int index = 1;
  while (index <= 29) {
    std::string next_file = path_no_ext + "_" + std::to_string(index) + ".bin";
    if (fs::exists(next_file)) {
      files.push_back(next_file);
    }
    index++;
  }
  return files;
}

// Save old model's trunk+value weights into new model before training
/*void transfer_weights(ChessChebyKAN& new_model, const std::string& old_weights_path) {
    // Temporarily define old model structure
    ChessChebyKANValueOnly old_model;
    torch::load(old_model, old_weights_path);
    
    // Transfer layer by layer
    auto copy_kan_layer = [](ChebyKANLayer& dst, ChebyKANLayer& src) {
        torch::NoGradGuard no_grad;
        dst->weights.data().copy_(src->weights.data());
    };
    
    auto copy_linear = [](torch::nn::Linear& dst, torch::nn::Linear& src) {
        torch::NoGradGuard no_grad;
        dst->weight.data().copy_(src->weight.data());
        dst->bias.data().copy_(src->bias.data());
    };
    
    copy_kan_layer(new_model->layer1, old_model->layer1);
    copy_kan_layer(new_model->layer2, old_model->layer2);
    copy_linear(new_model->value_head, old_model->value_head);
    
    std::cout << "Transferred layer1, layer2, value_head from " << old_weights_path << "\n";
    std::cout << "policy_head randomly initialized\n";
}*/


// ---------------------------------------------------------------- joint-policy helpers
struct PolicyBatch {
    torch::Tensor value;      // [B,1]
    torch::Tensor n_pv;       // [B]
    torch::Tensor pv_idx;     // [B,3]   from*64+to, -1 where unused
    torch::Tensor pv_prob;    // [B,3]
    torch::Tensor legal_idx;  // [B,MAX_LEGAL]
};
static PolicyBatch unpack_targets(const torch::Tensor& t) {
    const int P = MAX_PVS;
    return { t.slice(1, 0, 1), t.slice(1, 1, 2).squeeze(1),
             t.slice(1, 2, 2 + P), t.slice(1, 2 + P, 2 + 2 * P),
             t.slice(1, 3 + 2 * P, 3 + 2 * P + MAX_LEGAL) };
}

// Dense [B,4096] legality mask from the padded index list. Padding is -1, clamped to
// index 0 and written with value 0 -- safe because index 0 means a1->a1, which is
// never a real move, so no padding entry can mark a square pair as legal.
static torch::Tensor legality_mask(const torch::Tensor& logits, const torch::Tensor& legal_idx) {
    //Padding is -1. Clamping it to 0 was safe only for the bilinear scheme, where index 0
    //is a1->a1 and never a real move. Under the directional scheme index 0 is "a1, north,
    //one square" -- a1a2 -- so padding wrote 0 over a genuine move's slot. Scatter padding
    //into a spare column and drop it.
    auto mask  = torch::zeros({logits.size(0), logits.size(1) + 1}, logits.options());
    auto idx   = torch::where(legal_idx >= 0, legal_idx, torch::full_like(legal_idx,
                              (double)logits.size(1))).to(torch::kLong);
    auto valid = (legal_idx >= 0).to(logits.scalar_type());
    mask.scatter_(1, idx, valid);
    return mask.narrow(1, 0, logits.size(1)).contiguous();
}

// 0-based rank of the PV1 move among the legal moves, by logit. rank 0 == the model
// agreed with Stockfish; rank < K == PV1 survives a top-K cut.
static torch::Tensor pv1_rank(const torch::Tensor& masked, const torch::Tensor& mask,
                              const torch::Tensor& pv_idx) {
    auto pv1       = pv_idx.select(1, 0).clamp_min(0).to(torch::kLong).unsqueeze(1);
    auto pv1_logit = masked.gather(1, pv1);
    return ((masked > pv1_logit) & (mask > 0.5f)).sum(1);
}

// Positions where PV1 is actually legal in the decoded position. bench_prior_acc.cpp
// skips the rest (234 of 20,000), so the top-K numbers stay comparable.
static torch::Tensor pv1_is_legal(const torch::Tensor& mask, const torch::Tensor& pv_idx) {
    auto pv1 = pv_idx.select(1, 0).clamp_min(0).to(torch::kLong).unsqueeze(1);
    return (mask.gather(1, pv1).squeeze(1) > 0.5f) & (pv_idx.select(1, 0) >= 0);
}

int main() {
    Stockfish::Bitboards::init();
#ifdef SEED
    torch::manual_seed(SEED);
    std::cout << "Seed: " << SEED << std::endl;
#endif

    // 1. Hyperparameters
    // Was 12288. The policy head now produces a dense [batch, 4096] logit tensor, and
    // the mask, the masked copy and the log-softmax are each the same size again. At
    // 12288 that is ~200 MB per tensor before gradients, which will not fit in 8 GB.
    // 2048 keeps each around 34 MB.
    const int64_t batch_size = 2048;
    const int num_epochs = 1;
    const int num_workers = 1;
    double learning_rate = 2e-4; 
    const double LR_MAX = learning_rate; 
    const double LR_MIN = 1e-5;
    const std::string base_data_path = "../lichess_db_pvs_eval.bin";
    //Was ../Downloads/lichess_db_broadcast_2026-02.bin - a PRE-PV-format file read by
    //the PV-aware reader, so every validation number this trainer has ever printed was
    //computed on garbage. lichess_db_pv_eval_test.bin is the same format as training and
    //cannot leak into it: get_data_files() only matches the base name plus _1..29.bin.
    const std::string test_data_path = "../lichess_db_pvs_eval_test.bin";
    const std::string weights_file = "dual_head_kan.pt";
    const std::string checkpoint_prefix = "dual_head_kan_checkpoint_";

    auto file_list = get_data_files(base_data_path);
    std::cout << "Found " << file_list.size() << " data files." << std::endl;
        
    // 2. Setup Device (Use CUDA or MPS if available, else CPU)
    torch::Device device(torch::kCPU);
    if (torch::cuda::is_available()) {
        std::cout << "CUDA is available! Training on GPU." << std::endl;
        device = torch::Device(torch::kCUDA);
    } else if (torch::hasMPS()) {
        device = torch::Device(torch::kMPS);
        std::cout << "Using MPS device" << std::endl;        
    } else {
        std::cout << "Neither CUDA nor MPS not available. Training on CPU." << std::endl;
    }
    
    // 3. Initialize Model
    ChessChebyKAN model;
    //Timing does not need trained weights, and the size guard below would reject any
    //architecture the current checkpoint was not trained for -- which is exactly the set
    //of configurations a cost sweep wants to measure.
    if (std::getenv("BENCH_FORWARD")) {
        std::cout << "(BENCH_FORWARD: skipping weight load)" << std::endl;
    } else if (std::filesystem::exists(weights_file)) {
        try {
            std::cout << "Loading weights from " << weights_file << "..." << std::endl;
            // torch::load populates PARTIALLY on a shape mismatch without throwing, which
            // silently produces a half-random model. Tensor bytes are param_count * 4 plus a
            // few KB of archive overhead, so a size check catches a wrong-architecture resume.
            {
              int64_t np = 0;
              for (const auto& t : model->parameters()) np += t.numel();
              const auto sz  = (int64_t)std::filesystem::file_size(weights_file);
              const auto exp = np * 4;
              if (sz < exp || sz - exp > 65536) {
                std::cerr << "FATAL: " << weights_file << " is " << sz << " bytes but this build\n"
                          << "       has " << np << " parameters (" << exp << " bytes of tensors).\n"
                          << "       That checkpoint was trained with a different architecture.\n";
                return 3;
              }
            }
            torch::load(model, weights_file);
            std::cout << "Loaded weights from " << weights_file << std::endl;
        } catch (const c10::Error& e) {
            std::cerr << "Error loading weights: " << e.what() << std::endl;
            std::cerr << "Continuing with untrained model..." << std::endl;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error loading weights: " << e.what() << std::endl;
            std::cerr << "Continuing with untrained model..." << std::endl;
        }
    } else {
        std::cout << "Weights file " << weights_file << " not found, using untrained model" << std::endl;
    }
        
    // Count parameters
    int64_t param_count = 0;
    for (const auto& p : model->parameters()) {
        param_count += p.numel();
    }
    std::cout << "Config: POLICY_DIM=" << POLICY_DIM
              << " ATTN_DIM=" << ATTN_DIM
              << " USE_ATTENTION=" << USE_ATTENTION
              << " MAX_PVS=" << MAX_PVS
              << " POLICY_TEMP=" << POLICY_TEMP
              << " POLICY_WEIGHT=" << POLICY_WEIGHT
              << " MAX_TRAIN_PVS=" << MAX_TRAIN_PVS
              << " KAN_DEGREE=" << KAN_DEGREE << " KAN_HIDDEN=" << KAN_HIDDEN
              << " CONV_POLICY=" << CONV_POLICY << std::endl;
    std::cout << "Total number of parameters: " << param_count << std::endl;
        // PARAMS_ONLY=1 prints the model size and exits, so the architecture can be
        // checked without loading a shard.
        if (std::getenv("PARAMS_ONLY")) return 0;
    
    model->to(device); // Move model to GPU/MPS/CPU

    // BENCH_FORWARD=1 times the model alone -- no data loading, no move generation, no
    // backward pass. This is the number that matters for the engine, and it is NOT what
    // the training nps figures measure: ChessDataset::get() runs the whole move generator
    // per sample, which is model-size-independent CPU work.
    if (std::getenv("BENCH_FORWARD")) {
        model->eval();
        torch::NoGradGuard no_grad;
        const int64_t B = std::getenv("BENCH_BATCH") ? std::atoll(std::getenv("BENCH_BATCH")) : 2048;
        //BENCH_HALF=1 measures what the ENGINE actually runs: uci-kan.cpp does
        //model->to(torch::kHalf) and feeds a kHalf batch. Comparing an fp32 benchmark
        //against the engine's observed nps is not apples to apples.
        const bool half = std::getenv("BENCH_HALF") != nullptr;
        if (half) model->to(torch::kHalf);
        auto x = torch::randn({B, 576}, torch::TensorOptions().device(device));
        if (half) x = x.to(torch::kHalf);
        double sink = 0.0;
        for (int i = 0; i < 5; ++i) { auto o = model->forward(x); sink += o.first.sum().item<double>(); }
        auto t0 = std::chrono::high_resolution_clock::now();
        const int ITERS = 30;
        for (int i = 0; i < ITERS; ++i) { auto o = model->forward(x); sink += o.first.sum().item<double>(); }
        double el = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
        std::printf("forward-only: batch %lld  %.2f us/batch  %.4f us/position  (sink %.1f)\n",
                    (long long)B, el / ITERS * 1e6, el / ITERS / B * 1e6, sink);
        return 0;
    }
    
    torch::optim::Adam optimizer(model->parameters(), torch::optim::AdamOptions(learning_rate));

    // 6. Training Loop
    std::cout << "Starting training..." << std::endl;       
    
    // Estimate total batches: ~60M positions per epoch / 1024 batch size * 10 epochs
    // You can hardcode this or calculate it dynamically if you know the exact file sizes.
    const int64_t total_estimated_positions = 300000000ULL * num_epochs; 
    const int64_t TOTAL_STEPS = total_estimated_positions / batch_size; 
    int64_t global_step = 0;
    std::cout << "Schedule: TOTAL_STEPS=" << TOTAL_STEPS
              << " batch_size=" << batch_size
              << " num_epochs=" << num_epochs
              << " LR " << LR_MAX << " -> " << LR_MIN << std::endl;
    // -----------------------------------
    
    for (int epoch = 1; epoch <= num_epochs; ++epoch) {        
        // Optional: Shuffle file order to mix data slightly better
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(file_list.begin(), file_list.end(), g);
  
        double epoch_total_loss = 0.0;
        double epoch_value_loss = 0.0;
        double epoch_src_loss = 0.0;
        double epoch_dst_loss = 0.0;
        size_t epoch_total_batches = 0;
        int file_number = 0; //when the list is randomized, it's easy to keep track
                
        constexpr float policy_weight = 1.0f;
        for (const auto& filepath : file_list) {
            std::cout << "  Processing file " << ++file_number << ": " << filepath << std::endl;
            {
                auto dataset = ChessDataset(filepath).map(torch::data::transforms::Stack<>());
                size_t ds_size = dataset.size().value();
                
                if (ds_size == 0) continue;
                auto data_loader = torch::data::make_data_loader<torch::data::samplers::RandomSampler>(
                    std::move(dataset), 
                    torch::data::DataLoaderOptions().batch_size(batch_size).workers(num_workers)
                );
        
                model->train();
                auto start = std::chrono::high_resolution_clock::now();
                // epoch_total_batches runs across ALL files while `start` resets per file,
                // so it cannot be used for a rate. Count this file's batches separately.
                size_t file_batches = 0;
                for (auto& batch : *data_loader) {
                    auto data    = batch.data.to(device);
                    auto targets = batch.target.to(device);          // [batch, 4097]
        
                    auto tg = unpack_targets(targets);

                    optimizer.zero_grad();

                    auto [value_raw, policy_logits] = model->forward(data);
                    auto value_out = torch::tanh(value_raw);              // [batch, 1]

                    auto mask   = legality_mask(policy_logits, tg.legal_idx);
                    auto masked = policy_logits.masked_fill(mask < 0.5f, -1e9f);
                    auto logp   = torch::log_softmax(masked, 1);

                    auto loss_value = torch::mse_loss(value_out, tg.value);

                    // Listwise cross-entropy over the LEGAL moves only, against the
                    // softmaxed PV scores. Samples carrying no PV contribute nothing.
                    // A PV move is not always legal in the decoded position -- about 1.2%
                    // of records, measured. Its target index then points at an entry
                    // masked to -1e9, contributing ~1e9 to the loss and a gradient that
                    // uniformly suppresses every legal move. Drop those PV entries and
                    // renormalise what is left.
                    auto pvi      = tg.pv_idx.clamp_min(0).to(torch::kLong);
                    auto pv_legal = mask.gather(1, pvi);                       // [B,3]
                    auto wpv_raw  = tg.pv_prob * (tg.pv_idx >= 0).to(logp.scalar_type()) * pv_legal;
                    auto wsum     = wpv_raw.sum(1, /*keepdim=*/true);
                    auto wpv      = wpv_raw / wsum.clamp_min(1e-6f);
                    auto per_sample = -(logp.gather(1, pvi) * wpv).sum(1);
                    auto has_pv     = (wsum.squeeze(1) > 0.0f).to(logp.scalar_type());
                    auto n_have     = has_pv.sum().clamp_min(1.0f);
                    auto loss_policy = (per_sample * has_pv).sum() / n_have;

                    auto rank1 = pv1_rank(masked, mask, tg.pv_idx);
                    has_pv = pv1_is_legal(mask, tg.pv_idx).to(logp.scalar_type()) * has_pv;
                    double batch_top1 = 100.0 * (((rank1 == 0).to(logp.scalar_type()) * has_pv).sum()
                                                 / n_have).item<double>();

                    // Random baselines: value MSE ~0.33, policy CE ~log(29) = 3.37
                    constexpr float w_value  = 1.0f / 0.33f;
                    constexpr float w_policy = (1.0f / 3.37f) * (POLICY_WEIGHT);

                    auto loss = w_value * loss_value + w_policy * loss_policy;
        
                    loss.backward();
        
                    // Optional but recommended with policy head added
                    torch::nn::utils::clip_grad_norm_(model->parameters(), 1.0);
        
                    optimizer.step();
                    global_step++;
        
                    // Cosine annealing (unchanged)
                    double progress = std::min(1.0, (double)global_step / TOTAL_STEPS);
                    double current_lr = LR_MIN + 0.5 * (LR_MAX - LR_MIN) * (1.0 + std::cos(progress * M_PI));
                    for (auto& group : optimizer.param_groups()) {
                        static_cast<torch::optim::AdamOptions&>(group.options()).lr(current_lr);
                    }
                            
                    epoch_total_loss += loss.item<double>();
                    epoch_value_loss += loss_value.item<double>();
                    epoch_src_loss += loss_policy.item<double>();
                    epoch_dst_loss += batch_top1;
                    epoch_total_batches++;
                    file_batches++;
                
                    if (epoch_total_batches % 100 == 0) {
                        double elapsed = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
                        std::printf("\r    Batch %ld | Loss: %.4f (V: %.4f P: %.4f Top1: %.1f%%) | LR %.7f | Batch %lld | %.0f nps\033[K",
                            epoch_total_batches,
                            loss.item<double>(),
                            loss_value.item<double>(),
                            loss_policy.item<double>(),
                            batch_top1,
                            current_lr, batch_size, file_batches * batch_size / elapsed);
                        std::fflush(stdout);
                    }
                }
            }            
            std::cout << "\n  Finished file: " << filepath << std::endl;

            // Save Checkpoint
            // Was checkpoint_prefix + epoch + ".pt". With num_epochs = 1 that is ONE filename
            // for the whole run, overwritten after every file -- and shared across builds, so
            // an attention run's weights were silently replaced by a no-attention diagnostic.
            // Encode the architecture and the file index instead.
            {
              char ckpt[256];
              //The name must carry EVERY flag that changes the architecture, or runs overwrite each
              //other: a hidden=128 conv run silently walked over a hidden=256 bilinear run's
              //checkpoints file by file, because only attention and the two dims were encoded.
              std::snprintf(ckpt, sizeof ckpt, "%sh%d_d%d_a%d_c%d_ad%d_pd%d_e%d_f%02d.pt",
                            checkpoint_prefix.c_str(), KAN_HIDDEN, KAN_DEGREE, USE_ATTENTION,
                            CONV_POLICY, ATTN_DIM, POLICY_DIM, epoch, file_number);
              torch::save(model, ckpt);
              std::cout << "  saved " << ckpt << std::endl;
            }

        
        double avg_loss = epoch_total_loss / (epoch_total_batches + 1); // Avoid div/0
        std::cout << "=== End of Epoch " << epoch << " | Avg Loss: " << avg_loss << " ===" << std::endl;
        
        // --- VALIDATION PHASE ---
        if (std::filesystem::exists(test_data_path)) {
            model->to(torch::kHalf);
            model->eval();
            torch::NoGradGuard no_grad;
        
            double test_value_loss = 0.0, test_src_loss = 0.0, test_dst_loss = 0.0f;
            double mean_cp_error_total = 0.0, sign_accuracy_total = 0.0;
            double pol_top1 = 0.0, pol_top4 = 0.0, pol_top6 = 0.0, pol_total = 0.0;
            // Same metrics split by how many PV moves the position carries. 57% of records
            // have only one, so if tail supervision is what limits Top-6, positions with more
            // PVs should score visibly better. If they do not, generating extra tail labels
            // (with NNUE or a CNN) would be wasted effort.
            double b_top1[3] = {0,0,0}, b_top4[3] = {0,0,0}, b_top6[3] = {0,0,0}, b_tot[3] = {0,0,0};
            double dst_top1_correct = 0.0, dst_top1_total = 0.0;
            size_t test_total_batches = 0;
            double elapsed = 0.0;   // was uninitialised; the Time: line below read garbage
            std::optional<size_t> positions;
        
            {
                //auto test_dataset = ChessDataset(test_data_path).map(torch::data::transforms::Stack<>());
                auto test_dataset = ChessDataset(test_data_path, 1000000).map(torch::data::transforms::Stack<>());
                positions = test_dataset.size();
                auto test_loader = torch::data::make_data_loader(
                    std::move(test_dataset),
                    torch::data::DataLoaderOptions().batch_size(batch_size).workers(num_workers)
                );
        
                auto start = std::chrono::high_resolution_clock::now();
                for (auto& batch : *test_loader) {
                    auto data    = batch.data.to(device).to(torch::kHalf);
                    auto targets = batch.target.to(device);             // [batch, 257]
        
                    auto tg = unpack_targets(targets);
                    auto value_target = tg.value.to(torch::kFloat32);

                    auto [value_raw, policy_raw] = model->forward(data);
                    auto value_out     = torch::tanh(value_raw.to(torch::kFloat32));
                    // Cast BEFORE masking: -1e9 is not representable in half.
                    auto policy_logits = policy_raw.to(torch::kFloat32);

                    // --- Value metrics (unchanged) ---
                    auto loss_value = torch::mse_loss(value_out, value_target);

                    auto pred_cp   = torch::atanh(torch::clamp(value_out,    -0.9999f, 0.9999f)) * eval_scale;
                    auto target_cp = torch::atanh(torch::clamp(value_target, -0.9999f, 0.9999f)) * eval_scale;
                    mean_cp_error_total += torch::abs(pred_cp - target_cp).mean().item<float>();
                    sign_accuracy_total += ((value_out * value_target) > 0).to(torch::kFloat32).mean().item<float>() * 100.0;

                    // --- Policy metrics, over legal moves only ---
                    auto mask   = legality_mask(policy_logits, tg.legal_idx);
                    auto masked = policy_logits.masked_fill(mask < 0.5f, -1e9f);
                    auto logp   = torch::log_softmax(masked, 1);

                    auto pvi      = tg.pv_idx.clamp_min(0).to(torch::kLong);
                    auto pv_legal = mask.gather(1, pvi);
                    auto wpv_raw  = tg.pv_prob.to(torch::kFloat32) * (tg.pv_idx >= 0).to(torch::kFloat32) * pv_legal;
                    auto wsum     = wpv_raw.sum(1, /*keepdim=*/true);
                    auto wpv      = wpv_raw / wsum.clamp_min(1e-6f);
                    auto per_sample = -(logp.gather(1, pvi) * wpv).sum(1);
                    auto has_pv     = (wsum.squeeze(1) > 0.0f).to(torch::kFloat32);
                    auto n_have     = has_pv.sum();
                    auto loss_policy = (per_sample * has_pv).sum() / n_have.clamp_min(1.0f);

                    // Directly comparable to bench_prior_acc.cpp: 26.9% top-1 and
                    // 77.4% top-6 are what the engine's current NNUE prior scores.
                    auto rank1  = pv1_rank(masked, mask, tg.pv_idx);
                    auto ok_pv1 = pv1_is_legal(mask, tg.pv_idx).to(torch::kFloat32);
                    has_pv = ok_pv1;   // top-K is only meaningful where PV1 is legal
                    n_have = has_pv.sum();
                    pol_top1  += (((rank1 == 0).to(torch::kFloat32) * has_pv).sum()).item<double>();
                    pol_top4  += (((rank1 <  4).to(torch::kFloat32) * has_pv).sum()).item<double>();
                    pol_top6  += (((rank1 <  6).to(torch::kFloat32) * has_pv).sum()).item<double>();
                    pol_total += n_have.item<double>();

                    for (int b = 0; b < 3; ++b) {
                      auto npv = tg.n_pv;
                      torch::Tensor sel = (b == 0) ? (npv <= 1.5f)
                                        : (b == 1) ? ((npv > 1.5f).logical_and(npv <= 3.5f))
                                                   : (npv > 3.5f);
                      auto m = sel.to(torch::kFloat32) * ok_pv1;
                      b_top1[b] += (((rank1 == 0).to(torch::kFloat32) * m).sum()).item<double>();
                      b_top4[b] += (((rank1 <  4).to(torch::kFloat32) * m).sum()).item<double>();
                      b_top6[b] += (((rank1 <  6).to(torch::kFloat32) * m).sum()).item<double>();
                      b_tot[b]  += m.sum().item<double>();
                    }

                    auto loss       = loss_value + policy_weight * loss_policy;
                
                    test_value_loss  += loss_value.item<double>();
                    test_src_loss += loss_policy.item<double>();
                                        test_total_batches++;
                }
                elapsed = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
            }
        
            auto avg = [&](double x) { return test_total_batches > 0 ? x / test_total_batches : 0.0; };
        
            std::cout << "    Value  Loss:     " << avg(test_value_loss)  << "\n";
            std::cout << "    Policy Loss:     " << avg(test_src_loss) << "\n";
            
            std::cout << "    Avg CP Error:    " << avg(mean_cp_error_total) << " centipawns\n";
            std::cout << "    Sign Accuracy:   " << avg(sign_accuracy_total) << "%\n";
            std::cout << "    --- policy vs Stockfish PV1, " << (size_t)pol_total << " positions ---\n";
            std::cout << "    Top-1:  " << (pol_total > 0 ? 100.0 * pol_top1 / pol_total : 0.0)
                      << "%   (engine's current NNUE prior: 26.9%)\n";
            std::cout << "    Top-4:  " << (pol_total > 0 ? 100.0 * pol_top4 / pol_total : 0.0)
                      << "%   (engine's current NNUE prior: 65.5%)\n";
            std::cout << "    Top-6:  " << (pol_total > 0 ? 100.0 * pol_top6 / pol_total : 0.0)
                      << "%   (engine's current NNUE prior: 77.4%)\n";
            static const char * bname[3] = {"1 PV   ", "2-3 PVs", "4+ PVs "};
            std::cout << "    --- split by how many PVs the position carries ---\n";
            for (int b = 0; b < 3; ++b) {
              if (b_tot[b] < 1) continue;
              std::printf("    %s  n=%-9.0f Top-1 %5.2f%%  Top-4 %5.2f%%  Top-6 %5.2f%%\n",
                          bname[b], b_tot[b], 100.0 * b_top1[b] / b_tot[b],
                          100.0 * b_top4[b] / b_tot[b], 100.0 * b_top6[b] / b_tot[b]);
            }
            std::cout << "    Time: " << elapsed << " sec, "
                      << positions.value() / elapsed << " positions/sec\n";
        
            model->to(torch::kFloat32);
        }        
        
      }     
    }
    return 0;
}