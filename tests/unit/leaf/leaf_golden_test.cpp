// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Byte identity against the committed golden vectors
// (docs/leaf-concentrator-design.md §2.3, §7.2).
//
// Every vector in tests/leaf/vectors/ is encoded by the backend this binary is
// linked with and compared byte for byte with its committed `.bin`. Each golden
// file is also decoded with upb and checked field by field against the
// scenario, so a golden file cannot silently encode the wrong thing.
//
// To regenerate after an intended wire change, run the binary with
// MICROTEL_LEAF_WRITE_VECTORS=1 and review the diff (`protoc --decode`).

#include "microtel/leaf.h"

#include "leaf/vectors/leaf_vectors.h"
#include "leaf_decode.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace
{

namespace lt = microtel::leaf_test;

constexpr std::size_t kOutSize = 65536;
constexpr std::uint64_t kClockStart = 1000000000ULL;
constexpr std::uint64_t kClockStep = 1000ULL;
constexpr std::uint64_t kSyncUnix = 1700000000000000000ULL;
constexpr std::size_t kLongString = 1000;

std::string GoldenPath(const std::string& name)
{
    return std::string(MICROTEL_LEAF_VECTORS_DIR) + "/" + name + ".bin";
}

std::vector<std::uint8_t> ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> EncodeVector(std::size_t index)
{
    std::vector<std::uint8_t> out(kOutSize);
    std::size_t written = 0;
    EXPECT_EQ(microtel_leaf_test_vector_encode(index, out.data(), out.size(), &written),
              MICROTEL_LEAF_OK);
    out.resize(written);
    return out;
}

// The configured Resource and scope every vector shares.
// NOLINTNEXTLINE(readability-function-size): each gtest assertion counts as a branch.
void CheckCommon(const lt::DecodedPayload& p, std::int64_t time_mode)
{
    EXPECT_EQ(p.resource_spans_count, 1U);
    EXPECT_EQ(p.scope_spans_count, 1U);
    EXPECT_TRUE(p.has_resource);
    EXPECT_TRUE(p.has_scope);
    EXPECT_EQ(p.scope_name, "firmware");
    EXPECT_EQ(p.scope_version, "2.3.1");
    const std::vector<std::pair<std::string, std::string>> leading = {
        {"device.id", "leaf-0042"}, {"service.name", "thermostat"}};
    ASSERT_GE(p.resource.size(), 4U);
    for (std::size_t i = 0; i < leading.size(); ++i)
    {
        EXPECT_EQ(p.resource.at(i).key, leading.at(i).first);
        EXPECT_EQ(p.resource.at(i).s, leading.at(i).second);
    }
    EXPECT_EQ(p.ResourceAttr("microtel.leaf.proto").value_or(lt::DecodedKv{}).i, 1);
    EXPECT_EQ(p.resource.at(3).key, "microtel.leaf.time_mode");
    EXPECT_EQ(p.resource.at(3).i, time_mode);
}

void CheckOneSpan(const lt::DecodedPayload& p, std::int64_t time_mode)
{
    CheckCommon(p, time_mode);
    ASSERT_EQ(p.spans.size(), 1U);
    const auto& span = p.spans.front();
    EXPECT_EQ(span.trace_id.size(), 16U);
    EXPECT_EQ(span.span_id.size(), 8U);
    EXPECT_TRUE(span.parent_span_id.empty());
    EXPECT_EQ(span.kind, MICROTEL_LEAF_SPAN_KIND_INTERNAL);
}

// A reserved integer attribute, or a sentinel when it is missing.
std::int64_t Reserved(const lt::DecodedPayload& p, const std::string& key)
{
    constexpr std::int64_t kMissing = -12345;
    const auto kv = p.ResourceAttr(key);
    return kv.has_value() ? kv->i : kMissing;
}

void CheckEmptyBatch(const lt::DecodedPayload& p)
{
    CheckCommon(p, 0);
    EXPECT_TRUE(p.spans.empty());
    EXPECT_EQ(Reserved(p, "microtel.leaf.encode_time"), static_cast<std::int64_t>(kClockStart));
}

void CheckOneSpanVector(const lt::DecodedPayload& p)
{
    CheckOneSpan(p, 0);
    ASSERT_EQ(p.spans.size(), 1U);
    EXPECT_EQ(p.spans.front().name, "sensor.read");
    EXPECT_EQ(p.spans.front().start, kClockStart);
    EXPECT_EQ(p.spans.front().end, kClockStart + kClockStep);
    EXPECT_EQ(p.resource.size(), 5U);
}

// NOLINTNEXTLINE(readability-function-size): each gtest assertion counts as a branch.
void CheckAttributeTypes(const lt::DecodedPayload& p)
{
    CheckCommon(p, 0);
    ASSERT_EQ(p.spans.size(), 1U);
    EXPECT_EQ(p.spans.front().kind, MICROTEL_LEAF_SPAN_KIND_CLIENT);
    const auto& a = p.spans.front().attrs;
    ASSERT_EQ(a.size(), 8U);
    EXPECT_EQ(a.at(0).s, "text");
    EXPECT_EQ(a.at(1).i, 300);
    EXPECT_EQ(a.at(2).i, -1);
    EXPECT_EQ(a.at(3).type, MICROTEL_LEAF_VALUE_INT64);
    EXPECT_EQ(a.at(3).i, 0);
    EXPECT_DOUBLE_EQ(a.at(4).d, 1.5);
    EXPECT_EQ(a.at(5).type, MICROTEL_LEAF_VALUE_DOUBLE);
    EXPECT_TRUE(std::signbit(a.at(5).d));
    EXPECT_TRUE(a.at(6).b);
    EXPECT_EQ(a.at(7).type, MICROTEL_LEAF_VALUE_BOOL);
    EXPECT_FALSE(a.at(7).b);
}

// NOLINTNEXTLINE(readability-function-size): each gtest assertion counts as a branch.
void CheckEvents(const lt::DecodedPayload& p)
{
    CheckCommon(p, 0);
    ASSERT_EQ(p.spans.size(), 1U);
    const auto& ev = p.spans.front().events;
    ASSERT_EQ(ev.size(), 2U);
    EXPECT_EQ(ev.at(0).name, "retry");
    EXPECT_EQ(ev.at(0).time, kClockStart + kClockStep);
    ASSERT_EQ(ev.at(0).attrs.size(), 2U);
    EXPECT_EQ(ev.at(0).attrs.at(0).i, 2);
    EXPECT_EQ(ev.at(0).attrs.at(1).s, "timeout");
    EXPECT_EQ(ev.at(1).name, "done");
    EXPECT_TRUE(ev.at(1).attrs.empty());
}

// NOLINTNEXTLINE(readability-function-size): each gtest assertion counts as a branch.
void CheckStatus(const lt::DecodedPayload& p)
{
    CheckCommon(p, 0);
    ASSERT_EQ(p.spans.size(), 3U);
    EXPECT_EQ(p.spans.at(0).status_code, MICROTEL_LEAF_STATUS_ERROR);
    EXPECT_EQ(p.spans.at(0).status_message, "boom");
    EXPECT_TRUE(p.spans.at(1).has_status);
    EXPECT_EQ(p.spans.at(1).status_code, MICROTEL_LEAF_STATUS_OK);
    EXPECT_TRUE(p.spans.at(2).has_status);
    EXPECT_EQ(p.spans.at(2).status_code, MICROTEL_LEAF_STATUS_UNSET);
    EXPECT_EQ(p.spans.at(2).status_message, "note");
}

// NOLINTNEXTLINE(readability-function-size): each gtest assertion counts as a branch.
void CheckRemoteParent(const lt::DecodedPayload& p)
{
    constexpr std::uint8_t kTraceFirst = 0xa0;
    constexpr std::uint8_t kParentFirst = 0xb0;
    CheckCommon(p, 0);
    ASSERT_EQ(p.spans.size(), 2U);
    const auto& child = p.spans.at(0);
    const auto& remote = p.spans.at(1);
    EXPECT_EQ(child.name, "actuate");
    EXPECT_EQ(remote.name, "handle.cmd");
    EXPECT_EQ(static_cast<std::uint8_t>(remote.trace_id.at(0)), kTraceFirst);
    EXPECT_EQ(static_cast<std::uint8_t>(remote.parent_span_id.at(0)), kParentFirst);
    EXPECT_EQ(child.trace_id, remote.trace_id);
    EXPECT_EQ(child.parent_span_id, remote.span_id);
}

void CheckNoClock(const lt::DecodedPayload& p)
{
    CheckOneSpan(p, 0);
    ASSERT_EQ(p.spans.size(), 1U);
    EXPECT_EQ(p.spans.front().start, 0U);
    EXPECT_EQ(p.spans.front().end, 0U);
    EXPECT_FALSE(p.ResourceAttr("microtel.leaf.encode_time").has_value());
}

void CheckSyncRelative(const lt::DecodedPayload& p)
{
    CheckOneSpan(p, 1);
    ASSERT_EQ(p.spans.size(), 1U);
    EXPECT_EQ(p.spans.front().start, kSyncUnix + kClockStep);
    EXPECT_EQ(p.spans.front().end, kSyncUnix + (2 * kClockStep));
    EXPECT_EQ(Reserved(p, "microtel.leaf.sync_age"), static_cast<std::int64_t>(3 * kClockStep));
}

void CheckSyncUnsynced(const lt::DecodedPayload& p)
{
    CheckOneSpan(p, 0);
    EXPECT_FALSE(p.ResourceAttr("microtel.leaf.sync_age").has_value());
}

void CheckBootRelative(const lt::DecodedPayload& p)
{
    constexpr std::int64_t kBootId = 77;
    CheckOneSpan(p, 2);
    EXPECT_EQ(Reserved(p, "microtel.leaf.boot_id"), kBootId);
}

void CheckDroppedCounters(const lt::DecodedPayload& p)
{
    CheckCommon(p, 0);
    ASSERT_EQ(p.spans.size(), 1U);
    EXPECT_EQ(p.spans.front().attrs.size(), 1U);
    EXPECT_EQ(p.spans.front().events.size(), 1U);
    EXPECT_EQ(Reserved(p, "microtel.leaf.dropped_items"), 2);
    EXPECT_FALSE(p.ResourceAttr("microtel.leaf.dropped_spans").has_value());
}

void CheckMaxStrings(const lt::DecodedPayload& p)
{
    constexpr std::size_t kAttrs = 16;
    CheckCommon(p, 0);
    ASSERT_EQ(p.spans.size(), 1U);
    const auto& span = p.spans.front();
    EXPECT_EQ(span.name, std::string(kLongString, 'a'));
    ASSERT_EQ(span.attrs.size(), kAttrs);
    EXPECT_EQ(span.attrs.front().s, std::string(kLongString, 'b'));
    EXPECT_EQ(span.status_message, std::string(kLongString, 'b'));
}

void CheckUtf8(const lt::DecodedPayload& p)
{
    CheckCommon(p, 0);
    ASSERT_EQ(p.spans.size(), 1U);
    const auto& span = p.spans.front();
    EXPECT_EQ(span.name, "caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x98\x80");
    ASSERT_EQ(span.attrs.size(), 1U);
    EXPECT_EQ(span.attrs.front().key, "\xc3\xbc");
    EXPECT_EQ(span.attrs.front().s, "\xef\xbf\xbd\xf4\x8f\xbf\xbf");
}

using Check = void (*)(const lt::DecodedPayload&);

const std::map<std::string, Check>& Checks()
{
    static const std::map<std::string, Check> kChecks = {
        {"empty_batch", &CheckEmptyBatch},
        {"one_span", &CheckOneSpanVector},
        {"attribute_types", &CheckAttributeTypes},
        {"events", &CheckEvents},
        {"status", &CheckStatus},
        {"remote_parent", &CheckRemoteParent},
        {"time_no_clock", &CheckNoClock},
        {"time_sync_relative", &CheckSyncRelative},
        {"time_sync_unsynced", &CheckSyncUnsynced},
        {"time_boot_relative", &CheckBootRelative},
        {"dropped_counters", &CheckDroppedCounters},
        {"max_strings", &CheckMaxStrings},
        {"utf8", &CheckUtf8},
    };
    return kChecks;
}

TEST(LeafGoldenTest, EveryVectorHasAStructuralCheck)
{
    EXPECT_EQ(Checks().size(), microtel_leaf_test_vector_count());
    for (std::size_t i = 0; i < microtel_leaf_test_vector_count(); ++i)
    {
        EXPECT_EQ(Checks().count(microtel_leaf_test_vector_name(i)), 1U)
            << microtel_leaf_test_vector_name(i);
    }
    EXPECT_EQ(microtel_leaf_test_vector_name(microtel_leaf_test_vector_count()), nullptr);
    std::size_t written = 0;
    EXPECT_EQ(
        microtel_leaf_test_vector_encode(microtel_leaf_test_vector_count(), nullptr, 0, &written),
        MICROTEL_LEAF_ERR_ARG);
}

TEST(LeafGoldenTest, EncodingIsDeterministic)
{
    for (std::size_t i = 0; i < microtel_leaf_test_vector_count(); ++i)
    {
        EXPECT_EQ(EncodeVector(i), EncodeVector(i)) << microtel_leaf_test_vector_name(i);
    }
}

TEST(LeafGoldenTest, MatchesCommittedGoldenFiles)
{
    const char* const write = std::getenv("MICROTEL_LEAF_WRITE_VECTORS");
    for (std::size_t i = 0; i < microtel_leaf_test_vector_count(); ++i)
    {
        const std::string name = microtel_leaf_test_vector_name(i);
        const auto bytes = EncodeVector(i);
        if (write != nullptr)
        {
            std::ofstream out(GoldenPath(name), std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
            continue;
        }
        const auto golden = ReadFile(GoldenPath(name));
        ASSERT_FALSE(golden.empty()) << "missing golden file " << GoldenPath(name);
        EXPECT_EQ(bytes, golden) << name;
    }
    if (write != nullptr)
    {
        GTEST_SKIP() << "golden files written to " << MICROTEL_LEAF_VECTORS_DIR;
    }
}

TEST(LeafGoldenTest, GoldenFilesDecodeToTheirScenario)
{
    for (std::size_t i = 0; i < microtel_leaf_test_vector_count(); ++i)
    {
        const std::string name = microtel_leaf_test_vector_name(i);
        SCOPED_TRACE(name);
        const auto golden = ReadFile(GoldenPath(name));
        ASSERT_FALSE(golden.empty());
        const auto p = lt::Decode(golden.data(), golden.size());
        ASSERT_TRUE(p.has_value());
        Checks().at(name)(*p);
    }
}

}  // namespace
