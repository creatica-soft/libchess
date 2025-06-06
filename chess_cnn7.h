#pragma once

#include <torch/torch.h>

class ResidualBlockImpl : public torch::nn::Module {
public:
    ResidualBlockImpl(int64_t channels);
    torch::Tensor forward(torch::Tensor x);

private:
    torch::nn::Conv2d conv1{nullptr};
    torch::nn::BatchNorm2d bn1{nullptr};
    torch::nn::Conv2d conv2{nullptr};
    torch::nn::BatchNorm2d bn2{nullptr};
    torch::nn::ReLU relu{nullptr};
};
TORCH_MODULE(ResidualBlock);

class ChessCNNImpl : public torch::nn::Module {
public:
    ChessCNNImpl();
    std::tuple<torch::Tensor, torch::Tensor, torch::Tensor, torch::Tensor> forward(torch::Tensor x);

private:
    torch::nn::Sequential board_conv{nullptr};
    torch::nn::Sequential channel_head{nullptr};
    torch::nn::Linear channel_embedding{nullptr};
    torch::nn::Sequential destination_head{nullptr};
    torch::nn::Linear destination_final{nullptr};
    torch::nn::Sequential value_head{nullptr};
};
TORCH_MODULE(ChessCNN);
