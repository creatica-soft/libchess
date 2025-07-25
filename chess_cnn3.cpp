#include "chess_cnn2.h"

ResidualBlockImpl::ResidualBlockImpl(int64_t channels) {
    // Configure InstanceNorm2d options
    torch::nn::InstanceNorm2dOptions options(channels);
    options.affine(true);              // Enable learnable affine parameters
    options.track_running_stats(false); // Track running mean and variance
    options.eps(1e-5);                 // Epsilon for numerical stability
    options.momentum(0.1);             // Momentum for running stats
    
    conv1 = register_module("conv1", torch::nn::Conv2d(
        torch::nn::Conv2dOptions(channels, channels, 7).padding(3)));
    in1 = register_module("in1", torch::nn::InstanceNorm2d(options));
    conv2 = register_module("conv2", torch::nn::Conv2d(
        torch::nn::Conv2dOptions(channels, channels, 7).padding(3)));
    in2 = register_module("in2", torch::nn::InstanceNorm2d(options));
    relu = register_module("relu", torch::nn::ReLU());
}

torch::Tensor ResidualBlockImpl::forward(torch::Tensor x) {
    auto identity = x;
    auto out = conv1->forward(x);
    out = in1->forward(out);
    out = relu->forward(out);
    out = conv2->forward(out);
    out = in2->forward(out);
    out += identity;
    return relu->forward(out);
}

ChessCNNImpl::ChessCNNImpl() {
    // Board convolution branch
    //conv1 = register_module("conv1", torch::nn::Conv2d(torch::nn::Conv2dOptions(26, 32, 7).padding(3)));
    //conv2 = register_module("conv2", torch::nn::Conv2d(torch::nn::Conv2dOptions(26, 32, 5).padding(2)));
    //conv3 = register_module("conv3", torch::nn::Conv2d(torch::nn::Conv2dOptions(26, 32, 3).padding(1)));
    board_conv = register_module("board_conv", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(14, 64, 7).padding(3)),    
        torch::nn::ReLU(),
        ResidualBlock(64),
        ResidualBlock(64),
        ResidualBlock(64),
        ResidualBlock(64),
        ResidualBlock(64)
    ));

    // Legal moves convolution branch
    legal_conv = register_module("legal_conv", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(64, 128, 1)),
        torch::nn::ReLU(),
        ResidualBlock(128)
    ));

    // Policy moves opening
    policy_moves_opening = register_module("policy_moves_opening", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(192, 64, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(4096, 4096),
        torch::nn::ReLU(),
        torch::nn::Linear(4096, 4096)
    ));

    // Policy promotions opening
    /*
    policy_promo_opening = register_module("policy_promo_opening", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(192, 16, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(1024, 64),
        torch::nn::ReLU(),
        torch::nn::Linear(64, 4)
    ));*/

    // Value head opening
    value_head_opening = register_module("value_head_opening", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(192, 8, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(512, 64),
        torch::nn::ReLU(),
        torch::nn::Linear(64, 3)
    ));

    // Policy moves middlegame
    policy_moves_middlegame = register_module("policy_moves_middlegame", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(192, 64, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(4096, 4096),
        torch::nn::ReLU(),
        torch::nn::Linear(4096, 4096)
    ));

    // Policy promotions middlegame
    /*
    policy_promo_middlegame = register_module("policy_promo_middlegame", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(192, 16, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(1024, 64),
        torch::nn::ReLU(),
        torch::nn::Linear(64, 4)
    ));*/

    // Value head middlegame
    value_head_middlegame = register_module("value_head_middlegame", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(192, 8, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(512, 64),
        torch::nn::ReLU(),
        torch::nn::Linear(64, 3)
    ));

    // Policy moves endgame
    policy_moves_endgame = register_module("policy_moves_endgame", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(192, 64, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(4096, 4096),
        torch::nn::ReLU(),
        torch::nn::Linear(4096, 4096)
    ));

    // Policy promotions endgame
    /*
    policy_promo_endgame = register_module("policy_promo_endgame", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(192, 16, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(1024, 64),
        torch::nn::ReLU(),
        torch::nn::Linear(64, 4)
    ));*/

    // Value head endgame
    value_head_endgame = register_module("value_head_endgame", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(192, 8, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(512, 64),
        torch::nn::ReLU(),
        torch::nn::Linear(64, 3)
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
    // Split input: first 14 channels for board, remaining 64 for legal moves, total 78 channels
    auto x_board = x.slice(1, 0, 14);
    auto x_legal = x.slice(1, 14);

    // Process board and legal move features
    //auto board_features1 = conv1->forward(x_board);
    //auto board_features2 = conv2->forward(x_board);
    //auto board_features3 = conv3->forward(x_board);
    //auto board_features_combined = torch::cat({board_features1, board_features2, board_features3}, 1);
    auto board_features = board_conv->forward(x_board);
    auto legal_features = legal_conv->forward(x_legal);

    // Concatenate features along channel dimension
    auto combined = torch::cat({board_features, legal_features}, 1);

    // Compute outputs for each head
    auto moves_logits_opening = policy_moves_opening->forward(combined);
    //auto promo_logits_opening = policy_promo_opening->forward(combined);
    auto value_logits_opening = value_head_opening->forward(combined);
    auto moves_logits_middlegame = policy_moves_middlegame->forward(combined);
    //auto promo_logits_middlegame = policy_promo_middlegame->forward(combined);
    auto value_logits_middlegame = value_head_middlegame->forward(combined);
    auto moves_logits_endgame = policy_moves_endgame->forward(combined);
    //auto promo_logits_endgame = policy_promo_endgame->forward(combined);
    auto value_logits_endgame = value_head_endgame->forward(combined);

    return std::make_tuple(
        moves_logits_opening, value_logits_opening,
        moves_logits_middlegame, value_logits_middlegame,
        moves_logits_endgame, value_logits_endgame,
        x_legal
    );
}