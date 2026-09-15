// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Tests for microtel::TraceState — storage, the W3C Trace Context §3.3
// grammar, and the copy-on-write mutators.
//
// Issue #208 / ICP 0025 packet 2.3a. Before this, `TraceState` was an empty
// class: no data member, no mutators, so `FromHeader` returned the empty
// state for every input and a vendor's `tracestate` was dropped across a
// microtel hop. The expectations this file used to hold — "FromHeader of a
// valid header is empty" — were the ones that ICP named as the ones to
// update, and this is that update.
//
// ── The contract now ─────────────────────────────────────────────────────────
//
//  - Storage is a `shared_ptr` to an immutable entry list, so every copy is a
//    refcount bump and `SpanContext`'s copy stays `noexcept` (hard rule 14 on
//    `Span::GetContext() const noexcept`). The static_asserts below are the
//    guard ICP 0025 §1 asks the implementing packet to plant.
//  - The empty state holds a null pointer, so `SpanContext{}` for an unsampled
//    span still allocates nothing (`memory-model.md` §8.1).
//  - `Set` / `Erase` are copy-on-write: they return a new state and leave the
//    receiver untouched.
//
// ── Parse strictness, and why ────────────────────────────────────────────────
//
// W3C Trace Context §4.3 permits either of two reactions to a bad
// `tracestate`: "If the `tracestate` header cannot be parsed the vendor MAY
// discard the entire header. Invalid `tracestate` entries MAY also be
// discarded." microtel takes the first, whole-header option — one malformed
// member, one duplicate key, or a 33rd member discards the lot. That is the
// behaviour trace.hpp has always documented ("Returns an empty `TraceState`
// on parse failure") and it is the predictable one: a caller never has to
// wonder which half of a header survived.

#include "microtel/trace.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace mt = microtel;

namespace
{

/// @brief The W3C Trace Context specification's own example `tracestate`.
constexpr std::string_view kSpecExample = "rojo=00f067aa0ba902b7,congo=t61rcWkgMzE";

/// @brief Asserts @p state holds nothing, by every observer.
void ExpectEmpty(const mt::TraceState& state)
{
    EXPECT_TRUE(state.Empty());
    EXPECT_EQ(state.Size(), 0U);
    EXPECT_TRUE(state.ToHeader().empty());
    EXPECT_FALSE(state.Get("anything").has_value());
}

/// @brief Builds a `key=value` header with @p count members, `k0=v` upward.
[[nodiscard]] std::string MembersHeader(std::size_t count)
{
    std::string header;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i != 0)
        {
            header.push_back(',');
        }
        header += "k" + std::to_string(i) + "=v";
    }
    return header;
}

/// @brief A key of exactly @p length characters, `a` then `b` repeated.
[[nodiscard]] std::string KeyOfLength(std::size_t length)
{
    return "a" + std::string(length - 1U, 'b');
}

/// @brief Asserts @p header parses to the empty state.
void ExpectHeaderRejected(std::string_view header)
{
    SCOPED_TRACE(std::string("header: [") + std::string(header) + ']');
    ExpectEmpty(mt::TraceState::FromHeader(header));
}

// ── The noexcept guard ICP 0025 §1 asks for ──────────────────────────────────
//
// This is the whole reason the entry list sits behind a `shared_ptr` rather
// than in a `std::vector` member: `Span::GetContext() const noexcept` returns
// a `SpanContext` by value, so a `TraceState` copy that can allocate would
// make that signature a lie.

static_assert(std::is_nothrow_copy_constructible_v<mt::TraceState>,
              "TraceState copy must not allocate — see ICP 0025 §1");
static_assert(std::is_nothrow_copy_assignable_v<mt::TraceState>);
static_assert(std::is_nothrow_move_constructible_v<mt::TraceState>);
static_assert(std::is_nothrow_move_assignable_v<mt::TraceState>);
static_assert(std::is_nothrow_destructible_v<mt::TraceState>);

static_assert(std::is_nothrow_copy_constructible_v<mt::SpanContext>,
              "SpanContext copy is returned from Span::GetContext() const noexcept");
static_assert(std::is_nothrow_copy_assignable_v<mt::SpanContext>);
static_assert(std::is_nothrow_move_constructible_v<mt::SpanContext>);

// Rule of zero: no special member is user-declared, so all five are implicit.
static_assert(std::is_trivially_copyable_v<mt::TraceId>);

// ── The default state ────────────────────────────────────────────────────────

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

TEST(TraceStateTest, MaxEntriesIsTheW3CLimit)
{
    EXPECT_EQ(mt::TraceState::kMaxEntries, 32U);
}

// ── FromHeader: the happy path ───────────────────────────────────────────────

TEST(TraceStateTest, ParsesTheSpecExample)
{
    const mt::TraceState state = mt::TraceState::FromHeader(kSpecExample);
    EXPECT_FALSE(state.Empty());
    EXPECT_EQ(state.Size(), 2U);
    EXPECT_EQ(state.Get("rojo"), std::string_view("00f067aa0ba902b7"));
    EXPECT_EQ(state.Get("congo"), std::string_view("t61rcWkgMzE"));
}

TEST(TraceStateTest, PreservesHeaderOrder)
{
    // Order is semantic in W3C tracestate: the left-most member is the most
    // recently updated system, so serialisation must not sort or reorder.
    EXPECT_EQ(mt::TraceState::FromHeader(kSpecExample).ToHeader(), kSpecExample);
}

TEST(TraceStateTest, ParsesASingleMember)
{
    const mt::TraceState state = mt::TraceState::FromHeader("vendor=value");
    EXPECT_EQ(state.Size(), 1U);
    EXPECT_EQ(state.Get("vendor"), std::string_view("value"));
}

TEST(TraceStateTest, TrimsOptionalWhitespaceAroundMembers)
{
    // `list = list-member 0*31( OWS "," OWS list-member )` — OWS is SP / HTAB
    // and is not part of either the key or the value.
    const mt::TraceState state = mt::TraceState::FromHeader("rojo=1 ,\t congo=2");
    ASSERT_EQ(state.Size(), 2U);
    EXPECT_EQ(state.Get("rojo"), std::string_view("1"));
    EXPECT_EQ(state.Get("congo"), std::string_view("2"));
    EXPECT_EQ(state.ToHeader(), "rojo=1,congo=2");
}

TEST(TraceStateTest, SkipsEmptyListMembers)
{
    // `list-member = (key "=" value) / OWS` — a member that is only OWS is
    // legal and carries nothing.
    const mt::TraceState state = mt::TraceState::FromHeader("rojo=1, ,congo=2");
    ASSERT_EQ(state.Size(), 2U);
    EXPECT_EQ(state.ToHeader(), "rojo=1,congo=2");
}

TEST(TraceStateTest, AcceptsExactlyThirtyTwoMembers)
{
    const mt::TraceState state = mt::TraceState::FromHeader(MembersHeader(mt::TraceState::kMaxEntries));
    EXPECT_EQ(state.Size(), mt::TraceState::kMaxEntries);
}

TEST(TraceStateTest, GetIsAbsentForAnUnknownKey)
{
    EXPECT_FALSE(mt::TraceState::FromHeader(kSpecExample).Get("nobody").has_value());
}

TEST(TraceStateTest, FromHeaderDoesNotAliasItsArgument)
{
    // string_view in, owned value out: the result must outlive the buffer.
    mt::TraceState state;
    {
        const std::string scoped(kSpecExample);
        state = mt::TraceState::FromHeader(scoped);
    }
    EXPECT_EQ(state.ToHeader(), kSpecExample);
}

// ── FromHeader: the key grammar (W3C §3.3) ───────────────────────────────────
//
//   key              = simple-key / multi-tenant-key
//   simple-key       = lcalpha 0*255( lcalpha / DIGIT / "_" / "-" / "*" / "/" )
//   multi-tenant-key = tenant-id "@" system-id
//   tenant-id        = ( lcalpha / DIGIT ) 0*240( lcalpha / DIGIT / "_" / "-" / "*" / "/" )
//   system-id        = lcalpha 0*13( lcalpha / DIGIT / "_" / "-" / "*" / "/" )

TEST(TraceStateTest, AcceptsEveryValidKeyVector)
{
    constexpr std::string_view kValidKeys[] = {
        "a",                      // the shortest simple-key
        "foo",                    //
        "foo123",                 // DIGIT in the tail
        "foo_bar",                // "_"
        "foo-bar",                // "-"
        "foo*bar",                // "*"
        "foo/bar",                // "/"
        "f_-*/0",                 // every tail character at once
        "fw529a3039@dt",          // the spec's own multi-tenant example
        "1a2b3c4d5e@dt",          // tenant-id may open with a DIGIT
        "9@a",                    // shortest multi-tenant-key
        "a-b_c*d/1@x-y_z*0/2",    // tail charset on both halves
    };

    for (const std::string_view key : kValidKeys)
    {
        const std::string header = std::string(key) + "=v";
        const mt::TraceState state = mt::TraceState::FromHeader(header);
        EXPECT_EQ(state.Size(), 1U) << "should have parsed key: " << key;
        EXPECT_EQ(state.Get(key), std::string_view("v")) << "key: " << key;
    }
}

TEST(TraceStateTest, RejectsEveryInvalidKeyVector)
{
    constexpr std::string_view kInvalidKeys[] = {
        "",              // empty key
        "1foo",          // simple-key must open with lcalpha
        "_foo",          // ditto
        "FOO",           // upper case is not lcalpha
        "fOo",           //
        "foo bar",       // SP is not a key character
        "foo\tbar",      // HTAB likewise
        "foo.bar",       // "." is not in the tail charset
        "foo+bar",       // nor "+"
        "foo@",          // empty system-id
        "@foo",          // empty tenant-id
        "@",             //
        "foo@bar@baz",   // "@" is not a system-id character
        "_@dt",          // tenant-id must open with lcalpha / DIGIT
        "dt@1x",         // system-id must open with lcalpha
        "dt@_x",         // ditto
        "föö",           // non-ASCII
    };

    for (const std::string_view key : kInvalidKeys)
    {
        ExpectHeaderRejected(std::string(key) + "=v");
    }
}

TEST(TraceStateTest, EnforcesTheSimpleKeyLengthLimit)
{
    constexpr std::size_t kMaxSimpleKeyChars = 256U;  // lcalpha 0*255( ... )
    EXPECT_EQ(mt::TraceState::FromHeader(KeyOfLength(kMaxSimpleKeyChars) + "=v").Size(), 1U);
    ExpectHeaderRejected(KeyOfLength(kMaxSimpleKeyChars + 1U) + "=v");
}

TEST(TraceStateTest, EnforcesTheTenantIdLengthLimit)
{
    constexpr std::size_t kMaxTenantIdChars = 241U;  // ( lcalpha / DIGIT ) 0*240( ... )
    EXPECT_EQ(mt::TraceState::FromHeader(KeyOfLength(kMaxTenantIdChars) + "@dt=v").Size(), 1U);
    ExpectHeaderRejected(KeyOfLength(kMaxTenantIdChars + 1U) + "@dt=v");
}

TEST(TraceStateTest, EnforcesTheSystemIdLengthLimit)
{
    constexpr std::size_t kMaxSystemIdChars = 14U;  // lcalpha 0*13( ... )
    EXPECT_EQ(mt::TraceState::FromHeader("dt@" + KeyOfLength(kMaxSystemIdChars) + "=v").Size(), 1U);
    ExpectHeaderRejected("dt@" + KeyOfLength(kMaxSystemIdChars + 1U) + "=v");
}

// ── FromHeader: the value grammar (W3C §3.3) ─────────────────────────────────
//
//   value    = 0*255(chr) nblk-chr
//   nblk-chr = %x21-2B / %x2D-3C / %x3E-7E
//   chr      = %x20 / nblk-chr
//
// So: printable ASCII minus "," and "=", at most 256 characters, never empty,
// and never ending in a space.

TEST(TraceStateTest, AcceptsEveryValidValueVector)
{
    constexpr std::string_view kValidValues[] = {
        "v",                     // the shortest value
        "t61rcWkgMzE",           // the spec's own example — upper case is fine
        "00f067aa0ba902b7",      //
        "a b",                   // an interior SP is a `chr`
        " a",                    // ... including a leading one
        "!#$%&'()*+",            // %x21-2B
        "-./0123456789:;<",      // %x2D-3C
        ">?@[\\]^_`{|}~",        // %x3E-7E
        "0",                     //
    };

    for (const std::string_view value : kValidValues)
    {
        const std::string header = "k=" + std::string(value);
        const mt::TraceState state = mt::TraceState::FromHeader(header);
        ASSERT_EQ(state.Size(), 1U) << "should have parsed value: [" << value << ']';
        EXPECT_EQ(state.Get("k"), value) << "value: [" << value << ']';
    }
}

TEST(TraceStateTest, RejectsEveryInvalidValueVector)
{
    const std::string kInvalidValues[] = {
        "",                      // `value` requires at least one nblk-chr
        "a\tb",                  // HTAB is not a `chr`
        "a,b",                   // %x2C is excluded — it is the list separator
        "a=b",                   // %x3D is excluded — it is the member separator
        "a\nb",                  // control characters are not `chr`
        std::string("a\0b", 3),  // NUL likewise
        "aéb",                   // non-ASCII
    };

    for (const std::string& value : kInvalidValues)
    {
        ExpectHeaderRejected("k=" + value);
    }
}

TEST(TraceStateTest, TrailingWhitespaceOnAMemberIsOwsAndIsTrimmedNotRejected)
{
    // Trailing SP / HTAB never reaches value validation: it is `OWS` around
    // the list separator, and RFC 9110 strips it from the field value outright
    // when the member is the last one. So `k=a ` carries the value `a`, and
    // the "must end in an nblk-chr" rule bites on `Set`, where no trimming
    // happens — see SetRejectsAnInvalidValueAndReturnsAnUnchangedCopy.
    EXPECT_EQ(mt::TraceState::FromHeader("k=a ").Get("k"), std::string_view("a"));
    EXPECT_EQ(mt::TraceState::FromHeader("k=a\t").Get("k"), std::string_view("a"));
    EXPECT_EQ(mt::TraceState::FromHeader("k=a ,j=b ").ToHeader(), "k=a,j=b");
}

TEST(TraceStateTest, EnforcesTheValueLengthLimit)
{
    constexpr std::size_t kMaxValueChars = 256U;  // 0*255(chr) nblk-chr
    EXPECT_EQ(mt::TraceState::FromHeader("k=" + std::string(kMaxValueChars, 'v')).Size(), 1U);
    ExpectHeaderRejected("k=" + std::string(kMaxValueChars + 1U, 'v'));
}

// ── FromHeader: whole-header rejection ───────────────────────────────────────

TEST(TraceStateTest, RejectsAMemberWithoutAnEqualsSign)
{
    ExpectHeaderRejected("no-equals-sign");
    ExpectHeaderRejected("rojo=1,no-equals-sign");
}

TEST(TraceStateTest, RejectsADuplicateKey)
{
    // W3C §3.3: a key may appear at most once. Rather than guess which
    // occurrence the sender meant, the whole header goes.
    ExpectHeaderRejected("rojo=1,rojo=2");
    ExpectHeaderRejected("rojo=1,congo=2,rojo=3");
}

TEST(TraceStateTest, RejectsMoreThanThirtyTwoMembers)
{
    ExpectHeaderRejected(MembersHeader(mt::TraceState::kMaxEntries + 1U));
}

TEST(TraceStateTest, OneBadMemberDiscardsTheWholeHeader)
{
    // The §4.3 "MAY discard the entire header" branch, chosen deliberately:
    // partial survival would leave callers guessing.
    ExpectHeaderRejected("rojo=00f067aa0ba902b7,BAD=value");
    ExpectHeaderRejected("BAD=value,rojo=00f067aa0ba902b7");
}

TEST(TraceStateTest, RejectsHeadersThatCarryNothing)
{
    ExpectHeaderRejected("");
    ExpectHeaderRejected(",,,");
    ExpectHeaderRejected("   ");
    ExpectHeaderRejected("=no-key");
    ExpectHeaderRejected("no-value=");
    ExpectHeaderRejected("UPPER=case-key-is-invalid");
}

// ── Set: copy-on-write ───────────────────────────────────────────────────────

TEST(TraceStateTest, SetLeavesTheOriginalUntouched)
{
    // The load-bearing property: the entry list is immutable and shared, so a
    // mutation must build a new one rather than write through the pointer.
    const mt::TraceState original = mt::TraceState::FromHeader(kSpecExample);
    const mt::TraceState mutated = original.Set("congo", "changed");

    EXPECT_EQ(original.ToHeader(), kSpecExample);
    EXPECT_EQ(original.Get("congo"), std::string_view("t61rcWkgMzE"));
    EXPECT_EQ(mutated.Get("congo"), std::string_view("changed"));
}

TEST(TraceStateTest, SetOnAnEmptyStateCreatesTheFirstEntry)
{
    const mt::TraceState state = mt::TraceState{}.Set("vendor", "value");
    EXPECT_EQ(state.Size(), 1U);
    EXPECT_EQ(state.ToHeader(), "vendor=value");
}

TEST(TraceStateTest, SetAddsANewKeyAtTheFront)
{
    // W3C §3.3.1: "The new key/value pair SHOULD be added to the beginning of
    // the list" — the left-most member is the most recently mutating system.
    const mt::TraceState state = mt::TraceState::FromHeader(kSpecExample).Set("fresh", "1");
    EXPECT_EQ(state.ToHeader(), "fresh=1,rojo=00f067aa0ba902b7,congo=t61rcWkgMzE");
}

TEST(TraceStateTest, SetMovesAnUpdatedKeyToTheFront)
{
    // W3C §3.3.1: a modified key "should be moved to the beginning (left) of
    // the list", not updated in place.
    const mt::TraceState state = mt::TraceState::FromHeader(kSpecExample).Set("congo", "new");
    EXPECT_EQ(state.ToHeader(), "congo=new,rojo=00f067aa0ba902b7");
    EXPECT_EQ(state.Size(), 2U);
}

TEST(TraceStateTest, SetPreservesTheOrderOfTheUntouchedMembers)
{
    const mt::TraceState state = mt::TraceState::FromHeader("a=1,b=2,c=3,d=4").Set("c", "9");
    EXPECT_EQ(state.ToHeader(), "c=9,a=1,b=2,d=4");
}

TEST(TraceStateTest, SetRejectsAnInvalidKeyAndReturnsAnUnchangedCopy)
{
    // No error channel on the public surface, so a rejected mutation is a
    // no-op copy rather than a silently corrupt state.
    const mt::TraceState original = mt::TraceState::FromHeader(kSpecExample);
    EXPECT_EQ(original.Set("UPPER", "v").ToHeader(), kSpecExample);
    EXPECT_EQ(original.Set("", "v").ToHeader(), kSpecExample);
    EXPECT_EQ(original.Set("bad key", "v").ToHeader(), kSpecExample);
}

TEST(TraceStateTest, SetRejectsAnInvalidValueAndReturnsAnUnchangedCopy)
{
    const mt::TraceState original = mt::TraceState::FromHeader(kSpecExample);
    EXPECT_EQ(original.Set("k", "").ToHeader(), kSpecExample);
    EXPECT_EQ(original.Set("k", "has,comma").ToHeader(), kSpecExample);
    EXPECT_EQ(original.Set("k", "has=equals").ToHeader(), kSpecExample);
    EXPECT_EQ(original.Set("k", "trailing ").ToHeader(), kSpecExample);
}

TEST(TraceStateTest, SetRefusesToGrowBeyondTheLimit)
{
    // A 33rd member is not representable, and evicting somebody else's entry
    // to make room would lose state the sender asked us to carry. The
    // mutation is refused instead.
    const mt::TraceState full = mt::TraceState::FromHeader(MembersHeader(mt::TraceState::kMaxEntries));
    ASSERT_EQ(full.Size(), mt::TraceState::kMaxEntries);

    const mt::TraceState attempted = full.Set("fresh", "1");
    EXPECT_EQ(attempted.Size(), mt::TraceState::kMaxEntries);
    EXPECT_FALSE(attempted.Get("fresh").has_value());
}

TEST(TraceStateTest, SetMayUpdateAnExistingKeyOnAFullState)
{
    // Updating does not grow the list, so the limit does not bite.
    const mt::TraceState full = mt::TraceState::FromHeader(MembersHeader(mt::TraceState::kMaxEntries));
    const mt::TraceState updated = full.Set("k5", "updated");
    EXPECT_EQ(updated.Size(), mt::TraceState::kMaxEntries);
    EXPECT_EQ(updated.Get("k5"), std::string_view("updated"));
}

// ── Erase: copy-on-write ─────────────────────────────────────────────────────

TEST(TraceStateTest, EraseLeavesTheOriginalUntouched)
{
    const mt::TraceState original = mt::TraceState::FromHeader(kSpecExample);
    const mt::TraceState erased = original.Erase("rojo");

    EXPECT_EQ(original.ToHeader(), kSpecExample);
    EXPECT_EQ(erased.ToHeader(), "congo=t61rcWkgMzE");
}

TEST(TraceStateTest, ErasePreservesTheOrderOfTheSurvivors)
{
    EXPECT_EQ(mt::TraceState::FromHeader("a=1,b=2,c=3").Erase("b").ToHeader(), "a=1,c=3");
}

TEST(TraceStateTest, EraseOfAnUnknownKeyIsANoOp)
{
    EXPECT_EQ(mt::TraceState::FromHeader(kSpecExample).Erase("nobody").ToHeader(), kSpecExample);
}

TEST(TraceStateTest, EraseOfTheLastEntryYieldsTheEmptyState)
{
    ExpectEmpty(mt::TraceState::FromHeader("only=1").Erase("only"));
}

TEST(TraceStateTest, EraseOnAnEmptyStateIsANoOp)
{
    ExpectEmpty(mt::TraceState{}.Erase("anything"));
}

// ── Round trips ──────────────────────────────────────────────────────────────

TEST(TraceStateTest, ToHeaderRoundTripsThroughFromHeader)
{
    const mt::TraceState built =
        mt::TraceState{}.Set("congo", "t61rcWkgMzE").Set("rojo", "00f067aa0ba902b7");
    EXPECT_EQ(built.ToHeader(), kSpecExample);

    const mt::TraceState reparsed = mt::TraceState::FromHeader(built.ToHeader());
    EXPECT_EQ(reparsed.ToHeader(), built.ToHeader());
    EXPECT_EQ(reparsed.Size(), built.Size());
}

TEST(TraceStateTest, CopiesShareTheSameEntriesAndReadAlike)
{
    const mt::TraceState original = mt::TraceState::FromHeader(kSpecExample);
    mt::TraceState copy = original;  // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_EQ(copy.ToHeader(), original.ToHeader());

    copy = copy.Set("rojo", "rewritten");
    EXPECT_EQ(original.Get("rojo"), std::string_view("00f067aa0ba902b7"));
    EXPECT_EQ(copy.Get("rojo"), std::string_view("rewritten"));
}

TEST(TraceStateTest, GetBorrowsFromTheStateAndSurvivesAnIntermediateCopy)
{
    // Doxygen says the view is "valid while any copy of it lives"; the shared
    // entry list is what makes that true.
    const mt::TraceState original = mt::TraceState::FromHeader(kSpecExample);
    std::string_view borrowed;
    {
        const mt::TraceState copy = original;
        borrowed = *copy.Get("rojo");
    }
    EXPECT_EQ(borrowed, std::string_view("00f067aa0ba902b7"));
}

}  // namespace
