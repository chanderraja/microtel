// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace microtel
{

/// @brief OTLP wire protocol selector.
///
/// One transport is opened per `(endpoint, protocol)` tuple per
/// `microtel-spec.md` §5.2. The choice is fixed at `Provider::Build` time;
/// there is no runtime switching in v1.
enum class Protocol : std::uint8_t
{
    /// @brief OTLP/HTTP-protobuf — `application/x-protobuf` over HTTP/2,
    /// default port 4318.
    Http = 0,

    /// @brief OTLP/gRPC over nghttp2 (no gRPC library), default port 4317.
    Grpc = 1,
};

}  // namespace microtel
