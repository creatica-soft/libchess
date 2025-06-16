#pragma once

#include <torch/torch.h>

class ResidualBlockImpl : public torch::nn::Module {
public:
    ResidualBlockImpl(int64_t channels);
    torch::Tensor forward(torch::Tensor x);

private:
    torch::nn::Conv2d conv1{nullptr};
    torch::nn::InstanceNorm2d in1{nullptr};
    torch::nn::Conv2d conv2{nullptr};
    torch::nn::InstanceNorm2d in2{nullptr};
    torch::nn::ReLU relu{nullptr};
};
TORCH_MODULE(ResidualBlock);

class ChessCNNImpl : public torch::nn::Module {
public:
    ChessCNNImpl();
    std::tuple<torch::Tensor, torch::Tensor,
               torch::Tensor, torch::Tensor,
               torch::Tensor, torch::Tensor,
               torch::Tensor> forward(torch::Tensor x);

private:
    //torch::nn::Conv2d conv1{nullptr};
    //torch::nn::Conv2d conv2{nullptr};
    //torch::nn::Conv2d conv3{nullptr};
    torch::nn::Sequential board_conv{nullptr};
    torch::nn::Sequential legal_conv{nullptr};
    torch::nn::Sequential policy_moves_opening{nullptr};
    //torch::nn::Sequential policy_promo_opening{nullptr};
    torch::nn::Sequential value_head_opening{nullptr};
    torch::nn::Sequential policy_moves_middlegame{nullptr};
    //torch::nn::Sequential policy_promo_middlegame{nullptr};
    torch::nn::Sequential value_head_middlegame{nullptr};
    torch::nn::Sequential policy_moves_endgame{nullptr};
    //torch::nn::Sequential policy_promo_endgame{nullptr};
    torch::nn::Sequential value_head_endgame{nullptr};
};
TORCH_MODULE(ChessCNN);