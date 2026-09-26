// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// See leaf_diff_program.hpp. The program format is private to this file and
// accepts any byte string: a short input just ends the program early, and an
// out-of-range choice is folded into range with `%`, so a fuzzer never wastes
// an input on a parse error.

#include "leaf/diff/leaf_diff_program.hpp"

#include "microtel/leaf.h"

#include "leaf/vectors/leaf_vectors.h"
#include "leaf_dual.h"

// upb C headers use flexible array members — suppress the pedantic warning.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "opentelemetry/proto/collector/trace/v1/trace_service.upb.h"
#include "upb/mem/arena.h"
#pragma GCC diagnostic pop

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microtel::leaf_diff
{
namespace
{

constexpr std::size_t kMaxOps = 256;
constexpr std::size_t kOutSize = 65536;
constexpr std::size_t kRecordBase = 192;
constexpr std::size_t kRecordStep = 128;
constexpr std::uint8_t kRecordSteps = 32;
constexpr std::uint64_t kClockStart = 1000000000ULL;
constexpr std::uint64_t kClockStep = 1000ULL;
constexpr std::uint64_t kSyncUnix = 1700000000000000000ULL;
constexpr std::uint8_t kFixedStringFrom = 240;
constexpr std::uint8_t kShortStringMax = 16;
constexpr std::size_t kLongString = 300;
constexpr std::size_t kTraceIdBytes = 16;
constexpr std::size_t kSpanIdBytes = 8;
constexpr std::size_t kMaxEventAttrs = 6;
constexpr unsigned kKinds = 6;        // 0 is invalid, 1..5 are OTLP kinds
constexpr unsigned kStatusCodes = 4;  // 3 is invalid
constexpr unsigned kOpCount = 10;
constexpr unsigned kBitsPerByte = 8;
constexpr std::uint64_t kSplitMixGamma = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t kSplitMixMul1 = 0xBF58476D1CE4E5B9ULL;
constexpr std::uint64_t kSplitMixMul2 = 0x94D049BB133111EBULL;
constexpr unsigned kSplitMixShift1 = 30;
constexpr unsigned kSplitMixShift2 = 27;
constexpr unsigned kSplitMixShift3 = 31;

constexpr std::array<const char*, 6> kKeys = {
    "k", "http.method", "temp.c", "flag", "count", "unicode.ké"};

constexpr std::array<const char*, 4> kScopes = {"", "fw", "firmware", "日本語"};

constexpr std::array<const char*, 5> kFixedStrings = {
    "", "héllo wörld", "日本語テキスト", "emoji \xF0\x9F\x98\x80", "tab\tand\nnewline"};

constexpr std::string_view kAlphabet = "abcdefghijklmnopqrstuvwxyz0123456789._-/ ";

// Reads the program. Past the end every read is 0.
class Reader
{
public:
    Reader(const std::uint8_t* data, std::size_t size) : m_data(data), m_size(size) {}

    [[nodiscard]] bool Done() const
    {
        return m_pos >= m_size;
    }

    std::uint8_t Byte()
    {
        return m_pos < m_size ? m_data[m_pos++] : 0;
    }

    std::uint64_t U64()
    {
        std::uint64_t v = 0;
        for (unsigned i = 0; i < sizeof(v); ++i)
        {
            v |= static_cast<std::uint64_t>(Byte()) << (kBitsPerByte * i);
        }
        return v;
    }

    std::string Str()
    {
        const std::uint8_t sel = Byte();
        if (sel >= kFixedStringFrom)
        {
            const std::size_t which = sel % (kFixedStrings.size() + 1U);
            return which < kFixedStrings.size() ? std::string(kFixedStrings.at(which))
                                                : std::string(kLongString, 'x');
        }
        std::string s;
        const std::size_t len = sel % kShortStringMax;
        for (std::size_t i = 0; i < len; ++i)
        {
            s.push_back(kAlphabet.at(Byte() % kAlphabet.size()));
        }
        return s;
    }

private:
    const std::uint8_t* m_data;
    std::size_t m_size;
    std::size_t m_pos = 0;
};

struct Clock
{
    std::uint64_t now = kClockStart;
};

std::uint64_t ReadClock(void* ctx)
{
    auto* c = static_cast<Clock*>(ctx);
    const std::uint64_t t = c->now;
    c->now += kClockStep;
    return t;
}

struct Random
{
    std::uint64_t state = 0;
};

// splitmix64: any seed gives a well-mixed, reproducible byte stream.
void FillRandom(void* ctx, std::uint8_t* out, std::size_t len)
{
    auto* r = static_cast<Random*>(ctx);
    for (std::size_t i = 0; i < len; ++i)
    {
        r->state += kSplitMixGamma;
        std::uint64_t z = r->state;
        z = (z ^ (z >> kSplitMixShift1)) * kSplitMixMul1;
        z = (z ^ (z >> kSplitMixShift2)) * kSplitMixMul2;
        z ^= z >> kSplitMixShift3;
        out[i] = static_cast<std::uint8_t>(z);
    }
}

// An attribute whose strings it owns.
struct OwnedKv
{
    std::string key;
    std::string str;
    microtel_leaf_kv_t kv{};

    [[nodiscard]] microtel_leaf_kv_t View() const
    {
        microtel_leaf_kv_t v = kv;
        v.key = key.data();
        v.key_len = key.size();
        if (v.type == MICROTEL_LEAF_VALUE_STRING)
        {
            v.value.s.ptr = str.data();
            v.value.s.len = str.size();
        }
        return v;
    }
};

OwnedKv ReadKv(Reader& r)
{
    OwnedKv o;
    const std::uint8_t k = r.Byte();
    o.key = k % (kKeys.size() + 2U) < kKeys.size() ? std::string(kKeys.at(k % (kKeys.size() + 2U)))
                                                   : r.Str();
    o.kv.type = static_cast<microtel_leaf_value_type_t>(r.Byte() % 4U);
    switch (o.kv.type)
    {
        case MICROTEL_LEAF_VALUE_BOOL:
            o.kv.value.b = r.Byte();  // any non-zero value is true
            break;
        case MICROTEL_LEAF_VALUE_INT64:
            o.kv.value.i = static_cast<std::int64_t>(r.U64());
            break;
        case MICROTEL_LEAF_VALUE_DOUBLE:
        {
            const std::uint64_t bits = r.U64();
            std::memcpy(&o.kv.value.d, &bits, sizeof(bits));
            break;
        }
        default:
            o.str = r.Str();
            break;
    }
    return o;
}

// One leaf with its memory and inputs.
struct Side
{
    microtel_leaf_t leaf{};
    std::vector<std::uint8_t> record;
    Clock clock;
    Random random;
};

bool Decodes(const std::vector<std::uint8_t>& bytes)
{
    upb_Arena* arena = upb_Arena_New();
    const bool ok =
        opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_parse(
            reinterpret_cast<const char*>(bytes.data()), bytes.size(), arena) != nullptr;
    upb_Arena_Free(arena);
    return ok;
}

struct EncodeResult
{
    microtel_leaf_status_t status = MICROTEL_LEAF_OK;
    std::size_t written = 0;
    std::vector<std::uint8_t> bytes;
    int calls = 0;
};

struct Collector
{
    std::vector<std::uint8_t> bytes;
    int calls = 0;
    bool fail = false;
};

int Collect(void* ctx, const std::uint8_t* bytes, std::size_t len)
{
    auto* c = static_cast<Collector*>(ctx);
    ++c->calls;
    if (c->fail)
    {
        return -1;
    }
    c->bytes.insert(c->bytes.end(), bytes, bytes + len);
    return 0;
}

EncodeResult EncodeBuffer(microtel_leaf_t* leaf, std::size_t cap)
{
    EncodeResult r;
    r.bytes.assign(cap, 0);
    r.status = microtel_leaf_encode(leaf, cap == 0 ? nullptr : r.bytes.data(), cap, &r.written);
    r.bytes.resize(r.status == MICROTEL_LEAF_OK ? r.written : 0U);
    return r;
}

EncodeResult EncodeStream(microtel_leaf_t* leaf, bool fail)
{
    Collector c;
    c.fail = fail;
    EncodeResult r;
    r.status = microtel_leaf_encode_to(leaf, &Collect, &c, &r.written);
    r.bytes = std::move(c.bytes);
    r.calls = c.calls;
    return r;
}

std::string Compare(const EncodeResult& upb, const EncodeResult& nanopb)
{
    if (upb.status != nanopb.status)
    {
        return "status upb=" + std::to_string(upb.status) +
               " nanopb=" + std::to_string(nanopb.status);
    }
    if (upb.written != nanopb.written)
    {
        return "written upb=" + std::to_string(upb.written) +
               " nanopb=" + std::to_string(nanopb.written);
    }
    if (upb.status == MICROTEL_LEAF_OK && upb.bytes != nanopb.bytes)
    {
        return "bytes differ (" + std::to_string(upb.bytes.size()) + " bytes)";
    }
    if (upb.status == MICROTEL_LEAF_OK && !Decodes(upb.bytes))
    {
        return "payload does not decode";
    }
    return {};
}

// The two leaves and the handles the program has seen.
class Program
{
public:
    Program(const std::uint8_t* data, std::size_t size) : m_r(data, size) {}

    Outcome Run()
    {
        if (!Init())
        {
            return m_out;
        }
        for (std::size_t op = 0; op < kMaxOps && !m_r.Done() && m_out.mismatch.empty(); ++op)
        {
            Step(m_r.Byte() % kOpCount);
        }
        return m_out;
    }

private:
    microtel_leaf_t* Upb()
    {
        return &m_sides.at(0).leaf;
    }
    microtel_leaf_t* Nanopb()
    {
        return &m_sides.at(1).leaf;
    }

    // Both leaves must answer every call alike.
    void Same(const char* call, microtel_leaf_status_t a, microtel_leaf_status_t b)
    {
        if (a != b && m_out.mismatch.empty())
        {
            m_out.mismatch = std::string(call) + ": status upb-leaf=" + std::to_string(a) +
                             " nanopb-leaf=" + std::to_string(b);
        }
    }

    // The Resource: up to three attributes, read from the program.
    std::vector<microtel_leaf_kv_t> ReadResource(unsigned count)
    {
        for (unsigned i = 0; i < count; ++i)
        {
            m_resource.push_back(ReadKv(m_r));
        }
        std::vector<microtel_leaf_kv_t> views;
        views.reserve(m_resource.size());
        for (const auto& kv : m_resource)
        {
            views.push_back(kv.View());
        }
        return views;
    }

    // The four configuration bytes, less the per-leaf clock and random source.
    static microtel_leaf_config_t Config(const std::array<std::uint8_t, 4>& c,
                                         const std::vector<microtel_leaf_kv_t>& resource)
    {
        const char* scope = kScopes.at((c.at(3) >> 4U) % kScopes.size());
        microtel_leaf_config_t cfg{};
        cfg.struct_size = sizeof(cfg);
        cfg.time_mode = static_cast<microtel_leaf_time_mode_t>(c.at(0) % 3U);
        const bool no_clock =
            cfg.time_mode == MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED && (c.at(0) & 0x4U) != 0;
        cfg.now_ns = no_clock ? nullptr : &ReadClock;
        cfg.random_bytes = &FillRandom;
        cfg.resource = resource.data();
        cfg.resource_count = resource.size();
        cfg.scope_name = scope;
        cfg.scope_name_len = std::strlen(scope);
        cfg.scope_version = "1.0";
        cfg.scope_version_len = 3;
        cfg.max_attributes_per_span = static_cast<std::uint16_t>(c.at(2) % 8U);
        cfg.max_events_per_span = static_cast<std::uint16_t>((c.at(2) >> 3U) % 4U);
        cfg.max_attributes_per_event = static_cast<std::uint16_t>(c.at(3) % 4U);
        cfg.boot_id = c.at(3) >> 6U;
        return cfg;
    }

    bool Init()
    {
        const std::array<std::uint8_t, 4> c = {m_r.Byte(), m_r.Byte(), m_r.Byte(), m_r.Byte()};
        const std::vector<microtel_leaf_kv_t> resource = ReadResource((c.at(3) >> 2U) % 4U);
        microtel_leaf_config_t cfg = Config(c, resource);
        std::array<microtel_leaf_status_t, 2> st{};
        for (std::size_t i = 0; i < m_sides.size(); ++i)
        {
            Side& s = m_sides.at(i);
            s.record.assign(kRecordBase + ((c.at(1) % kRecordSteps) * kRecordStep), 0);
            s.random.state = c.at(0) | (static_cast<std::uint64_t>(c.at(1)) << kBitsPerByte);
            cfg.clock_ctx = &s.clock;
            cfg.random_ctx = &s.random;
            st.at(i) =
                microtel_leaf_init(&s.leaf, sizeof(s.leaf), &cfg, s.record.data(), s.record.size());
        }
        Same("init", st.at(0), st.at(1));
        return st.at(0) == MICROTEL_LEAF_OK && m_out.mismatch.empty();
    }

    // A handle the program has seen, or 0 (always invalid).
    microtel_leaf_span_t Handle()
    {
        const std::uint8_t b = m_r.Byte();
        return m_handles.empty() ? 0U : m_handles.at(b % m_handles.size());
    }

    void Started(microtel_leaf_status_t a,
                 microtel_leaf_status_t b,
                 microtel_leaf_span_t ha,
                 microtel_leaf_span_t hb)
    {
        Same("span_start", a, b);
        if (ha != hb && m_out.mismatch.empty())
        {
            m_out.mismatch = "span_start: handles differ";
        }
        if (a == MICROTEL_LEAF_OK)
        {
            m_handles.push_back(ha);
        }
    }

    void Start()
    {
        const std::string name = m_r.Str();
        const auto kind = static_cast<microtel_leaf_span_kind_t>(m_r.Byte() % kKinds);
        const std::uint8_t p = m_r.Byte();
        const microtel_leaf_span_t parent = Handle();
        const microtel_leaf_span_t* pp = p % 3U == 0 ? nullptr : &parent;
        microtel_leaf_span_t ha = 0;
        microtel_leaf_span_t hb = 0;
        const auto a = microtel_leaf_span_start(Upb(), &ha, name.data(), name.size(), kind, pp);
        const auto b = microtel_leaf_span_start(Nanopb(), &hb, name.data(), name.size(), kind, pp);
        Started(a, b, ha, hb);
    }

    void StartRemote()
    {
        const std::string name = m_r.Str();
        const auto kind = static_cast<microtel_leaf_span_kind_t>(m_r.Byte() % kKinds);
        std::array<std::uint8_t, kTraceIdBytes> trace{};
        std::array<std::uint8_t, kSpanIdBytes> parent{};
        for (auto& v : trace)
        {
            v = m_r.Byte();
        }
        for (auto& v : parent)
        {
            v = m_r.Byte();
        }
        microtel_leaf_span_t ha = 0;
        microtel_leaf_span_t hb = 0;
        const auto a = microtel_leaf_span_start_remote(
            Upb(), &ha, name.data(), name.size(), kind, trace.data(), parent.data());
        const auto b = microtel_leaf_span_start_remote(
            Nanopb(), &hb, name.data(), name.size(), kind, trace.data(), parent.data());
        Started(a, b, ha, hb);
    }

    void SetAttribute()
    {
        const microtel_leaf_span_t h = Handle();
        const OwnedKv owned = ReadKv(m_r);
        const microtel_leaf_kv_t kv = owned.View();
        Same("set_attribute",
             microtel_leaf_span_set_attribute(Upb(), h, &kv),
             microtel_leaf_span_set_attribute(Nanopb(), h, &kv));
    }

    void AddEvent()
    {
        const microtel_leaf_span_t h = Handle();
        const std::string name = m_r.Str();
        const std::size_t n = m_r.Byte() % kMaxEventAttrs;
        std::vector<OwnedKv> owned;
        std::vector<microtel_leaf_kv_t> kvs;
        owned.reserve(n);
        kvs.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            owned.push_back(ReadKv(m_r));
        }
        for (const auto& o : owned)
        {
            kvs.push_back(o.View());
        }
        Same("add_event",
             microtel_leaf_span_add_event(Upb(), h, name.data(), name.size(), kvs.data(), n),
             microtel_leaf_span_add_event(Nanopb(), h, name.data(), name.size(), kvs.data(), n));
    }

    void SetStatus()
    {
        const microtel_leaf_span_t h = Handle();
        const auto code = static_cast<microtel_leaf_status_code_t>(m_r.Byte() % kStatusCodes);
        const std::string msg = m_r.Str();
        Same("set_status",
             microtel_leaf_span_set_status(Upb(), h, code, msg.data(), msg.size()),
             microtel_leaf_span_set_status(Nanopb(), h, code, msg.data(), msg.size()));
    }

    void End()
    {
        const microtel_leaf_span_t h = Handle();
        Same("span_end", microtel_leaf_span_end(Upb(), h), microtel_leaf_span_end(Nanopb(), h));
    }

    void Record(const EncodeResult& a, const EncodeResult& b)
    {
        ++m_out.encodes;
        const std::string diff = Compare(a, b);
        if (!diff.empty() && m_out.mismatch.empty())
        {
            m_out.mismatch = "encode #" + std::to_string(m_out.encodes) + ": " + diff;
        }
    }

    // Asks for the size, then (usually) encodes into a buffer of exactly that
    // size, one byte less, or plenty.
    void Encode()
    {
        const std::uint8_t how = m_r.Byte();
        microtel_leaf_dual_select(MICROTEL_LEAF_DUAL_UPB);
        const EncodeResult qa = EncodeBuffer(Upb(), 0);
        microtel_leaf_dual_select(MICROTEL_LEAF_DUAL_NANOPB);
        const EncodeResult qb = EncodeBuffer(Nanopb(), 0);
        Record(qa, qb);
        if (qa.status != MICROTEL_LEAF_ERR_BUFFER_SMALL || how % 4U == 0)
        {
            return;
        }
        std::size_t cap = kOutSize;
        if (how % 4U == 1)
        {
            cap = qa.written;
        }
        else if (how % 4U == 2)
        {
            cap = qa.written - 1U;
        }
        microtel_leaf_dual_select(MICROTEL_LEAF_DUAL_UPB);
        const EncodeResult a = EncodeBuffer(Upb(), cap);
        microtel_leaf_dual_select(MICROTEL_LEAF_DUAL_NANOPB);
        const EncodeResult b = EncodeBuffer(Nanopb(), cap);
        Record(a, b);
    }

    // Streaming; a failing sink refuses its first write.
    void EncodeTo()
    {
        const bool fail = m_r.Byte() % 4U == 0;
        microtel_leaf_dual_select(MICROTEL_LEAF_DUAL_UPB);
        const EncodeResult a = EncodeStream(Upb(), fail);
        microtel_leaf_dual_select(MICROTEL_LEAF_DUAL_NANOPB);
        const EncodeResult b = EncodeStream(Nanopb(), fail);
        Record(a, b);
    }

    void ClockSync()
    {
        const std::uint64_t back = static_cast<std::uint64_t>(m_r.Byte()) * kClockStep;
        const std::uint64_t unix_ns = kSyncUnix + (m_r.U64() % kSyncUnix);
        const std::uint64_t now = m_sides.at(0).clock.now;
        const std::uint64_t leaf_now = back < now ? now - back : 0U;
        Same("clock_sync",
             microtel_leaf_clock_sync(Upb(), unix_ns, leaf_now),
             microtel_leaf_clock_sync(Nanopb(), unix_ns, leaf_now));
    }

    void AdvanceClock()
    {
        const std::uint64_t by = static_cast<std::uint64_t>(m_r.Byte()) * kClockStep;
        for (auto& s : m_sides)
        {
            s.clock.now += by;
        }
    }

    void Step(unsigned op)
    {
        switch (op)
        {
            case 0:
                Start();
                break;
            case 1:
                StartRemote();
                break;
            case 2:
                SetAttribute();
                break;
            case 3:
                AddEvent();
                break;
            case 4:
                SetStatus();
                break;
            case 5:
                End();
                break;
            case 6:
                Encode();
                break;
            case 7:
                EncodeTo();
                break;
            case 8:
                ClockSync();
                break;
            default:
                AdvanceClock();
                break;
        }
    }

    Reader m_r;
    std::array<Side, 2> m_sides;
    std::vector<OwnedKv> m_resource;
    std::vector<microtel_leaf_span_t> m_handles;
    Outcome m_out;
};

}  // namespace

Outcome RunProgram(const std::uint8_t* data, std::size_t size)
{
    // Side holds a large leaf state; keep the pair off the fuzzer's stack.
    auto program = std::make_unique<Program>(data, size);
    return program->Run();
}

Outcome CompareVector(std::size_t index)
{
    EncodeResult a;
    EncodeResult b;
    a.bytes.assign(kOutSize, 0);
    b.bytes.assign(kOutSize, 0);
    microtel_leaf_dual_select(MICROTEL_LEAF_DUAL_UPB);
    a.status = microtel_leaf_test_vector_encode(index, a.bytes.data(), a.bytes.size(), &a.written);
    microtel_leaf_dual_select(MICROTEL_LEAF_DUAL_NANOPB);
    b.status = microtel_leaf_test_vector_encode(index, b.bytes.data(), b.bytes.size(), &b.written);
    a.bytes.resize(a.status == MICROTEL_LEAF_OK ? a.written : 0U);
    b.bytes.resize(b.status == MICROTEL_LEAF_OK ? b.written : 0U);
    Outcome out;
    out.encodes = 1;
    out.mismatch = Compare(a, b);
    return out;
}

}  // namespace microtel::leaf_diff
