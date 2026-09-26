// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The differential check between the leaf's two encoder backends
// (docs/leaf-concentrator-design.md §7.2), shared by the
// leaf_backend_diff_fuzz target and microtel_leaf_backend_diff_test.
//
// Both link `microtel_leaf_dual` (tests/leaf/dual/): the leaf core with the
// upb and the nanopb backends behind a run-time switch. A program is any byte
// string, read as a leaf configuration followed by builder calls. It is run on
// two leaves with identical inputs; at every encode the first leaf is encoded
// with upb and the second with nanopb, and the statuses, sizes and bytes must
// match. Every successful payload must also decode with upb.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace microtel::leaf_diff
{

/// The result of one comparison. `mismatch` is empty when the backends agreed.
struct Outcome
{
    std::string mismatch;
    std::size_t encodes = 0;  ///< encodes compared (successful or not)
};

/// Runs `data` as a builder program on two leaves and compares every encode.
Outcome RunProgram(const std::uint8_t* data, std::size_t size);

/// Encodes golden vector `index` (tests/leaf/vectors/) with both backends and
/// compares the bytes.
Outcome CompareVector(std::size_t index);

}  // namespace microtel::leaf_diff
