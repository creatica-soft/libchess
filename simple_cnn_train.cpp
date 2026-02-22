//c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -O3 -flto -I /Users/ap/Downloads/libtorch2/include -I /Users/ap/Downloads/libtorch2/include/torch/csrc/api/include -L /Users/ap/Downloads/libtorch2/lib -L /Users/ap/libchess -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/Users/ap/Downloads/libtorch2/lib,-rpath,/Users/ap/libchess -o simple_cnn_train simple_cnn_train.cpp

#include <torch/torch.h>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <cstdint>
#include <iomanip>
#include <random>
#include "libchess.h"
const float eval_scale = 620.0f; // conversion scale from cp to cnn target and back to cp from cnn output
//target = tanh(cp / eval_scale);
//cp = eval_scale * atanh(cnn_output);

// Define a simple, fast CNN module
struct SimpleCNNImpl : torch::nn::Module {
    // Layers
    torch::nn::Conv2d conv1{nullptr};
    torch::nn::BatchNorm2d bn1{nullptr};
    torch::nn::Conv2d conv2{nullptr};
    torch::nn::BatchNorm2d bn2{nullptr};
    torch::nn::Conv2d conv3{nullptr};
    torch::nn::BatchNorm2d bn3{nullptr};
    torch::nn::Linear fc1{nullptr};
    torch::nn::Linear fc_out{nullptr};

    SimpleCNNImpl() {
        // Input channels: 15 (12 pieces + 2 attack maps + 1 legal moves)
        // Output channels: 64 filters
        // Kernel: 3x3, Padding: 1 (to keep 8x8 size)
        conv1 = register_module("conv1", torch::nn::Conv2d(torch::nn::Conv2dOptions(15, 64, 3).padding(1)));
        bn1 = register_module("bn1", torch::nn::BatchNorm2d(64));

        // Layer 2: 64 -> 128 filters
        conv2 = register_module("conv2", torch::nn::Conv2d(torch::nn::Conv2dOptions(64, 128, 3).padding(1)));
        bn2 = register_module("bn2", torch::nn::BatchNorm2d(128));
        
        //Compression filter 128 -> 64
        conv3 = register_module("conv3", torch::nn::Conv2d(torch::nn::Conv2dOptions(128, 64, 1))); 
        bn3 = register_module("bn3", torch::nn::BatchNorm2d(64));

        // Fully Connected Head
        // Input size: 64 channels * 8 * 8 board = 4096 features, output 64, tried with 128 - was not stable
        fc1 = register_module("fc1", torch::nn::Linear(64 * 8 * 8, 64));
        fc_out = register_module("fc_out", torch::nn::Linear(64, 1));
    }

    torch::Tensor forward(torch::Tensor x) {
        // x shape: [BatchSize, 15, 8, 8]
        
        // Block 1: Conv -> BN -> ReLU
        x = torch::relu(bn1(conv1(x)));
        
        // Block 2: Conv -> BN -> ReLU
        x = torch::relu(bn2(conv2(x)));
        
        // Compression
        x = torch::relu(bn3(conv3(x)));

        // Flatten: [Batch, 64, 8, 8] -> [Batch, 4096]
        x = x.view({x.size(0), -1});
        
        // Dense Layers
        x = torch::relu(fc1(x));
        x = torch::tanh(fc_out(x)); // Output between -1 (Loss) and 1 (Win)
        
        return x;
    }
};
TORCH_MODULE(SimpleCNN);

void init_resnet_weights(SimpleCNN& model) {
    torch::NoGradGuard no_grad;

    // 1. Initialize Input Layer
    torch::nn::init::kaiming_normal_(model->conv2->weight, 0.0, torch::kFanIn, torch::kReLU);
    // 2. Initialize Value Head
    torch::nn::init::kaiming_normal_(model->conv2->weight, 0.0, torch::kFanIn, torch::kReLU);
    torch::nn::init::kaiming_normal_(model->fc1->weight);
    //model->fc1->weight.data().mul_(0.01); // Scale down by 100x
    //torch::nn::init::constant_(model->fc1->bias, 0.0);
    // Xavier uniform for the final tanh-bound output
    torch::nn::init::xavier_uniform_(model->fc_out->weight);
    torch::nn::init::constant_(model->fc_out->bias, 0.0);
}


struct CompressedPosition {
    uint8_t piecesOnSquares[64];
    int side_to_move;    // 0=White, 1=Black
    int castling_rights; // Bitmask
    int ep_file;         // 0-7, 8=None
    int eval_cp;         // Centipawns
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
    std::fill(std::begin(pos.piecesOnSquares), std::end(pos.piecesOnSquares), 0);

    // 2. Read Number of Pieces (5 bits); we subtracted 1 during encoding to allow 32 pieces using 5 bits, here we need to add 1
    int num_pieces = reader.read(5) + 1;

    // 3. Read Each Piece (10 bits: 6 sq + 1 col + 3 type)
    for (int i = 0; i < num_pieces; ++i) {
        int square = reader.read(6);
        int color = reader.read(1);
        int type = reader.read(3);
        pos.piecesOnSquares[square] = PC(color, type);
    }

    // 4. Global State
    pos.side_to_move = reader.read(1);
    pos.castling_rights = reader.read(4);
    pos.ep_file = reader.read(4);

    // 5. Eval (16 bits)
    // We must cast to int16_t to interpret the bits as a signed number
    uint16_t raw_eval = static_cast<uint16_t>(reader.read(16));
    pos.eval_cp = static_cast<int16_t>(raw_eval); //eval is from white point of view

    // 6. Align reader (discard padding)
    reader.align();
    return true;
}

// --- 1. Define the Custom Dataset ---
class ChessDataset : public torch::data::datasets::Dataset<ChessDataset> {
private:
    std::vector<CompressedPosition> samples;
public:
    ChessDataset(const std::string& filepath) {
        std::cout << "Opening dataset: " << filepath << "..." << std::endl;
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Could not open file!");
        }

        BitReader reader(file);
        CompressedPosition pos;
        int cp_min = 1000, cp_max = -1000;
        
        int count = 0;
        while (read_next_position(reader, pos)) {
            samples.push_back(pos);
            cp_min = std::min(cp_min, pos.eval_cp);
            cp_max = std::max(cp_max, pos.eval_cp);
            count++;
            if (count % 1000000 == 0) std::cout << "Loaded " << count << " positions. cp_min " << cp_min << ", cp_max " << cp_max << "\n" << std::flush;
        }
        std::cout << "Finished loading " << samples.size() << " positions. cp_min " << cp_min << ", cp_max " << cp_max << std::endl;

    }
    // The "Hot" Path: Converts 1 struct into Tensors (Input + Label)
    torch::data::Example<> get(size_t index) override {
        const CompressedPosition& pos = samples[index];
        Board board = {};
        board.sideToMove = (Color)pos.side_to_move;
        board.enPassant = (File)pos.ep_file;
        if (pos.castling_rights & 1) board.castlingRook[0][0] = FileH;
        else board.castlingRook[0][0] = FileNone;
        if (pos.castling_rights & 2) board.castlingRook[0][1] = FileA;
        else board.castlingRook[0][1] = FileNone;
        if (pos.castling_rights & 4) board.castlingRook[1][0] = FileH;
        else board.castlingRook[1][0] = FileNone;
        if (pos.castling_rights & 8) board.castlingRook[1][1] = FileA;
        else board.castlingRook[1][1] = FileNone;

        for(int sq = 0; sq < 64; ++sq) {
            board.piecesOnSquares[sq] = (Piece)pos.piecesOnSquares[sq];
            Piece pc = (Piece)pos.piecesOnSquares[sq];
            PieceType type = PC_TYPE(pc);
            Color color = PC_COLOR(pc);
            if (type != PieceTypeNone) {
              board.side[color] |= (1ULL << sq);
              board.pieceTypes[type - 1] |= (1ULL << sq);
            }
        }
        
    		struct MovesContext movesContext;
    		unsigned long long movesFromSquares[64] = {0};
    		unsigned long long whiteAttacks, blackAttacks, legalMoves = 0;
    		if (board.sideToMove == ColorWhite) {
    		  board.sideToMove = ColorBlack;
    		  whiteAttacks = getAttackedSquaresOnly(&board);
    		  board.sideToMove = ColorWhite;
    		  blackAttacks = getAttackedSquares(&board, &movesContext);
      		generateMoves(&board, &movesContext, blackAttacks, movesFromSquares);
    		} else {
    		  board.sideToMove = ColorWhite;
    		  blackAttacks = getAttackedSquaresOnly(&board);
    		  board.sideToMove = ColorBlack;
    		  whiteAttacks = getAttackedSquares(&board, &movesContext);
      		generateMoves(&board, &movesContext, whiteAttacks, movesFromSquares);
    		}
        
        // 3. Create the Tensor (16 channels x 8 x 8)
 /*       torch::Tensor input = torch::zeros({16, 8, 8}, torch::kFloat32);
        
        // Pointers to raw data for fast access
        float* data = input.data_ptr<float>();

        for(int sq = 0; sq < 64; ++sq) {
            Piece pc = (Piece)pos.piecesOnSquares[sq];
            if (pc != 0) {
              Color color = PC_COLOR(pc);
              PieceType type  = PC_TYPE(pc);
              int channel =  color * 6 + (type - 1);
              data[(channel << 6) | sq] = 1.0f;
            }
            
            // Fill Attack Planes (Channels 12, 13)
            if ((whiteAttacks >> sq) & 1) data[(12 << 6) | sq] = 1.0f;
            if ((blackAttacks >> sq) & 1) data[(13 << 6)| sq] = 1.0f;

            // Fill Legal Move Plane (Channel 14)
            if ((movesFromSquares[sq] >> sq) & 1) data[(14 << 6) | sq] = 1.0f;

            // Fill Meta Plane (Channel 15 - Side to Move)
            data[(15 << 6) | sq] = (pos.side_to_move == 0) ? 1.0f : -1.0f;
        }
*/

        //board is oriented for the side to move perspective to help the model learn it from that side
        torch::Tensor input = torch::zeros({15, 8, 8}, torch::kFloat32);
        
        // Pointers to raw data for fast access
        float* data = input.data_ptr<float>();
        
        bool is_black = (pos.side_to_move == ColorBlack);
        
        for(int sq = 0; sq < 64; ++sq) {
            // 1. Flip the square index if it's Black's turn
            // sq ^ 56 maps rank 0 to rank 7, rank 1 to rank 6, etc.
            int input_sq = is_black ? (sq ^ 56) : sq;
        
            int pc = pos.piecesOnSquares[sq];
            if (pc != 0) {
                int color = (pc >> 3) & 1;
                int type  = pc & 7;
        
                // 2. Swap piece colors: If it's Black's turn, Black's pieces 
                // go into the "Friendly" channels (0-5).
                int input_color = is_black ? (1 - color) : color;
                int channel = input_color * 6 + (type - 1);
                
                data[(channel << 6) | input_sq] = 1.0f;
            }
            
            // 1. Determine which bitboard belongs to 'Friendly' (side to move)
            uint64_t friendlyAttacks = is_black ? blackAttacks : whiteAttacks;
            uint64_t enemyAttacks    = is_black ? whiteAttacks : blackAttacks;
            
            // 2. Fill the planes using the flipped coordinate (input_sq)
            if ((friendlyAttacks >> sq) & 1) data[(12 << 6) | input_sq] = 1.0f;
            if ((enemyAttacks >> sq) & 1) data[(13 << 6) | input_sq] = 1.0f;        
            
            if ((movesFromSquares[sq] >> sq) & 1) data[(14 << 6) | input_sq] = 1.0f;
        
            // 4. Side to Move Plane (Channel 15) - no need
            // In this "Canonical" view, the side to move is ALWAYS 1.0f
            // because the board is already oriented for them.
            //data[(15 << 6) | input_sq] = 1.0f;
        }

        // 4. Create Target Tensor
        // Normalize centipawns (e.g., clamp between -1000 and 1000, then divide)
        //float normalized_score = std::max(-1000.0f, std::min(1000.0f, (float)pos.eval_cp));
        //normalized_score /= 1000.0f; // Range -1.0 to 1.0; negative score - white's loosing, positive - white's winning 
        //instead of linear clamping, use sigmoid
        float normalized_score = tanh((float)pos.eval_cp / eval_scale);
        
        // Important: If side_to_move is Black, but eval is from White's perspective, flip it!
        // NNUE usually trains on "Eval from Side-to-Move's perspective", so we do the same for our CNN
        if (pos.side_to_move == ColorBlack) normalized_score = -normalized_score;
        torch::Tensor target = torch::tensor({normalized_score}, torch::kFloat32);

        return {input, target};
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
    while (true) {
        std::string next_file = path_no_ext + "_" + std::to_string(index) + ".bin";
        if (fs::exists(next_file)) {
            files.push_back(next_file);
            index++;
        } else {
            break; 
        }
    }
    return files;
}

int main() {
    init_magic_bitboards();

    // 1. Hyperparameters
    const int64_t batch_size = 2048;
    double learning_rate = 0.0007;
    const int num_epochs = 10;
    const std::string base_data_path = "../lichess_db_eval.bin";
    const std::string test_data_path = "../Downloads/lichess_db_broadcast.bin";
    const std::string weights_file = "simple_cnnX.pt";

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
    SimpleCNN model;
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
        //init_resnet_weights(model);
        std::cout << "Weights file " << weights_file << " not found, using untrained model" << std::endl;
    }
    // Count parameters
    int64_t param_count = 0;
    for (const auto& p : model->parameters()) {
        param_count += p.numel();
    }
    std::cout << "Total number of parameters: " << param_count << std::endl;
    
    model->to(device); // Move model to GPU/MPS/CPU

    // 5. Initialize Optimizer (Adam is usually best for this)
    torch::optim::Adam optimizer(model->parameters(), torch::optim::AdamOptions(learning_rate));

    // 6. Training Loop
    std::cout << "Starting training..." << std::endl;
    for (int epoch = 1; epoch <= num_epochs; ++epoch) {        
        // Optional: Shuffle file order to mix data slightly better
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(file_list.begin(), file_list.end(), g);
  
  
        double epoch_total_loss = 0.0;
        size_t epoch_total_batches = 0;
        int file_number = 0; //when the list is randomized, it's easy to keep track
        
        // LR reductio logic
        double best_loss = 999.0f;
        int patience_counter = 0;
        const int MAX_PATIENCE = 5; // How many check-ins to wait
        const float RELATIVE_THRESHOLD = 0.005f; // 0.5% improvement required
        double running_loss = 0;
        
        for (const auto& filepath : file_list) {
            std::cout << "  Processing file " << ++file_number << ": " << filepath << std::endl;

            // SCOPE BLOCK: Creates Dataset and Loader, then DESTROYS them to free RAM
            { 
                // Load ONE file
                auto dataset = ChessDataset(filepath).map(torch::data::transforms::Stack<>());
                size_t ds_size = dataset.size().value();
                
                if (ds_size == 0) continue;

                auto data_loader = torch::data::make_data_loader<torch::data::samplers::RandomSampler>(
                    std::move(dataset), 
                    torch::data::DataLoaderOptions().batch_size(batch_size).workers(2)
                );

                // Train on this chunk
                model->train();
                for (auto& batch : *data_loader) {
                    auto data = batch.data.to(device);
                    auto targets = batch.target.to(device);

                    optimizer.zero_grad();
                    auto output = model->forward(data);
                    auto loss = torch::mse_loss(output, targets);
                    loss.backward();
                    optimizer.step();

                    epoch_total_loss += loss.item<double>();
                    epoch_total_batches++;
                    running_loss += loss.item<double>();
                    
                    if (epoch_total_batches % 10000 == 0) {
                        double current_window_loss = running_loss / 10000;
                        running_loss = 0;
                    
                        // Calculate the 'improvement' required based on the best result so far
                        double required_improvement = best_loss * RELATIVE_THRESHOLD;
                        
                        if (current_window_loss < (best_loss - required_improvement)) {
                            best_loss = current_window_loss;
                            patience_counter = 0; // Model is still successfully learning
                            std::cout << "[Check] New Best Loss: " << best_loss << " - Progress is steady." << std::endl;
                        } else {
                            patience_counter++;
                            std::cout << "[Check] No significant progress. Patience: " << patience_counter << "/" << MAX_PATIENCE << std::endl;
                        }           
                                 
                        if (patience_counter >= MAX_PATIENCE) {
                            // Reduce LR by 0.5x
                            learning_rate *= 0.75;
                            for (auto& group : optimizer.param_groups()) {
                                static_cast<torch::optim::AdamOptions&>(group.options()).lr(learning_rate);
                            }
                            std::cout << ">>> Plateau triggered! New LR: " << learning_rate << std::endl;
                            patience_counter = 0; // Reset after reduction
                        }
                    }                    

                    // Optional: Print less frequently to avoid console spam
                    if (epoch_total_batches % 1000 == 0) {
                         std::printf("\r    Batch %ld | Loss: %.4f", epoch_total_batches, loss.item<double>());
                         std::fflush(stdout);
                    }
                }
            } // END SCOPE: dataset is destructed here, RAM is freed!
            
            std::cout << "\n  Finished file: " << filepath << std::endl;

            // Save Checkpoint
            torch::save(model, "simple_cnn_checkpoint_" + std::to_string(epoch) + ".pt");

        }

        double avg_loss = epoch_total_loss / (epoch_total_batches + 1); // Avoid div/0
        std::cout << "=== End of Epoch " << epoch << " | Avg Loss: " << avg_loss << " ===" << std::endl;

        // --- VALIDATION PHASE (NEW) ---
        if (std::filesystem::exists(test_data_path)) {
            std::cout << "=== Epoch " << epoch << " Validation ===" << std::endl;
            model->to(torch::kHalf); // kFloat16 or try kBFloat16
            model->eval(); // Switch to eval mode (disable dropout, batchnorm stats update, etc)
            torch::NoGradGuard no_grad; // Disable gradient calculation (saves RAM and time)

            double test_total_loss = 0.0;
            size_t test_total_batches = 0;
            double mean_cp_error_total = 0;
            double sign_accuracy_total = 0;

            { // Scope for memory management
                auto test_dataset = ChessDataset(test_data_path).map(torch::data::transforms::Stack<>());
                auto test_loader = torch::data::make_data_loader(
                    std::move(test_dataset),
                    torch::data::DataLoaderOptions().batch_size(batch_size).workers(2)
                );

                for (auto& batch : *test_loader) {
                    auto data = batch.data.to(device).to(torch::kHalf);
                    auto targets = batch.target.to(device);

                    auto output = model->forward(data).to(torch::kFloat32);
                    
                    auto loss = torch::mse_loss(output, targets);

                    // 2. Convert both back to Centipawns
                    auto pred_cp = torch::atanh(torch::clamp(output, -0.9999, 0.9999)) * eval_scale;
                    auto target_cp = torch::atanh(torch::clamp(targets, -0.9999, 0.9999)) * eval_scale;
                    
                    // 3. Calculate the absolute difference
                    auto cp_error = torch::abs(pred_cp - target_cp);
                    
                    auto correct_side = (output * targets) > 0;
                    sign_accuracy_total += correct_side.to(torch::kFloat32).mean().item<float>() * 100.0;

                    mean_cp_error_total += cp_error.mean().item<float>();
                    test_total_loss += loss.item<double>();
                    test_total_batches++;
                }
            }
            
            double avg_test_loss = (test_total_batches > 0) ? (test_total_loss / test_total_batches) : 0.0;
            std::cout << "    Avg Test Loss:  " << avg_test_loss << std::endl;
            double avg_cp_loss = (test_total_batches > 0) ? (mean_cp_error_total / test_total_batches) : 0.0;
            std::cout << "    Average CP Error: " << avg_cp_loss << " centipawns" << std::endl;
            double avg_sign_error = (test_total_batches > 0) ? (sign_accuracy_total / test_total_batches) : 0.0;
            std::cout << "    Sign Accuracy: " << avg_sign_error << "%" << std::endl;
            
            model->to(torch::kFloat32);
        } else {
            std::cerr << "Warning: Test file " << test_data_path << " not found. Skipping validation." << std::endl;
        }
                
        // Save Checkpoint
        //torch::save(model, "simple_cnn_checkpoint_" + std::to_string(epoch) + ".pt");
    }

    std::cout << "Training complete." << std::endl;
    cleanup_magic_bitboards();
    return 0;
}