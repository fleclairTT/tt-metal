// SPDX-FileCopyrightText: (c) 2024 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "layernorm_op.hpp"

#include <core/ttnn_all_includes.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "autograd/auto_context.hpp"
#include "autograd/graph.hpp"
#include "autograd/graph_utils.hpp"
#include "core/compute_kernel_config.hpp"
#include "core/tt_tensor_utils.hpp"

namespace ttml::ops {

// simplified version of layernorm
// it works only for 4D tensors and for the last dimension
autograd::TensorPtr layernorm(
    const autograd::TensorPtr& tensor, const autograd::TensorPtr& gamma, const autograd::TensorPtr& beta) {
    auto tensor_shape = tensor->get_value().get_shape();
    auto mean = core::empty(
        core::create_shape({tensor_shape[0], tensor_shape[1], tensor_shape[2], 1}),
        &autograd::ctx().get_device(),
        tensor->get_value().memory_config());
    auto rstd = ttnn::empty_like(mean);
    auto output = ttnn::empty_like(tensor->get_value());

    auto out_tensors = ttnn::moreh_layer_norm(
        tensor->get_value(),
        1,
        1e-6F,
        /* gamma */ gamma->get_value(),
        /* beta */ beta->get_value(),
        output,
        mean,
        rstd,
        /* memory_config */ std::nullopt,
        /* compute_kernel_config */ std::nullopt);

    auto out = autograd::create_tensor();
    out->set_value(out_tensors[0].value());
    mean = out_tensors[1].value();
    rstd = out_tensors[2].value();

    autograd::GradFunction grad = [tensor, out, mean, rstd, gamma, beta]() {
        auto input_grad = ttnn::empty_like(tensor->get_value());
        auto gamma_grad = ttnn::empty_like(gamma->get_value());
        auto beta_grad = ttnn::empty_like(beta->get_value());

        auto res = ttnn::moreh_layer_norm_backward(
            out->get_grad(),
            tensor->get_value(),
            mean,
            rstd,
            1,
            gamma->get_value(),
            input_grad,
            gamma_grad,
            beta_grad,
            /* memory_config */ std::nullopt,
            /* compute_kernel_config */ std::nullopt);

        tensor->add_grad(res[0].value());
        gamma->add_grad(res[1].value());
        beta->add_grad(res[2].value());
    };

    auto links = autograd::get_links(tensor);
    out->set_node(autograd::ctx().add_backward_node(std::move(grad), links));

    return out;
}
}  // namespace ttml::ops