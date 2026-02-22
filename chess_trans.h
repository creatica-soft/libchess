#pragma once

#include <torch/torch.h>

class ChessTransformerImpl : public torch::nn::Module {
public:
    ChessTransformerImpl(int d_model = 128, int nhead = 4, int num_layers = 2, int num_move_types = 10);
    std::tuple<torch::Tensor, torch::Tensor, torch::Tensor, torch::Tensor, torch::Tensor> forward(torch::Tensor x);

private:
    int model_dim;
    torch::nn::Conv2d embed_board{nullptr}, embed_legal{nullptr};
    torch::Tensor pos_encoding;
    torch::nn::TransformerEncoder transformer_encoder{nullptr};
    torch::nn::Sequential move_type_head{nullptr}, source_head{nullptr}, destination_head{nullptr}, value_head{nullptr};
    torch::nn::Linear channel_embedding{nullptr}, source_final{nullptr}, destination_final{nullptr};
};
TORCH_MODULE(ChessTransformer);
