#include "chess_cnn5.h"

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
    policy_moves_opening = register_module("policy_moves_opening", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(128, 64, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(4096, 1024),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.3), //could be increased to 0.5 for better regularization
        torch::nn::Linear(1024, 4096)
    ));

    // Value head opening
    value_head_opening = register_module("value_head_opening", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(128, 64, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(4096, 128),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.3),
        torch::nn::Linear(128, 3)
    ));

    // Policy moves middlegame
    policy_moves_middlegame = register_module("policy_moves_middlegame", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(128, 64, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(4096, 1024),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.3),
        torch::nn::Linear(1024, 4096)
    ));

    // Value head middlegame
    value_head_middlegame = register_module("value_head_middlegame", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(128, 64, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(4096, 128),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.3),
        torch::nn::Linear(128, 3)
    ));

    // Policy moves endgame
    policy_moves_endgame = register_module("policy_moves_endgame", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(128, 64, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(4096, 1024),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.3), 
        torch::nn::Linear(1024, 4096)
    ));

    // Value head endgame
    value_head_endgame = register_module("value_head_endgame", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(128, 64, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(4096, 128),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.3),
        torch::nn::Linear(128, 3)
    ));

    // Xavier normal initialization for linear layers
    for (auto& head : {policy_moves_opening, value_head_opening,
                       policy_moves_middlegame, value_head_middlegame,
                       policy_moves_endgame, value_head_endgame}) {
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

std::tuple<torch::Tensor, torch::Tensor,
           torch::Tensor, torch::Tensor,
           torch::Tensor, torch::Tensor,
           torch::Tensor> ChessCNNImpl::forward(torch::Tensor x) {
    // Split input: first 64 channels for board, then 
    // the rest 64 channels is for masking model predictions, total 128 channels
    auto x_board = x.slice(1, 0, 64);
    auto x_legal_mask = x.slice(1, 64);

    // Process board and legal move features
    auto board_features = board_conv->forward(x_board);

    // Compute outputs for each head
    auto moves_logits_opening = policy_moves_opening->forward(board_features);
    auto value_logits_opening = value_head_opening->forward(board_features);
    auto moves_logits_middlegame = policy_moves_middlegame->forward(board_features);
    auto value_logits_middlegame = value_head_middlegame->forward(board_features);
    auto moves_logits_endgame = policy_moves_endgame->forward(board_features);
    auto value_logits_endgame = value_head_endgame->forward(board_features);

    return std::make_tuple(
        moves_logits_opening, value_logits_opening,
        moves_logits_middlegame, value_logits_middlegame,
        moves_logits_endgame, value_logits_endgame,
        x_legal_mask
    );
}