#include <cmath>
#include "chess_trans.h"

// Positional encoding for 64 squares
torch::Tensor create_pos_encoding(int d_model) {
    torch::Tensor pos = torch::arange(64, torch::kFloat).view({1, 64, 1});
    torch::Tensor div_term = torch::exp(torch::arange(0, d_model, 2, torch::kFloat) * -(M_PI / d_model));
    torch::Tensor pe = torch::zeros({1, 64, d_model});
    pe.slice(2, 0, d_model, 2) = torch::sin(pos * div_term);
    pe.slice(2, 1, d_model, 2) = torch::cos(pos * div_term);
    return pe;
}

ChessTransformerImpl::ChessTransformerImpl(int d_model, int nhead, int num_layers, int num_move_types) {
    model_dim = d_model;
    // Input embedding: map 19 board channels + 10 legal mask channels to d_model
    embed_board = register_module("embed_board", torch::nn::Conv2d(
        torch::nn::Conv2dOptions(19, d_model, 1))); // [batch, d_model, 8, 8]
    embed_legal = register_module("embed_legal", torch::nn::Conv2d(
        torch::nn::Conv2dOptions(10, d_model, 1))); // [batch, d_model, 8, 8]

    // Positional encoding
    pos_encoding = register_buffer("pos_encoding", create_pos_encoding(d_model));

    // Transformer encoder
    torch::nn::TransformerEncoderLayerOptions layer_options(d_model, nhead);
    layer_options.dropout(0.1);
    auto encoder_layer = torch::nn::TransformerEncoderLayer(layer_options);
    torch::nn::TransformerEncoderOptions encoder_options(encoder_layer, num_layers);
    transformer_encoder = register_module("transformer_encoder", torch::nn::TransformerEncoder(encoder_options));

    // Heads
    move_type_head = register_module("move_type_head", torch::nn::Sequential(
        torch::nn::Linear(d_model, d_model),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.3),
        torch::nn::Linear(d_model, num_move_types) // [batch, 10]
    ));

    source_head = register_module("source_head", torch::nn::Sequential(
        torch::nn::Linear(d_model * 64, d_model),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.3),
        torch::nn::Linear(d_model, 64) // [batch, 64]
    ));

    destination_head = register_module("destination_head", torch::nn::Sequential(
        torch::nn::Linear(d_model * 64, d_model),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.3),
        torch::nn::Linear(d_model, 64) // [batch, 64]
    ));

    value_head = register_module("value_head", torch::nn::Sequential(
        torch::nn::Linear(d_model, d_model),
        torch::nn::ReLU(),
        torch::nn::Dropout(0.3),
        torch::nn::Linear(d_model, 3) // [batch, 3]
    ));
    channel_embedding = register_module("channel_embedding", torch::nn::Linear(num_move_types, d_model)); // [10] -> [d_model]
    source_final = register_module("source_final", torch::nn::Linear(64 + d_model, 64)); // [64 + d_model] -> [64]
    destination_final = register_module("destination_final", torch::nn::Linear(64 + d_model, 64)); // [64 + d_model] -> [64]
}

std::tuple<torch::Tensor, torch::Tensor, torch::Tensor, torch::Tensor, torch::Tensor> ChessTransformerImpl::forward(torch::Tensor x) {
    auto x_board = x.slice(1, 0, 19); // [batch, 19, 8, 8]
    auto x_legal_mask = x.slice(1, 19); // [batch, 10, 8, 8]

    // Embed inputs
    auto board_embed = embed_board->forward(x_board); // [batch, d_model, 8, 8]
    auto legal_embed = embed_legal->forward(x_legal_mask); // [batch, d_model, 8, 8]
    auto combined_embed = board_embed + legal_embed; // [batch, d_model, 8, 8]

    auto seq = combined_embed.view({-1, model_dim, 64}); // [batch, model_dim, 64]

    // Add positional encoding
    seq = seq + pos_encoding; // [batch, 64, d_model]
   
    // Reshape to sequence: [batch_size, d_model, 64] -> [64, batch_size, d_model]
    // Transformer by default expect [seq_length, batch_size, d_model] and outputs the same shape!
    seq = seq.transpose(1, 2).transpose(0, 1); // [64, batch, d_model]

    // Transformer encoder
    auto encoder_output = transformer_encoder->forward(seq); // [64, batch, d_model]
    encoder_output = encoder_output.transpose(0, 1).transpose(1, 2); // [batch, d_model, 64]
    
    // Pool over sequence for move type and value
    auto pooled = encoder_output.mean(1); // [batch, d_model]

    // Heads
    auto channel_logits = move_type_head->forward(pooled); // [batch, 10]
    auto channel_prob = torch::softmax(channel_logits, 1);
    auto channel_emb = channel_embedding->forward(channel_prob); // [batch, 64]

    // Source and destination logits
    auto src_logits = source_head->forward(encoder_output.view({-1, 64 * model_dim})); // [batch, 64]
    auto src_input = torch::cat({src_logits, channel_emb}, 1); // [batch, 128]
    auto source_logits = source_final->forward(src_input); // [batch, 64]

    auto dst_logits = destination_head->forward(encoder_output.view({-1, 64 * model_dim})); // [batch, 64]
    auto dst_input = torch::cat({dst_logits, channel_emb}, 1); // [batch, 128]
    auto destination_logits = destination_final->forward(dst_input); // [batch, 64]

    auto value_logits = value_head->forward(pooled); // [batch, 3]

    return std::make_tuple(channel_logits, source_logits, destination_logits, value_logits, x_legal_mask);
}