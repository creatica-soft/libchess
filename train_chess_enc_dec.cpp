//c++ -O3 -I /opt/anaconda3/include -I /opt/anaconda3/include/torch/csrc/api/include -L /opt/anaconda3/lib -L /Users/ap/libchess -std=c++17 -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/opt/anaconda3/lib,-rpath,/Users/ap/libchess -o chess_trans train_chess_trans.cpp chess_trans.cpp
//c++ -O3 -I /Users/ap/Downloads/libtorch/include -I /Users/ap/Downloads/libtorch/include/torch/csrc/api/include -L /Users/ap/Downloads/libtorch/lib -L /Users/ap/libchess -std=c++17 -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/Users/ap/Downloads/libtorch/lib,-rpath,/Users/ap/libchess -o chess_enc_dec train_chess_enc_dec.cpp
#include <mach/mach.h>
#include <torch/torch.h>
#include <torch/script.h>
#include <torch/serialize.h>
#include <iostream>
#include <fstream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <string>
#include <filesystem>
#include <chrono>
#include <random>
#include <cmath>
#include "libchess.h"

#include <cmath>
#include <torch/torch.h>

class ChessTransformerImpl : public torch::nn::Module {
public:
    ChessTransformerImpl(int d_model=256, int nhead=8, int num_layers=4, int vocab_size=65) {
        model_dim = d_model;
        embed_board = register_module("embed_board", torch::nn::Conv2d(torch::nn::Conv2dOptions(19, d_model, 1)));
        embed_legal = register_module("embed_legal", torch::nn::Conv2d(torch::nn::Conv2dOptions(10, d_model, 1)));
        token_embedding = register_module("token_embedding", torch::nn::Embedding(vocab_size, d_model));
        token_norm = register_module("token_norm", torch::nn::LayerNorm(torch::nn::LayerNormOptions({d_model})));
        pos_encoding = register_buffer("pos_encoding", create_pos_encoding());
        transformer_encoder = register_module("transformer_encoder", torch::nn::TransformerEncoder(
            torch::nn::TransformerEncoderLayer(torch::nn::TransformerEncoderLayerOptions(d_model, nhead).dropout(0.2)),
            num_layers));
        transformer_decoder = register_module("transformer_decoder", torch::nn::TransformerDecoder(
            torch::nn::TransformerDecoderLayer(torch::nn::TransformerDecoderLayerOptions(d_model, nhead).dropout(0.2)),
            num_layers));
        output_head = register_module("output_head", torch::nn::Linear(d_model, vocab_size));
/*
//this initialization causes very slow learning in teacher forcing mode = 0
        for (auto& module : modules(false)) {
          if (auto* linear = dynamic_cast<torch::nn::LinearImpl*>(module.get())) {
              torch::nn::init::xavier_uniform_(linear->weight);
              torch::nn::init::zeros_(linear->bias);
          } else if (auto* conv = dynamic_cast<torch::nn::Conv2dImpl*>(module.get())) {
              torch::nn::init::xavier_uniform_(conv->weight);
              torch::nn::init::zeros_(conv->bias);
          } else if (auto* embedding = dynamic_cast<torch::nn::EmbeddingImpl*>(module.get())) {
              torch::nn::init::normal_(embedding->weight, 0.0, 1.0 / std::sqrt(d_model));
          }
        }
        // same is for kaiming weights. defaults work super fast for mode 0
        for (auto& module : modules(false)) {
            if (auto* linear = dynamic_cast<torch::nn::LinearImpl*>(module.get())) {
                torch::nn::init::kaiming_uniform_(linear->weight, 0.0, torch::kFanIn, torch::kLeakyReLU);
                if (linear->bias.defined()) {
                    torch::nn::init::zeros_(linear->bias);
                }
            } else if (auto* conv = dynamic_cast<torch::nn::Conv2dImpl*>(module.get())) {
                torch::nn::init::kaiming_uniform_(conv->weight, 0.0, torch::kFanIn, torch::kLeakyReLU);
                if (conv->bias.defined()) {
                    torch::nn::init::zeros_(conv->bias);
                }
            } else if (auto* embedding = dynamic_cast<torch::nn::EmbeddingImpl*>(module.get())) {
                torch::nn::init::normal_(embedding->weight, 0.0, 0.01); // Smaller std
            }
        }
  */
    }

    // Positional encoding for 64 squares
    torch::Tensor create_pos_encoding() {
        torch::Tensor pos = torch::arange(64, torch::kFloat).unsqueeze(1);
        torch::Tensor div_term = torch::exp(torch::arange(0, model_dim, 2, torch::kFloat) * (-std::log(10000.0) / model_dim));
        torch::Tensor pe = torch::zeros({1, 64, model_dim});
        pe.slice(2, 0, model_dim, 2) = torch::sin(pos * div_term);
        pe.slice(2, 1, model_dim, 2) = torch::cos(pos * div_term);
        return pe;
    }
    
    torch::Tensor forward(torch::Tensor x, torch::Tensor move_tokens, int mode = 1) {
        auto x_board = x.slice(1, 0, 19);
        auto x_legal_mask = x.slice(1, 19);
        
        auto board_embed = embed_board->forward(x_board); // [batch, d_model, 8, 8]
        auto legal_embed = embed_legal->forward(x_legal_mask); // [batch, d_model, 8, 8]
        auto combined_embed = board_embed + legal_embed;
        auto seq = combined_embed.view({x_board.size(0), model_dim, 64}).transpose(1, 2); // [batch, 64, d_model]
        auto memory = transformer_encoder->forward(seq); // [batch, 64, d_model]
        //checkpointing trades compute for memory
        /*
        auto memory = torch::checkpoint::checkpoint(
            [this](torch::Tensor seq) { return transformer_encoder->forward(seq); },
            {seq}
        );
        */
        auto tgt_mask = torch::triu(torch::ones({3, 3}, torch::TensorOptions().device(x.device())), 1).to(torch::kFloat); // [3, 3]
        tgt_mask = tgt_mask.masked_fill(tgt_mask.to(torch::kBool), -1e9); // [3, 3]
        auto batch_size = x_board.size(0);
        torch::Tensor logits = torch::zeros({batch_size, 3, 65}, torch::TensorOptions().device(x.device())); // Initialize logits
        if (mode == 0) { // both src and dst squares are known
          auto token_embed = token_embedding->forward(move_tokens); // [batch, 3, d_model]
          token_embed = token_norm->forward(token_embed); // Normalize
          token_embed = token_embed + pos_encoding.slice(1, 0, 3).to(x.device());
          token_embed = token_embed.contiguous().transpose(0, 1); // [3, batch, 128]
          auto decoder_output = transformer_decoder->forward(token_embed, memory * 1.5, tgt_mask); // [3, batch, 128]
          /*
          auto decoder_output = torch::checkpoint::checkpoint(
              [this](torch::Tensor token_embed, torch::Tensor memory, torch::Tensor tgt_mask) {
                  return transformer_decoder->forward(token_embed, memory * 1.5, tgt_mask);
              },
              {token_embed, memory, tgt_mask}
          );
          */
          decoder_output = decoder_output.contiguous().transpose(0, 1); // [batch, 3, 128]
          logits = output_head->forward(decoder_output); // [256, 3, 65]
        } else if (mode == 1) { // none is known
          // Autoregressive generation: Start with <START>
          auto start_tokens = torch::full({batch_size, 1}, 64,torch::TensorOptions().dtype(torch::kInt64)).to(x.device()); // [batch, 1]
          auto token_embed = token_embedding->forward(start_tokens); // [batch, 1, d_model]
          token_embed = token_norm->forward(token_embed); // Normalize
          token_embed = token_embed + pos_encoding.slice(1, 0, 1).to(x.device());
          token_embed = token_embed.contiguous().transpose(0, 1); // [1, batch, d_model]
          auto decoder_output = transformer_decoder->forward(token_embed, memory * 1.5, tgt_mask.slice(0, 0, 1).slice(1, 0, 1)); // [1, batch, d_model]
          decoder_output = decoder_output.transpose(0, 1); // [batch, 1, d_model]
          logits = output_head->forward(decoder_output); // [batch, 1, 65]
          auto src_pred = torch::argmax(logits, -1); // [batch, 1]
          // Generate destination
          token_embed = token_embedding->forward(src_pred); // [batch, 1, d_model]
          token_embed = token_norm->forward(token_embed); // Normalize
          token_embed = token_embed + pos_encoding.slice(1, 1, 2).to(x.device());
          token_embed = token_embed.contiguous().transpose(0, 1); // [1, batch, d_model]
          decoder_output = transformer_decoder->forward(token_embed, memory * 1.5, tgt_mask.slice(0, 1, 2).slice(1, 1, 2)); // [1, batch, d_model]
          decoder_output = decoder_output.transpose(0, 1); // [batch, 1, d_model]
          auto dst_logits = output_head->forward(decoder_output); // [batch, 1, 65]
          logits = torch::cat({logits, dst_logits}, 1); // [batch, 2, 65]
          logits = torch::cat({torch::zeros({batch_size, 1, 65}, logits.options()), logits}, 1); // [batch, 3, 65]          
        } else if (mode == 2) { //only src square is known
            tgt_mask = torch::triu(torch::ones({2, 2}, torch::TensorOptions().device(x.device())), 1).to(torch::kFloat);
            tgt_mask = tgt_mask.masked_fill(tgt_mask.to(torch::kBool), -1e9);
            auto input_tokens = move_tokens.slice(1, 0, 2); // [batch, 2] (<START>, source)
            auto token_embed = token_embedding->forward(input_tokens); // [batch, 2, d_model]
            token_embed = token_norm->forward(token_embed); // Normalize
            token_embed = token_embed + pos_encoding.slice(1, 0, 2).to(x.device());
            token_embed = token_embed.transpose(0, 1); // [2, batch, d_model]  
            auto decoder_output = transformer_decoder->forward(token_embed, memory * 1.5, tgt_mask); // [2, batch, d_model]
            decoder_output = decoder_output.transpose(0, 1); // [batch, 2, d_model]
            logits = output_head->forward(decoder_output.slice(1, 1, 2)); // [batch, 1, 65] (destination only)
            return torch::cat({torch::zeros({batch_size, 2, 65}, logits.options()), logits}, 1); // [batch, 3, 65]         
        }
        return logits;
    }

private:
    int model_dim;
    torch::nn::Conv2d embed_board{nullptr}, embed_legal{nullptr};
    torch::nn::Embedding token_embedding{nullptr};
    torch::nn::LayerNorm token_norm{nullptr};
    torch::Tensor pos_encoding;
    torch::nn::TransformerEncoder transformer_encoder{nullptr};
    torch::nn::TransformerDecoder transformer_decoder{nullptr};
    torch::nn::Linear output_head{nullptr};
};
TORCH_MODULE(ChessTransformer);


class CosineAnnealingLR {
public:
    CosineAnnealingLR(torch::optim::Optimizer& optimizer, int T_max, double lr_min)
        : optimizer_(optimizer), T_max_(T_max), lr_min_(lr_min) {
        // Store initial learning rates
        for (auto& group : optimizer_.param_groups()) {
            base_lrs_.push_back(group.options().get_lr());
        }
    }

    void step() {
        current_step_++;
        double cos_term = std::cos(M_PI * current_step_ / T_max_);
        double lr_factor = lr_min_ + 0.5 * (1.0 - lr_min_) * (1.0 + cos_term);

        // Update learning rate for each parameter group
        for (size_t i = 0; i < optimizer_.param_groups().size(); ++i) {
            auto& options = optimizer_.param_groups()[i].options();
            options.set_lr(base_lrs_[i] * lr_factor);
        }
        //std::cerr << "current (step " << current_step_ << ") LR factor: " << lr_factor << std::endl;
    }

    void reset() {
        current_step_ = 0;
    }

private:
    torch::optim::Optimizer& optimizer_;
    int T_max_;
    double lr_min_; // Relative to initial lr (e.g., 0.1 for lr_min = 0.1 * lr_max)
    std::vector<double> base_lrs_;
    int current_step_ = 0;
};

// Thread-safe queue for tensors
struct TensorBatch {
    torch::Tensor board_moves;
    torch::Tensor move_type;
    torch::Tensor dst_sq;
    torch::Tensor src_sq;
    torch::Tensor result;
    //torch::Tensor stage;
};

class TensorQueue {
public:
    TensorQueue(const int queue_len) {
      max_size_ = queue_len;
    }
    void enqueue(TensorBatch batch) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.size() >= max_size_) {
            //std::cout << "TensorQueue: Full, waiting..." << std::endl;
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            lock.lock();
        }
        queue_.push(std::move(batch));
        //std::cout << "TensorQueue: Enqueued batch, size = " << queue_.size() << std::endl;
        lock.unlock();
        cond_.notify_one();
    }

    TensorBatch dequeue() {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] { return !queue_.empty() || stopped_; });
        if (queue_.empty()) {
            std::cout << "TensorQueue: Dequeue returning empty batch" << std::endl;
            return TensorBatch{};
        }
        auto batch = std::move(queue_.front());
        queue_.pop();
        //std::cout << "TensorQueue: Dequeued batch, size = " << queue_.size() << std::endl;
        return batch;
    }

    void stop() {
        std::unique_lock<std::mutex> lock(mutex_);
        stopped_ = true;
        std::cout << "TensorQueue: Stopped" << std::endl;
        lock.unlock();
        cond_.notify_all();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    std::queue<TensorBatch> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    bool stopped_ = false;
    size_t max_size_;
};

//Producer thread: produces BMPR structs from PGN files, 
//dequeues them, converts to tensors and enqueues into TensorQueue
void producer(const std::vector<std::string>& files, int min_elo, int max_elo_diff, int minMoves,
              int num_channels, int num_samples, int bmpr_queue_len, const enum GameStage game_stage, const unsigned long steps, TensorQueue& queue, torch::Device device) {
    std::vector<char*> file_ptrs;
    for (const auto& file : files) {
        file_ptrs.push_back(const_cast<char*>(file.c_str()));
    }
    std::cout << "Producer: Calling getGame_detached with " << files.size() << " files" << std::endl;
    getGame_detached(file_ptrs.data(), files.size(), min_elo, max_elo_diff, minMoves,
                     num_channels, num_samples, bmpr_queue_len, game_stage, steps);

    int sleep_count = 0;
    while (true) {
        auto bmpr = dequeueBMPR();
        if (!bmpr) {
            if (sleep_count < 3) {
                std::cout << "Producer: BMPR queue empty, waiting 1 sec (attempt " << sleep_count + 1 << "/3)" << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                sleep_count++;
                continue;
            } else {
                std::cout << "Producer: BMPR queue empty, exiting after 3 attempts" << std::endl;
                break;
            }
        }
        sleep_count = 0;
        try {
            // Convert BMPR fields to tensors
            auto board_moves = torch::from_blob(
                bmpr->boards_legal_moves,
                {bmpr->samples, bmpr->channels, 8, 8},
                torch::kFloat32
            ).to(device, false);

            auto move_type = torch::from_blob(
                bmpr->move_type,
                {bmpr->samples},
                torch::kInt64
            ).to(device, false);

            auto src_sq = torch::from_blob(
                bmpr->src_sq,
                {bmpr->samples},
                torch::kInt64
            ).to(device, false);

            auto dst_sq = torch::from_blob(
                bmpr->dst_sq,
                {bmpr->samples},
                torch::kInt64
            ).to(device, false);

            auto result = torch::from_blob(
                bmpr->result,
                {bmpr->samples},
                torch::kInt64
            ).to(device, false);

/*
            auto stage = torch::from_blob(
                bmpr->stage,
                {bmpr->samples},
                torch::kInt64
            ).to(device, false);
*/

        //assert(src_sq.dtype() == torch::kInt64 && dst_sq.dtype() == torch::kInt64);
        //assert(src_sq.min().item<int64_t>() >= 0 && src_sq.max().item<int64_t>() < 64);
        //assert(dst_sq.min().item<int64_t>() >= 0 && dst_sq.max().item<int64_t>() < 64);

            // Enqueue tensor batch
            queue.enqueue({board_moves, move_type, dst_sq, src_sq, result});
        } catch (const std::exception& e) {
            std::cerr << "Producer: Error processing BMPR: " << e.what() << std::endl;
        }

        free_bmpr(bmpr); // .to(device) copies bmpr, so its ok to free it here
    }
    queue.stop();
    std::cout << "Producer: Stopped" << std::endl;
}

torch::Tensor create_move_tokens(torch::Tensor src_sq, torch::Tensor dst_sq, int64_t batch_size, c10::Device device) {
    // Step 1: Create <START> token tensor [batch_size, 1]
    auto start_tokens = torch::full({batch_size, 1}, 64, torch::TensorOptions().dtype(torch::kInt64).device(device));

    // Step 2: Reshape src_sq and dst_sq to [batch_size, 1]
    auto src_tokens = src_sq.unsqueeze(1); // [batch_size, 1]
    auto dst_tokens = dst_sq.unsqueeze(1); // [batch_size, 1]

    // Step 3: Concatenate along dim=1 to get [batch_size, 3]
    auto move_tokens = torch::cat({start_tokens, src_tokens, dst_tokens}, 1); // [batch_size, 3]

    return move_tokens;
}

// Consumer thread: Training loop
void consumer_train(TensorQueue& queue, ChessTransformer& model, torch::optim::Optimizer& optimizer, CosineAnnealingLR& scheduler, int display_stats_every, int save_weights_every, const std::string& weights_file, torch::Device device, const int accumulation_steps) {
    auto start = std::chrono::steady_clock::now();
    int64_t samples_total = 0, samples = 0, steps = 0;
    double total_loss = 0;
    double dequeue_time = 0, model_time = 0, loss_time = 0, back_time = 0, optim_time = 0;
    int accum_count = 0;
    
    model->train();
    while (true) {
        int sleep_counter = 0;
        while (queue.empty() && sleep_counter < 3) {
            std::cout << "Consumer: Tensor queue empty, sleeping 1 sec (attempt " << sleep_counter + 1 << "/3)" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            sleep_counter++;
        }
        if (sleep_counter == 3) {
            std::cout << "Consumer: Tensor queue empty, exiting after 2 attempts" << std::endl;
            break;
        }

        auto dequeue_start = std::chrono::steady_clock::now();
        auto batch = queue.dequeue();
        if (!batch.board_moves.defined()) {
            std::cout << "Consumer: Received empty batch, exiting" << std::endl;
            break;
        }
        dequeue_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - dequeue_start).count();
        samples += batch.board_moves.size(0);
        auto move_tokens = create_move_tokens(batch.src_sq, batch.dst_sq, batch.board_moves.size(0), device); // [batch_size, 3]
        //std::cout << "move_tokens shape: " << move_tokens.sizes() << ", sample: " << move_tokens[0] << std::endl;
        //std::cout << "Consumer: Processing batch with " << batch.board_moves.size(0) << " samples" << std::endl;

//to visually verify that the model gets the correct data
//run the training for few positions
/*
for (int64_t i = 0; i < batch.board_moves.size(0); i++) { //loop over samples
  std::cerr << "Batch (position) number " << i << std::endl;
  for (int64_t j = 0; j < batch.board_moves.size(1); j++) { //loop over channels
    std::string piece;
    if (j == 0 || j == 12) piece = "opponent's pawns";
    else if (j == 1 || j == 13) piece = "opponent's knights";
    else if (j == 2 || j == 14) piece = "opponent's bishops";
    else if (j == 3 || j == 15) piece = "opponent's rooks";
    else if (j == 4 || j == 16) piece = "opponent's queens";
    else if (j == 5 || j == 17) piece = "opponent's king";
    else if (j == 6 || j == 19) piece = "side to move pawns";
    else if (j == 18) piece = "legal moves";
    else if (j == 7 || j == 20) piece = "side to move knights";
    else if (j == 8 || j == 21) piece = "side to move bishops";
    else if (j == 9 || j == 22) piece = "side to move rooks";
    else if (j == 10 || j == 23) piece = "side to move queens";
    else if (j == 11 || j == 24) piece = "side to move king";
    else if (j == 25) piece = "side to move queen promotion";
    else if (j == 26) piece = "side to move rook promotion";
    else if (j == 27) piece = "side to move bishop promotion";
    else if (j == 28) piece = "side to move knight promotion";
    if (j <= 11)
      std::cerr << piece << " occupations:" << std::endl;
    else if (j >= 12 && j <= 17)
      std::cerr << piece << " control squares:" << std::endl;
    else if (j == 18)
      std::cerr << piece << " source squares:" << std::endl;
    else if (j >= 19)
      std::cerr << piece << " moves:" << std::endl;
    std::cerr << batch.board_moves.index({i, j}) << std::endl;
  }
  std::cerr << "Source channel (move_type): " << batch.move_type.index({i}) << std::endl;
  std::cerr << "Source square: " << squareName[(enum SquareName)batch.src_sq.index({i}).item<int64_t>()] << std::endl;
  std::cerr << "Destination square: " << squareName[(enum SquareName)batch.dst_sq.index({i}).item<int64_t>()] << std::endl;
  std::cerr << "Game outcome: " << batch.result.index({i}) << std::endl;
}
*/
        torch::Tensor loss;
        float teacher_forcing_ratio = 1.0f - std::pow(static_cast<float>(steps) / 5000, 2); // Quadratic decay
        teacher_forcing_ratio = std::max(0.0f, std::min(1.0f, teacher_forcing_ratio));
        float intermediate_ratio = 0.75 * (1.0f - teacher_forcing_ratio); // Scale mode 2
        float r = torch::rand({1}).item<float>();
        int mode = r < teacher_forcing_ratio ? 0 : (r < teacher_forcing_ratio + intermediate_ratio ? 2 : 1);
        float source_acc, dest_acc;
        static std::chrono::time_point<std::chrono::steady_clock> loss_start;
        std::vector<std::tuple<torch::Tensor, torch::Tensor, int>> replay_buffer;
        if (steps % 10 == 0 && replay_buffer.size() < 2000) {
            replay_buffer.push_back({batch.board_moves.clone(), move_tokens.clone(), mode});
        }
        if (torch::rand({1}).item<float>() < 0.5 && !replay_buffer.empty()) { //30% replay
          auto idx = torch::randint(replay_buffer.size(), {1}).item<int>();
          auto [replay_board, replay_tokens, replay_mode] = replay_buffer[idx];
          
          torch::Tensor loss_input, targets;        
          auto model_start = std::chrono::steady_clock::now();
          auto logits = model->forward(replay_board, replay_tokens, replay_mode);
          model_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - model_start).count();
          loss_start = std::chrono::steady_clock::now(); 
          if (replay_mode == 2) {
              loss_input = logits.slice(1, 2, 3).reshape({-1, 65});
              targets = replay_tokens.slice(1, 2, 3).reshape(-1);
          } else {
              loss_input = logits.slice(1, 1, 3).reshape({-1, 65});
              targets = replay_tokens.slice(1, 1, 3).reshape(-1);
          }
          source_acc = replay_mode == 2 ? 1.0 : (torch::argmax(logits.slice(1, 1, 2).reshape({-1, 65}), -1) == replay_tokens.slice(1, 1, 2).reshape(-1)).to(torch::kFloat).mean().item<float>();
          dest_acc = (torch::argmax(logits.slice(1, 2, 3).reshape({-1, 65}), -1) == replay_tokens.slice(1, 2, 3).reshape(-1)).to(torch::kFloat).mean().item<float>();
          loss = torch::nn::functional::cross_entropy(loss_input, targets);          
          total_loss += loss.item<float>();
          loss_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_start).count();
  
          // Backward pass
          auto back_start = std::chrono::steady_clock::now();
          loss.backward();
          back_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - back_start).count();
        } else
        {          
          // Forward pass
          auto model_start = std::chrono::steady_clock::now();
          auto logits = model->forward(batch.board_moves, move_tokens, mode);
          model_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - model_start).count();
          source_acc = mode == 2 ? 1.0 : (torch::argmax(logits.slice(1, 1, 2), -1) == move_tokens.slice(1, 1, 2)).to(torch::kFloat).mean().item<float>();
          dest_acc = (torch::argmax(logits.slice(1, 2, 3), -1) == move_tokens.slice(1, 2, 3)).to(torch::kFloat).mean().item<float>();
          // Compute losses
          auto loss_start = std::chrono::steady_clock::now();
          torch::Tensor loss_input, targets;        
          loss_input = mode == 2 ? logits.slice(1, 2, 3).reshape({-1, 65}) : logits.slice(1, 1, 3).reshape({-1, 65});
          targets = mode == 2 ? move_tokens.slice(1, 2, 3).reshape(-1) : move_tokens.slice(1, 1, 3).reshape(-1);
          loss = torch::nn::functional::cross_entropy(loss_input, targets);          
          total_loss += loss.item<float>();
          loss_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_start).count();
  
          // Backward pass
          auto back_start = std::chrono::steady_clock::now();
          loss.backward();
          back_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - back_start).count();
        }
        double grad_norm = 0.0;
        for (const auto& param : model->parameters()) {
            if (param.grad().defined()) {
                grad_norm += param.grad().norm().item<double>();
            }
        }

        // Optimizer step
        auto optim_start = std::chrono::steady_clock::now();
        accum_count++;
        if (accum_count >= accumulation_steps) {
            torch::nn::utils::clip_grad_norm_(model->parameters(), 1.0);
            optimizer.step();
            optimizer.zero_grad();
            scheduler.step(); // After optimizer.step()
            accum_count = 0;
        }        
        optim_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - optim_start).count();

        steps++;
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (steps % display_stats_every == 0) {
            samples_total += samples;
            // Debug
            /*
            auto src_pred = torch::argmax(logits.slice(1, 1, 2), -1); // [batch, 1]
            auto dst_pred = torch::argmax(logits.slice(1, 2, 3), -1); // [batch, 1]
            auto src_target = move_tokens.slice(1, 1, 2); // [batch, 1]
            auto dst_target = move_tokens.slice(1, 2, 3); // [batch, 1]
            auto source_acc = (src_pred == src_target).to(torch::kFloat).mean().item<float>();
            auto dest_acc = (dst_pred == dst_target).to(torch::kFloat).mean().item<float>();
            std::cout << "Sample src_pred: " << src_pred[0].item<int64_t>() << ", src_target: " << src_target[0].item<int64_t>() << std::endl;
            std::cout << "Sample dst_pred: " << dst_pred[0].item<int64_t>() << ", dst_target: " << dst_target[0].item<int64_t>() << std::endl;
            std::cout << "Acc: src " << source_acc << ", dest " << dest_acc << std::endl;
            */
            std::cout << "Current step " << steps << ". Mode " << mode << std::endl;
            std::cout << "Grad norm: " << grad_norm << std::endl;
            if (steps % 10000 == 0) {
                task_basic_info info;
                mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
                task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&info, &count);
                std::cout << "Memory (Resident): " << (info.resident_size / 1024.0 / 1024.0) << " MB" << std::endl;
            }
            std::cout << "Acc: src " << source_acc << ", dest " << dest_acc << std::endl;
            std::printf("Current loss %.3f\n", loss.item<float>());
            std::printf("Avg loss %.3f\n", total_loss / steps);
            std::printf("[Current / Total samples]: [%lld / %lld]: %.0f samples/s or %.6f s/sample\n", samples, samples_total, samples / elapsed, elapsed / samples);
            std::printf("Stats per sample: dequeueTime %.6fs, modelTime %.6fs, lossTime %.6fs, backTime %.6fs, optimTime %.6fs\n", dequeue_time / samples, model_time / samples, loss_time / samples, back_time / samples, optim_time / samples);
            start = std::chrono::steady_clock::now();
            samples = 0;
            dequeue_time = model_time = loss_time = back_time = optim_time = 0;
        }

        if (steps % save_weights_every == 0) {
            //std::string file = "";
            //file.append(weights_file).append(std::to_string(steps / save_weights_every)).append(".pt");
            std::cout << "Saving weights to " << weights_file << "..." << std::endl;
            torch::save(model, weights_file);
            std::cout << "Saved weights to " << weights_file << std::endl;
            //file.clear();
        }

    }
    // Final optimizer step if any gradients remain
    if (accum_count > 0) {
        optimizer.step();
        optimizer.zero_grad();
    }
    std::cout << "Consumer: Training loop exited" << std::endl;
}

// Consumer thread: Testing loop
void consumer_test(TensorQueue& queue, ChessTransformer& model, torch::Device device) {
    auto start = std::chrono::steady_clock::now();
    int64_t samples = 0, steps = 0;
    double correct = 0;
    double total_loss = 0;
    double dequeue_time = 0, model_time = 0, loss_time = 0;

    model->eval();
    torch::NoGradGuard no_grad;
    while (true) {
        int sleep_counter = 0;
        while (queue.empty() && sleep_counter < 5) {
            std::cout << "ConsumerTest: Tensor queue empty, sleeping 1 sec (attempt " << sleep_counter + 1 << "/5)" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            sleep_counter++;
        }
        if (sleep_counter == 5) {
            std::cout << "ConsumerTest: Tensor queue empty, exiting after 5 attempts" << std::endl;
            break;
        }

        auto dequeue_start = std::chrono::steady_clock::now();
        auto batch = queue.dequeue();
        if (!batch.board_moves.defined()) {
            std::cout << "ConsumerTest: Received empty batch, exiting" << std::endl;
            break;
        }
        dequeue_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - dequeue_start).count();
        samples += batch.board_moves.size(0);
        auto move_tokens = create_move_tokens(batch.src_sq, batch.dst_sq, batch.board_moves.size(0), device); // [batch_size, 3]

        // Forward pass
        auto model_start = std::chrono::steady_clock::now();
        auto logits = model->forward(batch.board_moves, move_tokens, 1);
        model_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - model_start).count();
        
        auto loss_start = std::chrono::steady_clock::now();
        auto loss = torch::nn::functional::cross_entropy(logits.slice(1, 1, 3).reshape({-1, 65}), move_tokens.slice(1, 1, 3).reshape(-1));
        total_loss += loss.item<float>();
        loss_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_start).count();
        
        auto predicted_source = move_tokens.slice(1, 1, 2).squeeze();
        auto predicted_destination = move_tokens.slice(1, 2, 3).squeeze();
        auto pred_move = predicted_source * 64 + predicted_destination;
        auto target_move = batch.src_sq * 64 + batch.dst_sq;
        correct += (pred_move == target_move).to(torch::kFloat32).sum().item<float>();
        
        loss_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_start).count();

        steps++;
    }

    if (samples > 0) {
        correct /= samples;
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("Test loss %.3f\n", total_loss / steps);
        std::printf("Test Error: Accuracy: %.1f%%. %.0f samples/s or %.6f s/sample\n", (100 * correct), samples / elapsed, elapsed / samples);
        std::printf("Stats per sample: dequeueTime %.6f, modelTime %.6f, lossTime %.6f\n", dequeue_time / samples, model_time / samples, loss_time / samples);
    } else {
        std::cout << "No samples processed in test!" << std::endl;
    }
    std::cout << "ConsumerTest: Testing loop exited" << std::endl;
}

int main(int argc, char ** argv) {
    const float learning_rate = 5e-4;
    const float weight_decay = 0.02; //for AdamW optimizer, affects regularization, the higher, the better
    const bool AMSgrad = true; //might improve Adam convergence
    //CosineAnnealingLR() gradually adjusts the learning rate from 
    //learning_rate (1e-4) to lr_min (0.1 * learning_rate = 1e-5) over T_max (10000) steps
    //it can be reset at each epoch or alternatively increase T_max
    const double lr_min = 0.1; //i.e. a factor such as min_lr = 0.1 * learning_rate, another words, 10 times less
    const int T_max = 2000;
    const int epochs = 1;
    const unsigned long steps = 10000000; //for curriculum learning (from simple positions of 5 legal moves to complex positions of 20 legal moves over number of steps)
    const std::string weights_file = "chess_enc_dec.pt";
    const int min_elo = 2400;
    const int max_elo_diff = 200;
    const int minMoves = 40;
    const int num_channels = 29;
    const int num_samples = 64; //batch_size
    const enum GameStage game_stage = FullGame; //not used anymore
    const int display_stats_every = 16;
    const int save_weights_every = 4096;
    const int bmpr_queue_len_train = 5;
    const int bmpr_queue_len_test = 3;
    const int accumulation_steps = 1; // Accumulate gradients over 1 batches
    //const std::string pgn_dir = "/Users/ap/Downloads/Lichess Elite Database/";
    const std::string pgn_dir = "/Users/ap/pgn";
    const std::vector<std::string> test_files = {"KingBaseLite2019.pgn"};

    // Device selection
    torch::Device device(torch::kCPU);
    if (torch::cuda::is_available()) {
        device = torch::Device(torch::kCUDA);
        std::cout << "Using CUDA device" << std::endl;
    } else if (torch::hasMPS()) {
        device = torch::Device(torch::kMPS);
        std::cout << "Using MPS device" << std::endl;
    } else {
        std::cout << "Using CPU device" << std::endl;
    }

    // Initialize model
    ChessTransformer model;
    //std::string f = "";
    //f.append(weights_file).append("1.pt");
    if (std::filesystem::exists(weights_file)) {
        try {
            std::cout << "Loading weights from " << weights_file << "..." << std::endl;
            // Load traced model
            auto traced_module = torch::jit::load(weights_file, device);
            // Copy parameters from traced module to model
            auto traced_params = traced_module.named_parameters();
            auto model_params = model->named_parameters();
            for (const auto& pair : traced_params) {
                std::string key = pair.name;
                // Detach the source tensor to avoid gradient issues
                torch::Tensor value = pair.value.detach().to(device);
                auto param = model_params.find(key);
                if (param != nullptr) {
                    // Disable requires_grad temporarily
                    bool orig_requires_grad = param->requires_grad();
                    param->set_requires_grad(false);
                    param->copy_(value);
                    param->set_requires_grad(orig_requires_grad);
                } else {
                    std::cerr << "Warning: Parameter " << key << " not found in model" << std::endl;
                }
            }
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
    //f.clear();
            
    model->to(device);
    
    // Initialize optimizer
    torch::optim::AdamW optimizer(model->parameters(), torch::optim::AdamWOptions().lr(learning_rate).weight_decay(weight_decay).amsgrad(AMSgrad));
    CosineAnnealingLR scheduler(optimizer, T_max, lr_min);

    // Count parameters
    int64_t param_count = 0;
    for (const auto& p : model->parameters()) {
        param_count += p.numel();
    }
    std::cout << "Total number of parameters: " << param_count << std::endl;

    // Collect training files
    std::vector<std::string> train_files;
    if (argc == 2) {
      train_files.push_back(argv[1]);
    } else {
      for (const auto& entry : std::filesystem::directory_iterator(pgn_dir)) {
          if (entry.path().extension() == ".pgn") {
              train_files.push_back(entry.path().string());
          }
      }    
      std::random_device rd;
      std::mt19937 g(rd());
      std::shuffle(train_files.begin(), train_files.end(), g);
    }

    // Main loop
    auto main_start = std::chrono::steady_clock::now();
    for (int epoch = 0; epoch < epochs; ++epoch) {
        auto epoch_start = std::chrono::steady_clock::now();
        std::cout << "Epoch " << (epoch + 1) << "\n-------------------------------" << std::endl;

        // Training
        scheduler.reset(); //optional or increase T_max
        TensorQueue tensor_queue_train(bmpr_queue_len_train);
        std::cout << "Starting producer train" << std::endl;
        std::thread producer_thread_train(producer, train_files, min_elo, max_elo_diff, minMoves,
                                         num_channels, num_samples, bmpr_queue_len_train, game_stage, steps,
                                         std::ref(tensor_queue_train), device);
        std::cout << "Starting consumer train" << std::endl;
        std::thread consumer_thread_train(consumer_train, std::ref(tensor_queue_train),
                                         std::ref(model), std::ref(optimizer), std::ref(scheduler), 
                                         display_stats_every, save_weights_every, weights_file, device, accumulation_steps);
        producer_thread_train.join();
        consumer_thread_train.join();

        std::cout << "Saving weights to " << weights_file << "..." << std::endl;
        torch::save(model, weights_file);
        std::cout << "Saved weights to " << weights_file << std::endl;

        // Testing
        if (argc == 1) { 
          TensorQueue tensor_queue_test(bmpr_queue_len_test);
          std::cout << "Starting producer test" << std::endl;
          std::thread producer_thread_test(producer, test_files, min_elo, max_elo_diff, minMoves,
                                          num_channels, num_samples, bmpr_queue_len_test, game_stage, steps, 
                                          std::ref(tensor_queue_test), device);
          std::cout << "Starting consumer test" << std::endl;
          std::thread consumer_thread_test(consumer_test, std::ref(tensor_queue_test),
                                          std::ref(model), device);
          producer_thread_test.join();
          consumer_thread_test.join();
        }
        
        auto epoch_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch_start).count();
        std::cout << "Time spent for epoch " << (epoch + 1) << ": " << epoch_time << "s" << std::endl;
    }
    auto total_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - main_start).count();
    std::cout << "Done!" << std::endl;
    std::cout << "Time spent for " << epochs << " epochs: " << total_time << "s" << std::endl;
    return 0;
}