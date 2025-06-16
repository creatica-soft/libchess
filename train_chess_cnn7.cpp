//c++ -O3 -I /opt/anaconda3/include -I /opt/anaconda3/include/torch/csrc/api/include -L /opt/anaconda3/lib -L /Users/ap/libchess -std=c++17 -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/opt/anaconda3/lib,-rpath,/Users/ap/libchess -o chess_cnn2 train_chess_cnn2.cpp chess_cnn2.cpp
//c++ -Wno-writable-strings -O3 -I /Users/ap/Downloads/libtorch/include -I /Users/ap/Downloads/libtorch/include/torch/csrc/api/include -L /Users/ap/Downloads/libtorch/lib -L /Users/ap/libchess -std=c++17 -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/Users/ap/Downloads/libtorch/lib,-rpath,/Users/ap/libchess -o chess_cnn7 train_chess_cnn7.cpp chess_cnn7.cpp
#include "chess_cnn7.h"
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
        std::cerr << "current (step " << current_step_ << ") LR factor: " << lr_factor << std::endl;
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

// Consumer thread: Training loop
void consumer_train(TensorQueue& queue, ChessCNN& model, torch::optim::Optimizer& optimizer, CosineAnnealingLR& scheduler, int display_stats_every, int save_weights_every, const std::string& weights_file, torch::Device device, const int accumulation_steps) {
    auto start = std::chrono::steady_clock::now();
    int64_t samples_total = 0, samples = 0, steps = 0;
    double channel_loss_total = 0, source_loss_total = 0, destination_loss_total = 0, value_loss_total = 0, total_loss_avg = 0;
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
        // Forward pass
        auto model_start = std::chrono::steady_clock::now();
        //moves_logits
        auto [channel_logits, source_logits, destination_logits, value_logits, x_legal] = model->forward(batch.board_moves);
        model_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - model_start).count();

        // channel_logits: [batch, 10]
        // destination_logits: [batch, 64]
        // value_logits: [batch, 3]
        // x_legal: [batch, 10, 8, 8], 1s for legal destination squares, 0s for illegal

        // Compute losses
        auto loss_start = std::chrono::steady_clock::now();        
        auto batch_size = batch.board_moves.size(0);
        // Reshape target_channel for batched indexing
        auto target_indices = batch.move_type.view({batch_size, 1, 1, 1}).expand({batch_size, 1, 8, 8}); // Shape: [batch, 1, 1, 1]
        // Use gather to select one channel per batch sample
        auto channel_mask = x_legal.gather(1, target_indices).squeeze(1).flatten(1, 2);        
        auto channel_loss = torch::nn::functional::cross_entropy(channel_logits, batch.move_type);
        
        // Manual weighted source loss
        auto src_mask = batch.board_moves.select(1, 18).flatten(1, 2); // [batch, 64]
        auto src_target = torch::nn::functional::one_hot(batch.src_sq, 64).to(torch::kFloat); // [batch, 64]
        auto src_prob = torch::log_softmax(source_logits, 1); // [batch, 64]
        auto src_weights = (src_mask > 0).to(torch::kFloat); // [batch, 64]
        auto source_loss = -(src_target * src_prob * src_weights).sum(1).mean();
        
        // Manual weighted destination loss
        auto dest_target = torch::nn::functional::one_hot(batch.dst_sq, 64).to(torch::kFloat); // [batch, 64]        
        auto dest_prob = torch::log_softmax(destination_logits, 1); // [batch, 64]
        auto dest_weights = (channel_mask > 0).to(torch::kFloat); // [batch, 64]
        auto destination_loss = -(dest_target * dest_prob * dest_weights).sum(1).mean();
        
        auto value_loss = torch::nn::functional::cross_entropy(value_logits, batch.result);
        //auto loss = (channel_loss + source_loss + destination_loss) + 0.1 * value_loss;
        auto loss = channel_loss + source_loss + destination_loss + value_loss;

        // Debug
        //std::cout << "Src non-zero: " << (src_mask > 0).to(torch::kFloat).sum(1).mean().item<float>() << std::endl;
        //std::cout << "Dest non-zero: " << (channel_mask > 0).to(torch::kFloat).sum(1).mean().item<float>() << std::endl;
        auto src_acc = (torch::argmax(source_logits, 1) == batch.src_sq).to(torch::kFloat).mean().item<float>();
        auto dest_acc = (torch::argmax(destination_logits, 1) == batch.dst_sq).to(torch::kFloat).mean().item<float>();
        auto channel_acc = (torch::argmax(channel_logits, 1) == batch.move_type).to(torch::kFloat).mean().item<float>();
        std::cout << "Acc: src " << src_acc << ", dest " << dest_acc << ", channel " << channel_acc << std::endl;
                
        channel_loss_total += channel_loss.item<double>();
        source_loss_total += source_loss.item<double>();
        destination_loss_total += destination_loss.item<double>();
        value_loss_total += value_loss.item<double>();
        total_loss_avg += loss.item<double>();
        loss_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_start).count();

        // Backward pass
        auto back_start = std::chrono::steady_clock::now();
        loss.backward();
        back_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - back_start).count();

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
        if (steps % display_stats_every == 0) {
            samples_total += samples;
            auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::printf("Current loss: move type %.3f + source %.3f + destination %.3f + value %.6f = %.6f\n", channel_loss.item<float>(), source_loss.item<float>(), destination_loss.item<float>(), value_loss.item<float>(), loss.item<float>());
            std::printf("Avg loss: move type %.3f + source %.3f + destination %.3f + value %.6f = %.6f\n", channel_loss_total / steps, source_loss_total / steps, destination_loss_total / steps, value_loss_total / steps, total_loss_avg / steps);
            std::printf("[Current / Total samples]: [%lld / %lld]: %.0f samples/s or %.6f s/sample\n", samples, samples_total, samples / elapsed, elapsed / samples);
            std::printf("Stats per sample: dequeueTime %.6fs, modelTime %.6fs, lossTime %.6fs, backTime %.6fs, optimTime %.6fs\n", dequeue_time / samples, model_time / samples, loss_time / samples, back_time / samples, optim_time / samples);
            samples = 0;
            dequeue_time = model_time = loss_time = back_time = optim_time = 0;
            start = std::chrono::steady_clock::now();
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
void consumer_test(TensorQueue& queue, ChessCNN& model, torch::Device device) {
    auto start = std::chrono::steady_clock::now();
    int64_t samples = 0, steps = 0;
    double correct = 0;
    double channel_loss_total = 0, source_loss_total = 0, destination_loss_total = 0, value_loss_total = 0, total_loss = 0;
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
        auto batch_size = batch.board_moves.size(0);

        // Forward pass
        auto model_start = std::chrono::steady_clock::now();
        auto [channel_logits, source_logits, destination_logits, value_logits, x_legal] = model->forward(batch.board_moves);
        model_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - model_start).count();

        auto loss_start = std::chrono::steady_clock::now();
        auto channel_loss = torch::nn::functional::cross_entropy(channel_logits, batch.move_type);
        auto source_loss = torch::nn::functional::cross_entropy(source_logits, batch.src_sq);
        auto destination_loss = torch::nn::functional::cross_entropy(destination_logits, batch.dst_sq);
        auto value_loss = torch::nn::functional::cross_entropy(value_logits, batch.result);
        auto loss = channel_loss + source_loss + destination_loss + value_loss;
                
        channel_loss_total += channel_loss.item<double>();
        source_loss_total += source_loss.item<double>();
        destination_loss_total += destination_loss.item<double>();
        value_loss_total += value_loss.item<double>();
        total_loss += loss.item<double>();
        loss_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_start).count();
        
        auto mask = x_legal.sum({2, 3}).gt(0); // [batch, 10], 1 if any legal destinations, 0 otherwise
        channel_logits.masked_fill_(mask == 0, -1e6); // Mask illegal move types
        auto channel_probs = torch::softmax(channel_logits, 1); // [batch, 10]
        auto predicted_channel = torch::argmax(channel_probs, 1); // [batch]
        auto predicted_indices = predicted_channel.view({batch_size, 1, 1, 1}).expand({batch_size, 1, 8, 8}); // [batch, 1, 8, 8]
        auto channel_mask = x_legal.gather(1, predicted_indices).squeeze(1).flatten(1, 2); // [batch, 64]
        destination_logits.masked_fill_(channel_mask == 0, -1e6); // Mask illegal destinations
        auto destination_probs = torch::softmax(destination_logits, 1); // [batch, 64]
        auto predicted_destination = torch::argmax(destination_probs, 1); // [batch]

        auto source_mask = batch.board_moves.select(1, 18).flatten(1, 2); // [batch, 64]
        source_logits.masked_fill_(source_mask == 0, -1e6);
        auto source_probs = torch::softmax(source_logits, 1); // [batch, 64]
        auto predicted_source = torch::argmax(source_probs, 1); // [batch]
        
        auto pred_move = predicted_source * 64 + predicted_destination;
        auto target_move = batch.src_sq * 64 + batch.dst_sq;
        correct += (pred_move == target_move).to(torch::kFloat32).sum().item<float>();

        steps++;

         std::cout << "Acc: channel " << (predicted_channel == batch.move_type).to(torch::kFloat).mean().item<float>()
          << ", src " << (predicted_source == batch.src_sq).to(torch::kFloat).mean().item<float>()
          << ", dest " << (predicted_destination == batch.dst_sq).to(torch::kFloat).mean().item<float>()
          << ", move " << correct / (steps * batch_size) << std::endl;
    }
    
    
    if (samples > 0) {
        correct /= samples;
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("Test loss: move type %.3f + source %.3f + destination %.3f + value %.6f = %.6f\n", channel_loss_total / steps, source_loss_total / steps, destination_loss_total / steps, value_loss_total / steps, total_loss / steps);
        std::printf("Test Error: Accuracy: %.1f%%, Avg loss: %.6f. %.0f samples/s or %.6f s/sample\n", (100 * correct), total_loss / steps, samples / elapsed, elapsed / samples);
        std::printf("Stats per sample: dequeueTime %.6f, modelTime %.6f, lossTime %.6f\n", dequeue_time / samples, model_time / samples, loss_time / samples);
    } else {
        std::cout << "No samples processed in test!" << std::endl;
    }
    std::cout << "ConsumerTest: Testing loop exited" << std::endl;
}

int main(int argc, char ** argv) {
    const float learning_rate = 1e-4;
    const float weight_decay = 0.01; //for AdamW optimizer, affects regularization, the higher, the better
    const bool AMSgrad = true; //might improve Adam convergence
    //CosineAnnealingLR() gradually adjusts the learning rate from 
    //learning_rate (1e-4) to lr_min (0.1 * learning_rate = 1e-5) over T_max (10000) steps
    //it can be reset at each epoch or alternatively increase T_max
    const double lr_min = 0.1; //i.e. a factor such as min_lr = 0.1 * learning_rate, another words, 10 times less
    const int T_max = 500;
    const int epochs = 3;
    const unsigned long steps = 10000000; //for curriculum learning (from simple positions of 5 legal moves to complex positions of 20 legal moves over number of steps)
    const std::string weights_file = "chessCNN7.pt";
    const int min_elo = 2400;
    const int max_elo_diff = 200;
    const int minMoves = 40;
    const int num_channels = 29;
    const int num_samples = 1000;
    const enum GameStage game_stage = FullGame; //not used anymore
    const int display_stats_every = 1;
    const int save_weights_every = 1000;
    const int bmpr_queue_len_train = 2;
    const int bmpr_queue_len_test = 3;
    const int accumulation_steps = 16; // Accumulate gradients over 16 batches
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

    // Initialize model and optimizer
    ChessCNN model;
    //std::string f = "";
    //f.append(weights_file).append("10.pt");
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
