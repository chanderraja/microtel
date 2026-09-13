// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace microtel::wire
{

/// @brief Default ceiling on decompressed response bytes.
///
/// Mirrors `MemoryLimitOptions::max_decompressed_bytes` so a codec constructed
/// without an explicit limit behaves like one the `SdkBuilder` configured.
inline constexpr std::uint32_t kDefaultMaxDecompressedBytes = 4U * 1024U * 1024U;

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

/// @brief Why a `GzipDecompress` call produced no bytes.
///
/// Deliberately not `microtel::Error`: the caller has to tell the two apart to
/// pick a drop counter (`decompression_too_large` vs `malformed_response`) and
/// `Error::Kind` carries no decompression-specific member — adding one would
/// churn a public enum for a distinction only this call site makes.
enum class GzipDecompressError : std::uint8_t
{
    /// Not a valid gzip stream: bad magic, truncated, failed CRC, or trailing
    /// bytes after the stream's end.
    Corrupt = 0,
    /// The stream inflates to more than `max_output` bytes. Decompression
    /// stops at the ceiling; the remaining input is never expanded.
    TooLarge = 1,
};

/// @brief Inflate a gzip stream under a hard output ceiling.
///
/// The decompression-bomb guard for response bodies (`content-encoding: gzip`
/// on OTLP/HTTP, the per-message `CF=0x01` flag on OTLP/gRPC). Output grows in
/// chunks and never exceeds @p max_output by more than the single byte needed
/// to notice the overrun, so a 4 KiB stream claiming to be 8 GiB costs a 4 KiB
/// buffer and an early return, not 8 GiB of address space.
///
/// @param input the gzip bytes; the whole stream and nothing after it.
/// @param max_output ceiling on the decompressed size, inclusive — output of
///        exactly this many bytes is accepted.
/// @return the decompressed bytes, or the reason there are none.
/// @note `noexcept`: called from exporter worker threads on the response path.
///       Allocation failure is reported as `Corrupt` rather than escaping into
///       a `noexcept` frame.
[[nodiscard]] microtel::Expected<std::vector<std::byte>, GzipDecompressError> GzipDecompress(
    std::span<const std::byte> input, std::size_t max_output) noexcept;

}  // namespace microtel::wire
