//c++ -std=c++20 -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -O3 -flto -I /Users/ap/Downloads/libtorch/include -I /Users/ap/Downloads/libtorch2/include/torch/csrc/api/include -L /Users/ap/Downloads/libtorch2/lib -L /Users/ap/libchess -Wl,-ltorch,-ltorch_cpu,-lc10,-lchess,-rpath,/Users/ap/Downloads/libtorch2/lib,-rpath,/Users/ap/libchess -o train_cnn my_md5.cpp board_legal_moves.c game_omp.c train_cnn.cpp

#pragma once
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

// Define a simple, fast CNN module
struct TacticalCNNImpl : torch::nn::Module {
    // Layers
    torch::nn::Conv2d conv1{nullptr};
    torch::nn::BatchNorm2d bn1{nullptr};
    torch::nn::Conv2d conv2{nullptr};
    torch::nn::BatchNorm2d bn2{nullptr};
    torch::nn::Linear fc1{nullptr};
    torch::nn::Linear fc_out{nullptr};

    TacticalCNNImpl() {
        // Input channels: 16 (12 pieces + 2 attack maps + 1 legal moves + 1 meta)
        // Output channels: 32 filters
        // Kernel: 3x3, Padding: 1 (to keep 8x8 size)
        conv1 = register_module("conv1", torch::nn::Conv2d(torch::nn::Conv2dOptions(16, 32, 3).padding(1)));
        bn1 = register_module("bn1", torch::nn::BatchNorm2d(32));

        // Layer 2: 32 -> 64 filters
        conv2 = register_module("conv2", torch::nn::Conv2d(torch::nn::Conv2dOptions(32, 64, 3).padding(1)));
        bn2 = register_module("bn2", torch::nn::BatchNorm2d(64));

        // Fully Connected Head
        // Input size: 64 channels * 8 * 8 board = 4096 features
        fc1 = register_module("fc1", torch::nn::Linear(64 * 8 * 8, 128));
        fc_out = register_module("fc_out", torch::nn::Linear(128, 1));
    }

    torch::Tensor forward(torch::Tensor x) {
        // x shape: [BatchSize, 16, 8, 8]
        
        // Block 1: Conv -> BN -> ReLU
        x = torch::relu(bn1(conv1(x)));
        
        // Block 2: Conv -> BN -> ReLU
        x = torch::relu(bn2(conv2(x)));
        
        // Flatten: [Batch, 64, 8, 8] -> [Batch, 4096]
        x = x.view({x.size(0), -1});
        
        // Dense Layers
        x = torch::relu(fc1(x));
        x = torch::tanh(fc_out(x)); // Output between -1 (Loss) and 1 (Win)
        
        return x;
    }
};

TORCH_MODULE(TacticalCNN);