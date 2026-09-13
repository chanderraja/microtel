// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Tests for microtel::TraceState::FromHeader / ToHeader / Size / Empty.
//
// Contract under test (issue #188 — all four were declared in the public
// header and defined in no shipped translation unit, so every out-of-library
// caller got an undefined-symbol link error).
//
// ── What this contract actually is ────────────────────────────────────────────
//
// `TraceState` as declared in include/microtel/trace.hpp carries **no data
// member**: it is an empty class with four methods and no storage, and it has
// no mutation methods either. So the shipped type cannot hold an entry, and
// `FromHeader` returns the empty state for every input — which is also the
// documented return for a parse failure ("Returns an empty `TraceState` on
// parse failure", trace.hpp:99-100).
//
// These tests therefore lock the contract the header actually declares, not
// the one its class comment describes ("v1 stores at most 32 entries ...
// Mutation methods enforce the limit", trace.hpp:90-91) — that comment
// describes storage and mutators that no declaration provides. Giving
// `TraceState` storage is an ABI change to a public header and would put a
// throwing copy inside `Span::GetContext() const noexcept`; it is ICP work,
// not part of the #188 link fix. See the PR body for #188.
//
// When that ICP lands, these expectations are the ones it must update.

#include "microtel/trace.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace mt = microtel;

namespace
{

/// @brief A well-formed multi-member `tracestate` from the W3C examples.
constexpr std::string_view kValidHeader = "rojo=00f067aa0ba902b7,congo=t61rcWkgMzE";

/// @brief Asserts @p state holds nothing, by all three observers.
void ExpectEmpty(const mt::TraceState& state)
{
    EXPECT_TRUE(state.Empty());
    EXPECT_EQ(state.Size(), 0U);
    EXPECT_TRUE(state.ToHeader().empty());
}

// ── The default state ─────────────────────────────────────────────────────────

TEST(TraceStateTest, DefaultConstructedIsEmpty)
{
    ExpectEmpty(mt::TraceState{});
}

TEST(TraceStateTest, DefaultInsideASpanContextIsEmpty)
{
    ExpectEmpty(mt::SpanContext{}.trace_state);
}

TEST(TraceStateTest, ToHeaderOfAnEmptyStateIsTheEmptyString)
{
    // Not `","` or `" "` — an empty state serialises to nothing at all, which
    // is what lets Inject decide to omit the header entirely.
    EXPECT_EQ(mt::TraceState{}.ToHeader(), std::string{});
}

// ── FromHeader: the storage-free contract ─────────────────────────────────────

TEST(TraceStateTest, FromHeaderOfAValidHeaderIsEmpty)
{
    // Not a parse failure — the declared type has nowhere to put the entries.
    ExpectEmpty(mt::TraceState::FromHeader(kValidHeader));
}

TEST(TraceStateTest, FromHeaderOfASingleMemberIsEmpty)
{
    ExpectEmpty(mt::TraceState::FromHeader("vendor=value"));
}

TEST(TraceStateTest, FromHeaderOfAnEmptyHeaderIsEmpty)
{
    ExpectEmpty(mt::TraceState::FromHeader(""));
}

TEST(TraceStateTest, FromHeaderOfAMalformedHeaderIsEmpty)
{
    // The documented failure mode: "failures are silently elided per the W3C
    // 'be liberal in what you accept' guidance" (trace.hpp:99-100). No throw,
    // no error channel — an empty state.
    ExpectEmpty(mt::TraceState::FromHeader("=no-key"));
    ExpectEmpty(mt::TraceState::FromHeader("no-value="));
    ExpectEmpty(mt::TraceState::FromHeader("no-equals-sign"));
    ExpectEmpty(mt::TraceState::FromHeader(",,,"));
    ExpectEmpty(mt::TraceState::FromHeader("UPPER=case-key-is-invalid"));
}

TEST(TraceStateTest, FromHeaderOfAnOverlongListIsEmpty)
{
    // W3C caps a tracestate at 32 list members; the header comment names the
    // same limit. 33 members exercises the over-limit input.
    constexpr std::size_t kOverLimit = 33U;
    std::string header;
    for (std::size_t i = 0; i < kOverLimit; ++i)
    {
        if (i != 0)
        {
            header.push_back(',');
        }
        header += "k" + std::to_string(i) + "=v";
    }
    ExpectEmpty(mt::TraceState::FromHeader(header));
}

TEST(TraceStateTest, FromHeaderDoesNotAliasItsArgument)
{
    // string_view in, owned value out: the result must outlive the buffer.
    mt::TraceState state;
    {
        const std::string scoped(kValidHeader);
        state = mt::TraceState::FromHeader(scoped);
    }
    ExpectEmpty(state);
}

}  // namespace
