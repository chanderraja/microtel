// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Leaf C API unit tests (docs/leaf-concentrator-design.md §7.1), run against
// the backend the binary is linked with: this source builds both
// microtel_leaf_upb_test and microtel_leaf_nanopb_test, which see
// MICROTEL_LEAF_TEST_BACKEND_UPB or MICROTEL_LEAF_TEST_BACKEND_NANOPB. Every
// payload is decoded with upb and checked field by field.

#include "microtel/leaf.h"

#include "microtel/version.hpp"

#include "leaf_decode.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace
{

namespace lt = microtel::leaf_test;

constexpr std::size_t kRecordSize = 4096;
constexpr std::size_t kOutSize = 8192;
constexpr std::uint64_t kClockStart = 1000000;
constexpr std::uint64_t kClockStep = 1000;
constexpr std::uint8_t kCanary = 0xA5;

// Test clock: every read advances by kClockStep.
struct FakeClock
{
    std::uint64_t now = kClockStart;
};

std::uint64_t ReadClock(void* ctx)
{
    auto* clock = static_cast<FakeClock*>(ctx);
    const std::uint64_t t = clock->now;
    clock->now += kClockStep;
    return t;
}

// A weak random source: always the same byte.
struct ConstantRandom
{
    std::uint8_t value = 0;
};

void FillConstant(void* ctx, std::uint8_t* out, std::size_t len)
{
    std::memset(out, static_cast<const ConstantRandom*>(ctx)->value, len);
}

// A scripted random source: returns the next queued 64-bit words (big-endian
// byte order is irrelevant here: the leaf reads them little-endian), then 0.
struct ScriptedRandom
{
    std::vector<std::uint64_t> words;
    std::size_t next = 0;
};

void FillScripted(void* ctx, std::uint8_t* out, std::size_t len)
{
    auto* r = static_cast<ScriptedRandom*>(ctx);
    std::uint64_t w = 0;
    if (r->next < r->words.size())
    {
        w = r->words[r->next++];
    }
    for (std::size_t i = 0; i < len; ++i)
    {
        out[i] = static_cast<std::uint8_t>(w >> (8U * (i % 8U)));
    }
}

microtel_leaf_kv_t StrKv(const char* key, const char* value)
{
    microtel_leaf_kv_t kv{};
    kv.key = key;
    kv.key_len = std::strlen(key);
    kv.type = MICROTEL_LEAF_VALUE_STRING;
    kv.value.s.ptr = value;
    kv.value.s.len = std::strlen(value);
    return kv;
}

microtel_leaf_kv_t IntKv(const char* key, std::int64_t value)
{
    microtel_leaf_kv_t kv{};
    kv.key = key;
    kv.key_len = std::strlen(key);
    kv.type = MICROTEL_LEAF_VALUE_INT64;
    kv.value.i = value;
    return kv;
}

microtel_leaf_kv_t DoubleKv(const char* key, double value)
{
    microtel_leaf_kv_t kv{};
    kv.key = key;
    kv.key_len = std::strlen(key);
    kv.type = MICROTEL_LEAF_VALUE_DOUBLE;
    kv.value.d = value;
    return kv;
}

microtel_leaf_kv_t BoolKv(const char* key, int value)
{
    microtel_leaf_kv_t kv{};
    kv.key = key;
    kv.key_len = std::strlen(key);
    kv.type = MICROTEL_LEAF_VALUE_BOOL;
    kv.value.b = value;
    return kv;
}

// A leaf with a fake clock, a constant random source and one Resource
// attribute, in a record buffer of `record_size` bytes.
class LeafTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_resource = {StrKv("device.id", "dev-1")};
        m_config.struct_size = sizeof(microtel_leaf_config_t);
        m_config.time_mode = MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED;
        m_config.now_ns = &ReadClock;
        m_config.clock_ctx = &m_clock;
        m_config.random_bytes = &FillConstant;
        m_config.random_ctx = &m_random;
        m_config.resource = m_resource.data();
        m_config.resource_count = m_resource.size();
        m_config.scope_name = "fw";
        m_config.scope_name_len = 2;
        m_config.scope_version = "1.0";
        m_config.scope_version_len = 3;
        m_random.value = 0x11;
    }

    void TearDown() override
    {
        microtel_leaf_free(&m_leaf);
    }

    microtel_leaf_status_t Init(std::size_t record_size = kRecordSize)
    {
        m_record.assign(record_size, 0);
        return microtel_leaf_init(
            &m_leaf, sizeof(m_leaf), &m_config, m_record.data(), m_record.size());
    }

    microtel_leaf_span_t Start(const char* name, const microtel_leaf_span_t* parent = nullptr)
    {
        microtel_leaf_span_t s = 0;
        EXPECT_EQ(
            microtel_leaf_span_start(
                &m_leaf, &s, name, std::strlen(name), MICROTEL_LEAF_SPAN_KIND_INTERNAL, parent),
            MICROTEL_LEAF_OK);
        return s;
    }

    // Encodes into a buffer and decodes the result; fails the test on error.
    lt::DecodedPayload EncodeAndDecode()
    {
        std::vector<std::uint8_t> out(kOutSize);
        std::size_t written = 0;
        EXPECT_EQ(microtel_leaf_encode(&m_leaf, out.data(), out.size(), &written),
                  MICROTEL_LEAF_OK);
        return lt::DecodeOrFail(out.data(), written);
    }

    std::vector<std::uint8_t> EncodeBytes()
    {
        std::vector<std::uint8_t> out(kOutSize);
        std::size_t written = 0;
        EXPECT_EQ(microtel_leaf_encode(&m_leaf, out.data(), out.size(), &written),
                  MICROTEL_LEAF_OK);
        out.resize(written);
        return out;
    }

    microtel_leaf_counters_t Counters()
    {
        microtel_leaf_counters_t c{};
        microtel_leaf_get_counters(&m_leaf, &c, sizeof(c));
        return c;
    }

    microtel_leaf_t m_leaf{};
    microtel_leaf_config_t m_config{};
    std::vector<microtel_leaf_kv_t> m_resource;
    std::vector<std::uint8_t> m_record;
    FakeClock m_clock;
    ConstantRandom m_random;
};

// ---------------------------------------------------------------------------
// Version and lifecycle
// ---------------------------------------------------------------------------

TEST(LeafVersionTest, LibraryHeaderAndProjectVersionAgree)
{
    EXPECT_EQ(microtel_leaf_version(), MICROTEL_LEAF_VERSION);
    EXPECT_EQ(MICROTEL_LEAF_VERSION_MAJOR, microtel::kVersionMajor);
    EXPECT_EQ(MICROTEL_LEAF_VERSION_MINOR, microtel::kVersionMinor);
    EXPECT_EQ(MICROTEL_LEAF_VERSION_PATCH, microtel::kVersionPatch);
}

TEST_F(LeafTest, Init_Succeeds)
{
    EXPECT_EQ(Init(), MICROTEL_LEAF_OK);
}

TEST_F(LeafTest, Init_NullArguments_AreErrArg)
{
    m_record.assign(kRecordSize, 0);
    EXPECT_EQ(microtel_leaf_init(nullptr, sizeof(m_leaf), &m_config, m_record.data(), kRecordSize),
              MICROTEL_LEAF_ERR_ARG);
    EXPECT_EQ(microtel_leaf_init(&m_leaf, sizeof(m_leaf), nullptr, m_record.data(), kRecordSize),
              MICROTEL_LEAF_ERR_ARG);
    EXPECT_EQ(microtel_leaf_init(&m_leaf, sizeof(m_leaf), &m_config, nullptr, kRecordSize),
              MICROTEL_LEAF_ERR_ARG);
}

TEST_F(LeafTest, Init_LeafSizeOneByteShort_IsErrArgAndTouchesNothing)
{
    std::vector<std::uint8_t> leaf_mem(sizeof(microtel_leaf_t), kCanary);
    std::vector<std::uint8_t> record(kRecordSize, kCanary);
    auto* leaf = reinterpret_cast<microtel_leaf_t*>(leaf_mem.data());

    EXPECT_EQ(microtel_leaf_init(
                  leaf, sizeof(microtel_leaf_t) - 1, &m_config, record.data(), record.size()),
              MICROTEL_LEAF_ERR_ARG);

    for (const std::uint8_t b : leaf_mem)
    {
        ASSERT_EQ(b, kCanary);
    }
    for (const std::uint8_t b : record)
    {
        ASSERT_EQ(b, kCanary);
    }
}

TEST_F(LeafTest, Init_StructSizeOlderThanLibrary_IsErrArg)
{
    m_config.struct_size = sizeof(microtel_leaf_config_t) - 1;
    EXPECT_EQ(Init(), MICROTEL_LEAF_ERR_ARG);
    m_config.struct_size = 0;
    EXPECT_EQ(Init(), MICROTEL_LEAF_ERR_ARG);
}

TEST_F(LeafTest, Init_StructSizeNewerThanLibrary_IsAccepted)
{
    // A newer header's config: the library reads only the fields it knows.
    struct Newer
    {
        microtel_leaf_config_t base;
        std::uint64_t appended;
    } newer{};
    newer.base = m_config;
    newer.base.struct_size = sizeof(Newer);
    newer.appended = ~0ULL;
    m_record.assign(kRecordSize, 0);
    EXPECT_EQ(
        microtel_leaf_init(&m_leaf, sizeof(m_leaf), &newer.base, m_record.data(), kRecordSize),
        MICROTEL_LEAF_OK);
}

TEST_F(LeafTest, Init_InvalidConfigFields_AreErrArg)
{
    const microtel_leaf_config_t good = m_config;

    m_config.random_bytes = nullptr;
    EXPECT_EQ(Init(), MICROTEL_LEAF_ERR_ARG);
    m_config = good;

    m_config.time_mode = static_cast<microtel_leaf_time_mode_t>(3);
    EXPECT_EQ(Init(), MICROTEL_LEAF_ERR_ARG);
    m_config = good;

    m_config.now_ns = nullptr;
    m_config.time_mode = MICROTEL_LEAF_TIME_SYNC_RELATIVE;
    EXPECT_EQ(Init(), MICROTEL_LEAF_ERR_ARG);
    m_config.time_mode = MICROTEL_LEAF_TIME_BOOT_RELATIVE;
    EXPECT_EQ(Init(), MICROTEL_LEAF_ERR_ARG);
    m_config = good;

    m_config.resource = nullptr;
    EXPECT_EQ(Init(), MICROTEL_LEAF_ERR_ARG);
    m_config = good;

    m_config.scope_name = nullptr;
    EXPECT_EQ(Init(), MICROTEL_LEAF_ERR_ARG);
    m_config = good;

    m_config.scope_version = nullptr;
    EXPECT_EQ(Init(), MICROTEL_LEAF_ERR_ARG);
    m_config = good;

    std::array<std::uint8_t, 64> scratch{};
    m_config.scratch = scratch.data();
    m_config.scratch_size = 0;
    EXPECT_EQ(Init(), MICROTEL_LEAF_ERR_ARG);
    m_config.scratch = nullptr;
    m_config.scratch_size = scratch.size();
    EXPECT_EQ(Init(), MICROTEL_LEAF_ERR_ARG);
    m_config = good;

    EXPECT_EQ(Init(), MICROTEL_LEAF_OK);
}

TEST_F(LeafTest, Init_InvalidResourceAttributes_AreErrArg)
{
    const auto check = [this](const microtel_leaf_kv_t& kv)
    {
        m_resource = {kv};
        m_config.resource = m_resource.data();
        return Init();
    };

    microtel_leaf_kv_t empty_key = StrKv("k", "v");
    empty_key.key_len = 0;
    EXPECT_EQ(check(empty_key), MICROTEL_LEAF_ERR_ARG);

    microtel_leaf_kv_t null_key = StrKv("k", "v");
    null_key.key = nullptr;
    EXPECT_EQ(check(null_key), MICROTEL_LEAF_ERR_ARG);

    microtel_leaf_kv_t bad_type = StrKv("k", "v");
    bad_type.type = static_cast<microtel_leaf_value_type_t>(4);
    EXPECT_EQ(check(bad_type), MICROTEL_LEAF_ERR_ARG);

    microtel_leaf_kv_t null_string = StrKv("k", "v");
    null_string.value.s.ptr = nullptr;
    EXPECT_EQ(check(null_string), MICROTEL_LEAF_ERR_ARG);

    EXPECT_EQ(check(IntKv("microtel.leaf.proto", 2)), MICROTEL_LEAF_ERR_ARG);

    EXPECT_EQ(check(StrKv("k", "v")), MICROTEL_LEAF_OK);
}

TEST_F(LeafTest, Init_BufferTooSmallForResource_IsNoSpaceAndTouchesNothing)
{
    std::vector<std::uint8_t> record(16, kCanary);
    EXPECT_EQ(microtel_leaf_init(&m_leaf, sizeof(m_leaf), &m_config, record.data(), record.size()),
              MICROTEL_LEAF_ERR_NO_SPACE);
    for (const std::uint8_t b : record)
    {
        ASSERT_EQ(b, kCanary);
    }
    EXPECT_EQ(microtel_leaf_encoded_size(&m_leaf), 0U);
}

TEST_F(LeafTest, UseAfterFree_IsErrState)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    microtel_leaf_free(&m_leaf);
    microtel_leaf_free(&m_leaf);  // a second free is harmless
    microtel_leaf_free(nullptr);

    microtel_leaf_span_t out = 0;
    EXPECT_EQ(
        microtel_leaf_span_start(&m_leaf, &out, "b", 1, MICROTEL_LEAF_SPAN_KIND_INTERNAL, nullptr),
        MICROTEL_LEAF_ERR_STATE);
    EXPECT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_ERR_STATE);
    std::array<std::uint8_t, 64> buf{};
    std::size_t written = 0;
    EXPECT_EQ(microtel_leaf_encode(&m_leaf, buf.data(), buf.size(), &written),
              MICROTEL_LEAF_ERR_STATE);
    EXPECT_EQ(microtel_leaf_clock_sync(&m_leaf, 1, 1), MICROTEL_LEAF_ERR_STATE);
    EXPECT_EQ(microtel_leaf_encoded_size(&m_leaf), 0U);
}

TEST_F(LeafTest, NeverInitialisedLeaf_IsErrState)
{
    microtel_leaf_t zeroed{};
    microtel_leaf_span_t out = 0;
    EXPECT_EQ(
        microtel_leaf_span_start(&zeroed, &out, "b", 1, MICROTEL_LEAF_SPAN_KIND_INTERNAL, nullptr),
        MICROTEL_LEAF_ERR_STATE);
    EXPECT_EQ(
        microtel_leaf_span_start(nullptr, &out, "b", 1, MICROTEL_LEAF_SPAN_KIND_INTERNAL, nullptr),
        MICROTEL_LEAF_ERR_ARG);
    std::array<std::uint8_t, 16> trace{};
    trace.fill(1);
    std::array<std::uint8_t, 8> parent{};
    parent.fill(1);
    EXPECT_EQ(
        microtel_leaf_span_start_remote(
            &zeroed, &out, "b", 1, MICROTEL_LEAF_SPAN_KIND_INTERNAL, trace.data(), parent.data()),
        MICROTEL_LEAF_ERR_STATE);
}

TEST_F(LeafTest, GetCounters_WritesOnlyOutSizeBytes)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    std::array<std::uint8_t, sizeof(microtel_leaf_counters_t)> out{};
    out.fill(kCanary);
    microtel_leaf_get_counters(&m_leaf, reinterpret_cast<microtel_leaf_counters_t*>(out.data()), 4);
    for (std::size_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(out.at(i), 0);
    }
    for (std::size_t i = 4; i < out.size(); ++i)
    {
        EXPECT_EQ(out.at(i), kCanary);
    }
    // Harmless on NULL arguments.
    microtel_leaf_get_counters(nullptr, reinterpret_cast<microtel_leaf_counters_t*>(out.data()), 4);
    microtel_leaf_get_counters(&m_leaf, nullptr, 4);
}

// ---------------------------------------------------------------------------
// Ids (§1.6.1)
// ---------------------------------------------------------------------------

// The documented derivation, restated here so the test pins it.
std::uint64_t Fnv1a(std::uint64_t h, const void* data, std::size_t len)
{
    constexpr std::uint64_t kPrime = 0x100000001b3ULL;
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < len; ++i)
    {
        h ^= p[i];
        h *= kPrime;
    }
    return h;
}

std::uint64_t SplitMix64(std::uint64_t x)
{
    std::uint64_t z = x + 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31U);
}

std::string IdBytes(std::uint64_t word)
{
    std::string s(8, '\0');
    for (std::size_t i = 0; i < 8; ++i)
    {
        s[i] = static_cast<char>(word >> (8U * (7U - i)));
    }
    return s;
}

// device_key for a Resource of string attributes and a boot_id.
std::uint64_t DeviceKey(const std::vector<std::pair<std::string, std::string>>& resource,
                        std::uint32_t boot_id)
{
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const auto& [k, v] : resource)
    {
        const std::uint8_t type = MICROTEL_LEAF_VALUE_STRING;
        h = Fnv1a(h, k.data(), k.size());
        h = Fnv1a(h, &type, 1);
        h = Fnv1a(h, v.data(), v.size());
    }
    std::array<std::uint8_t, 4> boot{};
    for (std::size_t i = 0; i < boot.size(); ++i)
    {
        boot.at(i) = static_cast<std::uint8_t>(boot_id >> (8U * i));
    }
    h = Fnv1a(h, boot.data(), boot.size());
    return SplitMix64(h);
}

TEST_F(LeafTest, Ids_FollowTheDocumentedDerivation)
{
    m_random.value = 0;
    m_config.boot_id = 7;
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 1U);

    const std::uint64_t key = DeviceKey({{"device.id", "dev-1"}}, 7);
    EXPECT_EQ(p.spans[0].trace_id, IdBytes(SplitMix64(key + 0)) + IdBytes(SplitMix64(key + 1)));
    EXPECT_EQ(p.spans[0].span_id, IdBytes(SplitMix64(key + 2)));
}

TEST_F(LeafTest, Ids_ScalarResourceValuesAreHashedByTheirBytes)
{
    // Scalars enter the device key as their little-endian bytes (one byte for
    // a bool), after the key and the type byte.
    m_random.value = 0;
    m_resource = {IntKv("n", -2), BoolKv("b", 5), DoubleKv("d", 0.5)};
    m_config.resource = m_resource.data();
    m_config.resource_count = m_resource.size();
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 1U);

    const auto le = [](std::uint64_t v, std::size_t n)
    {
        std::string out(n, '\0');
        for (std::size_t i = 0; i < n; ++i)
        {
            out.at(i) = static_cast<char>(v >> (8U * i));
        }
        return out;
    };
    std::uint64_t dbits = 0;
    const double half = 0.5;
    std::memcpy(&dbits, &half, sizeof(dbits));
    const std::string bytes =
        std::string("n") + static_cast<char>(MICROTEL_LEAF_VALUE_INT64) +
        le(static_cast<std::uint64_t>(-2), 8) + "b" + static_cast<char>(MICROTEL_LEAF_VALUE_BOOL) +
        le(1, 1) + "d" + static_cast<char>(MICROTEL_LEAF_VALUE_DOUBLE) + le(dbits, 8) + le(0, 4);
    const std::uint64_t key = SplitMix64(Fnv1a(0xcbf29ce484222325ULL, bytes.data(), bytes.size()));
    EXPECT_EQ(p.spans[0].span_id, IdBytes(SplitMix64(key + 2)));
}

TEST_F(LeafTest, Ids_AllZeroResultIsRetried)
{
    // Script random words that cancel the first trace id exactly.
    const std::uint64_t key = DeviceKey({{"device.id", "dev-1"}}, 0);
    ScriptedRandom scripted;
    scripted.words = {SplitMix64(key + 0), SplitMix64(key + 1)};
    m_config.random_bytes = &FillScripted;
    m_config.random_ctx = &scripted;
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 1U);
    EXPECT_EQ(p.spans[0].trace_id, IdBytes(SplitMix64(key + 2)) + IdBytes(SplitMix64(key + 3)));
    EXPECT_NE(p.spans[0].trace_id, std::string(16, '\0'));
}

// Collects every trace and span id from `spans` spans of one leaf.
std::set<std::string> CollectIds(microtel_leaf_config_t config, int spans)
{
    std::vector<std::uint8_t> record(kRecordSize * 4);
    microtel_leaf_t leaf{};
    std::set<std::string> ids;
    if (microtel_leaf_init(&leaf, sizeof(leaf), &config, record.data(), record.size()) !=
        MICROTEL_LEAF_OK)
    {
        return ids;
    }
    for (int i = 0; i < spans; ++i)
    {
        microtel_leaf_span_t s = 0;
        microtel_leaf_span_start(&leaf, &s, "s", 1, MICROTEL_LEAF_SPAN_KIND_INTERNAL, nullptr);
        microtel_leaf_span_end(&leaf, s);
    }
    std::vector<std::uint8_t> out(kOutSize * 4);
    std::size_t written = 0;
    microtel_leaf_encode(&leaf, out.data(), out.size(), &written);
    const auto p = lt::Decode(out.data(), written);
    for (const auto& span : p.value_or(lt::DecodedPayload{}).spans)
    {
        ids.insert(span.trace_id);
        ids.insert(span.span_id);
    }
    microtel_leaf_free(&leaf);
    return ids;
}

TEST_F(LeafTest, Ids_IdenticalRandomDifferentResources_AreDisjoint)
{
    constexpr int kSpans = 20;
    m_random.value = 0;  // the same, weak source on both devices
    const std::array<microtel_leaf_kv_t, 1> a = {StrKv("device.id", "A")};
    const std::array<microtel_leaf_kv_t, 1> b = {StrKv("device.id", "B")};
    microtel_leaf_config_t ca = m_config;
    ca.resource = a.data();
    microtel_leaf_config_t cb = m_config;
    cb.resource = b.data();
    FakeClock clock_b;
    cb.clock_ctx = &clock_b;

    const auto ids_a = CollectIds(ca, kSpans);
    const auto ids_b = CollectIds(cb, kSpans);
    ASSERT_EQ(ids_a.size(), 2U * kSpans);
    ASSERT_EQ(ids_b.size(), 2U * kSpans);
    for (const auto& id : ids_a)
    {
        EXPECT_EQ(ids_b.count(id), 0U);
    }
}

TEST_F(LeafTest, Ids_IdenticalRandomAndResourceDifferentBootId_AreDisjoint)
{
    constexpr int kSpans = 20;
    m_random.value = 0;
    microtel_leaf_config_t ca = m_config;
    ca.boot_id = 1;
    microtel_leaf_config_t cb = m_config;
    cb.boot_id = 2;

    const auto ids_a = CollectIds(ca, kSpans);
    const auto ids_b = CollectIds(cb, kSpans);
    ASSERT_EQ(ids_a.size(), 2U * kSpans);
    ASSERT_EQ(ids_b.size(), 2U * kSpans);
    for (const auto& id : ids_a)
    {
        EXPECT_EQ(ids_b.count(id), 0U);
    }
}

// ---------------------------------------------------------------------------
// Building spans
// ---------------------------------------------------------------------------

TEST_F(LeafTest, OneSpan_EncodesEveryField)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    microtel_leaf_span_t s = 0;
    ASSERT_EQ(
        microtel_leaf_span_start(&m_leaf, &s, "read", 4, MICROTEL_LEAF_SPAN_KIND_CLIENT, nullptr),
        MICROTEL_LEAF_OK);
    EXPECT_NE(s, 0U);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);

    const auto p = EncodeAndDecode();
    EXPECT_EQ(p.resource_spans_count, 1U);
    EXPECT_TRUE(p.has_resource);
    EXPECT_EQ(p.scope_spans_count, 1U);
    EXPECT_TRUE(p.has_scope);
    EXPECT_EQ(p.scope_name, "fw");
    EXPECT_EQ(p.scope_version, "1.0");
    ASSERT_EQ(p.spans.size(), 1U);
    const auto& span = p.spans[0];
    EXPECT_EQ(span.name, "read");
    EXPECT_EQ(span.kind, MICROTEL_LEAF_SPAN_KIND_CLIENT);
    EXPECT_EQ(span.trace_id.size(), 16U);
    EXPECT_EQ(span.span_id.size(), 8U);
    EXPECT_TRUE(span.parent_span_id.empty());
    EXPECT_EQ(span.start, kClockStart);
    EXPECT_EQ(span.end, kClockStart + kClockStep);
    EXPECT_FALSE(span.has_status);

    // The configured Resource comes first, then the reserved attributes.
    ASSERT_GE(p.resource.size(), 3U);
    EXPECT_EQ(p.resource[0].key, "device.id");
    EXPECT_EQ(p.resource[0].s, "dev-1");
    EXPECT_EQ(p.resource[1].key, "microtel.leaf.proto");
    EXPECT_EQ(p.resource[1].i, 1);
    EXPECT_EQ(p.resource[2].key, "microtel.leaf.time_mode");
    EXPECT_EQ(p.resource[2].type, MICROTEL_LEAF_VALUE_INT64);
    EXPECT_EQ(p.resource[2].i, 0);
}

TEST_F(LeafTest, SpanStart_InvalidArguments)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    microtel_leaf_span_t s = 0;
    EXPECT_EQ(microtel_leaf_span_start(
                  &m_leaf, nullptr, "a", 1, MICROTEL_LEAF_SPAN_KIND_INTERNAL, nullptr),
              MICROTEL_LEAF_ERR_ARG);
    EXPECT_EQ(microtel_leaf_span_start(
                  &m_leaf, &s, nullptr, 1, MICROTEL_LEAF_SPAN_KIND_INTERNAL, nullptr),
              MICROTEL_LEAF_ERR_ARG);
    EXPECT_EQ(microtel_leaf_span_start(
                  &m_leaf, &s, "a", 1, static_cast<microtel_leaf_span_kind_t>(0), nullptr),
              MICROTEL_LEAF_ERR_ARG);
    EXPECT_EQ(microtel_leaf_span_start(
                  &m_leaf, &s, "a", 1, static_cast<microtel_leaf_span_kind_t>(6), nullptr),
              MICROTEL_LEAF_ERR_ARG);
    const microtel_leaf_span_t bogus = 0;
    EXPECT_EQ(
        microtel_leaf_span_start(&m_leaf, &s, "a", 1, MICROTEL_LEAF_SPAN_KIND_INTERNAL, &bogus),
        MICROTEL_LEAF_ERR_STATE);
    // An empty name is allowed.
    EXPECT_EQ(microtel_leaf_span_start(
                  &m_leaf, &s, nullptr, 0, MICROTEL_LEAF_SPAN_KIND_INTERNAL, nullptr),
              MICROTEL_LEAF_OK);
    EXPECT_EQ(Counters().dropped_spans, 0U);
}

TEST_F(LeafTest, ChildSpan_InheritsTraceAndParentIds)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t parent = Start("parent");
    const microtel_leaf_span_t child = Start("child", &parent);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, parent), MICROTEL_LEAF_OK);
    // An ended (not yet encoded) span can still be a parent.
    const microtel_leaf_span_t late = Start("late", &parent);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, child), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, late), MICROTEL_LEAF_OK);

    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 3U);
    // End order: parent, child, late.
    EXPECT_EQ(p.spans[0].name, "parent");
    EXPECT_EQ(p.spans[1].name, "child");
    EXPECT_EQ(p.spans[2].name, "late");
    EXPECT_EQ(p.spans[1].trace_id, p.spans[0].trace_id);
    EXPECT_EQ(p.spans[1].parent_span_id, p.spans[0].span_id);
    EXPECT_EQ(p.spans[2].parent_span_id, p.spans[0].span_id);
    EXPECT_NE(p.spans[1].span_id, p.spans[0].span_id);

    // After the encode the parent's handle is stale.
    microtel_leaf_span_t s = 0;
    EXPECT_EQ(
        microtel_leaf_span_start(&m_leaf, &s, "x", 1, MICROTEL_LEAF_SPAN_KIND_INTERNAL, &parent),
        MICROTEL_LEAF_ERR_STATE);
}

TEST_F(LeafTest, RemoteParent_IsUsed)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    std::array<std::uint8_t, 16> trace{};
    std::array<std::uint8_t, 8> parent{};
    for (std::size_t i = 0; i < trace.size(); ++i)
    {
        trace.at(i) = static_cast<std::uint8_t>(i + 1);
    }
    parent.fill(0x42);
    microtel_leaf_span_t s = 0;
    ASSERT_EQ(
        microtel_leaf_span_start_remote(
            &m_leaf, &s, "cmd", 3, MICROTEL_LEAF_SPAN_KIND_SERVER, trace.data(), parent.data()),
        MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 1U);
    EXPECT_EQ(p.spans[0].trace_id, std::string(trace.begin(), trace.end()));
    EXPECT_EQ(p.spans[0].parent_span_id, std::string(parent.begin(), parent.end()));
    EXPECT_EQ(p.spans[0].kind, MICROTEL_LEAF_SPAN_KIND_SERVER);
}

TEST_F(LeafTest, RemoteParent_InvalidIds_AreErrArg)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    std::array<std::uint8_t, 16> trace{};
    std::array<std::uint8_t, 8> parent{};
    parent.fill(1);
    microtel_leaf_span_t s = 0;
    EXPECT_EQ(microtel_leaf_span_start_remote(
                  &m_leaf, &s, "c", 1, MICROTEL_LEAF_SPAN_KIND_SERVER, trace.data(), parent.data()),
              MICROTEL_LEAF_ERR_ARG);
    trace.fill(1);
    parent.fill(0);
    EXPECT_EQ(microtel_leaf_span_start_remote(
                  &m_leaf, &s, "c", 1, MICROTEL_LEAF_SPAN_KIND_SERVER, trace.data(), parent.data()),
              MICROTEL_LEAF_ERR_ARG);
    EXPECT_EQ(microtel_leaf_span_start_remote(
                  &m_leaf, &s, "c", 1, MICROTEL_LEAF_SPAN_KIND_SERVER, nullptr, parent.data()),
              MICROTEL_LEAF_ERR_ARG);
    EXPECT_EQ(microtel_leaf_span_start_remote(
                  &m_leaf, &s, "c", 1, MICROTEL_LEAF_SPAN_KIND_SERVER, trace.data(), nullptr),
              MICROTEL_LEAF_ERR_ARG);
}

TEST_F(LeafTest, Attributes_EveryType_InFirstSetOrder)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    const microtel_leaf_kv_t kvs[] = {
        StrKv("s", "text"), IntKv("i", -5), DoubleKv("d", 2.5), BoolKv("b", 0), BoolKv("t", 7)};
    for (const auto& kv : kvs)
    {
        ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &kv), MICROTEL_LEAF_OK);
    }
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 1U);
    const auto& a = p.spans[0].attrs;
    ASSERT_EQ(a.size(), 5U);
    EXPECT_EQ(a[0].key, "s");
    EXPECT_EQ(a[0].s, "text");
    EXPECT_EQ(a[1].key, "i");
    EXPECT_EQ(a[1].i, -5);
    EXPECT_EQ(a[2].key, "d");
    EXPECT_DOUBLE_EQ(a[2].d, 2.5);
    EXPECT_EQ(a[3].key, "b");
    EXPECT_EQ(a[3].type, MICROTEL_LEAF_VALUE_BOOL);
    EXPECT_FALSE(a[3].b);
    EXPECT_TRUE(a[4].b);
}

TEST_F(LeafTest, Attributes_OverwriteKeepsPosition)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    const auto a1 = IntKv("first", 1);
    const auto b1 = StrKv("second", "x");
    const auto c1 = IntKv("third", 3);
    const auto a2 = IntKv("first", 10);                                  // same size: in place
    const auto b2 = StrKv("second", "a much longer value than before");  // grows: relinked
    for (const auto* kv : {&a1, &b1, &c1, &a2, &b2})
    {
        ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, kv), MICROTEL_LEAF_OK);
    }
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 1U);
    const auto& a = p.spans[0].attrs;
    ASSERT_EQ(a.size(), 3U);
    EXPECT_EQ(a[0].key, "first");
    EXPECT_EQ(a[0].i, 10);
    EXPECT_EQ(a[1].key, "second");
    EXPECT_EQ(a[1].s, "a much longer value than before");
    EXPECT_EQ(a[2].key, "third");
}

TEST_F(LeafTest, Attributes_OverwriteWithAnotherType)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    const auto a1 = StrKv("k", "long string value");
    const auto a2 = IntKv("k", 4);
    ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &a1), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &a2), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 1U);
    ASSERT_EQ(p.spans[0].attrs.size(), 1U);
    EXPECT_EQ(p.spans[0].attrs[0].type, MICROTEL_LEAF_VALUE_INT64);
    EXPECT_EQ(p.spans[0].attrs[0].i, 4);
}

TEST_F(LeafTest, Attributes_InvalidArguments)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    EXPECT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, nullptr), MICROTEL_LEAF_ERR_ARG);
    auto bad = StrKv("k", "v");
    bad.key_len = 0;
    EXPECT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &bad), MICROTEL_LEAF_ERR_ARG);
    bad = StrKv("k", "v");
    bad.type = static_cast<microtel_leaf_value_type_t>(9);
    EXPECT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &bad), MICROTEL_LEAF_ERR_ARG);
    const auto good = IntKv("k", 1);
    EXPECT_EQ(microtel_leaf_span_set_attribute(&m_leaf, 0, &good), MICROTEL_LEAF_ERR_STATE);
    EXPECT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s + 1, &good), MICROTEL_LEAF_ERR_STATE);
}

TEST_F(LeafTest, Attributes_CapIsEnforcedAndCounted)
{
    m_config.max_attributes_per_span = 2;
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    const auto a = IntKv("a", 1);
    const auto b = IntKv("b", 2);
    const auto c = IntKv("c", 3);
    const auto a2 = IntKv("a", 4);
    EXPECT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &a), MICROTEL_LEAF_OK);
    EXPECT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &b), MICROTEL_LEAF_OK);
    EXPECT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &c), MICROTEL_LEAF_ERR_LIMIT);
    // Overwriting an existing key at the cap is not a new attribute.
    EXPECT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &a2), MICROTEL_LEAF_OK);
    EXPECT_EQ(Counters().dropped_attributes, 1U);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 1U);
    EXPECT_EQ(p.spans[0].attrs.size(), 2U);
}

TEST_F(LeafTest, DefaultAttributeCapIsSixteen)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    constexpr int kDefaultCap = 16;
    std::vector<std::string> keys;
    for (int i = 0; i <= kDefaultCap; ++i)
    {
        keys.push_back("k" + std::to_string(i));
    }
    for (int i = 0; i <= kDefaultCap; ++i)
    {
        const auto kv = IntKv(keys[static_cast<std::size_t>(i)].c_str(), i);
        EXPECT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &kv),
                  i < kDefaultCap ? MICROTEL_LEAF_OK : MICROTEL_LEAF_ERR_LIMIT);
    }
}

TEST_F(LeafTest, Events_AreEncodedInAddOrder)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    const microtel_leaf_kv_t attrs[] = {IntKv("n", 1), StrKv("why", "retry")};
    ASSERT_EQ(microtel_leaf_span_add_event(&m_leaf, s, "e1", 2, attrs, 2), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_add_event(&m_leaf, s, "e2", 2, nullptr, 0), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 1U);
    const auto& ev = p.spans[0].events;
    ASSERT_EQ(ev.size(), 2U);
    EXPECT_EQ(ev[0].name, "e1");
    EXPECT_EQ(ev[0].time, kClockStart + kClockStep);
    ASSERT_EQ(ev[0].attrs.size(), 2U);
    EXPECT_EQ(ev[0].attrs[0].key, "n");
    EXPECT_EQ(ev[0].attrs[1].s, "retry");
    EXPECT_EQ(ev[1].name, "e2");
    EXPECT_TRUE(ev[1].attrs.empty());
}

TEST_F(LeafTest, Events_CapsAreEnforcedAndCounted)
{
    m_config.max_events_per_span = 1;
    m_config.max_attributes_per_event = 1;
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    const microtel_leaf_kv_t attrs[] = {IntKv("a", 1), IntKv("b", 2), IntKv("c", 3)};
    // Surplus attributes are dropped; the event is kept.
    EXPECT_EQ(microtel_leaf_span_add_event(&m_leaf, s, "e1", 2, attrs, 3), MICROTEL_LEAF_OK);
    EXPECT_EQ(microtel_leaf_span_add_event(&m_leaf, s, "e2", 2, nullptr, 0),
              MICROTEL_LEAF_ERR_LIMIT);
    const auto c = Counters();
    EXPECT_EQ(c.dropped_attributes, 2U);
    EXPECT_EQ(c.dropped_events, 1U);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 1U);
    ASSERT_EQ(p.spans[0].events.size(), 1U);
    EXPECT_EQ(p.spans[0].events[0].attrs.size(), 1U);
}

TEST_F(LeafTest, Events_InvalidArguments)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    EXPECT_EQ(microtel_leaf_span_add_event(&m_leaf, s, nullptr, 2, nullptr, 0),
              MICROTEL_LEAF_ERR_ARG);
    EXPECT_EQ(microtel_leaf_span_add_event(&m_leaf, s, "e", 1, nullptr, 1), MICROTEL_LEAF_ERR_ARG);
    auto bad = IntKv("k", 1);
    bad.key_len = 0;
    EXPECT_EQ(microtel_leaf_span_add_event(&m_leaf, s, "e", 1, &bad, 1), MICROTEL_LEAF_ERR_ARG);
    EXPECT_EQ(microtel_leaf_span_add_event(&m_leaf, 0, "e", 1, nullptr, 0),
              MICROTEL_LEAF_ERR_STATE);
}

TEST_F(LeafTest, Status_IsEncodedAndOverwritten)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    ASSERT_EQ(microtel_leaf_span_set_status(&m_leaf, s, MICROTEL_LEAF_STATUS_ERROR, "boom", 4),
              MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_set_status(
                  &m_leaf, s, MICROTEL_LEAF_STATUS_ERROR, "a longer failure message", 24),
              MICROTEL_LEAF_OK);
    const microtel_leaf_span_t ok = Start("ok");
    ASSERT_EQ(microtel_leaf_span_set_status(&m_leaf, ok, MICROTEL_LEAF_STATUS_OK, nullptr, 0),
              MICROTEL_LEAF_OK);
    const microtel_leaf_span_t unset_msg = Start("unset");
    ASSERT_EQ(microtel_leaf_span_set_status(&m_leaf, unset_msg, MICROTEL_LEAF_STATUS_UNSET, "m", 1),
              MICROTEL_LEAF_OK);
    const microtel_leaf_span_t cleared = Start("cleared");
    ASSERT_EQ(microtel_leaf_span_set_status(&m_leaf, cleared, MICROTEL_LEAF_STATUS_ERROR, "x", 1),
              MICROTEL_LEAF_OK);
    ASSERT_EQ(
        microtel_leaf_span_set_status(&m_leaf, cleared, MICROTEL_LEAF_STATUS_UNSET, nullptr, 0),
        MICROTEL_LEAF_OK);
    for (const auto h : {s, ok, unset_msg, cleared})
    {
        ASSERT_EQ(microtel_leaf_span_end(&m_leaf, h), MICROTEL_LEAF_OK);
    }
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 4U);
    EXPECT_TRUE(p.spans[0].has_status);
    EXPECT_EQ(p.spans[0].status_code, MICROTEL_LEAF_STATUS_ERROR);
    EXPECT_EQ(p.spans[0].status_message, "a longer failure message");
    EXPECT_TRUE(p.spans[1].has_status);
    EXPECT_EQ(p.spans[1].status_code, MICROTEL_LEAF_STATUS_OK);
    EXPECT_TRUE(p.spans[2].has_status);
    EXPECT_EQ(p.spans[2].status_message, "m");
    EXPECT_FALSE(p.spans[3].has_status);
}

TEST_F(LeafTest, Status_InvalidArguments)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    EXPECT_EQ(microtel_leaf_span_set_status(
                  &m_leaf, s, static_cast<microtel_leaf_status_code_t>(3), nullptr, 0),
              MICROTEL_LEAF_ERR_ARG);
    EXPECT_EQ(microtel_leaf_span_set_status(&m_leaf, s, MICROTEL_LEAF_STATUS_OK, nullptr, 1),
              MICROTEL_LEAF_ERR_ARG);
    EXPECT_EQ(microtel_leaf_span_set_status(&m_leaf, 0, MICROTEL_LEAF_STATUS_OK, nullptr, 0),
              MICROTEL_LEAF_ERR_STATE);
}

TEST_F(LeafTest, EndedHandle_RejectsFurtherChanges)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto kv = IntKv("k", 1);
    EXPECT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_ERR_STATE);
    EXPECT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &kv), MICROTEL_LEAF_ERR_STATE);
    EXPECT_EQ(microtel_leaf_span_add_event(&m_leaf, s, "e", 1, nullptr, 0),
              MICROTEL_LEAF_ERR_STATE);
    EXPECT_EQ(microtel_leaf_span_set_status(&m_leaf, s, MICROTEL_LEAF_STATUS_OK, nullptr, 0),
              MICROTEL_LEAF_ERR_STATE);
}

TEST_F(LeafTest, StaleHandleAfterEncode_IsRejectedEvenWhenTheSlotIsReused)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t old = Start("old");
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, old), MICROTEL_LEAF_OK);
    (void)EncodeBytes();
    const microtel_leaf_span_t reused = Start("new");
    EXPECT_NE(reused, old);
    const auto kv = IntKv("k", 1);
    EXPECT_EQ(microtel_leaf_span_set_attribute(&m_leaf, old, &kv), MICROTEL_LEAF_ERR_STATE);
    EXPECT_EQ(microtel_leaf_span_end(&m_leaf, old), MICROTEL_LEAF_ERR_STATE);
    EXPECT_EQ(microtel_leaf_span_set_attribute(&m_leaf, reused, &kv), MICROTEL_LEAF_OK);
}

TEST_F(LeafTest, OpenSpan_SurvivesEncodeAndCompaction)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t longrun = Start("long");
    const auto a1 = StrKv("phase", "one");
    ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, longrun, &a1), MICROTEL_LEAF_OK);
    for (int i = 0; i < 3; ++i)
    {
        const microtel_leaf_span_t shortrun = Start("short");
        const auto kv = StrKv("payload", "some bytes to reclaim");
        ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, shortrun, &kv), MICROTEL_LEAF_OK);
        ASSERT_EQ(microtel_leaf_span_end(&m_leaf, shortrun), MICROTEL_LEAF_OK);
    }
    const auto first = EncodeAndDecode();
    ASSERT_EQ(first.spans.size(), 3U);
    for (const auto& span : first.spans)
    {
        EXPECT_EQ(span.name, "short");
    }

    const auto a2 = StrKv("phase", "two, and longer");
    const auto b = IntKv("after", 2);
    ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, longrun, &a2), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, longrun, &b), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_add_event(&m_leaf, longrun, "tick", 4, nullptr, 0),
              MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, longrun), MICROTEL_LEAF_OK);
    const auto second = EncodeAndDecode();
    ASSERT_EQ(second.spans.size(), 1U);
    const auto& span = second.spans[0];
    EXPECT_EQ(span.name, "long");
    ASSERT_EQ(span.attrs.size(), 2U);
    EXPECT_EQ(span.attrs[0].key, "phase");
    EXPECT_EQ(span.attrs[0].s, "two, and longer");
    EXPECT_EQ(span.attrs[1].key, "after");
    ASSERT_EQ(span.events.size(), 1U);
    EXPECT_EQ(span.events[0].name, "tick");
    EXPECT_EQ(span.start, kClockStart);
}

TEST_F(LeafTest, RecordBufferExhaustion_DropsAndCountsThenRecovers)
{
    constexpr std::size_t kSmall = 1024;
    ASSERT_EQ(Init(kSmall), MICROTEL_LEAF_OK);
    int started = 0;
    microtel_leaf_status_t st = MICROTEL_LEAF_OK;
    while (st == MICROTEL_LEAF_OK && started < 1000)
    {
        microtel_leaf_span_t s = 0;
        st = microtel_leaf_span_start(
            &m_leaf, &s, "span", 4, MICROTEL_LEAF_SPAN_KIND_INTERNAL, nullptr);
        if (st == MICROTEL_LEAF_OK)
        {
            ++started;
            ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
        }
    }
    EXPECT_EQ(st, MICROTEL_LEAF_ERR_NO_SPACE);
    EXPECT_GT(started, 0);
    EXPECT_EQ(Counters().dropped_spans, 1U);

    const auto p = EncodeAndDecode();
    EXPECT_EQ(p.spans.size(), static_cast<std::size_t>(started));
    ASSERT_TRUE(p.ResourceAttr("microtel.leaf.dropped_spans").has_value());
    EXPECT_EQ(p.ResourceInt("microtel.leaf.dropped_spans"), 1);
    EXPECT_EQ(Counters().dropped_spans, 0U);

    // Usable again.
    const microtel_leaf_span_t again = Start("again");
    EXPECT_EQ(microtel_leaf_span_end(&m_leaf, again), MICROTEL_LEAF_OK);
}

TEST_F(LeafTest, RecordBufferExhaustion_AttributesAndEventsAreCounted)
{
    constexpr std::size_t kSmall = 512;
    m_config.max_attributes_per_span = 1000;
    m_config.max_events_per_span = 1000;
    ASSERT_EQ(Init(kSmall), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    const std::string big(kSmall, 'x');
    microtel_leaf_kv_t kv = StrKv("k", "v");
    kv.value.s.ptr = big.data();
    kv.value.s.len = big.size();
    EXPECT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &kv), MICROTEL_LEAF_ERR_NO_SPACE);
    EXPECT_EQ(microtel_leaf_span_add_event(&m_leaf, s, big.data(), big.size(), nullptr, 0),
              MICROTEL_LEAF_ERR_NO_SPACE);
    microtel_leaf_span_t s2 = 0;
    EXPECT_EQ(microtel_leaf_span_start(
                  &m_leaf, &s2, big.data(), big.size(), MICROTEL_LEAF_SPAN_KIND_INTERNAL, nullptr),
              MICROTEL_LEAF_ERR_NO_SPACE);
    microtel_leaf_span_t s3 = 0;
    std::array<std::uint8_t, 16> trace{};
    trace.fill(1);
    std::array<std::uint8_t, 8> parent{};
    parent.fill(1);
    EXPECT_EQ(microtel_leaf_span_start_remote(&m_leaf,
                                              &s3,
                                              big.data(),
                                              big.size(),
                                              MICROTEL_LEAF_SPAN_KIND_INTERNAL,
                                              trace.data(),
                                              parent.data()),
              MICROTEL_LEAF_ERR_NO_SPACE);
    EXPECT_EQ(microtel_leaf_span_set_status(
                  &m_leaf, s, MICROTEL_LEAF_STATUS_ERROR, big.data(), big.size()),
              MICROTEL_LEAF_ERR_NO_SPACE);
    const auto c = Counters();
    EXPECT_EQ(c.dropped_attributes, 1U);
    EXPECT_EQ(c.dropped_events, 1U);
    EXPECT_EQ(c.dropped_spans, 2U);

    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_TRUE(p.ResourceAttr("microtel.leaf.dropped_items").has_value());
    EXPECT_EQ(p.ResourceInt("microtel.leaf.dropped_items"), 2);
    EXPECT_EQ(p.ResourceInt("microtel.leaf.dropped_spans"), 2);
}

TEST_F(LeafTest, RepeatedOverwrites_ReclaimDeadSpace)
{
    // Each overwrite with a longer value leaves the old record dead; without
    // reclaiming it the small buffer would fill long before 200 rounds.
    constexpr std::size_t kSmall = 768;
    ASSERT_EQ(Init(kSmall), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    std::string value;
    for (int i = 0; i < 200; ++i)
    {
        value = std::string(static_cast<std::size_t>(8 + (i % 64)), 'v');
        auto kv = StrKv("k", "v");
        kv.value.s.ptr = value.data();
        kv.value.s.len = value.size();
        ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &kv), MICROTEL_LEAF_OK) << i;
        ASSERT_EQ(microtel_leaf_span_set_status(
                      &m_leaf, s, MICROTEL_LEAF_STATUS_ERROR, value.data(), value.size()),
                  MICROTEL_LEAF_OK)
            << i;
    }
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 1U);
    EXPECT_EQ(p.spans[0].attrs[0].s, value);
    EXPECT_EQ(p.spans[0].status_message, value);
}

TEST_F(LeafTest, SpanTableFull_IsNoSpaceAndCounted)
{
    constexpr std::size_t kSmall = 512;
    ASSERT_EQ(Init(kSmall), MICROTEL_LEAF_OK);
    microtel_leaf_status_t st = MICROTEL_LEAF_OK;
    int started = 0;
    while (st == MICROTEL_LEAF_OK && started < 100)
    {
        microtel_leaf_span_t s = 0;
        // No name: only the span table grows.
        st = microtel_leaf_span_start(
            &m_leaf, &s, nullptr, 0, MICROTEL_LEAF_SPAN_KIND_INTERNAL, nullptr);
        started += st == MICROTEL_LEAF_OK ? 1 : 0;
    }
    EXPECT_EQ(st, MICROTEL_LEAF_ERR_NO_SPACE);
    EXPECT_GT(started, 1);
    EXPECT_EQ(Counters().dropped_spans, 1U);
}

TEST_F(LeafTest, FreedSlotBelowAnOpenSpan_IsReusedWithANewGeneration)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t first = Start("first");  // slot 0
    const microtel_leaf_span_t open = Start("open");    // slot 1, stays open
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, first), MICROTEL_LEAF_OK);
    ASSERT_EQ(EncodeAndDecode().spans.size(), 1U);

    const microtel_leaf_span_t reused = Start("reused");
    EXPECT_EQ(reused & 0xffffU, first & 0xffffU);
    EXPECT_NE(reused, first);
    EXPECT_EQ(microtel_leaf_span_end(&m_leaf, first), MICROTEL_LEAF_ERR_STATE);
    EXPECT_EQ(microtel_leaf_span_end(&m_leaf, reused), MICROTEL_LEAF_OK);
    EXPECT_EQ(microtel_leaf_span_end(&m_leaf, open), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 2U);
    EXPECT_EQ(p.spans[0].name, "reused");
    EXPECT_EQ(p.spans[1].name, "open");
}

TEST_F(LeafTest, UnnamedSpan_OverwriteAndClearAtTheHeadOfItsList)
{
    // With no name chunk, the first attribute or status message is the head
    // of the span's list, so replacing or removing it rewrites the head.
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    microtel_leaf_span_t s = 0;
    ASSERT_EQ(microtel_leaf_span_start(
                  &m_leaf, &s, nullptr, 0, MICROTEL_LEAF_SPAN_KIND_INTERNAL, nullptr),
              MICROTEL_LEAF_OK);
    const auto a1 = IntKv("k", 1);
    const auto a2 = StrKv("k", "now a longer string value");
    ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &a1), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &a2), MICROTEL_LEAF_OK);

    microtel_leaf_span_t t = 0;
    ASSERT_EQ(microtel_leaf_span_start(
                  &m_leaf, &t, nullptr, 0, MICROTEL_LEAF_SPAN_KIND_INTERNAL, nullptr),
              MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_set_status(&m_leaf, t, MICROTEL_LEAF_STATUS_ERROR, "x", 1),
              MICROTEL_LEAF_OK);
    const auto b = IntKv("b", 2);
    ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, t, &b), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_set_status(&m_leaf, t, MICROTEL_LEAF_STATUS_ERROR, nullptr, 0),
              MICROTEL_LEAF_OK);

    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, t), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 2U);
    EXPECT_TRUE(p.spans[0].name.empty());
    ASSERT_EQ(p.spans[0].attrs.size(), 1U);
    EXPECT_EQ(p.spans[0].attrs[0].s, "now a longer string value");
    EXPECT_TRUE(p.spans[1].status_message.empty());
    EXPECT_EQ(p.spans[1].status_code, MICROTEL_LEAF_STATUS_ERROR);
    ASSERT_EQ(p.spans[1].attrs.size(), 1U);
}

TEST_F(LeafTest, OverwriteWithoutSpace_KeepsTheOldValueAndCounts)
{
    constexpr std::size_t kSmall = 512;
    ASSERT_EQ(Init(kSmall), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    const auto small = StrKv("k", "v");
    ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &small), MICROTEL_LEAF_OK);
    const std::string big(kSmall, 'x');
    auto large = StrKv("k", "v");
    large.value.s.ptr = big.data();
    large.value.s.len = big.size();
    EXPECT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &large), MICROTEL_LEAF_ERR_NO_SPACE);
    EXPECT_EQ(Counters().dropped_attributes, 1U);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 1U);
    ASSERT_EQ(p.spans[0].attrs.size(), 1U);
    EXPECT_EQ(p.spans[0].attrs[0].s, "v");
}

TEST_F(LeafTest, SpanTableGrowth_CompactsDataFirst)
{
    // Dead data in the middle of the buffer must not stop the span table
    // growing into it.
    constexpr std::size_t kSmall = 1024;
    ASSERT_EQ(Init(kSmall), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    std::string value(200, 'x');
    for (int i = 0; i < 3; ++i)
    {
        value.push_back('y');
        auto kv = StrKv("k", "v");
        kv.value.s.ptr = value.data();
        kv.value.s.len = value.size();
        ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &kv), MICROTEL_LEAF_OK);
    }
    int started = 0;
    for (int i = 0; i < 6; ++i)
    {
        microtel_leaf_span_t t = 0;
        if (microtel_leaf_span_start(
                &m_leaf, &t, nullptr, 0, MICROTEL_LEAF_SPAN_KIND_INTERNAL, nullptr) ==
            MICROTEL_LEAF_OK)
        {
            ++started;
        }
    }
    EXPECT_EQ(started, 6);
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

TEST_F(LeafTest, EmptyBatch_CarriesResourceAndScopeOnly)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t open = Start("still open");
    (void)open;
    const auto p = EncodeAndDecode();
    EXPECT_EQ(p.resource_spans_count, 1U);
    EXPECT_TRUE(p.has_resource);
    EXPECT_EQ(p.scope_spans_count, 1U);
    EXPECT_TRUE(p.has_scope);
    EXPECT_TRUE(p.spans.empty());
}

TEST_F(LeafTest, Encode_BufferTooSmall_ReportsExactSizeAndConsumesNothing)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);

    std::size_t needed = 0;
    EXPECT_EQ(microtel_leaf_encode(&m_leaf, nullptr, 0, &needed), MICROTEL_LEAF_ERR_BUFFER_SMALL);
    ASSERT_GT(needed, 0U);
    std::vector<std::uint8_t> small(needed - 1);
    std::size_t again = 0;
    EXPECT_EQ(microtel_leaf_encode(&m_leaf, small.data(), small.size(), &again),
              MICROTEL_LEAF_ERR_BUFFER_SMALL);
    EXPECT_EQ(again, needed);

    std::vector<std::uint8_t> exact(needed);
    std::size_t written = 0;
    ASSERT_EQ(microtel_leaf_encode(&m_leaf, exact.data(), exact.size(), &written),
              MICROTEL_LEAF_OK);
    const auto p = lt::DecodeOrFail(exact.data(), written);
    EXPECT_EQ(p.spans.size(), 1U);
}

TEST_F(LeafTest, Encode_InvalidArguments)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    std::array<std::uint8_t, 16> buf{};
    std::size_t written = 0;
    EXPECT_EQ(microtel_leaf_encode(&m_leaf, buf.data(), buf.size(), nullptr),
              MICROTEL_LEAF_ERR_ARG);
    EXPECT_EQ(microtel_leaf_encode(&m_leaf, nullptr, 4, &written), MICROTEL_LEAF_ERR_ARG);
    EXPECT_EQ(microtel_leaf_encode(nullptr, buf.data(), buf.size(), &written),
              MICROTEL_LEAF_ERR_ARG);
    EXPECT_EQ(microtel_leaf_encode_to(&m_leaf, nullptr, nullptr, &written), MICROTEL_LEAF_ERR_ARG);
    EXPECT_EQ(microtel_leaf_encoded_size(nullptr), 0U);
}

struct Collector
{
    std::vector<std::uint8_t> bytes;
    int calls = 0;
    int fail = 0;
};

int CollectWrite(void* ctx, const std::uint8_t* bytes, std::size_t len)
{
    auto* c = static_cast<Collector*>(ctx);
    ++c->calls;
    if (c->fail != 0)
    {
        return c->fail;
    }
    c->bytes.insert(c->bytes.end(), bytes, bytes + len);
    return 0;
}

TEST_F(LeafTest, EncodeTo_ProducesTheSameBytesAsBufferEncode)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    const auto kv = StrKv("k", "v");
    ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &kv), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);

    // Size the buffer encode first; the clock does not move for a failed encode.
    std::size_t needed = 0;
    ASSERT_EQ(microtel_leaf_encode(&m_leaf, nullptr, 0, &needed), MICROTEL_LEAF_ERR_BUFFER_SMALL);

    Collector c;
    std::size_t written = 0;
    const FakeClock saved = m_clock;
    ASSERT_EQ(microtel_leaf_encode_to(&m_leaf, &CollectWrite, &c, &written), MICROTEL_LEAF_OK);
    EXPECT_EQ(written, c.bytes.size());
    EXPECT_EQ(written, needed);
    const auto p = lt::DecodeOrFail(c.bytes.data(), c.bytes.size());
    ASSERT_EQ(p.spans.size(), 1U);
    EXPECT_EQ(p.spans[0].attrs.size(), 1U);
    (void)saved;
}

TEST_F(LeafTest, EncodeTo_FailingWrite_ConsumesNothing)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    Collector c;
    c.fail = -1;
    std::size_t written = 0;
    EXPECT_EQ(microtel_leaf_encode_to(&m_leaf, &CollectWrite, &c, &written),
              MICROTEL_LEAF_ERR_ENCODE);
    EXPECT_EQ(c.calls, 1);
    c.fail = 0;
    ASSERT_EQ(microtel_leaf_encode_to(&m_leaf, &CollectWrite, &c, &written), MICROTEL_LEAF_OK);
    const auto p = lt::DecodeOrFail(c.bytes.data(), c.bytes.size());
    EXPECT_EQ(p.spans.size(), 1U);
}

TEST_F(LeafTest, EncodedSize_IsAnUpperBound)
{
    ASSERT_EQ(Init(kRecordSize * 4), MICROTEL_LEAF_OK);
    const auto check = [this]()
    {
        const std::size_t bound = microtel_leaf_encoded_size(&m_leaf);
        std::size_t needed = 0;
        EXPECT_EQ(microtel_leaf_encode(&m_leaf, nullptr, 0, &needed),
                  MICROTEL_LEAF_ERR_BUFFER_SMALL);
        EXPECT_GE(bound, needed);
    };
    check();
    const microtel_leaf_span_t parent = Start("parent");
    const microtel_leaf_kv_t kvs[] = {
        StrKv("s", "text"), IntKv("i", INT64_MIN), DoubleKv("d", -0.0), BoolKv("b", 1)};
    for (const auto& kv : kvs)
    {
        ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, parent, &kv), MICROTEL_LEAF_OK);
    }
    ASSERT_EQ(microtel_leaf_span_add_event(&m_leaf, parent, "ev", 2, kvs, 4), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_set_status(&m_leaf, parent, MICROTEL_LEAF_STATUS_ERROR, "bad", 3),
              MICROTEL_LEAF_OK);
    const microtel_leaf_span_t child = Start("child", &parent);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, child), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, parent), MICROTEL_LEAF_OK);
    check();
}

// ---------------------------------------------------------------------------
// Time modes (§5)
// ---------------------------------------------------------------------------

TEST_F(LeafTest, ConcentratorStamped_WithClock_SendsEncodeTime)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    EXPECT_EQ(p.ResourceInt("microtel.leaf.time_mode"), 0);
    ASSERT_TRUE(p.ResourceAttr("microtel.leaf.encode_time").has_value());
    EXPECT_EQ(p.ResourceInt("microtel.leaf.encode_time"),
              static_cast<std::int64_t>(kClockStart + 2 * kClockStep));
    EXPECT_FALSE(p.ResourceAttr("microtel.leaf.sync_age").has_value());
    EXPECT_FALSE(p.ResourceAttr("microtel.leaf.boot_id").has_value());
    EXPECT_FALSE(p.ResourceAttr("microtel.leaf.dropped_spans").has_value());
    EXPECT_FALSE(p.ResourceAttr("microtel.leaf.dropped_items").has_value());
}

TEST_F(LeafTest, NoClock_SendsZeroTimestampsAndNoEncodeTime)
{
    m_config.now_ns = nullptr;
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    ASSERT_EQ(microtel_leaf_span_add_event(&m_leaf, s, "e", 1, nullptr, 0), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    ASSERT_EQ(p.spans.size(), 1U);
    EXPECT_EQ(p.spans[0].start, 0U);
    EXPECT_EQ(p.spans[0].end, 0U);
    EXPECT_EQ(p.spans[0].events[0].time, 0U);
    EXPECT_FALSE(p.ResourceAttr("microtel.leaf.encode_time").has_value());
    EXPECT_EQ(microtel_leaf_clock_sync(&m_leaf, 5, 0), MICROTEL_LEAF_ERR_CLOCK);
}

TEST_F(LeafTest, SyncRelative_BeforeFirstSync_FallsBackToStamped)
{
    m_config.time_mode = MICROTEL_LEAF_TIME_SYNC_RELATIVE;
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    EXPECT_EQ(p.ResourceInt("microtel.leaf.time_mode"), 0);
    EXPECT_FALSE(p.ResourceAttr("microtel.leaf.sync_age").has_value());
    EXPECT_EQ(p.spans[0].start, kClockStart);
}

TEST_F(LeafTest, SyncRelative_AfterSync_ConvertsAtEncodeTime)
{
    constexpr std::uint64_t kUnix = 1700000000000000000ULL;
    m_config.time_mode = MICROTEL_LEAF_TIME_SYNC_RELATIVE;
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");  // raw start = kClockStart
    // Sync after the span started: leaf time kClockStart + 1 step is kUnix.
    const std::uint64_t sync_leaf = kClockStart + kClockStep;
    ASSERT_EQ(microtel_leaf_clock_sync(&m_leaf, kUnix, sync_leaf), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    EXPECT_EQ(p.ResourceInt("microtel.leaf.time_mode"), 1);
    ASSERT_EQ(p.spans.size(), 1U);
    EXPECT_EQ(p.spans[0].start, kUnix - kClockStep);
    const std::uint64_t raw_end = p.spans[0].end - kUnix + sync_leaf;
    const std::uint64_t encode_leaf = raw_end + kClockStep;
    EXPECT_EQ(p.ResourceInt("microtel.leaf.encode_time"),
              static_cast<std::int64_t>(kUnix + (encode_leaf - sync_leaf)));
    EXPECT_EQ(p.ResourceInt("microtel.leaf.sync_age"),
              static_cast<std::int64_t>(encode_leaf - sync_leaf));
}

TEST_F(LeafTest, ClockSync_RejectsInvalidTimes)
{
    m_config.time_mode = MICROTEL_LEAF_TIME_SYNC_RELATIVE;
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    EXPECT_EQ(microtel_leaf_clock_sync(&m_leaf, 0, kClockStart), MICROTEL_LEAF_ERR_CLOCK);
    EXPECT_EQ(microtel_leaf_clock_sync(&m_leaf, 5, kClockStart * 1000), MICROTEL_LEAF_ERR_CLOCK);
    EXPECT_EQ(microtel_leaf_clock_sync(nullptr, 5, 0), MICROTEL_LEAF_ERR_ARG);
}

TEST_F(LeafTest, BootRelative_SendsBootId)
{
    m_config.time_mode = MICROTEL_LEAF_TIME_BOOT_RELATIVE;
    m_config.boot_id = 42;
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    const auto p = EncodeAndDecode();
    EXPECT_EQ(p.ResourceInt("microtel.leaf.time_mode"), 2);
    EXPECT_EQ(p.ResourceInt("microtel.leaf.boot_id"), 42);
    EXPECT_TRUE(p.ResourceAttr("microtel.leaf.encode_time").has_value());
    EXPECT_EQ(p.spans[0].start, kClockStart);
}

TEST_F(LeafTest, Counters_ResetOnlyAfterASuccessfulEncode)
{
    m_config.max_events_per_span = 1;
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    const microtel_leaf_span_t s = Start("a");
    ASSERT_EQ(microtel_leaf_span_add_event(&m_leaf, s, "e", 1, nullptr, 0), MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_add_event(&m_leaf, s, "e", 1, nullptr, 0),
              MICROTEL_LEAF_ERR_LIMIT);
    std::size_t needed = 0;
    ASSERT_EQ(microtel_leaf_encode(&m_leaf, nullptr, 0, &needed), MICROTEL_LEAF_ERR_BUFFER_SMALL);
    EXPECT_EQ(Counters().dropped_events, 1U);
    const auto p = EncodeAndDecode();
    EXPECT_EQ(p.ResourceInt("microtel.leaf.dropped_items"), 1);
    EXPECT_EQ(Counters().dropped_events, 0U);
    const auto p2 = EncodeAndDecode();
    EXPECT_FALSE(p2.ResourceAttr("microtel.leaf.dropped_items").has_value());
}

#if defined(MICROTEL_LEAF_TEST_BACKEND_UPB)

// ---------------------------------------------------------------------------
// upb backend: a fixed scratch buffer never touches the heap (§2.2)
// ---------------------------------------------------------------------------

struct HeapCounter
{
    static inline int s_calls = 0;
    static inline upb_alloc_func* s_real = nullptr;

    static void* Count(upb_alloc* alloc, void* ptr, std::size_t oldsize, std::size_t size)
    {
        ++s_calls;
        return s_real(alloc, ptr, oldsize, size);
    }
};

class LeafUpbScratchTest : public LeafTest
{
protected:
    void SetUp() override
    {
        LeafTest::SetUp();
        HeapCounter::s_calls = 0;
        HeapCounter::s_real = upb_alloc_global.func;
        upb_alloc_global.func = &HeapCounter::Count;
    }

    void TearDown() override
    {
        upb_alloc_global.func = HeapCounter::s_real;
        LeafTest::TearDown();
    }

    void BuildSpans()
    {
        for (int i = 0; i < 4; ++i)
        {
            const microtel_leaf_span_t s = Start("span");
            const auto kv = StrKv("key", "value");
            ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &kv), MICROTEL_LEAF_OK);
            ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
        }
    }
};

TEST_F(LeafUpbScratchTest, FixedScratch_NeverCallsTheHeap)
{
    constexpr std::size_t kScratch = 16384;
    alignas(16) static std::array<std::uint8_t, kScratch> s_scratch{};
    m_config.scratch = s_scratch.data();
    m_config.scratch_size = s_scratch.size();
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    BuildSpans();
    const auto bytes = EncodeBytes();
    EXPECT_EQ(HeapCounter::s_calls, 0);
    const auto p = lt::DecodeOrFail(bytes.data(), bytes.size());
    EXPECT_EQ(p.spans.size(), 4U);
}

TEST_F(LeafUpbScratchTest, ExhaustedScratch_FailsWithoutTheHeapAndKeepsSpans)
{
    constexpr std::size_t kTiny = 256;
    alignas(16) static std::array<std::uint8_t, kTiny> s_scratch{};
    m_config.scratch = s_scratch.data();
    m_config.scratch_size = s_scratch.size();
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    BuildSpans();
    std::vector<std::uint8_t> out(kOutSize);
    std::size_t written = 0;
    EXPECT_EQ(microtel_leaf_encode(&m_leaf, out.data(), out.size(), &written),
              MICROTEL_LEAF_ERR_ENCODE);
    EXPECT_EQ(HeapCounter::s_calls, 0);
    Collector c;
    EXPECT_EQ(microtel_leaf_encode_to(&m_leaf, &CollectWrite, &c, &written),
              MICROTEL_LEAF_ERR_ENCODE);
    EXPECT_EQ(c.calls, 0);
    EXPECT_EQ(HeapCounter::s_calls, 0);

    // Too small even for the arena's own header.
    constexpr std::size_t kTooSmall = 8;
    m_config.scratch_size = kTooSmall;
    microtel_leaf_free(&m_leaf);
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    BuildSpans();
    EXPECT_EQ(microtel_leaf_encode(&m_leaf, out.data(), out.size(), &written),
              MICROTEL_LEAF_ERR_ENCODE);
    EXPECT_EQ(HeapCounter::s_calls, 0);
}

TEST_F(LeafUpbScratchTest, NoScratch_UsesTheHeap)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    BuildSpans();
    (void)EncodeBytes();
    EXPECT_GT(HeapCounter::s_calls, 0);
}

// upb cannot stream (§1.8): the sink gets the whole payload in one write.
TEST_F(LeafUpbScratchTest, EncodeTo_WritesThePayloadOnce)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    BuildSpans();
    Collector c;
    std::size_t written = 0;
    ASSERT_EQ(microtel_leaf_encode_to(&m_leaf, &CollectWrite, &c, &written), MICROTEL_LEAF_OK);
    EXPECT_EQ(c.calls, 1);
    EXPECT_EQ(c.bytes.size(), written);
}

#endif  // MICROTEL_LEAF_TEST_BACKEND_UPB

#if defined(MICROTEL_LEAF_TEST_BACKEND_NANOPB)

// ---------------------------------------------------------------------------
// nanopb backend: streams through the sink, ignores scratch (§1.8, §2.2)
// ---------------------------------------------------------------------------

class LeafNanopbTest : public LeafTest
{
protected:
    void BuildSpans(int count)
    {
        for (int i = 0; i < count; ++i)
        {
            const microtel_leaf_span_t s = Start("span");
            const auto kv = StrKv("key", "value");
            ASSERT_EQ(microtel_leaf_span_set_attribute(&m_leaf, s, &kv), MICROTEL_LEAF_OK);
            ASSERT_EQ(microtel_leaf_span_add_event(&m_leaf, s, "ev", 2, &kv, 1), MICROTEL_LEAF_OK);
            ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
        }
    }
};

// The sink sees the payload piece by piece, never as one buffer, and the
// pieces add up to what the buffer encode produces.
TEST_F(LeafNanopbTest, EncodeTo_StreamsInPieces)
{
    constexpr int kSpans = 3;
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    BuildSpans(kSpans);
    std::size_t needed = 0;
    ASSERT_EQ(microtel_leaf_encode(&m_leaf, nullptr, 0, &needed), MICROTEL_LEAF_ERR_BUFFER_SMALL);

    Collector c;
    std::size_t written = 0;
    ASSERT_EQ(microtel_leaf_encode_to(&m_leaf, &CollectWrite, &c, &written), MICROTEL_LEAF_OK);
    EXPECT_GT(c.calls, 1);
    EXPECT_EQ(written, needed);
    EXPECT_EQ(c.bytes.size(), written);
    const auto p = lt::DecodeOrFail(c.bytes.data(), c.bytes.size());
    EXPECT_EQ(p.spans.size(), static_cast<std::size_t>(kSpans));
}

// A sink that fails at any point aborts the encode and keeps the spans: fail
// at every write in turn, which reaches every level of the message.
struct FailAt
{
    int fail_at = 0;
    int calls = 0;
};

int FailAtWrite(void* ctx, const std::uint8_t* /*bytes*/, std::size_t /*len*/)
{
    auto* f = static_cast<FailAt*>(ctx);
    return f->calls++ == f->fail_at ? -1 : 0;
}

TEST_F(LeafNanopbTest, EncodeTo_FailingAtAnyWrite_ConsumesNothing)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    BuildSpans(2);
    const microtel_leaf_span_t s = Start("status");
    ASSERT_EQ(microtel_leaf_span_set_status(&m_leaf, s, MICROTEL_LEAF_STATUS_ERROR, "bad", 3),
              MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s), MICROTEL_LEAF_OK);
    std::size_t written = 0;
    ASSERT_EQ(microtel_leaf_encode(&m_leaf, nullptr, 0, &written), MICROTEL_LEAF_ERR_BUFFER_SMALL);
    const std::size_t needed = written;
    // Count the writes of a full encode without consuming the spans.
    FailAt count;
    count.fail_at = -1;
    ASSERT_EQ(microtel_leaf_encode_to(&m_leaf, &FailAtWrite, &count, &written), MICROTEL_LEAF_OK);
    ASSERT_EQ(written, needed);
    const int total = count.calls;
    ASSERT_GT(total, 1);

    // That encode consumed the spans; rebuild the same batch.
    microtel_leaf_free(&m_leaf);
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    BuildSpans(2);
    const microtel_leaf_span_t s2 = Start("status");
    ASSERT_EQ(microtel_leaf_span_set_status(&m_leaf, s2, MICROTEL_LEAF_STATUS_ERROR, "bad", 3),
              MICROTEL_LEAF_OK);
    ASSERT_EQ(microtel_leaf_span_end(&m_leaf, s2), MICROTEL_LEAF_OK);
    for (int i = 0; i < total; ++i)
    {
        FailAt f;
        f.fail_at = i;
        EXPECT_EQ(microtel_leaf_encode_to(&m_leaf, &FailAtWrite, &f, &written),
                  MICROTEL_LEAF_ERR_ENCODE)
            << "failing write " << i;
        EXPECT_EQ(f.calls, i + 1);
        EXPECT_EQ(written, 0U);
    }
    const auto p = EncodeAndDecode();
    EXPECT_EQ(p.spans.size(), 3U);
}

// Every buffer size short of the payload reports the exact size and consumes
// nothing, wherever in the message the encode runs out of room.
TEST_F(LeafNanopbTest, Encode_AnyShortBuffer_ReportsExactSize)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    BuildSpans(2);
    std::size_t needed = 0;
    ASSERT_EQ(microtel_leaf_encode(&m_leaf, nullptr, 0, &needed), MICROTEL_LEAF_ERR_BUFFER_SMALL);
    std::vector<std::uint8_t> out(needed);
    for (std::size_t cap = 0; cap < needed; ++cap)
    {
        std::size_t written = 0;
        EXPECT_EQ(microtel_leaf_encode(&m_leaf, out.data(), cap, &written),
                  MICROTEL_LEAF_ERR_BUFFER_SMALL)
            << "cap " << cap;
        EXPECT_EQ(written, needed);
    }
    const auto p = EncodeAndDecode();
    EXPECT_EQ(p.spans.size(), 2U);
}

// scratch is a upb option; nanopb needs no arena, so even a tiny one is fine.
TEST_F(LeafNanopbTest, Scratch_IsIgnored)
{
    constexpr std::size_t kTiny = 8;
    std::array<std::uint8_t, kTiny> scratch{};
    m_config.scratch = scratch.data();
    m_config.scratch_size = scratch.size();
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    BuildSpans(1);
    const auto p = EncodeAndDecode();
    EXPECT_EQ(p.spans.size(), 1U);
    EXPECT_EQ(scratch, (std::array<std::uint8_t, kTiny>{}));
}

// A buffer one byte short reports the exact size and leaves the rest alone.
TEST_F(LeafNanopbTest, Encode_BufferOneShort_ReportsExactSize)
{
    ASSERT_EQ(Init(), MICROTEL_LEAF_OK);
    BuildSpans(2);
    std::size_t needed = 0;
    ASSERT_EQ(microtel_leaf_encode(&m_leaf, nullptr, 0, &needed), MICROTEL_LEAF_ERR_BUFFER_SMALL);
    std::vector<std::uint8_t> out(needed + 1U, kCanary);
    std::size_t again = 0;
    EXPECT_EQ(microtel_leaf_encode(&m_leaf, out.data(), needed - 1U, &again),
              MICROTEL_LEAF_ERR_BUFFER_SMALL);
    EXPECT_EQ(again, needed);
    EXPECT_EQ(out.back(), kCanary);
    std::size_t written = 0;
    ASSERT_EQ(microtel_leaf_encode(&m_leaf, out.data(), needed, &written), MICROTEL_LEAF_OK);
    EXPECT_EQ(written, needed);
    EXPECT_EQ(out.back(), kCanary);
}

#endif  // MICROTEL_LEAF_TEST_BACKEND_NANOPB

}  // namespace
