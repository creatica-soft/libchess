// c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -O3 -flto -I /Users/ap/Downloads/libtorch2/include -I /Users/ap/Downloads/libtorch2/include/torch/csrc/api/include -I /Users/ap/libchess -L /Users/ap/Downloads/libtorch2/lib -L /Users/ap/libchess -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/Users/ap/Downloads/libtorch2/lib,-rpath,/Users/ap/libchess -o nnue_policy_train nnue_policy_train.cpp
//
// POLICY-ONLY trainer over Stockfish NNUE's own representation.
//
// Input is the feature transformer's post-activation output -- 1024 uint8 values, the
// same buffer the value network's first affine layer reads. Two reasons that is the
// right representation here. It is maintained INCREMENTALLY by the accumulator stack,
// so at a search node it is already computed and free to read, which is what lets a
// single position be scored without assembling a GPU batch -- measured, a batch of one
// through the KAN model on MPS costs 4.3 ms against 17 us at batch 2048. And the value
// comes from evaluate_nnue(), the function that earned the engine its rating, so there
// is no second-rate value head dragging play down.
//
// There is deliberately NO value head. NNUE already evaluates better than anything we
// trained against it, so this model has one job: rank the legal moves.
//
// Extracting features costs 6.66 us/sample from scratch (150k samples/s on one core),
// so the data loader keeps up: about 66 s per 9.9M-position shard.
#include <fcntl.h>    // F_NOCACHE on the streaming feature-cache reads
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

//From libchess: the NNUE feature transformer's output, and its width.
struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;
};
void init_nnue();
void init_nnue_context(NNUEContext&);
void accumulator_stack_reset(NNUEContext&);
int  nnue_feature_dims();
int  nnue_features(const Board&, NNUEContext&, unsigned char*);

//One context per data-loader thread: the accumulator stack is not thread-safe.
static thread_local NNUEContext tls_ctx{nullptr, nullptr};
static NNUEContext& thread_ctx() {
    if (!tls_ctx.accumulator_stack) init_nnue_context(tls_ctx);
    return tls_ctx;
}
static constexpr int NNUE_DIMS = 1024;

const float eval_scale = 600.0f; // conversion scale from cp to cnn target and back to cp from cnn output
//target = tanh(cp / eval_scale);
//cp = eval_scale * atanh(cnn_output);


// -DMAX_TRAIN_PVS=1 trains against PV1 only, discarding the rest. Pair it with a default
// run to measure what tail supervision is actually worth: same positions, same difficulty,
// only the number of labelled moves differs. Splitting the TEST set by PV count cannot
// answer this -- Stockfish emits more PVs when moves are close, so PV count measures
// difficulty first: measured Top-6 was 65.86% at 1 PV and 61.67% at 4+.
#ifndef MAX_TRAIN_PVS
#define MAX_TRAIN_PVS MAX_PVS
#endif

// Temperature of the softmax over PV centipawn scores forming the target. Measured over
// 300,000 records at 1.5: PV1 holds 0.764 of the mass, PV2-3 hold 0.196, beyond PV3 0.040
// -- an effective 1.88 moves. Raising it to 5 moved Top-6 by 0.24 points, inside noise.
#ifndef POLICY_TEMP
#define POLICY_TEMP 1.5
#endif

#ifndef CONV_POLICY
#define CONV_POLICY 0
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

#ifndef POLICY_H1
#define POLICY_H1 256
#endif
#ifndef POLICY_H2
#define POLICY_H2 128
#endif

//Policy over NNUE features. The trunk is small and dense because the expensive part --
//turning a board into 1024 numbers -- has already been paid for by the value network.
//
//Scores every move as context . embedding[move]. At TRAINING time that is one dense matmul
//over all POLICY_OUT moves, which suits the GPU and lets the loss mask to the legal set.
//At INFERENCE only the ~30 legal moves need scoring, so the same weights cost 128 MACs per
//move instead of a 4096-wide output -- which is what makes single-position scoring cheap.
//
//The input is also SPARSE: measured 12-16% non-zero (about 130 of 1024). A sparse first
//layer at inference costs ~33k MACs rather than 262k, the same trick NNUE's own feature
//transformer uses. Training keeps it dense; the GPU does not care.
struct NNUEPolicyImpl : torch::nn::Module {
    torch::Tensor W1, b1, W2, b2, emb;
    NNUEPolicyImpl(int in = NNUE_DIMS, int h1 = POLICY_H1, int h2 = POLICY_H2,
                   int nmoves = POLICY_OUT) {
        auto u = [](std::initializer_list<int64_t> shp, int fan) {
            float sc = std::sqrt(1.0f / fan);
            return torch::empty(shp).uniform_(-sc, sc);
        };
        W1  = register_parameter("W1",  u({in, h1}, in));
        b1  = register_parameter("b1",  torch::zeros({h1}));
        W2  = register_parameter("W2",  u({h1, h2}, h1));
        b2  = register_parameter("b2",  torch::zeros({h2}));
        emb = register_parameter("emb", u({nmoves, h2}, h2));
    }
    // x [B, NNUE_DIMS] -> [B, POLICY_OUT]
    torch::Tensor forward(torch::Tensor x) {
        auto h   = torch::relu(torch::matmul(x, W1) + b1);
        auto ctx = torch::relu(torch::matmul(h, W2) + b2);
        return torch::matmul(ctx, emb.transpose(0, 1));
    }
};
TORCH_MODULE(NNUEPolicy);

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

//Board construction from a decoded record. Shared deliberately: the cache builder and
//get() MUST produce byte-identical boards, because a cached feature vector is only valid
//for the exact position it was computed from. Two copies of this could drift apart and the
//mismatch would be invisible -- training would simply learn from wrong inputs.
static inline void board_from_record(const CompressedPosition& pos, Board& board) {
  board = Board{};
  board.sideToMove = static_cast<Color>(pos.side_to_move);
  board.enPassant = static_cast<File>(pos.ep_file);
  board.castlingRights = pos.castling_rights;

  for (int sq = 0; sq < 64; ++sq) {
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
}

// --- Feature cache -------------------------------------------------------------------
//
// nnue_features() is 73% of the CPU cost of preparing a sample (17.45 us of 23.95), because
// every sample pays a full accumulator refresh that the engine gets incrementally for free.
// It is also the same answer every epoch, so it is worth computing once and storing.
//
// One .feat file per shard, records in EXACTLY the order read_next_position() yields them,
// so record i corresponds to samples[i]. Fixed-size records mean a chunk is one seek and
// one sequential read. The stored count is checked against the shard on open: a cache built
// from a different shard, or a truncated one, is refused rather than silently feeding the
// wrong features for a position.
struct FeatCacheHeader {
  char    magic[4];    // "NFEA"
  int32_t version;     // 1
  int32_t dims;        // 1024 for the big net
  int32_t reserved;
  int64_t count;
};
static constexpr int32_t FEAT_CACHE_VERSION = 1;

static std::string feat_cache_path(const std::string& dir, const std::string& shard) {
  std::string base = std::filesystem::path(shard).filename().string();
  return (std::filesystem::path(dir) / (base + ".feat")).string();
}

// --- 1. Define the Custom Dataset ---
class ChessDataset : public torch::data::datasets::Dataset<ChessDataset> {
private:
  //Shared so a shard is decoded once and then presented as a series of chunks. Decoding
  //9.9M positions per chunk instead of per shard would cost more than the cache saves.
  //ALWAYS a shared_ptr, never a pointer into a member of this object. torch's .map() and
  //make_data_loader() move the dataset, and a `const vector* p = &my_member` survives that
  //move as a dangling pointer into the destroyed temporary -- which segfaults on the first
  //get(). The shared_ptr's target lives on the heap and is unaffected by moving the dataset.
  std::shared_ptr<std::vector<CompressedPosition>> samples_ref;
  size_t base_ = 0, n_ = 0;
  //This chunk's cached features, or empty when computing them on the fly.
  std::vector<unsigned char> feats_;
  int feat_dims_ = 0;
public:
  //Read once per call rather than per phase; getenv on the hot path would itself distort
  //what we are trying to measure.
  static bool profiling() {
    static const bool on = std::getenv("PROFILE_GET") != nullptr;
    return on;
  }
  //Decode a whole shard. Kept as a free function so the builder and the chunked path can
  //share it without constructing a dataset.
  static std::shared_ptr<std::vector<CompressedPosition>>
  load_shard(const std::string& filepath, size_t max_samples = 0) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Could not open file!");
    auto out = std::make_shared<std::vector<CompressedPosition>>();
    BitReader reader(file);
    CompressedPosition pos;
    while (read_next_position(reader, pos)) {
        out->push_back(pos);
        if (max_samples > 0 && out->size() >= max_samples) break; // early exit
    }
    std::cout << "Finished loading " << out->size() << " positions.\n";
    return out;
  }

  //Legacy path: the whole shard, features computed in get().
  ChessDataset(const std::string& filepath, size_t max_samples = 0)
      : samples_ref(load_shard(filepath, max_samples)) {
    base_ = 0;
    n_ = samples_ref->size();
  }

  //Chunked path. Presents [base, base+n) of an already-decoded shard. When cache_path is
  //given, that chunk's features are read from disk once, here, instead of being recomputed
  //per sample in get() -- one sequential read rather than n random ones, which is what
  //keeps this fast on an external drive.
  ChessDataset(std::shared_ptr<std::vector<CompressedPosition>> shard,
               size_t base, size_t n,
               const std::string& cache_path = "")
      : samples_ref(std::move(shard)), base_(base), n_(n) {
    if (base_ > samples_ref->size()) base_ = samples_ref->size();
    if (base_ + n_ > samples_ref->size()) n_ = samples_ref->size() - base_;
    if (cache_path.empty()) return;

    FILE * f = std::fopen(cache_path.c_str(), "rb");
    if (!f) throw std::runtime_error("feature cache missing: " + cache_path);
    //Tell the OS not to cache these pages.
    //
    //Each chunk is read exactly once per epoch and will be evicted long before it comes
    //round again, so caching it buys nothing -- but macOS still fills every free page with
    //it, which pushes the trainer's own 2.5 GB working set into the compressor. That was
    //measured at ~99k decompressions and ~107k compressions per second, roughly 1.6 GB/s of
    //pure overhead stealing the CPU that feeds the GPU, and throughput fell from 75k to
    //under 50k pos/s. F_NOCACHE keeps the streaming read from evicting anything.
#if defined(F_NOCACHE)
    fcntl(fileno(f), F_NOCACHE, 1);
#elif defined(__APPLE__)
#error "F_NOCACHE missing on Apple: <fcntl.h> not included, the fix would silently do nothing"
#endif
    FeatCacheHeader h{};
    if (std::fread(&h, sizeof h, 1, f) != 1) {
      std::fclose(f); throw std::runtime_error("feature cache header short: " + cache_path);
    }
    if (std::memcmp(h.magic, "NFEA", 4) != 0 || h.version != FEAT_CACHE_VERSION) {
      std::fclose(f); throw std::runtime_error("feature cache bad magic/version: " + cache_path);
    }
    //The guard that matters. Records are positional, so a cache whose length disagrees with
    //the shard would pair every sample with another position's features and train happily
    //on nonsense.
    if ((size_t)h.count != samples_ref->size()) {
      std::fclose(f);
      throw std::runtime_error("feature cache holds " + std::to_string(h.count) +
                               " positions but the shard has " +
                               std::to_string(samples_ref->size()) + ": " + cache_path);
    }
    feat_dims_ = h.dims;
    feats_.resize((size_t)n_ * feat_dims_);
    if (fseeko(f, (off_t)sizeof(FeatCacheHeader) + (off_t)base_ * feat_dims_, SEEK_SET) != 0) {
      std::fclose(f); throw std::runtime_error("feature cache seek failed: " + cache_path);
    }
    const size_t want = feats_.size();
    const size_t got  = std::fread(feats_.data(), 1, want, f);
    std::fclose(f);
    if (got != want) throw std::runtime_error("feature cache truncated: " + cache_path);
  }

  // The "Hot" Path: Converts 1 struct into Tensors (Input + Label)
  //
  //PROFILE_GET=1 times the three phases of this function. The GPU sits at roughly 70-75%
  //while the CPU is pinned, so the pipeline -- not the model -- sets the training rate, and
  //the only way to fix that is to know which phase actually costs the time. Atomics on a
  //hot path are not free, which is why this is opt-in and off by default.
  static inline std::atomic<uint64_t> t_board{0}, t_moves{0}, t_feat{0}, t_targ{0}, n_calls{0};
  static void dump_profile() {
    const uint64_t n = n_calls.load();
    if (!n) return;
    const double b = t_board.load() / 1e3 / n, m = t_moves.load() / 1e3 / n;
    const double f = t_feat.load()  / 1e3 / n, g = t_targ.load()  / 1e3 / n;
    const double tot = b + m + f + g;
    std::printf("\n  get() profile over %llu samples, per sample:\n", (unsigned long long)n);
    std::printf("    board build   %7.2f us  %5.1f%%\n", b, 100*b/tot);
    std::printf("    move gen      %7.2f us  %5.1f%%\n", m, 100*m/tot);
    std::printf("    nnue_features %7.2f us  %5.1f%%\n", f, 100*f/tot);
    std::printf("    targets       %7.2f us  %5.1f%%\n", g, 100*g/tot);
    std::printf("    total         %7.2f us   -> %.0f pos/s per worker\n", tot, 1e6/tot);
    std::fflush(stdout);
  }
  torch::data::Example<> get(size_t index) override {
    const bool prof = ChessDataset::profiling();
    std::chrono::high_resolution_clock::time_point _t0, _t1;
    if (prof) _t0 = std::chrono::high_resolution_clock::now();
    constexpr double policy_temperature = POLICY_TEMP;
    const CompressedPosition& pos = (*samples_ref)[base_ + index];
    Board board;
    board_from_record(pos, board);
    
    //torch::Tensor legal_mask = torch::zeros({4096}, torch::kFloat32);
    //float* mask_data = legal_mask.data_ptr<float>();
    // A move is a (from,to) PAIR, so legality is a mask over pairs, not two masks over
    // squares. A dense [4096] mask per sample would be 16 KB; keep the indices instead
    // and let the training loop scatter them onto the device.
    int legal_idx[MAX_LEGAL];
    int n_legal = 0;

    if (prof) {
      _t1 = std::chrono::high_resolution_clock::now();
      t_board.fetch_add((_t1 - _t0).count(), std::memory_order_relaxed);
      _t0 = _t1;
    }
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

    //The 9 hand-designed planes are gone. The representation is NNUE's own feature
    //transformer output -- 1024 uint8 values, post-activation, the same buffer the value
    //network's first affine layer reads. Scaled to [0,1]; it is already clipped to [0,127].
    //Computed from scratch here (6.66 us) because training samples are independent; in the
    //engine the accumulator is maintained incrementally and this is free.
    if (prof) {
      _t1 = std::chrono::high_resolution_clock::now();
      t_moves.fetch_add((_t1 - _t0).count(), std::memory_order_relaxed);
      _t0 = _t1;
    }
    unsigned char raw[NNUE_DIMS];
    int got = 0;
    if (!feats_.empty()) {
      //Cached. This is the whole point of the cache: the 17.45 us accumulator refresh below
      //never runs, and what is left is the board build, move generation and targets.
      got = std::min(feat_dims_, (int)NNUE_DIMS);
      std::memcpy(raw, feats_.data() + (size_t)index * feat_dims_, got);
    } else {
      NNUEContext& nctx = thread_ctx();
      accumulator_stack_reset(nctx);
      got = nnue_features(board, nctx, raw);
    }
    //Kept as uint8: 1 KB per sample instead of 4, and a quarter of the host-to-device
    //traffic. Scaled to [0,1] on the GPU, where the divide is free.
    torch::Tensor input = torch::empty({NNUE_DIMS}, torch::kUInt8);
    {
      unsigned char * d = input.data_ptr<unsigned char>();
      for (int i = 0; i < NNUE_DIMS; ++i) d[i] = (i < got) ? raw[i] : 0;
    }

    if (prof) {
      _t1 = std::chrono::high_resolution_clock::now();
      t_feat.fetch_add((_t1 - _t0).count(), std::memory_order_relaxed);
      _t0 = _t1;
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

    if (prof) {
      _t1 = std::chrono::high_resolution_clock::now();
      t_targ.fetch_add((_t1 - _t0).count(), std::memory_order_relaxed);
      //A shard takes far too long to wait for, so report periodically instead.
      if ((n_calls.fetch_add(1, std::memory_order_relaxed) + 1) % 400000 == 0) dump_profile();
    }
    return {input, meta};
  }

  torch::optional<size_t> size() const override {
    return n_;
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

//Build the feature cache for every shard in file_list.
//
//Resumable per shard, because a 1h41m job on this machine will be interrupted. A shard is
//skipped only when its .feat already carries the right magic, version and count; anything
//else is rebuilt. The write goes to a .tmp and is renamed on success, so a cache file that
//exists is always a complete one -- a half-written file that merely looked plausible would
//pair positions with the wrong features and poison training silently.
//Read the cache back and recompute features for a random sample of positions, comparing
//byte for byte.
//
//A feature cache is positional: record i is only meaningful if it belongs to samples[i]. An
//off-by-one, a shard read in a different order, or a rebuilt-but-stale file would all train
//perfectly happily on features belonging to other positions, and no loss curve would look
//obviously wrong. Nothing else in the pipeline can catch that, so it is checked directly.
static int verify_feature_cache(const std::string& dir,
                                const std::vector<std::string>& file_list,
                                int per_shard) {
  const int dims = nnue_feature_dims();
  int64_t checked = 0, bad = 0, shards = 0;
  std::mt19937_64 rng(12345);
  for (const auto& shard : file_list) {
    const std::string cpath = feat_cache_path(dir, shard);
    if (!std::filesystem::exists(cpath)) continue;
    ++shards;
    auto samples = ChessDataset::load_shard(shard);
    FILE * f = std::fopen(cpath.c_str(), "rb");
    if (!f) { std::cerr << "  cannot open " << cpath << std::endl; ++bad; continue; }
    FeatCacheHeader h{};
    if (std::fread(&h, sizeof h, 1, f) != 1 || std::memcmp(h.magic, "NFEA", 4) != 0) {
      std::cerr << "  bad header " << cpath << std::endl; std::fclose(f); ++bad; continue;
    }
    if ((size_t)h.count != samples->size() || h.dims != dims) {
      std::cerr << "  MISMATCH " << cpath << ": cache " << h.count << "x" << h.dims
                << " vs shard " << samples->size() << "x" << dims << std::endl;
      std::fclose(f); ++bad; continue;
    }
    NNUEContext& nctx = thread_ctx();
    Board board;
    std::vector<unsigned char> stored(dims), fresh(NNUE_DIMS);
    for (int k = 0; k < per_shard; ++k) {
      const int64_t i = (int64_t)(rng() % (uint64_t)h.count);
      if (fseeko(f, (off_t)sizeof(FeatCacheHeader) + (off_t)i * dims, SEEK_SET) != 0) break;
      if (std::fread(stored.data(), 1, dims, f) != (size_t)dims) break;
      board_from_record((*samples)[i], board);
      accumulator_stack_reset(nctx);
      std::memset(fresh.data(), 0, fresh.size());
      const int got = nnue_features(board, nctx, fresh.data());
      ++checked;
      if (got != dims || std::memcmp(stored.data(), fresh.data(), dims) != 0) {
        ++bad;
        if (bad <= 5) {
          size_t diff = 0;
          for (int d = 0; d < dims; ++d) if (stored[d] != fresh[d]) ++diff;
          std::cerr << "  MISMATCH " << cpath << " record " << i
                    << ": " << diff << "/" << dims << " bytes differ (got " << got << ")"
                    << std::endl;
        }
      }
    }
    std::fclose(f);
    std::cout << "  verified " << cpath << std::endl;
  }
  std::cout << "Verify: " << checked << " positions across " << shards << " shards, "
            << bad << " mismatches." << std::endl;
  return bad == 0 ? 0 : 5;
}

static int build_feature_cache(const std::string& dir,
                               const std::vector<std::string>& file_list) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const int dims = nnue_feature_dims();
  if (dims <= 0 || dims > NNUE_DIMS) {
    std::cerr << "FATAL: nnue_feature_dims() returned " << dims << std::endl;
    return 3;
  }
  std::cout << "Building feature cache in " << dir << " (" << dims << " dims/position)"
            << std::endl;

  int64_t grand_total = 0;
  const auto t_start = std::chrono::high_resolution_clock::now();

  for (size_t fi = 0; fi < file_list.size(); ++fi) {
    const std::string& shard = file_list[fi];
    const std::string out = feat_cache_path(dir, shard);

    if (std::filesystem::exists(out)) {
      FILE * f = std::fopen(out.c_str(), "rb");
      FeatCacheHeader h{};
      bool ok = f && std::fread(&h, sizeof h, 1, f) == 1
                && std::memcmp(h.magic, "NFEA", 4) == 0
                && h.version == FEAT_CACHE_VERSION && h.dims == dims && h.count > 0;
      if (ok) {
        const uintmax_t want = sizeof(FeatCacheHeader) + (uintmax_t)h.count * dims;
        ok = std::filesystem::file_size(out, ec) == want && !ec;
      }
      if (f) std::fclose(f);
      if (ok) {
        std::cout << "  [" << (fi + 1) << "/" << file_list.size() << "] " << out
                  << " already complete (" << h.count << " positions), skipping" << std::endl;
        grand_total += h.count;
        continue;
      }
      std::cout << "  [" << (fi + 1) << "/" << file_list.size()
                << "] existing cache incomplete, rebuilding" << std::endl;
    }

    std::cout << "  [" << (fi + 1) << "/" << file_list.size() << "] " << shard << std::endl;
    auto shard_samples = ChessDataset::load_shard(shard);
    const int64_t n = (int64_t)shard_samples->size();
    if (n == 0) { std::cout << "    empty shard, skipped" << std::endl; continue; }

    const std::string tmp = out + ".tmp";
    FILE * f = std::fopen(tmp.c_str(), "wb");
    if (!f) { std::cerr << "FATAL: cannot write " << tmp << std::endl; return 4; }
    FeatCacheHeader h{};
    std::memcpy(h.magic, "NFEA", 4);
    h.version = FEAT_CACHE_VERSION; h.dims = dims; h.reserved = 0; h.count = n;
    if (std::fwrite(&h, sizeof h, 1, f) != 1) {
      std::fclose(f); std::cerr << "FATAL: header write failed" << std::endl; return 4;
    }

    //Buffered so the external drive sees large sequential writes rather than 1 KB ones.
    const size_t BATCH = 8192;
    std::vector<unsigned char> buf((size_t)BATCH * dims);
    NNUEContext& nctx = thread_ctx();
    Board board;
    const auto t0 = std::chrono::high_resolution_clock::now();
    size_t filled = 0;
    for (int64_t i = 0; i < n; ++i) {
      board_from_record((*shard_samples)[i], board);
      accumulator_stack_reset(nctx);
      unsigned char raw[NNUE_DIMS];
      const int got = nnue_features(board, nctx, raw);
      unsigned char * dst = buf.data() + filled * dims;
      //A position that fails to produce features must still occupy its slot: records are
      //positional, and skipping one would shift every later position onto the wrong entry.
      std::memset(dst, 0, dims);
      if (got > 0) std::memcpy(dst, raw, std::min(got, dims));
      if (++filled == BATCH || i + 1 == n) {
        if (std::fwrite(buf.data(), dims, filled, f) != filled) {
          std::fclose(f); std::cerr << "FATAL: write failed (disk full?)" << std::endl; return 4;
        }
        filled = 0;
      }
      if ((i & 0xFFFFF) == 0xFFFFF) {
        const double el = std::chrono::duration<double>(
            std::chrono::high_resolution_clock::now() - t0).count();
        std::printf("\r    %lld/%lld positions  %.0f pos/s  ETA %.1f min\033[K",
                    (long long)(i + 1), (long long)n, (i + 1) / el,
                    (n - i - 1) / ((i + 1) / el) / 60.0);
        std::fflush(stdout);
      }
    }
    std::fclose(f);
    std::filesystem::rename(tmp, out, ec);
    if (ec) { std::cerr << "\nFATAL: rename failed: " << ec.message() << std::endl; return 4; }
    const double el = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now() - t0).count();
    std::printf("\r    %lld positions in %.1f min (%.0f pos/s), %.1f GB\033[K\n",
                (long long)n, el / 60.0, n / el,
                (sizeof(FeatCacheHeader) + (double)n * dims) / 1073741824.0);
    grand_total += n;
  }

  const double el = std::chrono::duration<double>(
      std::chrono::high_resolution_clock::now() - t_start).count();
  std::printf("Cache complete: %lld positions, %.1f GB, %.1f min\n",
              (long long)grand_total,
              (double)grand_total * dims / 1073741824.0, el / 60.0);
  return 0;
}

int main() {
    Stockfish::Bitboards::init();
    init_nnue();   //embedded nets; thread contexts are created lazily per worker
#ifdef SEED
    torch::manual_seed(SEED);
    std::cout << "Seed: " << SEED << std::endl;
#endif

    // 1. Hyperparameters
    // Was 12288. The policy head now produces a dense [batch, 4096] logit tensor, and
    // the mask, the masked copy and the log-softmax are each the same size again. At
    // 12288 that is ~200 MB per tensor before gradients, which will not fit in 8 GB.
    // 2048 keeps each around 34 MB.
//Kept at 2048. Shrinking it would REDUCE GPU efficiency, not help: MPS is
    //dispatch-bound, so fewer positions per dispatch means more dispatches for the same
    //data. Measured cost per position is 16.9 us at 2048, 17.6 at 512, 80 at 64 and
    //4289 at batch 1. Memory pressure is real, but the batch is not where to pay for it.
    //Sweepable without a rebuild. The pipeline, not the model, sets the training rate on
    //this machine -- the GPU runs at 70-75% while the CPU is pinned -- so these three are
    //the knobs that matter and they want to be measured, not guessed.
    auto env_i = [](const char* k, int64_t d) { const char* v = std::getenv(k); return v ? std::atoll(v) : d; };
    auto env_d = [](const char* k, double  d) { const char* v = std::getenv(k); return v ? std::atof(v)  : d; };
    //8192, not 2048. Measured: once the feature cache removes the CPU bottleneck, 8192 is what
    //actually feeds the GPU -- CPU under 70%, GPU over 90%, ~80k positions/s. The old default
    //left the GPU starved at about 75% while the CPU pegged. This was documented as the value
    //to use and then never made the default, so anyone running the trainer without setting the
    //environment got the configuration the measurement rejected.
    const int64_t batch_size = env_i("BATCH_SIZE", 8192);
    //Share of the target mass spread uniformly over the legal moves. 0 is the old loss.
    //0.05-0.15 is the usual range; the right value here depends on how often an unlisted
    //move is actually good, which is exactly what the experiment measures.
    const double label_smooth = env_d("LABEL_SMOOTH", 0.0);
    //FEATURE_CACHE=<dir> reads pre-extracted features instead of computing them.
    //
    //A shard's cache is 10.1 GB, far more than this machine's RAM, and the DataLoader
    //samples randomly -- so the shard is presented in chunks. Each chunk is one sequential
    //read of CHUNK_SIZE*1024 bytes, then shuffled fully in RAM. Random 1 KB reads straight
    //off an external drive would need ~56k IOPS to keep the GPU fed and would not get close.
    const char * cache_env = std::getenv("FEATURE_CACHE");
    const std::string cache_dir = cache_env ? cache_env : "";
    //250000, not 1000000. At the larger value the OS memory compressor thrashed and throughput
    //halved. Like BATCH_SIZE and LR_MAX, this was measured, written down, and then left at the
    //value the measurement rejected.
    const size_t chunk_size = (size_t)env_i("CHUNK_SIZE", 250000);
    //A run was one pass. Multi-epoch training had to be done by re-invoking, which restarted
    //the schedule each time -- so the anneal never completed. EPOCHS drives both the loop and
    //TOTAL_STEPS below, keeping the cosine sized to the whole run.
    const int num_epochs = (int)(std::getenv("EPOCHS") ? std::atoll(std::getenv("EPOCHS")) : 1);
    //The loader, not the GPU, is the bottleneck: MPS sits around 75% while CPU exceeds
    //100%, because every sample costs board reconstruction, full legal-move generation,
    //NNUE feature extraction (6.6 us) and a tensor allocation -- all on one thread with
    //workers(1). This machine has 4 performance cores, so leave one for the main thread
    //driving the GPU and give the rest to sample production. Each worker builds its own
    //NNUEContext lazily via thread_ctx(), since the accumulator stack is not thread-safe.
#ifndef NUM_WORKERS
#define NUM_WORKERS 1
#endif
    const int num_workers = (int)env_i("NUM_WORKERS", NUM_WORKERS);
    //2e-3, not 2e-4. Measured better at batch 8192; the original was roughly 5-10x too low, and
    //the two belong together -- a larger batch wants a larger peak rate. Only ever used as the
    //default for LR_MAX and as the optimizer's initial value, which the cosine schedule
    //overwrites on the first step.
    double learning_rate = 2e-3; 
    //2e-4 is conservative for Adam on a model this size at batch 2048, where the usual
    //scaling argument says a larger batch wants a LARGER step, not a smaller one.
    const double LR_MAX = env_d("LR_MAX", learning_rate); 
    const double LR_MIN = 1e-5;
    const std::string base_data_path = "../lichess_db_pvs_eval.bin";
    //Was ../Downloads/lichess_db_broadcast_2026-02.bin - a PRE-PV-format file read by
    //the PV-aware reader, so every validation number this trainer has ever printed was
    //computed on garbage. lichess_db_pv_eval_test.bin is the same format as training and
    //cannot leak into it: get_data_files() only matches the base name plus _1..29.bin.
    const std::string test_data_path = "../lichess_db_pvs_eval_test.bin";
    //Overridable so a differently-shaped model can be trained without colliding with the
    //current one. The architecture is also encoded in every checkpoint name, and the size
    //guard on load refuses a checkpoint trained for other dimensions, so the two cannot be
    //mixed up silently.
    const std::string weights_file = std::getenv("WEIGHTS") ? std::getenv("WEIGHTS")
                                                            : "nnue_policy.pt";
    const std::string checkpoint_prefix = std::getenv("CKPT_PREFIX")
                                        ? std::getenv("CKPT_PREFIX")
                                        : "nnue_policy_checkpoint_";

    auto file_list = get_data_files(base_data_path);
    std::cout << "Found " << file_list.size() << " data files." << std::endl;

    //BUILD_CACHE=<dir> extracts features for every shard and exits. Run it once; after that
    //FEATURE_CACHE=<dir> trains from it. Needs the NNUE nets, nothing else -- no model, no
    //optimiser, no GPU.
    {
        //CACHE_SHARDS=n limits either operation to the first n shards, so the cache can be
        //built a few at a time and a change can be tested without a 1h41m run.
        auto subset = file_list;
        if (const char * ns = std::getenv("CACHE_SHARDS")) {
            size_t k = (size_t)std::atoll(ns);
            if (k > 0 && k < subset.size()) subset.resize(k);
        }
        if (const char * cd = std::getenv("BUILD_CACHE")) return build_feature_cache(cd, subset);
        if (const char * cd = std::getenv("VERIFY_CACHE")) {
            const int per = std::getenv("VERIFY_N") ? std::atoi(std::getenv("VERIFY_N")) : 200;
            return verify_feature_cache(cd, subset, per);
        }
    }
        
    //With FEATURE_CACHE set, train only on shards whose cache exists. The cache is built a
    //few shards at a time, so the alternative is either crashing on the first uncached shard
    //or silently falling back to recomputing features -- and a silent fallback would look
    //exactly like the cache not helping.
    if (const char * cd = std::getenv("FEATURE_CACHE")) {
        std::vector<std::string> cached;
        for (const auto& f : file_list)
            if (std::filesystem::exists(feat_cache_path(cd, f))) cached.push_back(f);
        std::cout << "Feature cache " << cd << ": " << cached.size() << " of "
                  << file_list.size() << " shards available" << std::endl;
        if (cached.empty()) {
            std::cerr << "FATAL: FEATURE_CACHE set but no shard has a cache. "
                         "Run BUILD_CACHE=" << cd << " first." << std::endl;
            return 6;
        }
        file_list.swap(cached);
    }

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
    NNUEPolicy model;
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
    std::cout << "Label smoothing: " << label_smooth
              << (label_smooth > 0.0 ? "" : "  (0 = original loss)") << std::endl;
    std::cout << "Config: NNUE_DIMS=" << NNUE_DIMS
              << " H1=" << POLICY_H1 << " H2=" << POLICY_H2
              << " POLICY_OUT=" << POLICY_OUT
              << " CONV_POLICY=" << CONV_POLICY
              << " MAX_PVS=" << MAX_PVS << " MAX_TRAIN_PVS=" << MAX_TRAIN_PVS
              << " POLICY_TEMP=" << POLICY_TEMP << std::endl;
    std::cout << "Total number of parameters: " << param_count << std::endl;
        // PARAMS_ONLY=1 prints the model size and exits, so the architecture can be
        // checked without loading a shard.
        if (std::getenv("PARAMS_ONLY")) return 0;

    // EXPORT_WEIGHTS=<path> writes the trained parameters as a flat float32 blob the
    // engine can mmap, so creatica needs no libtorch at all. Layout is the
    // natural row-major order of each tensor, which is already what the engine's inference
    // loops want: W1 is [in][h1] so a sparse pass accumulates whole contiguous rows, and
    // emb is [out][h2] so scoring one move is one contiguous dot product.
    if (const char* xp = std::getenv("EXPORT_WEIGHTS")) {
        auto& m = *model;
        const torch::Tensor ts[5] = { m.W1, m.b1, m.W2, m.b2, m.emb };
        const char* nm[5] = { "W1", "b1", "W2", "b2", "emb" };
        FILE* f = std::fopen(xp, "wb");
        if (!f) { std::cerr << "EXPORT_WEIGHTS: cannot write " << xp << std::endl; return 4; }
        const int32_t hdr[7] = { 0x4C4F5043 /*"CPOL"*/, 1, NNUE_DIMS, POLICY_H1, POLICY_H2,
                                 POLICY_OUT, CONV_POLICY };
        std::fwrite(hdr, sizeof(int32_t), 7, f);
        for (int i = 0; i < 5; ++i) {
            auto t = ts[i].detach().to(torch::kCPU).to(torch::kFloat32).contiguous();
            std::fwrite(t.data_ptr<float>(), sizeof(float), t.numel(), f);
            std::cout << "  " << nm[i] << " " << t.sizes() << " -> " << t.numel() << " floats" << std::endl;
        }
        std::fclose(f);
        std::cout << "Exported to " << xp << std::endl;
        return 0;
    }
    
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
        for (int i = 0; i < 5; ++i) { auto o = model->forward(x); sink += o.sum().item<double>(); }
        auto t0 = std::chrono::high_resolution_clock::now();
        const int ITERS = 30;
        for (int i = 0; i < ITERS; ++i) { auto o = model->forward(x); sink += o.sum().item<double>(); }
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
    const int64_t TOTAL_STEPS = env_i("TOTAL_STEPS", total_estimated_positions / batch_size); 
    int64_t global_step = 0;
    //Resume the SCHEDULE, not just the weights. torch::load restores the parameters but
    //nothing about where the run had got to, so a restarted run re-entered the cosine at
    //progress 0 and pushed the learning rate back to LR_MAX -- re-warming an already
    //trained model and discarding the anneal. Runs here get killed often enough (memory
    //pressure on an 8 GB machine) that without this a long training run can never reach the
    //low-LR tail at all, which is where much of the final quality comes from.
    //
    //The step counter lives beside the weights, keyed to the checkpoint file, so a resume
    //picks up the schedule exactly where it stopped. Delete the .step file to start over.
    const std::string step_file = weights_file + ".step";
    if (!std::getenv("BENCH_FORWARD") && std::filesystem::exists(step_file)) {
        FILE * sf = std::fopen(step_file.c_str(), "r");
        if (sf) {
            long long v = 0;
            if (std::fscanf(sf, "%lld", &v) == 1 && v > 0) global_step = v;
            std::fclose(sf);
            std::cout << "Resuming schedule at step " << global_step << std::endl;
        }
    }
    //TOTAL_STEPS is what makes the cosine reach its floor at the end of the run. Raising the
    //epoch count without raising this bottoms the schedule out partway and trains the
    //remainder at LR_MIN.
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
        double last_top1 = 0.0;   //carried between prints, see the Top-1 note below
    int file_number = 0; //when the list is randomized, it's easy to keep track
                
        constexpr float policy_weight = 1.0f;
        for (const auto& filepath : file_list) {
            std::cout << "  Processing file " << ++file_number << ": " << filepath << std::endl;
            auto shard_samples = ChessDataset::load_shard(filepath);
            const size_t shard_n = shard_samples->size();
            const std::string cpath = cache_dir.empty() ? std::string()
                                                        : feat_cache_path(cache_dir, filepath);
            //Without a cache there is nothing to stream, so the whole shard stays one chunk
            //and behaviour is exactly what it was.
            const size_t chunk_n = cache_dir.empty() ? shard_n
                                                     : std::min<size_t>(chunk_size, shard_n);
            for (size_t chunk_base = 0; chunk_base < shard_n; chunk_base += chunk_n) {
                const size_t this_n = std::min(chunk_n, shard_n - chunk_base);
                auto dataset = ChessDataset(shard_samples, chunk_base, this_n, cpath)
                                   .map(torch::data::transforms::Stack<>());
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
                    auto data    = batch.data.to(device).to(torch::kFloat32).div_(127.0f);
                    auto targets = batch.target.to(device);          // [batch, 4097]
        
                    auto tg = unpack_targets(targets);

                    optimizer.zero_grad();

                    auto policy_logits = model->forward(data);            // [batch, POLICY_OUT]

                    auto mask   = legality_mask(policy_logits, tg.legal_idx);
                    auto masked = policy_logits.masked_fill(mask < 0.5f, -1e9f);
                    auto logp   = torch::log_softmax(masked, 1);


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

                    // Label smoothing over the LEGAL moves.
                    //
                    // The target above is a proper softmax over the PV centipawn scores,
                    // so the loss is already score-aware where there are several PVs to be
                    // aware of. The trouble is that 57% of records carry exactly ONE PV, and
                    // for those the target is a one-hot: 1.0 on PV1 and exactly 0.0 on the
                    // other ~28 legal moves. Cross-entropy then drives the probability of
                    // every unlisted move toward zero, which asserts something the data never
                    // says. Stockfish did not report those moves as bad; it only reported
                    // fewer PVs. On a position where four moves are near-equal, three of them
                    // are being actively punished for being good.
                    //
                    // Mixing in a uniform distribution over the legal moves removes the false
                    // part of the claim while keeping the true part: PV1 is still by far the
                    // most likely move, the rest are merely no longer impossible. This is the
                    // usual remedy for confidently-wrong labels and it costs one extra masked
                    // sum per batch.
                    //
                    // LABEL_SMOOTH=0 reproduces the old loss exactly, so this is A/B-able
                    // against the existing checkpoints rather than a one-way change.
                    if (label_smooth > 0.0) {
                        auto n_legal_f  = mask.sum(1).clamp_min(1.0f);
                        auto uniform_ce = -(logp * mask).sum(1) / n_legal_f;
                        per_sample = (1.0f - (float)label_smooth) * per_sample
                                   + (float)label_smooth * uniform_ce;
                    }
                    auto has_pv     = (wsum.squeeze(1) > 0.0f).to(logp.scalar_type());
                    auto n_have     = has_pv.sum().clamp_min(1.0f);
                    auto loss_policy = (per_sample * has_pv).sum() / n_have;

                    //Top-1 costs several tensor ops plus an .item() that forces a GPU->CPU
                    //sync, and it is only ever read when printing. On MPS every operation is
                    //a synchronous dispatch -- profiling showed the step dominated by
                    //dispatch_sync_with_rethrow -- so paying this on every batch is pure
                    //latency. Compute it only on the batches that print it.
                    double batch_top1 = last_top1;
                    if ((epoch_total_batches + 1) % 100 == 0) {
                      auto rank1  = pv1_rank(masked, mask, tg.pv_idx);
                      auto ok_pv1 = pv1_is_legal(mask, tg.pv_idx).to(logp.scalar_type()) * has_pv;
                      auto nn     = ok_pv1.sum().clamp_min(1.0f);
                      batch_top1  = 100.0 * (((rank1 == 0).to(logp.scalar_type()) * ok_pv1).sum()
                                             / nn).item<double>();
                      last_top1   = batch_top1;
                    }

                    //Policy only: NNUE supplies the value, better than anything trainable
                    //alongside it. Nothing to weight against, so the loss IS the policy
                    //cross-entropy. Random baseline is log(~29) = 3.37.
                    auto loss = loss_policy;
        
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
                                                                                epoch_total_batches++;
                    file_batches++;
                
                    if (epoch_total_batches % 100 == 0) {
                        double elapsed = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
                        std::printf("\r    Batch %ld | policy loss %.4f (random = 3.37) | Top1 %.1f%% "
                                    "| LR %.7f | batch %lld | %.0f pos/s\033[K",
                                    epoch_total_batches,
                                    loss_policy.item<double>(),
                                    batch_top1,
                                    current_lr, batch_size,
                                    file_batches * batch_size / elapsed);
                        std::fflush(stdout);
                    }
                }
            }            
            std::cout << "\n  Finished file: " << filepath << std::endl;
            if (ChessDataset::profiling()) ChessDataset::dump_profile();

            //Written next to the checkpoint below, and for the same reason: a run that dies
            //between files must resume the schedule, not restart it.
            {
                FILE * sf = std::fopen(step_file.c_str(), "w");
                if (sf) { std::fprintf(sf, "%lld\n", (long long)global_step); std::fclose(sf); }
            }

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
              std::snprintf(ckpt, sizeof ckpt, "%sh1%d_h2%d_c%d_e%d_f%02d.pt",
                            checkpoint_prefix.c_str(), POLICY_H1, POLICY_H2, CONV_POLICY,
                            epoch, file_number);
              torch::save(model, ckpt);
              std::cout << "  saved " << ckpt << std::endl;
            }

        
        double avg_loss = epoch_total_loss / (epoch_total_batches + 1); // Avoid div/0
        std::cout << "=== End of Epoch " << epoch << " | Avg Loss: " << avg_loss << " ===" << std::endl;
        
        // --- VALIDATION PHASE ---
        //Validation runs after every FILE, not every epoch -- the "End of Epoch" banner above
        //is misnamed and always has been. That is 28 validations per epoch, each reloading and
        //scoring VALIDATE_N positions with features computed from scratch (the test file has
        //no cache). Over a 10-epoch run that is 280 of them, which can outweigh the training
        //itself. VALIDATE_EVERY=n runs it every n files instead; VALIDATE_N sizes it.
        const int validate_every = std::getenv("VALIDATE_EVERY")
                                 ? std::atoi(std::getenv("VALIDATE_EVERY")) : 1;
        const bool do_validate = validate_every > 0
                              && (file_number % validate_every == 0
                                  || file_number == (int)file_list.size());
        if (do_validate && std::filesystem::exists(test_data_path)) {
            //Validation runs in float32.
            //
            //This used to be model->to(torch::kHalf) while the loop fed float32 batches. It
            //survived only while the model was freshly initialised: torch::load() rebinds the
            //registered parameters, so after a resume to(kHalf) really does convert the
            //tensors forward() multiplies, and the next matmul dies with
            //"expected mat1 and mat2 to have the same dtype". A validation path that works
            //only when you are NOT resuming is worse than a marginally slower one, and float32
            //is the more accurate measurement anyway. BENCH_FORWARD still has its own kHalf
            //mode for engine-comparable timings, which is where half actually belonged.
            model->to(torch::kFloat32);
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
                const size_t val_n = std::getenv("VALIDATE_N")
                                   ? (size_t)std::atoll(std::getenv("VALIDATE_N")) : 1000000;
                auto test_dataset = ChessDataset(test_data_path, val_n).map(torch::data::transforms::Stack<>());
                positions = test_dataset.size();
                auto test_loader = torch::data::make_data_loader(
                    std::move(test_dataset),
                    torch::data::DataLoaderOptions().batch_size(batch_size).workers(num_workers)
                );
        
                auto start = std::chrono::high_resolution_clock::now();
                for (auto& batch : *test_loader) {
                    auto data    = batch.data.to(device).to(torch::kFloat32).div_(127.0f);
                    auto targets = batch.target.to(device);             // [batch, 257]
        
                    auto tg = unpack_targets(targets);
                    auto value_target = tg.value.to(torch::kFloat32);

                    // Cast BEFORE masking: -1e9 is not representable in half.
                    auto policy_logits = model->forward(data).to(torch::kFloat32);


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

                    auto loss       = loss_policy;
                
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