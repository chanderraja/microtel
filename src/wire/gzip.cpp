// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "wire/gzip.hpp"

#include "common/raii/deflate_stream.hpp"
#include "common/raii/inflate_stream.hpp"

#include <algorithm>
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

/// First output allocation, and the floor for every growth step after it.
/// Small enough that the common case — an empty or few-hundred-byte OTLP
/// response — costs one page, large enough that a real body rarely needs a
/// second round.
constexpr std::size_t kInitialOutputChunk = std::size_t{8} * 1024U;

/// @brief The working ceiling: one byte past the caller's, so that filling the
///        buffer is itself proof the stream exceeds `max_output`.
[[nodiscard]] constexpr std::size_t HardLimit(std::size_t max_output) noexcept
{
    const auto size_max = std::numeric_limits<std::size_t>::max();
    return (max_output == size_max) ? size_max : max_output + 1U;
}

/// @brief Enlarge @p out towards @p hard_limit.
/// @return false when @p out already sits at the limit — the caller's signal
///         that the stream is too large, since it still has bytes to write.
[[nodiscard]] bool Grow(std::vector<std::byte>& out, std::size_t hard_limit)
{
    if (out.size() >= hard_limit)
    {
        return false;
    }
    const std::size_t doubled = (out.size() > hard_limit / 2U) ? hard_limit : out.size() * 2U;
    out.resize(std::min(hard_limit, std::max(kInitialOutputChunk, doubled)));
    return true;
}

/// @brief One `inflate` pass into the unwritten tail of @p out.
/// @param produced in/out: bytes written so far, advanced by this call.
[[nodiscard]] int InflateChunk(z_stream* zs, std::vector<std::byte>& out, std::size_t& produced)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    zs->next_out = reinterpret_cast<Bytef*>(out.data() + produced);
    zs->avail_out = static_cast<uInt>(out.size() - produced);
    const int rc = inflate(zs, Z_NO_FLUSH);
    produced = out.size() - zs->avail_out;
    return rc;
}

/// Why the inflate loop stopped.
struct InflateOutcome
{
    int zlib_rc{Z_OK};
    /// Set when the loop hit the hard limit with the stream still unfinished.
    bool too_large{false};
};

/// @brief Inflate until the stream ends, zlib objects, or the limit is hit.
[[nodiscard]] InflateOutcome RunInflate(z_stream* zs,
                                        std::vector<std::byte>& out,
                                        std::size_t hard_limit,
                                        std::size_t& produced)
{
    int rc = Z_OK;
    while (rc != Z_STREAM_END)
    {
        if (produced == out.size() && !Grow(out, hard_limit))
        {
            return InflateOutcome{.zlib_rc = rc, .too_large = true};
        }
        rc = InflateChunk(zs, out, produced);
        if (rc != Z_OK && rc != Z_STREAM_END)
        {
            // Z_BUF_ERROR lands here too: with the whole input already handed
            // over, "no progress possible" means the stream is truncated.
            return InflateOutcome{.zlib_rc = rc, .too_large = false};
        }
    }
    return InflateOutcome{.zlib_rc = rc, .too_large = false};
}

}  // namespace

microtel::Expected<std::vector<std::byte>, microtel::Error> GzipCompress(
    std::span<const std::byte> input) noexcept
{
    // A single deflate pass takes its lengths as uInt. A batch is bounded well
    // below this — `max_export_batch_size` records, each held to
    // `max_record_bytes` by `BatchSpanProcessor::OnEnd` — so the check is a
    // guard rather than a supported path.
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

microtel::Expected<std::vector<std::byte>, GzipDecompressError> GzipDecompress(
    std::span<const std::byte> input, std::size_t max_output) noexcept
{
    // A single inflate pass takes its input length as uInt. A response that
    // large never reaches here: the transport stops buffering a response body
    // at `ConnectOptions::max_response_bytes` (1 MiB by default) and fails the
    // request, so this is a guard, not a supported path.
    if (input.size() > std::numeric_limits<uInt>::max())
    {
        return microtel::make_unexpected(GzipDecompressError::Corrupt);
    }

    try
    {
        common::raii::InflateStream stream;
        if (!stream.Init(kGzipWindowBits))
        {
            return microtel::make_unexpected(GzipDecompressError::Corrupt);
        }
        z_stream* const zs = stream.Get();
        zs->next_in = reinterpret_cast<const Bytef*>(input.data());
        zs->avail_in = static_cast<uInt>(input.size());

        std::vector<std::byte> out;
        std::size_t produced = 0;
        const auto outcome = RunInflate(zs, out, HardLimit(max_output), produced);

        if (outcome.too_large || produced > max_output)
        {
            return microtel::make_unexpected(GzipDecompressError::TooLarge);
        }
        if (outcome.zlib_rc != Z_STREAM_END)
        {
            return microtel::make_unexpected(GzipDecompressError::Corrupt);
        }
        // Bytes after the gzip trailer are not part of this stream. Silently
        // dropping them would let a peer hide payload from the size
        // accounting, so the whole response is refused instead.
        if (zs->avail_in != 0U)
        {
            return microtel::make_unexpected(GzipDecompressError::Corrupt);
        }
        out.resize(produced);
        return out;
    }
    catch (const std::exception&)
    {
        return microtel::make_unexpected(GzipDecompressError::Corrupt);
    }
}

}  // namespace microtel::wire
