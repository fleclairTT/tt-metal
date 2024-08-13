// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0


#include <pybind11/pybind11.h>
#include <pybind11/stl.h>


#include "indexed_fill_pybind.hpp"
#include "indexed_fill.hpp"
#include "ttnn/cpp/pybind11/decorators.hpp"


namespace ttnn::operations::data_movement {
namespace detail {

void bind_indexed_fill(pybind11::module& module) {
    auto doc = fmt::format(
        R"doc({0}(batch_id: ttnn.Tensor, input_tensor_a: ttnn.Tensor, input_tensor_b: ttnn.Tensor, *, memory_config: Optional[MemoryConfig] = std::nullopt, dim: int = 0, queue_id: int = 0) -> ttnn.Tensor

            Replaces batch of input in input_b denoted by batch_ids into input_a.

            Args:
                * :attr:`batch_id`
                * :attr:`input_tensor_a`
                * :attr:`input_tensor_b`

            Keyword Args:
                * :attr:`memory_config` : Memory Config of the output tensor
                * :attr:`dim` : Default value is 0
                * :attr:`queue_id`: command queue id.

            Example:
                >>> batch_id = ttnn.to_device(ttnn.from_torch(torch.tensor((1, 2), dtype=torch.UINT32)), device)
                >>> input_a = ttnn.to_device(ttnn.from_torch(torch.tensor((1, 2), dtype=torch.bfloat16)), device)
                >>> input_b = ttnn.to_device(ttnn.from_torch(torch.tensor((0, 1), dtype=torch.bfloat16)), device)
                >>> output = ttnn.indexed_fill(batch_id, tensor1, tensor2)
        )doc",
        ttnn::indexed_fill.base_name());

    using OperationType = decltype(ttnn::indexed_fill);
    ttnn::bind_registered_operation(
        module,
        ttnn::indexed_fill,
        doc,
        ttnn::pybind_overload_t{
            [] (const OperationType& self,
                const ttnn::Tensor& batch_id,
                const ttnn::Tensor& input_tensor_a,
                const ttnn::Tensor& input_tensor_b,
                const std::optional<ttnn::MemoryConfig>& memory_config,
                int64_t dim,
                uint8_t queue_id) {
                    return self(queue_id, batch_id, input_tensor_a, input_tensor_b, memory_config, dim);
                },
                pybind11::arg("batch_id").noconvert(),
                pybind11::arg("input_tensor_a").noconvert(),
                pybind11::arg("input_tensor_b").noconvert(),
                pybind11::kw_only(),
                pybind11::arg("memory_config") = std::nullopt,
                pybind11::arg("dim") = 0,
                pybind11::arg("queue_id") = 0});
}

}  // detail

} // namespace ttnn::operations::data_movement::detail