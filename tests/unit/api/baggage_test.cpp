// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Tests for microtel::Baggage — the W3C Baggage grammar, percent-encoding,
// the `;`-property tail, the three limits, and the copy-on-write mutators.
//
// ICP 0025 §2, packet 2.3c. Structured after `trace_state_test.cpp` (packet
// 2.3a): a vector table per grammar rule, then the limits at-and-over, then
// the mutators.
//
// ── Storage, and why it is a shared_ptr ──────────────────────────────────────
//
// `Baggage` holds one `std::shared_ptr<const internal::BaggageImpl>`, for the
// same reason `TraceState` does: `Context` copies must be `noexcept`
// (ICP 0025 §2), and `Context` is copied into every `ScopedContext`. The
// static_asserts below are the guard the ICP asks the implementing packet to
// plant. The empty state is a null pointer and allocates nothing.
//
// ── Parse strictness, and how it differs from TraceState ─────────────────────
//
// `TraceState::FromHeader` discards the **whole header** on the first bad
// member, which W3C Trace Context §4.3 explicitly permits. W3C Baggage says no
// such thing: its only normative drop guidance is per-`list-member` — "a
// platform MAY drop list-members until both conditions are met" and "If a
// platform cannot propagate all baggage, it MUST NOT propagate any partial
// list-members". So `Baggage::FromHeader` is **per-member**: one malformed
// member is dropped and its neighbours survive. The two headers differ because
// the two specifications differ, and both behaviours are pinned here and in
// `trace_state_test.cpp` so neither drifts into the other.
//
// ── Where the limits bite ────────────────────────────────────────────────────
//
//  - `kMaxEntryBytes` (4096) is per list-member: an over-long member is
//    **skipped** and parsing continues.
//  - `kMaxEntries` (180, the grammar's `0*179` repetition) and
//    `kMaxTotalBytes` (8192, the specification's "size 8192 bytes or less")
//    **stop** the parse — later members are dropped from the end, which is the
//    direction the specification's "drop until both conditions are met"
//    describes.
//
// Both limits are measured against the **canonical serialised** form of the
// member — what microtel would put back on the wire — not the raw received
// text. That is what makes `FromHeader(ToHeader(b))` reproduce `b` exactly:
// anything that fits once fits forever.

#include "microtel/baggage.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace mt = microtel;

namespace
{

/// @brief The W3C Baggage specification's own example header value.
constexpr std::string_view kSpecExample = "key1=value1;property1;property2,key2=value2";

/// @brief Asserts @p bag holds nothing, by every observer.
void ExpectEmpty(const mt::Baggage& bag)
{
    EXPECT_TRUE(bag.Empty());
    EXPECT_EQ(bag.Size(), 0U);
    EXPECT_TRUE(bag.ToHeader().empty());
    EXPECT_FALSE(bag.Get("anything").has_value());
}

/// @brief Asserts @p header parses to the empty baggage.
void ExpectHeaderEmpty(std::string_view header)
{
    SCOPED_TRACE(std::string("header: [") + std::string(header) + ']');
    ExpectEmpty(mt::Baggage::FromHeader(header));
}

/// @brief Asserts the single list-member @p member is dropped but its
/// neighbours are not — the per-member leniency this parser promises.
void ExpectMemberDropped(std::string_view member)
{
    SCOPED_TRACE(std::string("member: [") + std::string(member) + ']');
    const std::string header = std::string("before=b,") + std::string(member) + ",after=a";
    const mt::Baggage bag = mt::Baggage::FromHeader(header);
    EXPECT_EQ(bag.Size(), 2U);
    EXPECT_EQ(bag.ToHeader(), "before=b,after=a");
}

/// @brief Asserts the single list-member @p member survives with @p key
/// mapping to @p value.
void ExpectMemberKept(std::string_view member, std::string_view key, std::string_view value)
{
    SCOPED_TRACE(std::string("member: [") + std::string(member) + ']');
    const mt::Baggage bag = mt::Baggage::FromHeader(member);
    ASSERT_EQ(bag.Size(), 1U);
    ASSERT_TRUE(bag.Get(key).has_value());
    EXPECT_EQ(*bag.Get(key), value);
}

/// @brief A header of @p count members, `k0=v` upward.
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

/// @brief A list-member `<key>=vvv…` of exactly @p total serialised bytes.
///
/// `v` needs no percent-encoding, so the raw and canonical lengths agree and
/// the vector says what it means.
[[nodiscard]] std::string MemberOfLength(std::string_view key, std::size_t total)
{
    const std::size_t value_chars = total - key.size() - 1U;
    return std::string(key) + '=' + std::string(value_chars, 'v');
}

// ── The noexcept guard ICP 0025 §2 asks for ──────────────────────────────────
//
// `Baggage` rides `Context`, and a `Context` is copied into every
// `ScopedContext` and every `OnStart` parent. An allocating copy here would
// make `Context`'s own `noexcept` copy a lie.

static_assert(std::is_nothrow_copy_constructible_v<mt::Baggage>,
              "Baggage copy must not allocate — see ICP 0025 §2");
static_assert(std::is_nothrow_copy_assignable_v<mt::Baggage>);
static_assert(std::is_nothrow_move_constructible_v<mt::Baggage>);
static_assert(std::is_nothrow_move_assignable_v<mt::Baggage>);
static_assert(std::is_nothrow_destructible_v<mt::Baggage>);
static_assert(std::is_nothrow_default_constructible_v<mt::Baggage>);

// The limits are the ones ICP 0025 §2 locked.
static_assert(mt::Baggage::kMaxEntries == 180U);
static_assert(mt::Baggage::kMaxEntryBytes == 4096U);
static_assert(mt::Baggage::kMaxTotalBytes == 8192U);

// ── The empty state ──────────────────────────────────────────────────────────

TEST(BaggageTest, DefaultConstructedIsEmpty)
{
    ExpectEmpty(mt::Baggage{});
}

TEST(BaggageTest, EmptyHeaderIsEmpty)
{
    ExpectHeaderEmpty("");
}

TEST(BaggageTest, WhitespaceOnlyHeaderIsEmpty)
{
    ExpectHeaderEmpty("   ");
    ExpectHeaderEmpty("\t");
    ExpectHeaderEmpty(" \t \t ");
}

TEST(BaggageTest, SeparatorsOnlyHeaderIsEmpty)
{
    ExpectHeaderEmpty(",");
    ExpectHeaderEmpty(",,,");
    ExpectHeaderEmpty(" , , ");
}

// ── The happy path ───────────────────────────────────────────────────────────

TEST(BaggageTest, ParsesTheSpecExample)
{
    const mt::Baggage bag = mt::Baggage::FromHeader(kSpecExample);
    ASSERT_EQ(bag.Size(), 2U);
    ASSERT_TRUE(bag.Get("key1").has_value());
    EXPECT_EQ(*bag.Get("key1"), "value1");
    ASSERT_TRUE(bag.Get("key2").has_value());
    EXPECT_EQ(*bag.Get("key2"), "value2");
}

TEST(BaggageTest, RoundTripsTheSpecExampleVerbatim)
{
    EXPECT_EQ(mt::Baggage::FromHeader(kSpecExample).ToHeader(), kSpecExample);
}

TEST(BaggageTest, PreservesMemberOrder)
{
    const mt::Baggage bag = mt::Baggage::FromHeader("c=3,a=1,b=2");
    EXPECT_EQ(bag.ToHeader(), "c=3,a=1,b=2");
}

TEST(BaggageTest, GetReturnsNulloptForAnAbsentKey)
{
    const mt::Baggage bag = mt::Baggage::FromHeader("a=1");
    EXPECT_FALSE(bag.Get("b").has_value());
    EXPECT_FALSE(bag.Get("").has_value());
}

TEST(BaggageTest, GetIsBorrowedFromTheSharedEntryList)
{
    const mt::Baggage original = mt::Baggage::FromHeader("a=1");
    const mt::Baggage copy = original;
    const std::optional<std::string_view> borrowed = copy.Get("a");
    ASSERT_TRUE(borrowed.has_value());
    EXPECT_EQ(*borrowed, "1");
}

// ── Keys: RFC 7230 token ─────────────────────────────────────────────────────

TEST(BaggageTest, AcceptsEveryTokenCharacterInAKey)
{
    // tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." /
    //         "^" / "_" / "`" / "|" / "~" / DIGIT / ALPHA
    ExpectMemberKept("!#$%&'*+-.^_`|~=v", "!#$%&'*+-.^_`|~", "v");
    ExpectMemberKept("A=v", "A", "v");
    ExpectMemberKept("0=v", "0", "v");
    ExpectMemberKept("MiXeD09=v", "MiXeD09", "v");
}

TEST(BaggageTest, DropsAMemberWhoseKeyIsNotAToken)
{
    ExpectMemberDropped("=v");           // empty key
    ExpectMemberDropped("ke y=v");       // SP is not a tchar
    ExpectMemberDropped("ke\ty=v");      // HTAB is not a tchar
    ExpectMemberDropped("key@host=v");   // '@' is not a tchar
    ExpectMemberDropped("key:sub=v");    // ':' is not a tchar
    ExpectMemberDropped("(key)=v");      // parentheses are not tchars
    ExpectMemberDropped("\"key\"=v");    // DQUOTE is not a tchar
    ExpectMemberDropped("key\\sub=v");   // backslash is not a tchar
    ExpectMemberDropped("k\xC3\xA9=v");  // non-ASCII is not a tchar
}

TEST(BaggageTest, DropsAMemberWithNoEqualsSign)
{
    ExpectMemberDropped("novalue");
    ExpectMemberDropped("   ");
}

TEST(BaggageTest, KeysAreNeverPercentDecoded)
{
    // '%' is a tchar, so `%41` is a three-character key, not an escaped 'A'.
    ExpectMemberKept("%41=v", "%41", "v");
    EXPECT_FALSE(mt::Baggage::FromHeader("%41=v").Get("A").has_value());
}

// ── Values: baggage-octet and percent-encoding ───────────────────────────────

TEST(BaggageTest, AcceptsAnEmptyValue)
{
    // value = *baggage-octet, so zero octets is a legal value.
    ExpectMemberKept("k=", "k", "");
    EXPECT_EQ(mt::Baggage::FromHeader("k=").ToHeader(), "k=");
}

TEST(BaggageTest, AcceptsAnEqualsSignInsideAValue)
{
    // '=' is %x3D, inside the baggage-octet range: only the first '=' splits.
    ExpectMemberKept("k=a=b", "k", "a=b");
    EXPECT_EQ(mt::Baggage::FromHeader("k=a=b").ToHeader(), "k=a=b");
}

TEST(BaggageTest, DecodesPercentEscapes)
{
    ExpectMemberKept("k=%20", "k", " ");
    ExpectMemberKept("k=a%20b", "k", "a b");
    ExpectMemberKept("k=%25", "k", "%");
    ExpectMemberKept("k=%2C", "k", ",");
    ExpectMemberKept("k=%3B", "k", ";");
    ExpectMemberKept("k=%22", "k", "\"");
    ExpectMemberKept("k=%5C", "k", "\\");
}

TEST(BaggageTest, AcceptsLowerCaseHexInAnEscape)
{
    ExpectMemberKept("k=%2c", "k", ",");
}

TEST(BaggageTest, ReEncodesUnrepresentableOctetsOnTheWayOut)
{
    // Round-trip is by value, not byte-for-byte: an escape that did not need
    // to be one is normalised away, and the canonical form is stable.
    EXPECT_EQ(mt::Baggage::FromHeader("k=%20").ToHeader(), "k=%20");
    EXPECT_EQ(mt::Baggage::FromHeader("k=%41").ToHeader(), "k=A");
    EXPECT_EQ(mt::Baggage::FromHeader("k=%2c").ToHeader(), "k=%2C");
}

TEST(BaggageTest, SerialisingIsIdempotent)
{
    const std::string once = mt::Baggage::FromHeader("k=%41%20%2c").ToHeader();
    const std::string twice = mt::Baggage::FromHeader(once).ToHeader();
    EXPECT_EQ(once, "k=A%20%2C");
    EXPECT_EQ(twice, once);
}

TEST(BaggageTest, DropsAMemberWithAnIllegalOctetInItsValue)
{
    ExpectMemberDropped("k=va lue");     // interior SP is not a baggage-octet
    ExpectMemberDropped("k=va\"lue");    // DQUOTE
    ExpectMemberDropped("k=va\\lue");    // backslash
    ExpectMemberDropped("k=v\x7F");      // DEL
    ExpectMemberDropped("k=v\x01");      // control character
    ExpectMemberDropped("k=v\xC3\xA9");  // non-ASCII must be percent-encoded
}

TEST(BaggageTest, DropsAMemberWithAMalformedEscape)
{
    ExpectMemberDropped("k=%");
    ExpectMemberDropped("k=%2");
    ExpectMemberDropped("k=%zz");
    ExpectMemberDropped("k=%2z");
    ExpectMemberDropped("k=a%b");
}

// ── Properties: the `;` tail, preserved opaquely ─────────────────────────────

TEST(BaggageTest, PreservesAValuelessProperty)
{
    EXPECT_EQ(mt::Baggage::FromHeader("k=v;p").ToHeader(), "k=v;p");
    ExpectMemberKept("k=v;p", "k", "v");
}

TEST(BaggageTest, PreservesAKeyValueProperty)
{
    EXPECT_EQ(mt::Baggage::FromHeader("k=v;p=1").ToHeader(), "k=v;p=1");
}

TEST(BaggageTest, PreservesSeveralProperties)
{
    EXPECT_EQ(mt::Baggage::FromHeader("k=v;p1;p2=2;p3").ToHeader(), "k=v;p1;p2=2;p3");
}

TEST(BaggageTest, PreservesAnEmptyPropertyValue)
{
    EXPECT_EQ(mt::Baggage::FromHeader("k=v;p=").ToHeader(), "k=v;p=");
}

TEST(BaggageTest, NormalisesWhitespaceInsideAProperty)
{
    // property = key OWS "=" OWS value — the OWS is not part of either half.
    EXPECT_EQ(mt::Baggage::FromHeader("k=v; p = 1 ; q").ToHeader(), "k=v;p=1;q");
}

TEST(BaggageTest, DropsAMemberWithAMalformedProperty)
{
    ExpectMemberDropped("k=v;");         // a ';' with no property after it
    ExpectMemberDropped("k=v;;p");       // an empty property
    ExpectMemberDropped("k=v;pro p");    // the property key is not a token
    ExpectMemberDropped("k=v;=1");       // a property with no key
    ExpectMemberDropped("k=v;p=a b");    // an illegal octet in a property value
    ExpectMemberDropped("k=v;p=\"q\"");  // DQUOTE in a property value
}

// ── Optional whitespace ──────────────────────────────────────────────────────

TEST(BaggageTest, TrimsOptionalWhitespaceAroundEveryDelimiter)
{
    const mt::Baggage bag = mt::Baggage::FromHeader("  k1 = v1 ,\tk2\t=\tv2\t");
    ASSERT_EQ(bag.Size(), 2U);
    EXPECT_EQ(*bag.Get("k1"), "v1");
    EXPECT_EQ(*bag.Get("k2"), "v2");
    EXPECT_EQ(bag.ToHeader(), "k1=v1,k2=v2");
}

TEST(BaggageTest, TrimmedWhitespaceIsNotPartOfTheValue)
{
    // A leading space that was meant to be kept has to be escaped.
    EXPECT_EQ(*mt::Baggage::FromHeader("k= %20v ").Get("k"), " v");
}

TEST(BaggageTest, SkipsEmptyListMembers)
{
    const mt::Baggage bag = mt::Baggage::FromHeader("a=1,,b=2,");
    ASSERT_EQ(bag.Size(), 2U);
    EXPECT_EQ(bag.ToHeader(), "a=1,b=2");
}

// ── Duplicate keys ───────────────────────────────────────────────────────────

TEST(BaggageTest, KeepsTheFirstOfADuplicateKey)
{
    // W3C leaves key uniqueness unguaranteed on the wire, but ICP 0025 §2
    // locks a `Get(key) -> optional` surface, which cannot represent two
    // entries under one key. The first occurrence wins and the later one is
    // dropped, so `Get` and `ToHeader` can never disagree.
    const mt::Baggage bag = mt::Baggage::FromHeader("a=1,a=2,b=3");
    ASSERT_EQ(bag.Size(), 2U);
    EXPECT_EQ(*bag.Get("a"), "1");
    EXPECT_EQ(bag.ToHeader(), "a=1,b=3");
}

// ── Limits ───────────────────────────────────────────────────────────────────

TEST(BaggageTest, AcceptsExactlyTheMaximumNumberOfMembers)
{
    const mt::Baggage bag = mt::Baggage::FromHeader(MembersHeader(mt::Baggage::kMaxEntries));
    EXPECT_EQ(bag.Size(), mt::Baggage::kMaxEntries);
}

TEST(BaggageTest, StopsAtTheMaximumNumberOfMembers)
{
    const mt::Baggage bag = mt::Baggage::FromHeader(MembersHeader(mt::Baggage::kMaxEntries + 1U));
    EXPECT_EQ(bag.Size(), mt::Baggage::kMaxEntries);
    EXPECT_FALSE(bag.Get("k" + std::to_string(mt::Baggage::kMaxEntries)).has_value());
}

TEST(BaggageTest, AcceptsAMemberOfExactlyTheMaximumSize)
{
    const std::string member = MemberOfLength("k", mt::Baggage::kMaxEntryBytes);
    ASSERT_EQ(member.size(), mt::Baggage::kMaxEntryBytes);
    const mt::Baggage bag = mt::Baggage::FromHeader(member);
    ASSERT_EQ(bag.Size(), 1U);
    EXPECT_EQ(bag.ToHeader().size(), mt::Baggage::kMaxEntryBytes);
}

TEST(BaggageTest, SkipsAnOversizedMemberAndKeepsItsNeighbours)
{
    const std::string oversized = MemberOfLength("big", mt::Baggage::kMaxEntryBytes + 1U);
    const mt::Baggage bag = mt::Baggage::FromHeader("a=1," + oversized + ",b=2");
    ASSERT_EQ(bag.Size(), 2U);
    EXPECT_FALSE(bag.Get("big").has_value());
    EXPECT_EQ(bag.ToHeader(), "a=1,b=2");
}

TEST(BaggageTest, MeasuresAMemberByItsCanonicalLengthNotItsRawLength)
{
    // `%41` is three raw bytes that canonicalise to one, so a member that is
    // over the limit as received can still be under it as propagated.
    const std::size_t value_chars = mt::Baggage::kMaxEntryBytes - 2U;
    std::string raw = "k=";
    for (std::size_t i = 0; i < value_chars; ++i)
    {
        raw += "%41";
    }
    ASSERT_GT(raw.size(), mt::Baggage::kMaxEntryBytes);
    const mt::Baggage bag = mt::Baggage::FromHeader(raw);
    ASSERT_EQ(bag.Size(), 1U);
    EXPECT_EQ(bag.ToHeader().size(), mt::Baggage::kMaxEntryBytes);
}

TEST(BaggageTest, AcceptsAHeaderOfExactlyTheMaximumTotalSize)
{
    const std::string first = MemberOfLength("a", mt::Baggage::kMaxEntryBytes);
    const std::string second = MemberOfLength("b", mt::Baggage::kMaxEntryBytes - 1U);
    const std::string header = first + ',' + second;
    ASSERT_EQ(header.size(), mt::Baggage::kMaxTotalBytes);

    const mt::Baggage bag = mt::Baggage::FromHeader(header);
    EXPECT_EQ(bag.Size(), 2U);
    EXPECT_EQ(bag.ToHeader().size(), mt::Baggage::kMaxTotalBytes);
}

TEST(BaggageTest, StopsAtTheMaximumTotalSize)
{
    // Two full-size members are 8193 bytes with their separator: the second
    // one does not fit, and — because the total limit *stops* the parse rather
    // than skipping one member — neither does the small member behind it.
    const std::string first = MemberOfLength("a", mt::Baggage::kMaxEntryBytes);
    const std::string second = MemberOfLength("b", mt::Baggage::kMaxEntryBytes);
    const mt::Baggage bag = mt::Baggage::FromHeader(first + ',' + second + ",z=1");

    ASSERT_EQ(bag.Size(), 1U);
    EXPECT_TRUE(bag.Get("a").has_value());
    EXPECT_FALSE(bag.Get("b").has_value());
    EXPECT_FALSE(bag.Get("z").has_value());
}

// ── Set: copy-on-write ───────────────────────────────────────────────────────

TEST(BaggageTest, SetLeavesTheReceiverUntouched)
{
    const mt::Baggage original = mt::Baggage::FromHeader("a=1");
    const mt::Baggage updated = original.Set("b", "2");
    EXPECT_EQ(original.Size(), 1U);
    EXPECT_FALSE(original.Get("b").has_value());
    EXPECT_EQ(updated.Size(), 2U);
}

TEST(BaggageTest, SetOnTheEmptyBaggageCreatesTheEntry)
{
    const mt::Baggage bag = mt::Baggage{}.Set("a", "1");
    ASSERT_EQ(bag.Size(), 1U);
    EXPECT_EQ(bag.ToHeader(), "a=1");
}

TEST(BaggageTest, SetAppendsANewKey)
{
    EXPECT_EQ(mt::Baggage::FromHeader("a=1").Set("b", "2").ToHeader(), "a=1,b=2");
}

TEST(BaggageTest, SetReplacesAnExistingKeyInPlace)
{
    EXPECT_EQ(mt::Baggage::FromHeader("a=1,b=2,c=3").Set("b", "9").ToHeader(), "a=1,b=9,c=3");
}

TEST(BaggageTest, SetDropsThePropertiesOfTheEntryItReplaces)
{
    // The public surface can neither read nor write properties, so carrying
    // invisible metadata forward under a caller's new value would be a
    // surprise. An explicit overwrite replaces the whole list-member.
    EXPECT_EQ(mt::Baggage::FromHeader("a=1;p=x").Set("a", "2").ToHeader(), "a=2");
}

TEST(BaggageTest, SetPercentEncodesTheValue)
{
    const mt::Baggage bag = mt::Baggage{}.Set("a", "x y,z;w\\q\"p%");
    EXPECT_EQ(*bag.Get("a"), "x y,z;w\\q\"p%");
    EXPECT_EQ(bag.ToHeader(), "a=x%20y%2Cz%3Bw%5Cq%22p%25");
    EXPECT_EQ(*mt::Baggage::FromHeader(bag.ToHeader()).Get("a"), "x y,z;w\\q\"p%");
}

TEST(BaggageTest, SetAcceptsAnEmptyValue)
{
    EXPECT_EQ(mt::Baggage{}.Set("a", "").ToHeader(), "a=");
}

TEST(BaggageTest, SetRefusesAnInvalidKey)
{
    const mt::Baggage bag = mt::Baggage::FromHeader("a=1");
    EXPECT_EQ(bag.Set("", "x").ToHeader(), "a=1");
    EXPECT_EQ(bag.Set("b c", "x").ToHeader(), "a=1");
    EXPECT_EQ(bag.Set("b=c", "x").ToHeader(), "a=1");
    EXPECT_EQ(bag.Set("b,c", "x").ToHeader(), "a=1");
}

TEST(BaggageTest, SetRefusesAnOversizedMember)
{
    const mt::Baggage bag = mt::Baggage::FromHeader("a=1");
    const std::string fits(mt::Baggage::kMaxEntryBytes - 2U, 'v');
    const std::string overflows(mt::Baggage::kMaxEntryBytes - 1U, 'v');

    EXPECT_EQ(bag.Set("b", fits).Size(), 2U);
    EXPECT_EQ(bag.Set("b", overflows).Size(), 1U);
}

TEST(BaggageTest, SetCountsEncodedBytesAgainstTheMemberLimit)
{
    // Each space becomes three bytes, so a value that fits raw need not fit
    // once it is encoded.
    const std::string spaces((mt::Baggage::kMaxEntryBytes - 2U) / 2U, ' ');
    EXPECT_TRUE(mt::Baggage{}.Set("b", spaces).Empty());
}

TEST(BaggageTest, SetRefusesANewKeyAtTheEntryLimit)
{
    const mt::Baggage full = mt::Baggage::FromHeader(MembersHeader(mt::Baggage::kMaxEntries));
    ASSERT_EQ(full.Size(), mt::Baggage::kMaxEntries);

    EXPECT_EQ(full.Set("extra", "v").Size(), mt::Baggage::kMaxEntries);
    EXPECT_FALSE(full.Set("extra", "v").Get("extra").has_value());
}

TEST(BaggageTest, SetStillReplacesAnExistingKeyAtTheEntryLimit)
{
    const mt::Baggage full = mt::Baggage::FromHeader(MembersHeader(mt::Baggage::kMaxEntries));
    const mt::Baggage updated = full.Set("k0", "9");
    EXPECT_EQ(updated.Size(), mt::Baggage::kMaxEntries);
    EXPECT_EQ(*updated.Get("k0"), "9");
}

TEST(BaggageTest, SetRefusesToExceedTheTotalLimit)
{
    // Two members of 4000 bytes and their separator are 8001 bytes, so 191
    // more bytes fit and 193 do not — with every member well inside
    // `kMaxEntryBytes`, which isolates the total limit from the member limit.
    constexpr std::size_t kHalf = 4000U;
    const mt::Baggage bag =
        mt::Baggage::FromHeader(MemberOfLength("a", kHalf) + ',' + MemberOfLength("bb", kHalf));
    ASSERT_EQ(bag.Size(), 2U);

    const std::size_t used = (kHalf * 2U) + 1U;
    const std::size_t room = mt::Baggage::kMaxTotalBytes - used - 1U;  // minus the next separator
    const std::string fits(room - 2U, 'v');                            // "c=" is two of them
    const std::string overflows(room - 1U, 'v');

    EXPECT_EQ(bag.Set("c", fits).Size(), 3U);
    EXPECT_EQ(bag.Set("c", fits).ToHeader().size(), mt::Baggage::kMaxTotalBytes);
    EXPECT_EQ(bag.Set("c", overflows).Size(), 2U);
}

// ── Erase: copy-on-write ─────────────────────────────────────────────────────

TEST(BaggageTest, EraseLeavesTheReceiverUntouched)
{
    const mt::Baggage original = mt::Baggage::FromHeader("a=1,b=2");
    const mt::Baggage reduced = original.Erase("a");
    EXPECT_EQ(original.Size(), 2U);
    EXPECT_EQ(reduced.Size(), 1U);
}

TEST(BaggageTest, ErasePreservesTheOrderOfTheSurvivors)
{
    EXPECT_EQ(mt::Baggage::FromHeader("a=1,b=2,c=3").Erase("b").ToHeader(), "a=1,c=3");
}

TEST(BaggageTest, EraseOfAnAbsentKeyChangesNothing)
{
    const mt::Baggage bag = mt::Baggage::FromHeader("a=1");
    EXPECT_EQ(bag.Erase("zz").ToHeader(), "a=1");
    EXPECT_EQ(bag.Erase("").ToHeader(), "a=1");
}

TEST(BaggageTest, EraseOfTheLastEntryReturnsTheEmptyState)
{
    ExpectEmpty(mt::Baggage::FromHeader("a=1").Erase("a"));
}

TEST(BaggageTest, EraseOnTheEmptyBaggageIsEmpty)
{
    ExpectEmpty(mt::Baggage{}.Erase("a"));
}

}  // namespace
