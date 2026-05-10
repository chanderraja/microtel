// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace microtel::wire
{

/// @brief Parse the `partial_success.rejected_spans` field from an
///        `ExportTraceServiceResponse` protobuf body.
///
/// Used by both `HttpWireCodec` (raw body) and `GrpcWireCodec` (body after
/// stripping the 5-byte gRPC DATA frame prefix). Returns 0 on any parse
/// error or when `partial_success` is absent.
[[nodiscard]] std::uint32_t ParseRejectedSpans(std::span<const std::byte> body) noexcept;

}  // namespace microtel::wire
