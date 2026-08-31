// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "wire/gzip.hpp"

#include "common/raii/deflate_stream.hpp"

#include <exception>
#include <limits>

namespace microtel::wire
{
namespace
{

/// Window bits 15 (32 KiB) plus 16 to select the gzip wrapper over the raw
/// zlib one — the encoding both `content-encoding` and `grpc-encoding` name.
constexpr int kGzipWindowBits = 15 + 16;
constexpr int kZlibMemLevel = 8;

/// Error strings stay within libstdc++'s 15-char SSO capacity so that
/// constructing the failure path does not itself allocate.
microtel::Error GzipError() noexcept
{
    return microtel::Error{
        .kind = microtel::Error::Kind::InternalFailure,
        .message = "gzip failed",
        .os_errno = 0,
    };
}

}  // namespace

microtel::Expected<std::vector<std::byte>, microtel::Error> GzipCompress(
    std::span<const std::byte> input) noexcept
{
    // A single deflate pass takes its lengths as uInt. Inputs are bounded by
    // max_record_bytes well below this, so the check is a guard rather than a
    // supported path.
    if (input.size() > std::numeric_limits<uInt>::max())
    {
        return microtel::make_unexpected(GzipError());
    }

    try
    {
        common::raii::DeflateStream stream;
        if (!stream.Init(Z_DEFAULT_COMPRESSION, kGzipWindowBits, kZlibMemLevel))
        {
            return microtel::make_unexpected(GzipError());
        }
        z_stream* const zs = stream.Get();

        // deflateBound is only valid after init, and is an upper bound for a
        // single Z_FINISH pass — so one allocation suffices and the loop that
        // a streaming API would need does not arise.
        const uLong bound = deflateBound(zs, static_cast<uLong>(input.size()));
        std::vector<std::byte> out(static_cast<std::size_t>(bound));

        zs->next_in = reinterpret_cast<const Bytef*>(input.data());
        zs->avail_in = static_cast<uInt>(input.size());
        zs->next_out = reinterpret_cast<Bytef*>(out.data());
        zs->avail_out = static_cast<uInt>(out.size());

        if (deflate(zs, Z_FINISH) != Z_STREAM_END)
        {
            return microtel::make_unexpected(GzipError());
        }
        out.resize(out.size() - zs->avail_out);
        return out;
    }
    catch (const std::exception&)
    {
        return microtel::make_unexpected(GzipError());
    }
}

}  // namespace microtel::wire
