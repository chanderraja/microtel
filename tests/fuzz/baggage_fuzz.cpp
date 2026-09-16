// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

// Fuzz harness for the W3C Baggage header parser.
//
// Required by the v1.1 ships-when gate clause 3 (ICP 0024 / roadmap §v1.1):
// "the baggage header parser has a fuzz target". The parser is reached by
// every inbound request that carries a `baggage` header, so its input is
// entirely attacker-controlled: an arbitrary byte string, of arbitrary
// length, with arbitrary percent-escapes.
//
// A harness that only looked for crashes would miss the failures that matter
// here, so three invariants are asserted outright:
//
//   1. **The limits hold for any input.** No parse may exceed kMaxEntries
//      members, kMaxEntryBytes for any one serialised member, or
//      kMaxTotalBytes for the whole serialised header. This is what stops a
//      hostile upstream from making microtel amplify a header it then
//      forwards.
//   2. **Serialisation round-trips.** Re-parsing what `ToHeader` produced must
//      yield the same header and the same member count. That is a strong
//      statement about the pair: the serialiser may only emit forms the parser
//      accepts (no escape it cannot read back, no member it would then drop),
//      and the parser may only accept forms that survive re-encoding.
//   3. **Reads agree with the serialised form.** Every key the parser kept is
//      still readable through `Get`, so `Get` and `ToHeader` cannot disagree
//      about what was propagated.
//
// The whole input is the header value — no prefix byte, because every byte
// pattern is a legal thing for a peer to send here and carving one out would
// just hide it from the mutator.
//
// Repro:
//   ./build-fuzz/tests/fuzz/baggage_fuzz <crash_file>

#include "microtel/baggage.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace
{

/// Asserts a documented bound. `abort` rather than a returned error: these are
/// the outcomes the harness exists to catch, and libFuzzer must see them.
void Require(bool condition)
{
    if (!condition)
    {
        std::abort();
    }
}

/// Serialised length of one list-member of @p header, which is the unit
/// `kMaxEntryBytes` bounds.
void RequireMemberLengths(std::string_view header)
{
    while (!header.empty())
    {
        const std::size_t comma = header.find(',');
        Require(header.substr(0U, comma).size() <= microtel::Baggage::kMaxEntryBytes);
        if (comma == std::string_view::npos)
        {
            return;
        }
        header = header.substr(comma + 1U);
    }
}

void RequireWithinLimits(const microtel::Baggage& bag)
{
    Require(bag.Size() <= microtel::Baggage::kMaxEntries);

    const std::string header = bag.ToHeader();
    Require(header.size() <= microtel::Baggage::kMaxTotalBytes);
    Require(header.empty() == bag.Empty());
    RequireMemberLengths(header);
}

/// Every key in the serialised form must still be readable through `Get`.
void RequireKeysReadable(const microtel::Baggage& bag, std::string_view header)
{
    while (!header.empty())
    {
        const std::size_t comma = header.find(',');
        const std::string_view member = header.substr(0U, comma);
        const std::size_t equals = member.find('=');
        Require(equals != std::string_view::npos);
        Require(bag.Get(member.substr(0U, equals)).has_value());

        if (comma == std::string_view::npos)
        {
            return;
        }
        header = header.substr(comma + 1U);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const std::string_view input(reinterpret_cast<const char*>(data), size);  // NOLINT

    const microtel::Baggage parsed = microtel::Baggage::FromHeader(input);
    RequireWithinLimits(parsed);

    const std::string once = parsed.ToHeader();
    RequireKeysReadable(parsed, once);

    // Round-trip: the canonical form must survive re-parsing unchanged, both
    // in text and in member count.
    const microtel::Baggage reparsed = microtel::Baggage::FromHeader(once);
    RequireWithinLimits(reparsed);
    Require(reparsed.ToHeader() == once);
    Require(reparsed.Size() == parsed.Size());

    return 0;
}
