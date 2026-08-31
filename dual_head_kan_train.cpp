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

/*#define STOCKFISH "/Users/ap/stockfish-macos-m1-apple-silicon"
#define MOVETIME 2000
#define DEPTH 0
#define HASH 256
#define THREADS 1
#define SYZYGY_PATH "/Users/ap/syzygy"
#define LOGGING false
#define LIMIT_STRENGTH false
#define ELO 2600
struct Engine stockfish;
struct NNUEContext {
    Stockfish::Eval::NNUE::AccumulatorStack * accumulator_stack;
    Stockfish::Eval::NNUE::AccumulatorCaches * caches;    
};
NNUEContext ctx;
void init_nnue();
void cleanup_nnue();
void init_nnue_context(NNUEContext& ctx);
void free_nnue_context(NNUEContext& ctx);
double evaluate_nnue(const Board& chess_board, NNUEContext& ctx);
void accumulator_stack_reset(NNUEContext& ctx);*/

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

struct ChessChebyKANImpl : torch::nn::Module {
    ChebyKANLayer layer1{nullptr}, layer2{nullptr};
    torch::nn::Linear heads{nullptr}; // fused: 32 -> (1 + 64 + 64) = 129
    //torch::nn::Linear value_head{nullptr};
    //torch::nn::Linear src_head{nullptr};
    //torch::nn::Linear dst_head{nullptr};

    ChessChebyKANImpl(int in_features = 576, int hidden = 256, int degree = 8) {
        layer1 = register_module("layer1", ChebyKANLayer(in_features, hidden, degree));
        layer2 = register_module("layer2", ChebyKANLayer(hidden, 64, degree));
        heads  = register_module("heads",  torch::nn::Linear(64, 129)); // all heads fused
        // Final mapping to a single scalar
        //value_head = register_module("value_head", torch::nn::Linear(64, 1));
        //src_head   = register_module("src_head",   torch::nn::Linear(64, 64));
        //dst_head   = register_module("dst_head",   torch::nn::Linear(64, 64));
    }

    //std::pair<torch::Tensor, torch::Tensor> forward(torch::Tensor x, torch::Tensor legal_move_mask  // [batch, 4096] float, 1=legal 0=illegal
    torch::Tensor forward(torch::Tensor x)  // [batch, 576] float
    {
        x = x.view({x.size(0), -1});         
        x = layer1->forward(x);
        x = layer2->forward(x);
        return heads->forward(x); // single matmul [batch, 129]
         
        //auto value  = torch::tanh(value_head->forward(x));
        //auto src_logits = src_head->forward(x);
        //auto dst_logits = dst_head->forward(x);
        //return {value, src_logits, dst_logits};
    }
};
TORCH_MODULE(ChessChebyKAN);

struct CompressedPosition {
    uint8_t piecesOnSquares[64];
    int8_t side_to_move;    // 0=White, 1=Black
    int8_t castling_rights; // Bitmask
    int8_t ep_file;         // 0-7, 8=None
    int16_t eval_cp;         // Centipawns
    int8_t pvs; //number of PVs - 1 (0, 1, 2)
    int8_t from_sq[3];
    int8_t to_sq[3];
    int8_t promo[3]; //0 - Queen, 1 - Knight, 2 - Bishop, 3 - Rook
    int16_t cp[3]; //PV eval
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
    pos.pvs = reader.read(2);
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
        if (count % 1000000 == 0) 
            std::cout << "Loaded " << count << " positions.\n" << std::flush;
        if (max_samples > 0 && samples.size() >= max_samples) break; // early exit
    }
    std::cout << "Finished loading " << samples.size() << " positions.\n";
  }

  // The "Hot" Path: Converts 1 struct into Tensors (Input + Label)
  torch::data::Example<> get(size_t index) override {
    constexpr double policy_temperature = 1.5;
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
    if (pos.castling_rights & 1) board.castlingRooks |= msBit(board.side[ColorWhite] & board.pieceTypes[Rook - 1]);
    if (pos.castling_rights & 2) board.castlingRooks |= lsBit(board.side[ColorWhite] & board.pieceTypes[Rook - 1]);
    if (pos.castling_rights & 4) board.castlingRooks |= msBit(board.side[ColorBlack] & board.pieceTypes[Rook - 1]);
    if (pos.castling_rights & 8) board.castlingRooks |= lsBit(board.side[ColorBlack] & board.pieceTypes[Rook - 1]);
    
    //torch::Tensor legal_mask = torch::zeros({4096}, torch::kFloat32);
    //float* mask_data = legal_mask.data_ptr<float>();
    torch::Tensor src_mask = torch::zeros({64}, torch::kFloat32);
    torch::Tensor dst_mask = torch::zeros({64}, torch::kFloat32);
    
    bool is_black = (board.sideToMove == ColorBlack);    		
    // 1. Determine which bitboard belongs to 'Friendly' (side to move)
    //board is oriented for the side to move perspective to help the model learn it only once
	  auto [king_moves, pinned, pinning, checkers, kingSquare] = kingMoves(board);
    uint64_t all_legal_moves = king_moves;
    int num_checkers = checkers ? bitCount(checkers) : 0;
    auto [check_mask, ep_mask] = checkers ? checkMask(board, kingSquare, checkers) : std::make_pair(0xffffffffffffffffULL, 0ULL);
    int input_from = is_black ? (kingSquare ^ 56) : kingSquare;
    src_mask[input_from] = 1.0f;
    while (king_moves) {
      const Square to_sq = popLSB(king_moves);
      int input_to = is_black ? (to_sq ^ 56) : to_sq;
      dst_mask[input_to] = 1.0f;
    }

    if (num_checkers <= 1) {
      // Loop through friendly pieces to gather their moves
      for (PieceType pt = Pawn; pt <= Queen; ++pt) {
        uint64_t occupations = board.side[is_black ? ColorBlack : ColorWhite] & board.pieceTypes[pt - 1]; 
        while (occupations) {
          const Square sq = popLSB(occupations);
          int input_from = is_black ? (sq ^ 56) : sq;
          src_mask[input_from] = 1.0f;
          uint64_t moves = piece_moves(board, pt, sq, kingSquare, pinned, pinning, check_mask, ep_mask);
          all_legal_moves |= moves;
          while (moves) {
            const Square to_sq = popLSB(moves);
            int input_to = is_black ? (to_sq ^ 56) : to_sq;
            dst_mask[input_to] = 1.0f;
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

    // Policy target [4096] sparse softmax over PV moves
    //torch::Tensor policy_target = torch::zeros({4096}, torch::kFloat32);
    torch::Tensor src_target = torch::zeros({64}, torch::kFloat32);
    torch::Tensor dst_target = torch::zeros({64}, torch::kFloat32);
    
    //from_sq/to_sq/cp are [3] but pvs is a 2-BIT field, so a corrupt or wrong-format
    //record decodes to num_pvs == 4 and the loop below writes one past the end of all
    //three stack arrays. Clamp; the writer only ever emits 1..3.
    int num_pvs = std::min<int>(pos.pvs + 1, 3); // pvs field is stored as count - 1
    if (num_pvs > 0 && pos.from_sq[0] >= 0) {
        if (num_pvs == 1) {
            // Single PV: one-hot
            int from_sq = is_black ? (pos.from_sq[0] ^ 56) : pos.from_sq[0];
            int to_sq   = is_black ? (pos.to_sq[0]   ^ 56) : pos.to_sq[0];
            src_target[from_sq] = 1.0f;
            dst_target[to_sq] = 1.0f;
        } else {
            // Multiple PVs: softmax over cp scores
            // Collect scores, normalize relative to best (PV1 is always best by construction)
            float scores[3];
            int src_indices[3];
            int dst_indices[3];
            for (int i = 0; i < num_pvs; ++i) {
                int from_sq = is_black ? (pos.from_sq[i] ^ 56) : pos.from_sq[i];
                int to_sq   = is_black ? (pos.to_sq[i]   ^ 56) : pos.to_sq[i];
                src_indices[i] = from_sq;
                dst_indices[i] = to_sq;
                //cp is WHITE-relative (see the value target above, which flips it for
                //Black). The squares were being flipped here but the scores were not, so
                //for a Black position PV1 - lichess orders PVs best-first for the side to
                //move - had the LOWEST cp, max_score below subtracted a minimum, and the
                //softmax put its weight on Black's WORST moves. Inverted on half the data.
                float cp_i = is_black ? -static_cast<float>(pos.cp[i]) : static_cast<float>(pos.cp[i]);
                float cp_clamped = std::max(-1000.0f, std::min(1000.0f, cp_i));
                scores[i] = cp_clamped / (100.0f * policy_temperature);                
            }
            // Softmax: subtract max for numerical stability (scores[0] is always max)
            float max_score = scores[0];
            float sum = 0.0f;
            for (int i = 0; i < num_pvs; ++i) {
                scores[i] = std::exp(scores[i] - max_score);
                sum += scores[i];
            }
            for (int i = 0; i < num_pvs; ++i) {
                //ACCUMULATE: these are marginals over 64 squares, and two PVs routinely
                //share a from-square (the same piece going to different squares) or a
                //to-square. Plain assignment silently dropped the earlier PV's mass.
                src_target[src_indices[i]] += scores[i] / sum;
                dst_target[dst_indices[i]] += scores[i] / sum;
            }
        }
    }

    // Return both targets stacked — value head gets targets[0], policy gets targets[1:]
    // Or return as separate tensors using a custom collate / multi-target example
    torch::Tensor targets = torch::cat({value_target, src_target, dst_target, src_mask, dst_mask}); // [1 + 64 + 64 + 64 + 64] = [257]
    return {input, targets};
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

int main() {
    Stockfish::Bitboards::init();

    // 1. Hyperparameters
    const int64_t batch_size = 12288;//2 * 8192; 
    const int num_epochs = 1;
    const int num_workers = 1;
    double learning_rate = 2e-5; 
    const double LR_MAX = learning_rate; 
    const double LR_MIN = 1e-6;
    const std::string base_data_path = "../lichess_db_pv_eval.bin";
    //Was ../Downloads/lichess_db_broadcast_2026-02.bin - a PRE-PV-format file read by
    //the PV-aware reader, so every validation number this trainer has ever printed was
    //computed on garbage. lichess_db_pv_eval_test.bin is the same format as training and
    //cannot leak into it: get_data_files() only matches the base name plus _1..29.bin.
    const std::string test_data_path = "../lichess_db_pv_eval_test.bin";
    const std::string weights_file = "dual_head_kan.pt";
    const std::string old_weights_file = "simple_bkan.pt";
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
    if (std::filesystem::exists(weights_file)) {
        try {
            std::cout << "Loading weights from " << weights_file << "..." << std::endl;
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
    std::cout << "Total number of parameters: " << param_count << std::endl;
    
    model->to(device); // Move model to GPU/MPS/CPU
    
    torch::optim::Adam optimizer(model->parameters(), torch::optim::AdamOptions(learning_rate));

    // 6. Training Loop
    std::cout << "Starting training..." << std::endl;       
    
    // Estimate total batches: ~60M positions per epoch / 1024 batch size * 10 epochs
    // You can hardcode this or calculate it dynamically if you know the exact file sizes.
    const int64_t total_estimated_positions = 300000000ULL * num_epochs; 
    const int64_t TOTAL_STEPS = total_estimated_positions / batch_size; 
    int64_t global_step = 0;
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
                for (auto& batch : *data_loader) {
                    auto data    = batch.data.to(device);
                    auto targets = batch.target.to(device);          // [batch, 4097]
        
                    auto value_target  = targets.slice(1, 0, 1);    // [batch, 1]
                    auto src_target = targets.slice(1, 1, 65); // [batch, 64]
                    auto mask_src = (src_target.sum(1) > 0.5f);
                    auto dst_target = targets.slice(1, 65, 129); // [batch, 64]
                    auto mask_dst = (dst_target.sum(1) > 0.5f);
                    auto src_mask = targets.slice(1, 129, 197); // [batch, 64]
                    auto dst_mask = targets.slice(1, 197, 257); // [batch, 64]
                    //auto policy_target = targets.slice(1, 1, 4097); // [batch, 4096]
                    //auto legal_mask    = targets.slice(1, 4097, 8193); // [batch, 4096]
        
                    optimizer.zero_grad();
        
                    //auto [value_out, policy_logits] = model->forward(data, legal_mask);
                    auto out = model->forward(data);
                    auto value_out      = torch::tanh(out.slice(1, 0, 1));    // [batch, 1]
                    auto src_logits = out.slice(1, 1, 65);                // [batch, 64]
                    auto dst_logits = out.slice(1, 65, 129);              // [batch, 64]
                    
                    // Value loss
                    auto loss_value = torch::mse_loss(value_out, value_target);
                    torch::Tensor loss_src = torch::zeros({1}, torch::TensorOptions().device(device));
                    torch::Tensor loss_dst = torch::zeros({1}, torch::TensorOptions().device(device));
                    
                    if (mask_src.any().item<bool>()) {
                      auto src_logits_masked = src_logits.index({mask_src});
                      auto src_target_masked = src_target.index({mask_src});
                      loss_src   = torch::nn::functional::cross_entropy(src_logits_masked, src_target_masked, torch::nn::functional::CrossEntropyFuncOptions().reduction(torch::kMean));
                    }
                    if (mask_dst.any().item<bool>()) {
                      auto dst_logits_masked = dst_logits.index({mask_dst});
                      auto dst_target_masked = dst_target.index({mask_dst});
                      loss_dst   = torch::nn::functional::cross_entropy(dst_logits_masked, dst_target_masked, torch::nn::functional::CrossEntropyFuncOptions().reduction(torch::kMean));
                    }
                    // Random baselines:
                    // Value MSE baseline: ~0.33 (random tanh output vs target)
                    // Src CE baseline: log(~16) ≈ 2.77
                    // Dst CE baseline: log(~29) ≈ 3.37
                    
                    constexpr float w_value = 1.0f / 0.33f;  // ≈ 3.0
                    constexpr float w_src   = 1.0f / 2.77f;  // ≈ 0.36
                    constexpr float w_dst   = 1.0f / 3.37f;  // ≈ 0.30
                    
                    auto loss = w_value * loss_value + w_src * loss_src + w_dst * loss_dst;
        
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
                    epoch_src_loss += loss_src.item<double>();
                    epoch_dst_loss += loss_dst.item<double>();
                    epoch_total_batches++;
                
                    if (epoch_total_batches % 100 == 0) {
                        double elapsed = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
                        std::printf("\r    Batch %ld | Loss: %.4f (V: %.4f S: %.4f D: %.4f | LR %.7f | Batch size %lld | %lld nps",
                            epoch_total_batches,
                            loss.item<double>(),
                            loss_value.item<double>(),
                            loss_src.item<double>(),
                            loss_dst.item<double>(),
                            current_lr, batch_size, epoch_total_batches * batch_size / elapsed);
                        std::fflush(stdout);
                    }
                }
            }            
            std::cout << "\n  Finished file: " << filepath << std::endl;

            // Save Checkpoint
            torch::save(model, checkpoint_prefix + std::to_string(epoch) + ".pt");

        
        double avg_loss = epoch_total_loss / (epoch_total_batches + 1); // Avoid div/0
        std::cout << "=== End of Epoch " << epoch << " | Avg Loss: " << avg_loss << " ===" << std::endl;
        
        // --- VALIDATION PHASE ---
        if (std::filesystem::exists(test_data_path)) {
            model->to(torch::kHalf);
            model->eval();
            torch::NoGradGuard no_grad;
        
            double test_value_loss = 0.0, test_src_loss = 0.0, test_dst_loss = 0.0f;
            double mean_cp_error_total = 0.0, sign_accuracy_total = 0.0;
            double src_top1_correct = 0.0, src_top1_total = 0.0;
            double dst_top1_correct = 0.0, dst_top1_total = 0.0;
            size_t test_total_batches = 0;
            double elapsed;
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
        
                    auto value_target  = targets.slice(1, 0, 1).to(torch::kFloat32);        // [batch, 1]
                    auto src_target = targets.slice(1, 1, 65).to(torch::kFloat32);     // [batch, 64]
                    auto dst_target = targets.slice(1, 65, 129).to(torch::kFloat32);     // [batch, 64]
                    auto src_mask    = targets.slice(1, 129, 197).to(torch::kFloat32); // [batch, 64]
                    auto dst_mask    = targets.slice(1, 197, 257).to(torch::kFloat32); // [batch, 64]
        
                    auto out = model->forward(data);
                    out = out.to(torch::kFloat32);
                    auto value_out  = torch::tanh(out.slice(1, 0, 1));    // [batch, 1]
                    auto src_logits = out.slice(1, 1, 65);                // [batch, 64]
                    auto dst_logits = out.slice(1, 65, 129);              // [batch, 64]
        
                    // --- Value metrics (unchanged) ---
                    auto loss_value = torch::mse_loss(value_out, value_target);
        
                    auto pred_cp   = torch::atanh(torch::clamp(value_out,    -0.9999f, 0.9999f)) * eval_scale;
                    auto target_cp = torch::atanh(torch::clamp(value_target, -0.9999f, 0.9999f)) * eval_scale;
                    mean_cp_error_total += torch::abs(pred_cp - target_cp).mean().item<float>();
                    sign_accuracy_total += ((value_out * value_target) > 0).to(torch::kFloat32).mean().item<float>() * 100.0;
        
                    auto mask_src = (src_target.sum(1) > 0.5f);
                    auto mask_dst = (dst_target.sum(1) > 0.5f);
                    torch::Tensor loss_src = torch::zeros({1}, torch::TensorOptions().device(device));
                    torch::Tensor loss_dst = torch::zeros({1}, torch::TensorOptions().device(device));
                    
                    // --- Policy metrics ---
                    if (mask_src.any().item<bool>()) {
                      auto src_logits_masked = src_logits.index({mask_src});
                      auto src_target_masked = src_target.index({mask_src});
                      loss_src   = torch::nn::functional::cross_entropy(src_logits_masked, src_target_masked, torch::nn::functional::CrossEntropyFuncOptions().reduction(torch::kMean));
                      // Top-1 accuracy: did argmax(logits) match argmax(policy_target)?
                      auto pred_src   = src_logits_masked.argmax(1);  // [M]
                      auto target_src = src_target_masked.argmax(1);  // [M]
                      src_top1_correct += (pred_src == target_src).to(torch::kFloat32).sum().item<float>();
                      src_top1_total   += src_logits_masked.size(0);
                    }
                    if (mask_dst.any().item<bool>()) {
                      auto dst_logits_masked = dst_logits.index({mask_dst});
                      auto dst_target_masked = dst_target.index({mask_dst});
                      loss_dst   = torch::nn::functional::cross_entropy(dst_logits_masked, dst_target_masked, torch::nn::functional::CrossEntropyFuncOptions().reduction(torch::kMean));
                      auto pred_dst   = dst_logits_masked.argmax(1);  // [M]
                      auto target_dst = dst_target_masked.argmax(1);  // [M]
                      dst_top1_correct += (pred_dst == target_dst).to(torch::kFloat32).sum().item<float>();
                      dst_top1_total   += dst_logits_masked.size(0);
                    }
                    auto loss       = loss_value + policy_weight * (loss_src + loss_dst);
                
                    test_value_loss  += loss_value.item<double>();
                    test_src_loss += loss_src.item<double>();
                    test_dst_loss += loss_dst.item<double>();
                    test_total_batches++;
                }
                double elapsed = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
            }
        
            auto avg = [&](double x) { return test_total_batches > 0 ? x / test_total_batches : 0.0; };
        
            std::cout << "    Value  Loss:     " << avg(test_value_loss)  << "\n";
            std::cout << "    Src Loss:     " << avg(test_src_loss) << "\n";
            std::cout << "    Dst Loss:     " << avg(test_dst_loss) << "\n";
            std::cout << "    Avg CP Error:    " << avg(mean_cp_error_total) << " centipawns\n";
            std::cout << "    Sign Accuracy:   " << avg(sign_accuracy_total) << "%\n";
            std::cout << "    Src Top-1:    "
                      << (src_top1_total > 0 ? 100.0 * src_top1_correct / src_top1_total : 0.0)
                      << "% (" << (size_t)src_top1_total << " positions with PV)\n";
            std::cout << "    Dst Top-1:    "
                      << (dst_top1_total > 0 ? 100.0 * dst_top1_correct / dst_top1_total : 0.0)
                      << "% (" << (size_t)dst_top1_total << " positions with PV)\n";
            std::cout << "    Time: " << elapsed << " sec, "
                      << positions.value() / elapsed << " positions/sec\n";
        
            model->to(torch::kFloat32);
        }        
        
      }     
    }
    return 0;
}