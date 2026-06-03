// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

// Fuzz harness for the ExportTraceServiceResponse proto parser.
//
// Exercises: varint decoding (including overlong / truncated varints),
// length-delimited field extraction with oversized length prefixes, field
// skipping for unknown wire types (0=varint, 1=64-bit, 2=len-delim,
// 5=32-bit, and the reserved 3/4 cases), and the partial_success →
// rejected_spans path including uint64→uint32 capping.

#include "wire/otlp_response.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const std::span<const std::byte> body{reinterpret_cast<const std::byte*>(data), size};
    (void)microtel::wire::ParseRejectedSpans(body);
    return 0;
}
