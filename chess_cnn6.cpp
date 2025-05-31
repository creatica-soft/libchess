#include "chess_cnn6.h"

ResidualBlockImpl::ResidualBlockImpl(int64_t channels) {
    conv1 = register_module("conv1", torch::nn::Conv2d(
        torch::nn::Conv2dOptions(channels, channels, 3).padding(1)));
    bn1 = register_module("bn1", torch::nn::BatchNorm2d(channels));
    conv2 = register_module("conv2", torch::nn::Conv2d(
        torch::nn::Conv2dOptions(channels, channels, 3).padding(1)));
    bn2 = register_module("bn2", torch::nn::BatchNorm2d(channels));
    relu = register_module("relu", torch::nn::ReLU());
}

torch::Tensor ResidualBlockImpl::forward(torch::Tensor x) {
    auto identity = x;
    auto out = conv1->forward(x);
    out = bn1->forward(out);
    out = relu->forward(out);
    out = conv2->forward(out);
    out = bn2->forward(out);
    out += identity;
    return relu->forward(out);
}

ChessCNNImpl::ChessCNNImpl() {
    // Board convolution branch
    board_conv = register_module("board_conv", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(64, 128, 3).padding(1)),
        torch::nn::ReLU(),
        ResidualBlock(128),
        ResidualBlock(128),
        ResidualBlock(128),
        ResidualBlock(128),
        ResidualBlock(128)
    ));

    // Policy moves opening
    policy_moves = register_module("policy_moves", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(128, 64, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(4096, 1024),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.3), //could be increased to 0.5 for better regularization
        torch::nn::Linear(1024, 4096)
    ));

    // Value head opening
    value_head = register_module("value_head", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(128, 64, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(4096, 128),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.3),
        torch::nn::Linear(128, 3)
    ));

    // Xavier normal initialization for linear layers
    for (auto& head : {policy_moves, value_head}) {
        // Iterate over modules in the sequential container
        for (const auto& module : *head) {
            try {
                auto& linear = module.get<torch::nn::LinearImpl>();
                torch::nn::init::xavier_normal_(linear.weight);
            } catch (const c10::Error&) {
                // Skip modules that are not LinearImpl
                continue;
            }
        }
    }
}

std::tuple<torch::Tensor, torch::Tensor, torch::Tensor> ChessCNNImpl::forward(torch::Tensor x) {
    // Split input: first 64 channels for board, then 
    // the rest 64 channels is for masking model predictions, total 128 channels
    auto x_board = x.slice(1, 0, 64);
    auto x_legal_mask = x.slice(1, 64);

    // Process board and legal move features
    auto board_features = board_conv->forward(x_board);

    // Compute outputs for each head
    auto moves_logits = policy_moves->forward(board_features);
    auto value_logits = value_head->forward(board_features);

    return std::make_tuple(moves_logits, value_logits, x_legal_mask);
}