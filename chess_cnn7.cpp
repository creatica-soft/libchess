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
        torch::nn::Conv2d(torch::nn::Conv2dOptions(19, 192, 3).padding(1)),
        torch::nn::BatchNorm2d(192),
        torch::nn::ReLU(),
        ResidualBlock(192),
        ResidualBlock(192),
        ResidualBlock(192),
        ResidualBlock(192),
        ResidualBlock(192),
        ResidualBlock(192)
    ));

    // Channel head: predicts one of 10 move types
    channel_head = register_module("channel_head", torch::nn::Sequential(
        torch::nn::AdaptiveAvgPool2d(1),
        torch::nn::Flatten(),
        torch::nn::Linear(224, 256),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.2),
        torch::nn::Linear(256, 10) // 10 channels for move types
    ));

    // Channel embedding: maps channel prediction to a dense vector
    channel_embedding = register_module("channel_embedding", torch::nn::Linear(10, 64));

    legal_embed_conv = register_module("legal_embed_conv", torch::nn::Conv2d(
        torch::nn::Conv2dOptions(10, 32, 1) // 10 input channels, 32 output, 1x1 kernel
    ));

    // Source (square) head: predicts one of 64 source squares, conditioned on channel in source_final
    source_head = register_module("source_head", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(192, 64, 3).padding(1)),
        torch::nn::BatchNorm2d(64),
        torch::nn::ReLU(),
        torch::nn::AdaptiveAvgPool2d(1),
        torch::nn::Flatten(),
        // Input size: 64 * 8 * 8 (board features) + 64 (channel embedding)
        torch::nn::Linear(64, 256),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.2)
        //torch::nn::Linear(256, 64) // 64 source squares - moved to source_final
                                    // to take additional input channel_emb 
    ));

    // Destination (square) head: predicts one of 64 destination squares, conditioned on channel in destination_final
    destination_head = register_module("destination_head", torch::nn::Sequential(
        torch::nn::Conv2d(torch::nn::Conv2dOptions(224, 64, 3).padding(1)),
        torch::nn::BatchNorm2d(64),
        torch::nn::ReLU(),
        torch::nn::AdaptiveAvgPool2d(1),
        torch::nn::Flatten(),
        // Input size: 64 * 8 * 8 (board features) + 64 (channel embedding)
        torch::nn::Linear(64, 256),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.2)
        //torch::nn::Linear(256, 64) // 64 destination squares - moved to destination_final
                                    // to take additional input channel_emb 
    ));

    // Final linear layer for source head after concatenation
    source_final = register_module("source_final", torch::nn::Linear(256 + 64, 64));

    // Final linear layer for destination head after concatenation
    destination_final = register_module("destination_final", torch::nn::Linear(256 + 64, 64));
    
    // Value head
    value_head = register_module("value_head", torch::nn::Sequential(
        torch::nn::AdaptiveAvgPool2d(1),
        torch::nn::Flatten(),
        torch::nn::Linear(224, 32),
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
    torch::nn::init::xavier_normal_(source_final->weight);
    torch::nn::init::xavier_normal_(destination_final->weight);
}

std::tuple<torch::Tensor, torch::Tensor, torch::Tensor, torch::Tensor, torch::Tensor> ChessCNNImpl::forward(torch::Tensor x) {
    auto x_board = x.slice(1, 0, 19);
    auto x_legal_mask = x.slice(1, 19); //slice along dim 1 from 19 to the end (10 channels: 6 + 4 for promotions) [batch, 10, 8, 8]
    
    auto legal_embed = legal_embed_conv->forward(x_legal_mask); // [batch, 32, 8, 8]
    auto board_features = board_conv->forward(x_board); // [batch, 19, 8, 8]
    auto combined_features = torch::cat({board_features, legal_embed}, 1); // [batch, 224, 8, 8]
    
    auto channel_logits = channel_head->forward(combined_features); // Shape: [batch, 10]
    auto channel_probs = torch::softmax(channel_logits, /*dim=*/1); // Shape: [batch, 10]
    auto channel_emb = channel_embedding->forward(channel_probs); // Shape: [batch, 64]

    auto src_features = source_head->forward(board_features); // Shape: [batch, 256]
    auto source_input = torch::cat({src_features, channel_emb}, /*dim=*/1); // Shape: [batch, 256 + 64]
    auto source_logits = source_final->forward(source_input); // Shape: [batch, 64]

    auto dest_features = destination_head->forward(combined_features); // Shape: [batch, 256]
    auto destination_input = torch::cat({dest_features, channel_emb}, /*dim=*/1); // Shape: [batch, 256 + 64]
    auto destination_logits = destination_final->forward(destination_input); // Shape: [batch, 64]

    auto value_logits = value_head->forward(combined_features); // Shape: [batch, 3]

    return std::make_tuple(channel_logits, source_logits, destination_logits, value_logits, x_legal_mask);
  }