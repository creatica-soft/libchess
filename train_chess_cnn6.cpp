//c++ -O3 -I /opt/anaconda3/include -I /opt/anaconda3/include/torch/csrc/api/include -L /opt/anaconda3/lib -L /Users/ap/libchess -std=c++17 -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/opt/anaconda3/lib,-rpath,/Users/ap/libchess -o chess_cnn2 train_chess_cnn2.cpp chess_cnn2.cpp
//c++ -O3 -I /Users/ap/Downloads/libtorch/include -I /Users/ap/Downloads/libtorch/include/torch/csrc/api/include -L /Users/ap/Downloads/libtorch/lib -L /Users/ap/libchess -std=c++17 -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/Users/ap/Downloads/libtorch/lib,-rpath,/Users/ap/libchess -o chess_cnn6 train_chess_cnn6.cpp chess_cnn6.cpp
#include "chess_cnn6.h"
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
    torch::Tensor move;
    torch::Tensor result;
    torch::Tensor stage;
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

            auto move = torch::from_blob(
                bmpr->moves,
                {bmpr->samples},
                torch::kInt32
            ).to(device, false);

            auto result = torch::from_blob(
                bmpr->result,
                {bmpr->samples},
                torch::kInt32
            ).to(device, false);

            auto stage = torch::from_blob(
                bmpr->stage,
                {bmpr->samples},
                torch::kInt32
            ).to(device, false);

            // Enqueue tensor batch
            queue.enqueue({board_moves, move, result, stage});
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
    double policy_loss_total = 0, value_loss_total = 0, total_loss_avg = 0;
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
        auto [moves_logits, value_logits, x_legal] = model->forward(batch.board_moves);
        model_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - model_start).count();

        // Compute loss
        auto loss_start = std::chrono::steady_clock::now();
        auto legal_moves = x_legal.view({batch.board_moves.size(0), -1});

        //Completely suppress illegal moves
        moves_logits.index_put_({legal_moves == 0}, -std::numeric_limits<float>::infinity());
        auto policy_loss = torch::nn::functional::cross_entropy(moves_logits, batch.move);
        auto value_loss = torch::nn::functional::cross_entropy(value_logits, batch.result);
        auto loss = policy_loss + 0.2 * value_loss;
                
        policy_loss_total += policy_loss.item<double>();
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
            std::printf("Current loss: policy %.3f + %.6f = %.6f\n", policy_loss.item<float>(), value_loss.item<float>(), loss.item<float>());
            std::printf("Avg loss: policy %.3f + value %.6f = %.6f\n", policy_loss_total / steps, value_loss_total / steps, total_loss_avg / steps);
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
    double policy_loss_total = 0, value_loss_total = 0, total_loss_avg = 0;
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
        auto [moves_logits, value_logits, x_legal] = model->forward(batch.board_moves);
        model_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - model_start).count();

        // Compute loss
        auto loss_start = std::chrono::steady_clock::now();
        auto move_probs = torch::tensor(0.0).to(device);
        auto legal_moves = x_legal.view({batch.board_moves.size(0), -1});
        
        //Completely suppress illegal moves
        moves_logits.index_put_({legal_moves == 0}, -std::numeric_limits<float>::infinity());
        auto policy_loss = torch::nn::functional::cross_entropy(moves_logits, batch.move);
        auto value_loss = torch::nn::functional::cross_entropy(value_logits, batch.result);
        auto loss = policy_loss + 0.2 * value_loss;
        // Compute accuracy
        move_probs = torch::softmax(moves_logits, 1);
        auto pred = move_probs.argmax(1);
        auto target = batch.move;
        correct += (pred == target).to(torch::kFloat32).sum().item<float>();
        
        policy_loss_total += policy_loss.item<double>();
        value_loss_total += value_loss.item<double>();
        total_loss_avg += loss.item<double>();
        loss_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - loss_start).count();

        steps++;
    }

    if (samples > 0) {
        correct /= samples;
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("Test loss: policy  %.3f + value %.6f = %.6f\n", policy_loss_total / steps, value_loss_total / steps, total_loss_avg / steps);
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
    const int T_max = 1000;
    const int epochs = 1;
    const unsigned long steps = 10000000; //for curriculum learning (from simple positions of 5 legal moves to complex positions of 20 legal moves over number of steps)
    const std::string weights_file = "chessCNN6.pt";
    const int min_elo = 2400;
    const int max_elo_diff = 200;
    const int minMoves = 40;
    const int num_channels = 128;
    const int num_samples = 1000;
    const enum GameStage game_stage = FullGame;
    const int display_stats_every = 1;
    const int save_weights_every = 1000;
    const int bmpr_queue_len_train = 2;
    const int bmpr_queue_len_test = 3;
    const int accumulation_steps = 8; // Accumulate gradients over 16 batches
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