// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/trace.hpp"

#include <concepts>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace microtel
{

/// @brief Forward declaration of the internal `ISampler` interface.
///
/// Defined in `include/microtel/internal/sampler.hpp`. Public consumers do
/// not see the abstract base directly; they construct samplers through the
/// factory functions below and pass the resulting `SamplerHandle` to
/// `SdkBuilder::WithSampler`.
namespace internal
{
class ISampler;
}  // namespace internal

/// @brief Opaque handle to a sampler. Move-only owning wrapper.
///
/// Constructed only via the factory functions in this header. Passed by value
/// (move) to `SdkBuilder::WithSampler`.
class SamplerHandle
{
public:
    SamplerHandle() noexcept = default;
    explicit SamplerHandle(std::unique_ptr<internal::ISampler> impl) noexcept;
    ~SamplerHandle() noexcept;

    SamplerHandle(const SamplerHandle&) = delete;
    SamplerHandle& operator=(const SamplerHandle&) = delete;

    // Defaulted, but out of line with the destructor and for the same
    // reason: defining a move operation here would instantiate
    // `~unique_ptr<internal::ISampler>`, and `ISampler` is deliberately
    // incomplete in this header. Every translation unit that holds a
    // `std::vector<SamplerHandle>` — including the `MakeChainSampler`
    // template below — needs these without seeing the internal header.
    SamplerHandle(SamplerHandle&&) noexcept;
    SamplerHandle& operator=(SamplerHandle&&) noexcept;

    /// @brief Internal accessor — used by `SdkBuilder` only.
    [[nodiscard]] internal::ISampler* Get() const noexcept;
    [[nodiscard]] std::unique_ptr<internal::ISampler> Release() noexcept;

private:
    std::unique_ptr<internal::ISampler> m_impl;
};

/// @brief Sampler that always samples (`RecordAndSample`).
[[nodiscard]] SamplerHandle MakeAlwaysOnSampler();

/// @brief Sampler that always drops (`Drop`).
[[nodiscard]] SamplerHandle MakeAlwaysOffSampler();

/// @brief TraceId-ratio sampler.
///
/// Samples a fraction of traces based on the trace ID, deterministically.
/// `ratio` is clamped to `[0.0, 1.0]`; NaN is normalised to `0.0` and so
/// samples nothing (#247). `Provider::SetSamplerRatio` rejects NaN and
/// out-of-range values rather than clamping them — at reload time a bad
/// ratio is an operator-visible bug, at build time it is a documented
/// convenience (ICP 0026 Decision 3).
///
/// @param ratio fraction of traces to sample.
[[nodiscard]] SamplerHandle MakeTraceIdRatioSampler(double ratio);

/// @brief Parent-based sampler.
///
/// If the parent context exists and is sampled, samples; otherwise falls back
/// to `root`. The four standard variants are constructed by passing
/// `MakeAlwaysOnSampler()`, `MakeAlwaysOffSampler()`, `MakeTraceIdRatioSampler(r)`,
/// or another sampler as `root`.
[[nodiscard]] SamplerHandle MakeParentBasedSampler(SamplerHandle root);

// --- Rule combinators ---------------------------------------------------
//
// A rule combinator is a predicate over what a head sampler can see when
// `ShouldSample` is called — the span's initial attributes, its name, its
// kind — plus two delegates: one consulted on a match, one on a miss. That
// is `MakeParentBasedSampler`'s shape (a predicate on the parent context,
// delegating to `root` when it does not hold) with a predicate the caller
// chooses.
//
// There is deliberately no sample-on-duration rule. `ShouldSample` runs at
// span start, before the span has a duration; selecting spans by how long
// they took is tail sampling and belongs in the collector. See
// `docs/icps/0024-v1.1-rescope.md`.
//
// Every combinator evaluates its predicate without allocating, and composes
// its description string once, at construction.

/// @brief Samples according to whether an initial attribute matches.
///
/// Matches when `ctx.initial_attributes` carries `key` **and** its value
/// compares equal to `expected_value` — equality is over the whole
/// `AttributeValue`, so the same key holding a different alternative is a
/// miss. An absent key is a miss.
///
/// @param key attribute key to look for. Compared exactly.
/// @param expected_value value the attribute must hold to match.
/// @param on_match sampler consulted when the attribute matches.
/// @param on_no_match sampler consulted when it does not. Pass
///        `MakeAlwaysOffSampler()` for "drop everything else".
[[nodiscard]] SamplerHandle MakeAttributeRuleSampler(std::string key,
                                                     AttributeValue expected_value,
                                                     SamplerHandle on_match,
                                                     SamplerHandle on_no_match);

/// @brief Samples according to the span name.
///
/// Matches on exact equality with `ctx.span_name`; there is no prefix,
/// glob, or regex form in v1.1.
///
/// @param name span name to match exactly.
/// @param on_match sampler consulted when the name matches.
/// @param on_no_match sampler consulted when it does not.
[[nodiscard]] SamplerHandle MakeSpanNameRuleSampler(std::string name,
                                                    SamplerHandle on_match,
                                                    SamplerHandle on_no_match);

/// @brief Samples according to the span kind.
///
/// @param kind span kind to match.
/// @param on_match sampler consulted when the kind matches.
/// @param on_no_match sampler consulted when it does not.
[[nodiscard]] SamplerHandle MakeSpanKindRuleSampler(SpanKind kind,
                                                    SamplerHandle on_match,
                                                    SamplerHandle on_no_match);

// --- Chain composition --------------------------------------------------

/// @brief How a chain combines its children.
enum class ChainMode : std::uint8_t
{
    /// @brief The first child whose predicate matches decides.
    ///
    /// Children are walked in order. A rule combinator is consulted for its
    /// predicate; the first one that matches decides the whole chain, and
    /// nothing after it is evaluated. A child that is **not** a rule
    /// combinator carries no predicate and so matches unconditionally — that
    /// is how a chain spells its default, and it terminates the walk.
    ///
    /// A rule whose predicate does not match is skipped whole: its own
    /// no-match delegate is **not** consulted, because in a chain the rest
    /// of the chain is the no-match path. If no child matches, the result is
    /// `Drop`.
    FirstMatch = 0,

    /// @brief Every child must return `RecordAndSample`.
    ///
    /// Children are walked in order and asked for a decision — a rule
    /// combinator therefore answers through whichever delegate its predicate
    /// selects. The first child that returns anything other than
    /// `RecordAndSample` ends the walk with `Drop`; nothing after it is
    /// evaluated. When every child agrees, the last child's result is
    /// returned, so its additional attributes and trace state survive.
    AllMustAgree = 1,
};

/// @brief Composes samplers into a chain.
///
/// Children are sized once here and never resized; evaluation short-circuits
/// and allocates nothing. An empty chain returns `Drop` under either mode, as
/// does a chain whose children are all empty handles — an empty
/// `SamplerHandle` is a contract violation and is discarded at construction
/// rather than dereferenced inside a `noexcept` hot path.
///
/// `SamplerHandle` is move-only, so there is no `initializer_list` form; pass
/// a vector, or use the variadic overload below.
///
/// @param children samplers to compose, in evaluation order.
/// @param mode how their answers combine.
[[nodiscard]] SamplerHandle MakeChainSampler(std::vector<SamplerHandle> children, ChainMode mode);

/// @brief Variadic convenience for `MakeChainSampler`.
///
/// Forwards its handles into a right-sized vector and calls the overload
/// above. `mode` comes first so the handles can form the parameter pack.
///
/// @param mode how the children's answers combine.
/// @param first the first child; at least one is required.
/// @param rest any further children, in evaluation order.
template <typename... Rest>
    requires(std::same_as<std::remove_cvref_t<Rest>, SamplerHandle> && ...)
[[nodiscard]] SamplerHandle MakeChainSampler(ChainMode mode, SamplerHandle first, Rest&&... rest)
{
    std::vector<SamplerHandle> children;
    children.reserve(1 + sizeof...(Rest));
    children.push_back(std::move(first));
    (children.push_back(std::forward<Rest>(rest)), ...);
    return MakeChainSampler(std::move(children), mode);
}

}  // namespace microtel
