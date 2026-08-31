// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace microtel::wire
{

/// @brief gzip-compress @p input in one shot.
///
/// Produces a gzip stream (RFC 1952 wrapper, not a bare zlib/deflate stream)
/// suitable for both the HTTP `content-encoding: gzip` body and the gRPC
/// `grpc-encoding: gzip` message payload.
///
/// @param input bytes to compress; may be empty.
/// @return the compressed bytes, or an `Error` when zlib fails or the input
///         exceeds what a single zlib pass accepts.
/// @note `noexcept`: called from exporter worker threads on the send path.
///       Allocation failure is caught and reported as an `Error` rather than
///       escaping into a `noexcept` frame.
[[nodiscard]] microtel::Expected<std::vector<std::byte>, microtel::Error> GzipCompress(
    std::span<const std::byte> input) noexcept;

}  // namespace microtel::wire
