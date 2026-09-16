// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The v1.1 sampler rule combinators and the two chain composition modes
// declared in include/microtel/sampler.hpp. They live beside, not inside,
// sampler_factories.cpp: that file holds the four v1.0 built-ins and the
// SamplerHandle out-of-line definitions, and these are a separable addition.
//
// The shape is ParentBasedSampler's, generalised: a predicate over the
// SamplingContext plus a delegate for each answer. Only what a head sampler
// can see at StartSpan time is available to a predicate -- initial
// attributes, span name, span kind. Duration is not: the span has not run.
// See docs/icps/0024-v1.1-rescope.md.
//
// The LOCKED ISampler contract (docs/interfaces.md 4.5) says ShouldSample is
// noexcept, thread-safe, and does not allocate on the hot path. Everything
// here is therefore fixed at construction: the children vector is sized once
// and never resized, each child's dynamic type is resolved once, and the
// composed description string is formatted once. What remains at call time is
// a short-circuiting walk over a vector of pointers.

#include "microtel/attribute.hpp"
#include "microtel/internal/sampler.hpp"
#include "microtel/sampler.hpp"
#include "microtel/trace.hpp"

#include "sdk/sampler_description.hpp"

#include <algorithm>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microtel
{

namespace
{

/// @brief The `Drop` result, with no attributes and no trace state.
///
/// An empty `std::vector` and a disengaged `std::optional` do not allocate,
/// which is what makes this usable on the hot path.
internal::SamplingResult DropResult() noexcept
{
    return internal::SamplingResult{
        .decision = internal::SamplingDecision::Drop,
        .additional_attributes = {},
        .trace_state = std::nullopt,
    };
}

/// @brief Description of a handle, or `<null>` for an empty one.
std::string_view DescriptionOf(const SamplerHandle& handle) noexcept
{
    if (handle.Get() == nullptr)
    {
        return "<null>";
    }
    return handle.Get()->Description();
}

/// @brief Forward a validated ratio to one delegate, tolerating an empty
/// handle. `false` means this delegate has no ratio to retune (ICP 0026 §5).
bool TryDelegate(const SamplerHandle& handle, double ratio) noexcept
{
    return handle.Get() != nullptr && handle.Get()->TrySetRatio(ratio);
}

/// @brief Name of a span kind, for description strings.
std::string_view SpanKindName(SpanKind kind) noexcept
{
    switch (kind)
    {
        case SpanKind::Internal:
            return "Internal";
        case SpanKind::Server:
            return "Server";
        case SpanKind::Client:
            return "Client";
        case SpanKind::Producer:
            return "Producer";
        case SpanKind::Consumer:
            return "Consumer";
    }
    return "Unknown";
}

/// @brief Name of a chain mode, for description strings.
std::string_view ChainModeName(ChainMode mode) noexcept
{
    if (mode == ChainMode::FirstMatch)
    {
        return "FirstMatch";
    }
    return "AllMustAgree";
}

/// @brief Base for the rule combinators: one predicate, two delegates.
///
/// `ShouldSample` evaluates the predicate and forwards to whichever delegate
/// it selects. `Matches` is also read directly by `ChainSampler` in
/// `FirstMatch` mode, which is why it is part of this type rather than a
/// private detail of each rule.
///
/// An empty delegate handle is a contract violation; rather than dereference
/// a null pointer inside a `noexcept` frame, such a delegate answers `Drop`.
class RuleSampler : public internal::ISampler
{
public:
    /// @param name the rule's type name, e.g. `SpanKindRuleSampler`.
    /// @param predicate the predicate, rendered, e.g. `kind=Server`.
    /// @param on_match sampler consulted when the predicate holds.
    /// @param on_no_match sampler consulted when it does not.
    RuleSampler(std::string_view name,
                std::string_view predicate,
                SamplerHandle on_match,
                SamplerHandle on_no_match)
        : m_on_match(std::move(on_match)),
          m_on_no_match(std::move(on_no_match)),
          m_name(name),
          m_predicate(predicate),
          m_description(Compose(m_name, m_predicate, m_on_match, m_on_no_match))
    {
    }

    [[nodiscard]] internal::SamplingResult ShouldSample(
        const internal::SamplingContext& ctx) const noexcept override
    {
        const SamplerHandle& delegate = Matches(ctx) ? m_on_match : m_on_no_match;
        if (delegate.Get() == nullptr)
        {
            return DropResult();
        }
        return delegate.Get()->ShouldSample(ctx);
    }

    [[nodiscard]] std::string_view Description() const noexcept override
    {
        return m_description.Get();
    }

    /// A rule owns two delegates and forwards to both, because a ratio sampler
    /// may sit on either arm. Success on either is success for the rule, and
    /// the rule then regenerates its own description — ICP 0026 §5's
    /// "sampler chains inherit the obligation".
    [[nodiscard]] bool TrySetRatio(double ratio) noexcept override
    {
        bool applied = TryDelegate(m_on_match, ratio);
        applied = TryDelegate(m_on_no_match, ratio) || applied;
        if (!applied)
        {
            return false;
        }
        return Republish();
    }

    /// @brief Whether this rule's predicate holds for `ctx`.
    ///
    /// Must not allocate: it runs on the hot path, once per chain child.
    [[nodiscard]] virtual bool Matches(const internal::SamplingContext& ctx) const noexcept = 0;

private:
    static std::string Compose(std::string_view name,
                               std::string_view predicate,
                               const SamplerHandle& on_match,
                               const SamplerHandle& on_no_match)
    {
        return std::format("{}{{{}, match={}, else={}}}",
                           name,
                           predicate,
                           DescriptionOf(on_match),
                           DescriptionOf(on_no_match));
    }

    /// Recompose after the forwards, never around them: a lock held across a
    /// delegate's own setter lock would nest two non-leaf locks
    /// (`docs/threading-model.md` §4 rule 2).
    bool Republish() noexcept
    {
        try
        {
            return m_description.Publish(Compose(m_name, m_predicate, m_on_match, m_on_no_match));
        }
        catch (const std::exception&)
        {
            return true;  // the ratio moved; only its rendering did not
        }
    }

    SamplerHandle m_on_match;
    SamplerHandle m_on_no_match;
    /// Kept so the description can be recomposed, not only formatted once.
    std::string m_name;
    std::string m_predicate;
    sdk::DescriptionSlot m_description;
};

/// @brief Matches when an initial attribute holds an expected value.
class AttributeRuleSampler final : public RuleSampler
{
public:
    AttributeRuleSampler(std::string key,
                         AttributeValue expected_value,
                         SamplerHandle on_match,
                         SamplerHandle on_no_match)
        : RuleSampler("AttributeRuleSampler",
                      std::format("key={}", key),
                      std::move(on_match),
                      std::move(on_no_match)),
          m_key(std::move(key)),
          m_expected_value(std::move(expected_value))
    {
    }

    [[nodiscard]] bool Matches(const internal::SamplingContext& ctx) const noexcept override
    {
        // Neither `std::string`'s nor `AttributeValue`'s `operator==` is
        // specified `noexcept`, and this frame is. Comparing allocates
        // nothing, so nothing is expected to throw; if something does, the
        // rule reads as a miss — which routes to the no-match delegate —
        // rather than terminating the process on the hot path.
        try
        {
            return MatchesOrThrow(ctx);
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

private:
    [[nodiscard]] bool MatchesOrThrow(const internal::SamplingContext& ctx) const
    {
        const auto found = std::ranges::find_if(ctx.initial_attributes,
                                                [this](const KeyValue& attribute)
                                                { return attribute.key == m_key; });
        if (found == ctx.initial_attributes.end())
        {
            return false;
        }
        return found->value == m_expected_value;
    }

    std::string m_key;
    AttributeValue m_expected_value;
};

/// @brief Matches on exact span-name equality.
class SpanNameRuleSampler final : public RuleSampler
{
public:
    SpanNameRuleSampler(std::string name, SamplerHandle on_match, SamplerHandle on_no_match)
        : RuleSampler("SpanNameRuleSampler",
                      std::format("name={}", name),
                      std::move(on_match),
                      std::move(on_no_match)),
          m_name(std::move(name))
    {
    }

    [[nodiscard]] bool Matches(const internal::SamplingContext& ctx) const noexcept override
    {
        return ctx.span_name == m_name;
    }

private:
    std::string m_name;
};

/// @brief Matches on span kind.
class SpanKindRuleSampler final : public RuleSampler
{
public:
    SpanKindRuleSampler(SpanKind kind, SamplerHandle on_match, SamplerHandle on_no_match)
        : RuleSampler("SpanKindRuleSampler",
                      std::format("kind={}", SpanKindName(kind)),
                      std::move(on_match),
                      std::move(on_no_match)),
          m_kind(kind)
    {
    }

    [[nodiscard]] bool Matches(const internal::SamplingContext& ctx) const noexcept override
    {
        return ctx.span_kind == m_kind;
    }

private:
    SpanKind m_kind;
};

/// @brief Composes child samplers under one of the two `ChainMode`s.
///
/// Children are resolved once, here: the owning handle, the borrowed
/// `ISampler*` it holds, and — for a child that carries a predicate — the
/// borrowed `RuleSampler*`. The `dynamic_cast` that decides the last of
/// those happens at construction, never at call time. Empty handles are
/// discarded rather than stored, so the hot path never tests for null.
class ChainSampler final : public internal::ISampler
{
public:
    ChainSampler(std::vector<SamplerHandle> children, ChainMode mode)
        : m_children(BuildChildren(std::move(children))),
          m_mode(mode),
          m_description(ComposeDescription(m_children, mode))
    {
    }

    [[nodiscard]] internal::SamplingResult ShouldSample(
        const internal::SamplingContext& ctx) const noexcept override
    {
        if (m_mode == ChainMode::FirstMatch)
        {
            return EvaluateFirstMatch(ctx);
        }
        return EvaluateAllMustAgree(ctx);
    }

    [[nodiscard]] std::string_view Description() const noexcept override
    {
        return m_description.Get();
    }

    /// Forwards to every child — a chain may hold more than one ratio sampler,
    /// and an operator retuning "the ratio" means all of them. Success from
    /// any child is success for the chain, which then recomposes its own
    /// description from the children's new ones (ICP 0026 §5).
    [[nodiscard]] bool TrySetRatio(double ratio) noexcept override
    {
        bool applied = false;
        for (Child& child : m_children)
        {
            applied = TryDelegate(child.handle, ratio) || applied;
        }
        if (!applied)
        {
            return false;
        }
        try
        {
            return m_description.Publish(ComposeDescription(m_children, m_mode));
        }
        catch (const std::exception&)
        {
            return true;  // the ratios moved; only the rendering did not
        }
    }

private:
    /// @brief One resolved child. `rule` is null when the child carries no
    /// predicate, which in `FirstMatch` mode means it matches everything.
    /// Both pointers are borrowed from `handle` and live as long as it does.
    struct Child
    {
        SamplerHandle handle;
        const internal::ISampler* sampler = nullptr;
        const RuleSampler* rule = nullptr;
    };

    static std::vector<Child> BuildChildren(std::vector<SamplerHandle> children)
    {
        std::vector<Child> resolved;
        resolved.reserve(children.size());
        for (SamplerHandle& handle : children)
        {
            const internal::ISampler* const sampler = handle.Get();
            if (sampler == nullptr)
            {
                continue;
            }
            resolved.push_back(Child{.handle = std::move(handle),
                                     .sampler = sampler,
                                     .rule = dynamic_cast<const RuleSampler*>(sampler)});
        }
        return resolved;
    }

    static std::string ComposeDescription(const std::vector<Child>& children, ChainMode mode)
    {
        std::string composed = std::format("ChainSampler{{{}, [", ChainModeName(mode));
        std::string_view separator;
        for (const Child& child : children)
        {
            composed += separator;
            composed += child.sampler->Description();
            separator = ", ";
        }
        composed += "]}";
        return composed;
    }

    [[nodiscard]] internal::SamplingResult EvaluateFirstMatch(
        const internal::SamplingContext& ctx) const noexcept
    {
        for (const Child& child : m_children)
        {
            if (child.rule == nullptr || child.rule->Matches(ctx))
            {
                return child.sampler->ShouldSample(ctx);
            }
        }
        return DropResult();
    }

    [[nodiscard]] internal::SamplingResult EvaluateAllMustAgree(
        const internal::SamplingContext& ctx) const noexcept
    {
        // Seeded with Drop so an empty chain drops. Each iteration overwrites
        // it, so what survives the loop is the last child's result — move
        // assignment of an already-empty vector, which does not allocate.
        internal::SamplingResult result = DropResult();
        for (const Child& child : m_children)
        {
            result = child.sampler->ShouldSample(ctx);
            if (result.decision != internal::SamplingDecision::RecordAndSample)
            {
                return DropResult();
            }
        }
        return result;
    }

    std::vector<Child> m_children;
    ChainMode m_mode;
    sdk::DescriptionSlot m_description;
};

}  // namespace

SamplerHandle MakeAttributeRuleSampler(std::string key,
                                       AttributeValue expected_value,
                                       SamplerHandle on_match,
                                       SamplerHandle on_no_match)
{
    return SamplerHandle{std::make_unique<AttributeRuleSampler>(
        std::move(key), std::move(expected_value), std::move(on_match), std::move(on_no_match))};
}

SamplerHandle MakeSpanNameRuleSampler(std::string name,
                                      SamplerHandle on_match,
                                      SamplerHandle on_no_match)
{
    return SamplerHandle{std::make_unique<SpanNameRuleSampler>(
        std::move(name), std::move(on_match), std::move(on_no_match))};
}

SamplerHandle MakeSpanKindRuleSampler(SpanKind kind,
                                      SamplerHandle on_match,
                                      SamplerHandle on_no_match)
{
    return SamplerHandle{
        std::make_unique<SpanKindRuleSampler>(kind, std::move(on_match), std::move(on_no_match))};
}

SamplerHandle MakeChainSampler(std::vector<SamplerHandle> children, ChainMode mode)
{
    return SamplerHandle{std::make_unique<ChainSampler>(std::move(children), mode)};
}

}  // namespace microtel
