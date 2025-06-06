#include "chess_cnn7.h"

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
        torch::nn::Conv2d(torch::nn::Conv2dOptions(89, 192, 3).padding(1)),
        torch::nn::ReLU(),
        ResidualBlock(192),
        ResidualBlock(192),
        ResidualBlock(192),
        ResidualBlock(192),
        ResidualBlock(192)
    ));

    // Channel head: predicts one of 21 move types
    channel_head = register_module("channel_head", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(192, 64, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(4096, 256),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.2),
        torch::nn::Linear(256, 21) // 21 channels for move types
    ));

    // Channel embedding: maps channel prediction to a dense vector
    channel_embedding = register_module("channel_embedding", torch::nn::Linear(21, 64));

    // Destination (square) head: predicts one of 64 destination squares, conditioned on channel in destination_final
    destination_head = register_module("destination_head", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(192, 64, 1)),
        torch::nn::Flatten(),
        // Input size: 64 * 8 * 8 (board features) + 64 (channel embedding)
        torch::nn::Linear(4096, 256),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.2)
        //torch::nn::Linear(256, 64) // 64 destination squares - moved to destination_final
                                    // to take additional input channel_emb 
    ));

    // Final linear layer for destination head after concatenation
    destination_final = register_module("destination_final", torch::nn::Linear(256 + 64, 64));
    
    // Value head
    value_head = register_module("value_head", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(192, 16, 1)),
        torch::nn::Flatten(),
        torch::nn::Linear(1024, 32),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.2),
        torch::nn::Linear(32, 3)
    ));

    // Xavier normal initialization for linear layers
    for (auto& head : {channel_head, destination_head, value_head}) {
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
    // Initialize channel embedding
    torch::nn::init::xavier_normal_(channel_embedding->weight);
    torch::nn::init::xavier_normal_(destination_final->weight);
}

std::tuple<torch::Tensor, torch::Tensor, torch::Tensor, torch::Tensor> ChessCNNImpl::forward(torch::Tensor x) {
    //21 x 2 = 42 piece channels + 5 special channels + 21 control square channels = 68 + 21 legal moves = 89 total
    auto x_legal_mask = x.slice(1, 68); //slice along dim 1 from 68 to the end (21 channels: 16 + 5 for promotions) [batch, 21, 8, 8]

    // Process board and legal move features
    auto board_features = board_conv->forward(x);

    // Channel prediction
    auto channel_logits = channel_head->forward(board_features); // Shape: [batch, 21]

    // Get channel probabilities and predicted channel (for conditioning)
    auto channel_probs = torch::softmax(channel_logits, /*dim=*/1); // Shape: [batch, 21]
    // During inference, you can use: predicted_channel = torch::argmax(channel_probs, /*dim=*/1, /*keepdim=*/true);

    // Compute channel embedding. Use soft probabilities for training stability
    auto channel_emb = channel_embedding->forward(channel_probs); // Shape: [batch, 64]

    // Process board features through destination head up to the final linear layer
    auto dest_features = destination_head->forward(board_features); // Shape: [batch, 256]

    // Concatenate destination features with channel embedding
    auto destination_input = torch::cat({dest_features, channel_emb}, /*dim=*/1); // Shape: [batch, 256 + 64]

    // Final destination prediction
    auto destination_logits = destination_final->forward(destination_input); // Shape: [batch, 64]

    // Predict game outcome
    auto value_logits = value_head->forward(board_features);

    return std::make_tuple(channel_logits, destination_logits, value_logits, x_legal_mask);
  }