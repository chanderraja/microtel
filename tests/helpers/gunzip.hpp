// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Test-only gzip decompression. Production code compresses but never
// decompresses (no `accept-encoding` is advertised), so this lives in the test
// tree rather than in src/.

#pragma once

// Makes z_stream::next_in a `const Bytef*`, so a borrowed span needs no
// const_cast. Must precede <zlib.h>.
#ifndef ZLIB_CONST
#define ZLIB_CONST
#endif

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <zlib.h>

namespace microtel::testing
{

/// @brief Inflates a gzip stream produced by `microtel::wire::GzipCompress`.
/// @param input the gzip bytes.
/// @param max_output ceiling on the decompressed size.
/// @return the decompressed bytes, or `nullopt` if @p input is not a valid
///         gzip stream or would exceed @p max_output.
inline std::optional<std::vector<std::byte>> Gunzip(std::span<const std::byte> input,
                                                    std::size_t max_output = 1U << 20U)
{
    constexpr int kGzipWindowBits = 15 + 16;

    z_stream zs{};
    if (inflateInit2(&zs, kGzipWindowBits) != Z_OK)
    {
        return std::nullopt;
    }

    std::vector<std::byte> out(max_output);
    zs.next_in = reinterpret_cast<const Bytef*>(input.data());
    zs.avail_in = static_cast<uInt>(input.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());

    const int rc = inflate(&zs, Z_FINISH);
    const uInt remaining = zs.avail_out;
    static_cast<void>(inflateEnd(&zs));

    if (rc != Z_STREAM_END)
    {
        return std::nullopt;
    }
    out.resize(out.size() - remaining);
    return out;
}

/// @brief Convenience overload for comparing against string literals.
inline std::optional<std::string> GunzipToString(std::span<const std::byte> input)
{
    auto bytes = Gunzip(input);
    if (!bytes)
    {
        return std::nullopt;
    }
    std::string s;
    s.reserve(bytes->size());
    for (const std::byte b : *bytes)
    {
        s.push_back(static_cast<char>(b));
    }
    return s;
}

}  // namespace microtel::testing
