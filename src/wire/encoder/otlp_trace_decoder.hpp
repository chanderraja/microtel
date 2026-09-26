// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/expected.hpp"
#include "microtel/internal/otlp_trace_decoder.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace microtel::wire
{

/// @brief The upb implementation of `IOtlpTraceDecoder`, behind the leaf
///        receiver (`docs/leaf-concentrator-design.md` §3.4).
///
/// The only upb user in the runtime besides `OtlpEncoder`, and in this
/// directory for the same reason (ICP 0031 Decision 5). No upb type appears
/// here. Each `Decode` builds one arena over a counting allocator that refuses
/// to grow past `DecodeLimits::max_arena_bytes`, runs `upb_Decode` with
/// `max_depth`, copies what it needs into plain C++ values, and frees the arena
/// before returning (`docs/memory-model.md` §3.1). Strings are copied, never
/// aliased (`kUpb_DecodeOption_AliasString` is not used).
///
/// Only compiled with `MICROTEL_WITH_CONCENTRATOR=ON` (design §6.2).
///
/// @threadsafety Thread-safe: stateless, `Decode` is `const` — unless built
///               with an `ArenaStats` observer, which every call writes.
class OtlpTraceDecoder final : public internal::IOtlpTraceDecoder
{
public:
    /// @brief What one `Decode` call's arena cost, for tests and the fuzzer
    ///        (design §7.3: "the decode arena never exceeds its cap").
    struct ArenaStats
    {
        std::size_t used = 0;  ///< bytes the counting allocator handed out
        std::size_t cap = 0;   ///< the `max_arena_bytes` the call was given
    };

    OtlpTraceDecoder() noexcept = default;

    /// @brief A decoder that reports each call's arena use.
    /// @param observe borrowed; overwritten at the end of every `Decode`.
    ///                Must outlive the decoder. With it set the decoder is no
    ///                longer safe to call from several threads at once.
    explicit OtlpTraceDecoder(ArenaStats* observe) noexcept;

    ~OtlpTraceDecoder() noexcept override = default;

    OtlpTraceDecoder(const OtlpTraceDecoder&) = delete;
    OtlpTraceDecoder& operator=(const OtlpTraceDecoder&) = delete;
    OtlpTraceDecoder(OtlpTraceDecoder&&) = delete;
    OtlpTraceDecoder& operator=(OtlpTraceDecoder&&) = delete;

    [[nodiscard]] Expected<std::vector<internal::DecodedResourceSpans>, internal::DecodeFailure>
    Decode(std::span<const std::byte> payload, const internal::DecodeLimits& limits) const override;

private:
    ArenaStats* m_observe = nullptr;
};

}  // namespace microtel::wire
