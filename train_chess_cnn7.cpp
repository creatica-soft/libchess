//c++ -O3 -I /opt/anaconda3/include -I /opt/anaconda3/include/torch/csrc/api/include -L /opt/anaconda3/lib -L /Users/ap/libchess -std=c++17 -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/opt/anaconda3/lib,-rpath,/Users/ap/libchess -o chess_cnn2 train_chess_cnn2.cpp chess_cnn2.cpp
//c++ -O3 -I /Users/ap/Downloads/libtorch/include -I /Users/ap/Downloads/libtorch/include/torch/csrc/api/include -L /Users/ap/Downloads/libtorch/lib -L /Users/ap/libchess -std=c++17 -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/Users/ap/Downloads/libtorch/lib,-rpath,/Users/ap/libchess -o chess_cnn7 train_chess_cnn7.cpp chess_cnn7.cpp
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
    torch::Tensor move_src;
    torch::Tensor move_dst;
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

            auto move_src = torch::from_blob(
                bmpr->move_src,
                {bmpr->samples},
                torch::kInt64
            ).to(device, false);

            auto move_dst = torch::from_blob(
                bmpr->move_dst,
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
            queue.enqueue({board_moves, move_src, move_dst, result});
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
    double channel_loss_total = 0, destination_loss_total = 0, value_loss_total = 0, total_loss_avg = 0;
    double dequeue_time = 0, model_time = 0, loss_time = 0, back_time = 0, optim_time = 0;
    int accum_count = 0;
    bool debug_mode = false;
    
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
/*
//to visually verify that the model gets the correct data
//run the training for few positions
for (int64_t i = 0; i < batch.board_moves.size(0); i++) {
  for (int64_t j = 0; j < batch.board_moves.size(1); j++) {
    std::cerr << batch.board_moves.index({i, j}) << std::endl;
  }
  std::cerr << batch.move.index({i}) << std::endl;
  std::cerr << batch.result.index({i}) << std::endl;
  std::cerr << batch.stage.index({i}) << std::endl;
}*/

        // Forward pass
        auto model_start = std::chrono::steady_clock::now();
        //moves_logits
        auto [channel_logits, destination_logits, value_logits, x_legal] = model->forward(batch.board_moves);
        model_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - model_start).count();
        
        // channel_logits: [batch, 21]
        // destination_logits: [batch, 64]
        // value_logits: [batch, 3]
        // x_legal: [batch, 21, 8, 8], 1s for legal destination squares, 0s for illegal
    
        if (debug_mode) {
            std::cout << "target_channel values (first 10): " << batch.move_src.slice(0, 0, 10) << std::endl;
            std::cout << "target_destination values (first 10): " << batch.move_dst.slice(0, 0, 10) << std::endl;            
            // Log raw input mask channel
            std::cout << "input mask channel (batch 0, 68 + target_channel[0]): " << batch.board_moves[0][68 + batch.move_src[0].item<int64_t>()] << std::endl;            
            assert(!destination_logits.isnan().any().item<bool>());
            assert(!destination_logits.isinf().any().item<bool>());
            std::cout << "x_legal_mask shape: " << x_legal.sizes() << std::endl;
            std::cout << "destination_logits before mask (first sample): " << destination_logits[0] << std::endl;
        }
    
       // Reshape target_channel for batched indexing
        auto target_indices = batch.move_src.view({batch.board_moves.size(0), 1, 1, 1}).expand({batch.board_moves.size(0), 1, 8, 8});; // Shape: [batch, 1, 1, 1]
        // Use gather to select one channel per batch sample
        //auto channel_mask = x_legal.gather(1, target_indices); // Shape: [batch, 1, 8, 8]
        //channel_mask = channel_mask.squeeze(1); // Shape: [batch, 8, 8]
        //channel_mask = torch::flatten(channel_mask, 1, 2); // Shape: [batch, 64]
        //combine above three lines into one
        auto channel_mask = x_legal.gather(1, target_indices).squeeze(1).flatten(1, 2);
        //auto mask_bool = (channel_mask == 0).to(torch::kBool); // Shape: [batch, 64] - unnecessary
        // Debug: Verify shapes
        if (debug_mode) {
          std::cout << "channel_mask after flatten: " << channel_mask.sizes() << std::endl;
          std::cout << "mask_bool: " << mask_bool.sizes() << std::endl;
          std::cout << "destination_logits: " << destination_logits.sizes() << std::endl;
          std::cout << "channel_mask values: " << channel_mask[0] << std::endl; // First sample
          std::cout << "mask_bool values: " << mask_bool[0] << std::endl; // First sample
          assert(channel_mask.sizes() == destination_logits.sizes());
          assert(mask_bool.sizes() == destination_logits.sizes());
        }
        // Apply legal move mask to destination logits
        destination_logits.masked_fill_(channel_mask == 0, -1e9);
    
        // Optional: Verify masking
        if (debug_mode) {
            //std::cout << "destination_logits after mask: " << destination_logits[0] << "\n";
            auto destination_probs = torch::softmax(destination_logits.clamp(-50, 50), 1);
            auto illegal_probs = destination_probs.masked_select(mask_bool);
            auto max_illegal_prob = illegal_probs.size(0) > 0 ? illegal_probs.max().item<float>() : 0.0f;
            std::cout << "Max illegal probability: " << max_illegal_prob << "\n";
        }    
    
        // Compute losses
        auto loss_start = std::chrono::steady_clock::now();
        auto channel_loss = torch::nn::functional::cross_entropy(channel_logits, batch.move_src);
        auto destination_loss = torch::nn::functional::cross_entropy(destination_logits, batch.move_dst);
        auto value_loss = torch::nn::functional::cross_entropy(value_logits, batch.result);
        auto loss = (channel_loss + destination_loss) + 0.2 * value_loss;
                
        channel_loss_total += channel_loss.item<double>();
        destination_loss_total += destination_loss.item<double>();
        value_loss_total += value_loss.item<double>();
        total_loss_avg += loss.item<double>();
        loss_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_start).count();

        // Backward pass
        auto back_start = std::chrono::steady_clock::now();
        loss.backward();
        //torch::nn::utils::clip_grad_norm_(model->parameters(), 1.0);
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
            std::printf("Current loss: channel %.3f + destination %.3f + %.6f = %.6f\n", channel_loss.item<float>(), destination_loss.item<float>(), value_loss.item<float>(), loss.item<float>());
            std::printf("Avg loss: channel %.3f + destination %.3f + value %.6f = %.6f\n", channel_loss_total / steps, destination_loss_total / steps, value_loss_total / steps, total_loss_avg / steps);
            std::printf("[Current / Total samples]: [%lld / %lld]: %.0f samples/s or %.6f s/sample\n", samples, samples_total, samples / elapsed, elapsed / samples);
            std::printf("Stats per sample: dequeueTime %.6fs, modelTime %.6fs, lossTime %.6fs, backTime %.6fs, optimTime %.6fs\n", dequeue_time / samples, model_time / samples, loss_time / samples, back_time / samples, optim_time / samples);
            samples = 0;
            dequeue_time = model_time = loss_time = back_time = optim_time = 0;
            start = std::chrono::steady_clock::now();
        }

        if (steps % save_weights_every == 0) {
            std::cout << "Saving weights to " << weights_file << "..." << std::endl;
            torch::save(model, weights_file);
            std::cout << "Saved weights to " << weights_file << std::endl;
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
    double channel_loss_total = 0, destination_loss_total = 0, value_loss_total = 0, total_loss_avg = 0;
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

        // Forward pass
        auto model_start = std::chrono::steady_clock::now();
        auto [channel_logits, destination_logits, value_logits, x_legal] = model->forward(batch.board_moves);
        model_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - model_start).count();

        // x_legal: [batch, 21, 8, 8]
        
        // Predict channel
        auto channel_probs = torch::softmax(channel_logits, /*dim=*/1); // Shape: [batch, 21]
        auto predicted_channel = torch::argmax(channel_probs, /*dim=*/1); // Shape: [batch]
        
        // Apply legal move mask for the predicted channel
        auto channel_mask = x_legal.index_select(1, predicted_channel).squeeze(1); // Shape: [batch, 8, 8]
        channel_mask = torch::flatten(channel_mask, 1); // Shape: [batch, 64]
        auto mask_bool = (channel_mask == 0).to(torch::kBool); // Shape: [batch, 64]
        // Mask illegal destination squares in-place
        destination_logits.masked_fill_(mask_bool, -1e9);        
    
        // Predict destination
        auto predicted_destination = torch::argmax(destination_logits, /*dim=*/1); // Shape: [batch]
        
        // Predict value (e.g., for evaluation)
        auto value_probs = torch::softmax(value_logits, /*dim=*/1); // Shape: [batch, 3]
        auto predicted_value = torch::argmax(value_probs, /*dim=*/1); // Shape: [batch]

        // Compute loss
        auto loss_start = std::chrono::steady_clock::now();
        auto channel_loss = torch::nn::functional::cross_entropy(channel_logits, batch.move_src);
        auto destination_loss = torch::nn::functional::cross_entropy(destination_logits, batch.move_dst);
        auto value_loss = torch::nn::functional::cross_entropy(value_logits, batch.result);
        auto loss = (channel_loss + destination_loss) + 0.2 * value_loss;
                
        channel_loss_total += channel_loss.item<double>();
        destination_loss_total += destination_loss.item<double>();
        value_loss_total += value_loss.item<double>();
        total_loss_avg += loss.item<double>();
        
        auto pred_move = predicted_channel * 64 + predicted_destination;
        auto target_move = batch.move_src * 64 + batch.move_dst;
        correct += (pred_move == target_move).to(torch::kFloat32).sum().item<float>();
        
        loss_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_start).count();

        steps++;
    }

    if (samples > 0) {
        correct /= samples;
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("Test loss: channel  %.3f + destination %.3f + value %.6f = %.6f\n", channel_loss_total / steps, destination_loss_total / steps, value_loss_total / steps, total_loss_avg / steps);
        std::printf("Test Error: Accuracy: %.1f%%, Avg loss: %.6f. %.0f samples/s or %.6f s/sample\n", (100 * correct), total_loss_avg / steps, samples / elapsed, elapsed / samples);
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
    const int epochs = 1;
    const unsigned long steps = 10000000; //for curriculum learning (from simple positions of 5 legal moves to complex positions of 20 legal moves over number of steps)
    const std::string weights_file = "chessCNN7.pt";
    const int min_elo = 2400;
    const int max_elo_diff = 200;
    const int minMoves = 40;
    const int num_channels = 89;
    const int num_samples = 1000;
    const enum GameStage game_stage = FullGame; //not used anymore
    const int display_stats_every = 1;
    const int save_weights_every = 1000;
    const int bmpr_queue_len_train = 2;
    const int bmpr_queue_len_test = 3;
    const int accumulation_steps = 16; // Accumulate gradients over 16 batches
    const std::string pgn_dir = "/Users/ap/Downloads/Lichess Elite Database/";
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