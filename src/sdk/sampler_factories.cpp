// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Sampler factories and the public `microtel::SamplerHandle` out-of-line
// definitions. The destructor must be defined here (not inline in the
// public header) because it requires the full definition of
// `internal::ISampler`, which is intentionally not exposed in
// include/microtel/sampler.hpp.
//
// All four built-in samplers (AlwaysOn, AlwaysOff, TraceIdRatio,
// ParentBased) are implemented in this file.

#include "microtel/internal/sampler.hpp"
#include "microtel/sampler.hpp"
#include "microtel/trace.hpp"

#include "sdk/sampler_description.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace microtel
{

// --- SamplerHandle out-of-line definitions ------------------------------

SamplerHandle::SamplerHandle(std::unique_ptr<internal::ISampler> impl) noexcept
    : m_impl(std::move(impl))
{
}

SamplerHandle::~SamplerHandle() noexcept = default;

// Defaulted here rather than in the header: defining a move operation
// inline would instantiate `~unique_ptr<internal::ISampler>` in every
// translation unit that sees the declaration, and `ISampler` is incomplete
// in the public header by design.
SamplerHandle::SamplerHandle(SamplerHandle&&) noexcept = default;

SamplerHandle& SamplerHandle::operator=(SamplerHandle&&) noexcept = default;

internal::ISampler* SamplerHandle::Get() const noexcept
{
    return m_impl.get();
}

std::unique_ptr<internal::ISampler> SamplerHandle::Release() noexcept
{
    return std::move(m_impl);
}

// --- AlwaysOn sampler ---------------------------------------------------

namespace
{

/// @brief Sampler that returns `RecordAndSample` for every span.
///
/// Stateless, allocation-free on the hot path (the `SamplingResult`'s
/// vector + optional members are default-constructed and remain empty,
/// so no heap allocation occurs).
class AlwaysOnSampler final : public internal::ISampler
{
public:
    [[nodiscard]] internal::SamplingResult ShouldSample(
        const internal::SamplingContext& /*ctx*/) const noexcept override
    {
        return internal::SamplingResult{
            .decision = internal::SamplingDecision::RecordAndSample,
            .additional_attributes = {},
            .trace_state = std::nullopt,
        };
    }

    [[nodiscard]] std::string_view Description() const noexcept override
    {
        return "AlwaysOnSampler";
    }
};

}  // namespace

SamplerHandle MakeAlwaysOnSampler()
{
    return SamplerHandle{std::make_unique<AlwaysOnSampler>()};
}

// --- AlwaysOff sampler --------------------------------------------------

namespace
{

/// @brief Sampler that returns `Drop` for every span. Stateless and
/// allocation-free on the hot path.
class AlwaysOffSampler final : public internal::ISampler
{
public:
    [[nodiscard]] internal::SamplingResult ShouldSample(
        const internal::SamplingContext& /*ctx*/) const noexcept override
    {
        return internal::SamplingResult{
            .decision = internal::SamplingDecision::Drop,
            .additional_attributes = {},
            .trace_state = std::nullopt,
        };
    }

    [[nodiscard]] std::string_view Description() const noexcept override
    {
        return "AlwaysOffSampler";
    }
};

}  // namespace

SamplerHandle MakeAlwaysOffSampler()
{
    return SamplerHandle{std::make_unique<AlwaysOffSampler>()};
}

// --- TraceIdRatio sampler -----------------------------------------------

namespace
{

/// @brief Deterministic ratio sampler.
///
/// Reads the lower 8 bytes of the 16-byte trace id as a big-endian
/// `std::uint64_t`. Samples when that value is below
/// `ratio * UINT64_MAX`.
///
/// Boundary handling:
/// - `ratio >= 1.0`: always samples (avoids the UB of casting `2^64` back
///   to `std::uint64_t` after the double-precision multiplication rounds
///   up).
/// - `ratio <= 0.0`: never samples.
/// - `ratio` outside `[0, 1]` is clamped to that range.
/// - `ratio` is NaN: normalised to `0.0`, so it never samples (#247).
///   `std::clamp` passes NaN through — neither comparison is true — and
///   both `ComputeThreshold` guards then fell through to a cast of a
///   non-representable value, undefined behaviour per [conv.fpint]. The
///   factory returns a handle rather than an `Expected`, so rejection is
///   not available to it; `docs/interfaces.md` §4.5 names "fall back to
///   drop" as the recovery for a sampler anomaly, which picks `0.0` over
///   `1.0`. `Provider::SetSamplerRatio` rejects NaN instead — the
///   asymmetry is deliberate, per ICP 0026 Decision 3.
///
/// Description string includes the resolved ratio at three decimals,
/// matching the substring assertion in
/// `tests/unit/sdk/trace_id_ratio_sampler_test.cpp`.
///
/// **Retunable in place** (ICP 0026 §5). `m_always_sample` used to be a
/// separate `bool` beside `m_threshold`; two atomics could be read torn, so it
/// is folded into the threshold instead: `ComputeThreshold` returns
/// `UINT64_MAX` if and only if `ratio >= 1.0` — for any ratio below 1.0 the
/// largest product is `2^64 - 2^11`, strictly smaller and representable — so
/// the sentinel test is exact and the decision is one relaxed load. Decisions
/// are bit-for-bit what they were before the fold, and nothing on the hot path
/// allocates or locks (`docs/memory-model.md` §8.1, §4.5 LOCKED).
class TraceIdRatioSampler final : public internal::ISampler
{
public:
    explicit TraceIdRatioSampler(double ratio)
        : m_ratio(NormaliseRatio(ratio)),
          m_threshold(ComputeThreshold(m_ratio)),
          m_description(std::format("TraceIdRatioSampler{{{:.3f}}}", m_ratio))
    {
    }

    [[nodiscard]] internal::SamplingResult ShouldSample(
        const internal::SamplingContext& ctx) const noexcept override
    {
        const auto decision = SampleDecision(ctx);
        return internal::SamplingResult{
            .decision = decision,
            .additional_attributes = {},
            .trace_state = std::nullopt,
        };
    }

    [[nodiscard]] std::string_view Description() const noexcept override
    {
        return m_description.Get();
    }

    [[nodiscard]] bool TrySetRatio(double ratio) noexcept override
    {
        const std::scoped_lock lock{m_ratio_mu};
        if (ratio == m_ratio)
        {
            return true;  // nothing moved, so nothing is appended
        }
        m_ratio = ratio;
        m_threshold.store(ComputeThreshold(ratio), std::memory_order_relaxed);
        m_description.Publish([ratio]
                              { return std::format("TraceIdRatioSampler{{{:.3f}}}", ratio); });
        return true;
    }

private:
    [[nodiscard]] internal::SamplingDecision SampleDecision(
        const internal::SamplingContext& ctx) const noexcept
    {
        const std::uint64_t threshold = m_threshold.load(std::memory_order_relaxed);
        if (threshold == std::numeric_limits<std::uint64_t>::max())
        {
            return internal::SamplingDecision::RecordAndSample;
        }
        const std::uint64_t low = ReadLowerBE64(ctx.trace_id);
        return (low < threshold) ? internal::SamplingDecision::RecordAndSample
                                 : internal::SamplingDecision::Drop;
    }

    static std::uint64_t ReadLowerBE64(const TraceId& tid) noexcept
    {
        const auto& bytes = tid.AsBytes();
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < 8; ++i)
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
            v = (v << 8U) | static_cast<std::uint64_t>(bytes[8U + i]);
        }
        return v;
    }

    /// @brief Clamps into `[0.0, 1.0]`, mapping NaN to `0.0`.
    ///
    /// `std::clamp` alone is not enough: NaN compares false against both
    /// bounds and is returned unchanged (#247).
    static double NormaliseRatio(double ratio) noexcept
    {
        if (std::isnan(ratio))
        {
            return 0.0;
        }
        return std::clamp(ratio, 0.0, 1.0);
    }

    static std::uint64_t ComputeThreshold(double ratio) noexcept
    {
        if (ratio >= 1.0)
        {
            return std::numeric_limits<std::uint64_t>::max();
        }
        if (ratio <= 0.0)
        {
            return 0;
        }
        const auto max_d = static_cast<double>(std::numeric_limits<std::uint64_t>::max());
        return static_cast<std::uint64_t>(ratio * max_d);
    }

    /// Taken only by `TrySetRatio`. Guards `m_ratio` (the same-value check)
    /// and orders the threshold store against the description publish, so two
    /// concurrent retunes cannot leave the two disagreeing. The only lock
    /// acquired while it is held is `m_description`'s own leaf.
    std::mutex m_ratio_mu;
    double m_ratio;
    /// The whole hot-path state: `UINT64_MAX` means always sample.
    std::atomic<std::uint64_t> m_threshold;
    sdk::DescriptionSlot m_description;
};

}  // namespace

SamplerHandle MakeTraceIdRatioSampler(double ratio)
{
    return SamplerHandle{std::make_unique<TraceIdRatioSampler>(ratio)};
}

// --- ParentBased sampler ------------------------------------------------

namespace
{

/// @brief Composite sampler that delegates based on the parent context.
///
/// v1 simplification (per `include/microtel/sampler.hpp`): if the parent
/// context is valid (non-zero TraceId + SpanId) and marked sampled, the
/// result is `RecordAndSample` regardless of the root sampler's view.
/// Otherwise the decision is delegated to the configured `root` sampler.
///
/// Description includes the root sampler's description so traces of
/// `ParentBasedSampler{AlwaysOnSampler}` etc. are self-describing.
///
/// `root` ownership is moved into the sampler at construction; the root's
/// lifetime is tied to this sampler's. Passing an empty `SamplerHandle`
/// (one whose `Get()` returns `nullptr`) is a contract violation —
/// production code does not pass empty handles.
class ParentBasedSampler final : public internal::ISampler
{
public:
    explicit ParentBasedSampler(SamplerHandle root)
        : m_root(std::move(root)), m_description(Compose(m_root))
    {
    }

    [[nodiscard]] internal::SamplingResult ShouldSample(
        const internal::SamplingContext& ctx) const noexcept override
    {
        if (ctx.parent.IsValid() && ctx.parent.trace_flags.IsSampled())
        {
            return internal::SamplingResult{
                .decision = internal::SamplingDecision::RecordAndSample,
                .additional_attributes = {},
                .trace_state = std::nullopt,
            };
        }
        return m_root.Get()->ShouldSample(ctx);
    }

    [[nodiscard]] std::string_view Description() const noexcept override
    {
        return m_description.Get();
    }

    /// `parentbased_traceidratio` is the deployment shape an operator most
    /// wants to retune, so the composite forwards. A `false` from the root is
    /// a `false` from the chain: nothing changes anywhere and the caller is
    /// told (ICP 0026 §5).
    [[nodiscard]] bool TrySetRatio(double ratio) noexcept override
    {
        if (m_root.Get() == nullptr || !m_root.Get()->TrySetRatio(ratio))
        {
            return false;
        }
        // Recomposed *after* the forward, never around it: holding this
        // sampler's lock across the call into the root would nest two setter
        // locks, which `docs/threading-model.md` §4 rule 2 forbids.
        m_description.Publish([this] { return Compose(m_root); });
        return true;
    }

private:
    static std::string Compose(const SamplerHandle& root)
    {
        const std::string_view root_desc =
            root.Get() != nullptr ? root.Get()->Description() : std::string_view{"<null>"};
        return std::format("ParentBasedSampler{{{}}}", root_desc);
    }

    SamplerHandle m_root;
    sdk::DescriptionSlot m_description;
};

}  // namespace

SamplerHandle MakeParentBasedSampler(SamplerHandle root)
{
    return SamplerHandle{std::make_unique<ParentBasedSampler>(std::move(root))};
}

}  // namespace microtel
