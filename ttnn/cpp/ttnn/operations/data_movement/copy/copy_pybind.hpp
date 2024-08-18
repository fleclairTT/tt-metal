// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include "pybind11/pybind_fwd.hpp"

namespace ttnn::operations::data_movement::detail {

std::string get_binary_doc_string(std::string op_name, std::string op_desc);
std::string get_unary_doc_string(std::string op_name, std::string op_desc);
void py_bind_copy(pybind11::module& m);
void py_bind_clone(pybind11::module& m);
void py_bind_assign(pybind11::module& m);

}  // namespace ttnn::operations::data_movement::detail