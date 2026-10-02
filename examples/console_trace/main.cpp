// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// console_trace — see your spans with nothing running but this program.
//
// No collector, no containers, no network. The Provider exports through an
// ExportTransport of the example's own (SdkBuilder::WithExportTransport,
// ICP 0036) that, instead of sending each encoded OTLP request anywhere,
// decodes it and prints the spans to stdout. Everything up to the last hop is
// the real pipeline: sampling, batching, the OTLP encoder, the exporter worker.
// What is printed is decoded from the exact bytes a collector would receive.
//
// Usage:
//   console_trace
//
// The decoder below reads only the OTLP fields it prints. It is example code:
// a hand-written protobuf walker, so the example links nothing beyond
// microtel_sdk. For anything real, export to a collector (examples/basic_trace).

#include "microtel/export_transport.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace
{

/// @brief Items the collector rejected via OTLP partial success. It answered
///        success, so their batch counts as sent, never as failed.
std::uint64_t PartialSuccessRejected(const microtel::HealthSnapshot& health)
{
    return health
        .drop_counters[static_cast<std::size_t>(microtel::DropReason::PartialSuccessRejection)];
}

constexpr std::chrono::seconds kFlushTimeout{5};
constexpr std::chrono::seconds kShutdownTimeout{5};
constexpr std::int64_t kHttpStatusOk{200};
constexpr std::chrono::milliseconds kQueryTime{3};
constexpr std::chrono::milliseconds kRenderTime{1};

// ---------------------------------------------------------------------------
// A minimal protobuf wire-format reader.
// ---------------------------------------------------------------------------

using Bytes = std::span<const std::byte>;

/// Protobuf wire types (protobuf.dev/programming-guides/encoding).
enum class WireType : std::uint8_t
{
    Varint = 0,
    Fixed64 = 1,
    Len = 2,
    Fixed32 = 5,
};

constexpr unsigned kTagTypeBits{3};
constexpr std::uint64_t kTagTypeMask{0x7};
constexpr unsigned kVarintPayloadBits{7};
constexpr std::uint8_t kVarintPayloadMask{0x7f};
constexpr std::uint8_t kVarintMoreBit{0x80};
constexpr unsigned kVarintMaxShift{63};
constexpr std::size_t kFixed64Size{8};
constexpr std::size_t kFixed32Size{4};
constexpr unsigned kBitsPerByte{8};
constexpr int kHexDigitsPerByte{2};
constexpr int kMillisecondDecimals{3};

/// One field of a message. `value` holds a varint or fixed number; `bytes`
/// holds a length-delimited payload (a string, bytes or a nested message).
struct Field
{
    std::uint32_t number = 0;
    WireType type = WireType::Varint;
    std::uint64_t value = 0;
    Bytes bytes;
};

class WireReader
{
public:
    explicit WireReader(Bytes buffer) noexcept : m_buffer(buffer) {}

    /// The next field, or nullopt at the end of the message or on bad input.
    std::optional<Field> Next() noexcept
    {
        if (m_pos >= m_buffer.size())
        {
            return std::nullopt;
        }
        const std::optional<std::uint64_t> tag = Varint();
        if (!tag)
        {
            return std::nullopt;
        }
        Field field{.number = static_cast<std::uint32_t>(*tag >> kTagTypeBits),
                    .type = static_cast<WireType>(*tag & kTagTypeMask),
                    .value = 0,
                    .bytes = {}};
        return ReadValue(field) ? std::optional<Field>{field} : std::nullopt;
    }

private:
    bool ReadValue(Field& field) noexcept
    {
        switch (field.type)
        {
            case WireType::Varint:
                return VarintValue(field.value);
            case WireType::Fixed64:
                return Fixed(kFixed64Size, field.value);
            case WireType::Fixed32:
                return Fixed(kFixed32Size, field.value);
            case WireType::Len:
                return LengthDelimited(field.bytes);
        }
        return false;  // a wire type OTLP never uses
    }

    std::optional<std::uint64_t> Varint() noexcept
    {
        std::uint64_t result = 0;
        unsigned shift = 0;
        for (const std::byte next : m_buffer.subspan(m_pos))
        {
            const auto byte = std::to_integer<std::uint8_t>(next);
            ++m_pos;
            result |= static_cast<std::uint64_t>(byte & kVarintPayloadMask) << shift;
            shift += kVarintPayloadBits;
            if ((byte & kVarintMoreBit) == 0)
            {
                return result;
            }
            if (shift > kVarintMaxShift)
            {
                break;
            }
        }
        return std::nullopt;
    }

    bool VarintValue(std::uint64_t& out) noexcept
    {
        const std::optional<std::uint64_t> value = Varint();
        out = value.value_or(0);
        return value.has_value();
    }

    /// Little-endian fixed-width integer.
    bool Fixed(const std::size_t size, std::uint64_t& out) noexcept
    {
        if (size > m_buffer.size() - m_pos)
        {
            return false;
        }
        out = 0;
        unsigned shift = 0;
        for (const std::byte next : m_buffer.subspan(m_pos, size))
        {
            out |= std::to_integer<std::uint64_t>(next) << shift;
            shift += kBitsPerByte;
        }
        m_pos += size;
        return true;
    }

    bool LengthDelimited(Bytes& out) noexcept
    {
        const std::optional<std::uint64_t> size = Varint();
        if (!size || *size > m_buffer.size() - m_pos)
        {
            return false;
        }
        out = m_buffer.subspan(m_pos, static_cast<std::size_t>(*size));
        m_pos += out.size();
        return true;
    }

    Bytes m_buffer;
    std::size_t m_pos = 0;
};

/// Calls `visit(field)` for every field of `message`.
template <typename Visit>
void ForEachField(const Bytes message, const Visit& visit)
{
    WireReader reader{message};
    for (std::optional<Field> field = reader.Next(); field; field = reader.Next())
    {
        visit(*field);
    }
}

std::string AsString(const Bytes bytes)
{
    std::string out;
    out.reserve(bytes.size());
    for (const std::byte byte : bytes)
    {
        out.push_back(static_cast<char>(byte));
    }
    return out;
}

std::string AsHex(const Bytes bytes)
{
    if (bytes.empty())
    {
        return "-";
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const std::byte byte : bytes)
    {
        out << std::setw(kHexDigitsPerByte) << std::to_integer<unsigned>(byte);
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// The OTLP messages, reduced to the fields printed. Field numbers are from
// opentelemetry/proto/{collector/trace/v1,trace/v1,common/v1}/*.proto.
// ---------------------------------------------------------------------------

namespace otlp
{
constexpr std::uint32_t kRequestResourceSpans{1};  // ExportTraceServiceRequest
constexpr std::uint32_t kResourceSpansResource{1};
constexpr std::uint32_t kResourceSpansScopeSpans{2};
constexpr std::uint32_t kResourceAttributes{1};
constexpr std::uint32_t kScopeSpansScope{1};
constexpr std::uint32_t kScopeSpansSpans{2};
constexpr std::uint32_t kScopeName{1};
constexpr std::uint32_t kSpanTraceId{1};
constexpr std::uint32_t kSpanSpanId{2};
constexpr std::uint32_t kSpanParentSpanId{4};
constexpr std::uint32_t kSpanName{5};
constexpr std::uint32_t kSpanKind{6};
constexpr std::uint32_t kSpanStartTime{7};
constexpr std::uint32_t kSpanEndTime{8};
constexpr std::uint32_t kSpanAttributes{9};
constexpr std::uint32_t kSpanEvents{11};
constexpr std::uint32_t kSpanStatus{15};
constexpr std::uint32_t kEventName{2};
constexpr std::uint32_t kStatusCode{3};
constexpr std::uint32_t kKeyValueKey{1};
constexpr std::uint32_t kKeyValueValue{2};
constexpr std::uint32_t kAnyString{1};
constexpr std::uint32_t kAnyBool{2};
constexpr std::uint32_t kAnyInt{3};
constexpr std::uint32_t kAnyDouble{4};
}  // namespace otlp

/// common.v1.AnyValue: string, bool, int and double; anything else is elided.
std::string AnyValueText(const Bytes any_value)
{
    std::string text = "<...>";
    ForEachField(any_value,
                 [&text](const Field& f)
                 {
                     switch (f.number)
                     {
                         case otlp::kAnyString:
                             text = '"' + AsString(f.bytes) + '"';
                             break;
                         case otlp::kAnyBool:
                             text = (f.value != 0) ? "true" : "false";
                             break;
                         case otlp::kAnyInt:
                             text = std::to_string(static_cast<std::int64_t>(f.value));
                             break;
                         case otlp::kAnyDouble:
                             text = std::to_string(std::bit_cast<double>(f.value));
                             break;
                         default:
                             break;
                     }
                 });
    return text;
}

/// common.v1.KeyValue as `key=value`.
std::string KeyValueText(const Bytes key_value)
{
    std::string key;
    std::string value;
    ForEachField(key_value,
                 [&](const Field& f)
                 {
                     if (f.number == otlp::kKeyValueKey)
                     {
                         key = AsString(f.bytes);
                     }
                     else if (f.number == otlp::kKeyValueValue)
                     {
                         value = AnyValueText(f.bytes);
                     }
                 });
    return key + '=' + value;
}

/// The string in field `number` of a message: an event's or a scope's name.
std::string NameField(const Bytes message, const std::uint32_t number)
{
    std::string name;
    ForEachField(message,
                 [&](const Field& f)
                 {
                     if (f.number == number)
                     {
                         name = AsString(f.bytes);
                     }
                 });
    return name;
}

/// The number in field `number` of a message: a status's code.
std::uint64_t NumberField(const Bytes message, const std::uint32_t number)
{
    std::uint64_t value = 0;
    ForEachField(message,
                 [&](const Field& f)
                 {
                     if (f.number == number)
                     {
                         value = f.value;
                     }
                 });
    return value;
}

const char* KindText(const std::uint64_t kind) noexcept
{
    constexpr auto kKinds = std::to_array<const char*>(
        {"Unspecified", "Internal", "Server", "Client", "Producer", "Consumer"});
    return kind < kKinds.size() ? kKinds.at(kind) : "?";
}

const char* StatusText(const std::uint64_t code) noexcept
{
    constexpr auto kCodes = std::to_array<const char*>({"Unset", "Ok", "Error"});
    return code < kCodes.size() ? kCodes.at(code) : "?";
}

/// trace.v1.Span, collected field by field, then printed as a header line and
/// one indented line per ID, attribute and event.
class SpanText
{
public:
    void Add(const Field& f)
    {
        switch (f.number)
        {
            case otlp::kSpanTraceId:
                m_ids += "    trace_id  " + AsHex(f.bytes) + '\n';
                break;
            case otlp::kSpanSpanId:
                m_ids += "    span_id   " + AsHex(f.bytes) + '\n';
                break;
            case otlp::kSpanParentSpanId:
                m_ids += "    parent    " + AsHex(f.bytes) + '\n';
                break;
            case otlp::kSpanName:
                m_name = AsString(f.bytes);
                break;
            case otlp::kSpanKind:
                m_kind = f.value;
                break;
            case otlp::kSpanStartTime:
                m_start_ns = f.value;
                break;
            case otlp::kSpanEndTime:
                m_end_ns = f.value;
                break;
            case otlp::kSpanAttributes:
                m_details += "    attr      " + KeyValueText(f.bytes) + '\n';
                break;
            case otlp::kSpanEvents:
                m_details += "    event     " + NameField(f.bytes, otlp::kEventName) + '\n';
                break;
            case otlp::kSpanStatus:
                m_status = NumberField(f.bytes, otlp::kStatusCode);
                break;
            default:
                break;
        }
    }

    void Print(std::ostream& out) const
    {
        const std::chrono::duration<double, std::milli> duration =
            std::chrono::nanoseconds(m_end_ns - m_start_ns);
        out << "  span " << m_name << "  kind=" << KindText(m_kind)
            << "  status=" << StatusText(m_status) << "  duration=" << std::fixed
            << std::setprecision(kMillisecondDecimals) << duration.count() << " ms\n"
            << m_ids << m_details;
    }

private:
    std::string m_name;
    std::string m_ids;
    std::string m_details;
    std::uint64_t m_kind = 0;
    std::uint64_t m_start_ns = 0;
    std::uint64_t m_end_ns = 0;
    std::uint64_t m_status = 0;
};

void PrintSpan(const Bytes span, std::ostream& out)
{
    SpanText text;
    ForEachField(span, [&text](const Field& f) { text.Add(f); });
    text.Print(out);
}

/// trace.v1.ScopeSpans: the instrumentation scope, then its spans.
void PrintScopeSpans(const Bytes scope_spans, std::ostream& out)
{
    ForEachField(scope_spans,
                 [&out](const Field& f)
                 {
                     if (f.number == otlp::kScopeSpansScope)
                     {
                         out << "scope " << NameField(f.bytes, otlp::kScopeName) << '\n';
                     }
                     else if (f.number == otlp::kScopeSpansSpans)
                     {
                         PrintSpan(f.bytes, out);
                     }
                 });
}

/// resource.v1.Resource: one line per attribute.
void PrintResource(const Bytes resource, std::ostream& out)
{
    ForEachField(resource,
                 [&out](const Field& f)
                 {
                     if (f.number == otlp::kResourceAttributes)
                     {
                         out << "resource " << KeyValueText(f.bytes) << '\n';
                     }
                 });
}

/// trace.v1.ResourceSpans: the resource's attributes, then each scope.
void PrintResourceSpans(const Bytes resource_spans, std::ostream& out)
{
    ForEachField(resource_spans,
                 [&out](const Field& f)
                 {
                     if (f.number == otlp::kResourceSpansResource)
                     {
                         PrintResource(f.bytes, out);
                     }
                     else if (f.number == otlp::kResourceSpansScopeSpans)
                     {
                         PrintScopeSpans(f.bytes, out);
                     }
                 });
}

// ---------------------------------------------------------------------------
// The transport.
// ---------------------------------------------------------------------------

/// Prints each ExportTraceServiceRequest instead of sending it.
///
/// Send never blocks on anything but the write to stdout, so it is bounded
/// without consulting the deadline; Cancel only makes later Sends return at
/// once, which is all a transport that cannot block owes the Provider.
class ConsoleExportTransport final : public microtel::ExportTransport
{
public:
    [[nodiscard]] microtel::SendResult Send(const microtel::ExportRequest& request) override
    {
        if (m_cancelled.load(std::memory_order_acquire))
        {
            return Result(microtel::SendOutcome::NonRetryable, "cancelled");
        }
        // Build the text first and write it once, so it cannot interleave
        // with the main thread's output.
        std::ostringstream text;
        text << "--- export request: " << request.bytes.size() << " bytes of OTLP protobuf\n";
        ForEachField(request.bytes,
                     [&text](const Field& f)
                     {
                         if (f.number == otlp::kRequestResourceSpans)
                         {
                             PrintResourceSpans(f.bytes, text);
                         }
                     });
        std::cout << text.str() << std::flush;
        return Result(microtel::SendOutcome::Success, {});
    }

    void Cancel() noexcept override
    {
        m_cancelled.store(true, std::memory_order_release);
    }

private:
    [[nodiscard]] static microtel::SendResult Result(const microtel::SendOutcome outcome,
                                                     std::string message)
    {
        return microtel::SendResult{.outcome = outcome,
                                    .retry_after = std::nullopt,
                                    .rejected = 0,
                                    .message = std::move(message)};
    }

    std::atomic<bool> m_cancelled{false};
};

// ---------------------------------------------------------------------------
// The application: the same request trace basic_trace emits.
// ---------------------------------------------------------------------------

const char* StatusToString(const microtel::Status status) noexcept
{
    switch (status)
    {
        case microtel::Status::Completed:
            return "Completed";
        case microtel::Status::TimedOut:
            return "TimedOut";
        case microtel::Status::AlreadyShutDown:
            return "AlreadyShutDown";
        case microtel::Status::Failed:
            return "Failed";
        case microtel::Status::InvalidArgument:
            return "InvalidArgument";
        case microtel::Status::Unsupported:
            return "Unsupported";
    }
    return "Unknown";
}

/// A Server parent span with two Internal children. Returns the trace ID.
std::string EmitRequestTrace(microtel::Tracer& tracer)
{
    const auto parent = tracer.StartSpan(
        "example.request",
        {.kind = microtel::SpanKind::Server, .parent = {}, .start_time = {}, .attributes = {}});
    parent->SetAttribute("http.request.method", std::string{"GET"});
    parent->SetAttribute("url.path", std::string{"/api/widgets"});

    const microtel::SpanContext parent_ctx = parent->GetContext();

    {
        const auto query = tracer.StartSpan("example.db.query",
                                            {.kind = microtel::SpanKind::Internal,
                                             .parent = parent_ctx,
                                             .start_time = {},
                                             .attributes = {}});
        query->SetAttribute("db.system", std::string{"postgresql"});
        query->AddEvent("query.start");
        std::this_thread::sleep_for(kQueryTime);
        query->End();
    }

    {
        const auto render = tracer.StartSpan("example.render",
                                             {.kind = microtel::SpanKind::Internal,
                                              .parent = parent_ctx,
                                              .start_time = {},
                                              .attributes = {}});
        render->SetAttribute("template", std::string{"widgets.html"});
        std::this_thread::sleep_for(kRenderTime);
        render->End();
    }

    parent->SetAttribute("http.response.status_code", kHttpStatusOk);
    parent->SetStatus(microtel::StatusCode::Ok);
    parent->End();

    return parent_ctx.trace_id.ToHex();
}

}  // namespace

int main()
{
    // WithExportTransport replaces microtel's HTTP/2 transport, so there is no
    // endpoint to set and nothing to Connect() to.
    auto built = microtel::SdkBuilder{}
                     .WithServiceName("microtel-console-example")
                     .WithServiceVersion("1.0.0")
                     .WithExportTransport(std::make_unique<ConsoleExportTransport>())
                     .Build();
    if (!built)
    {
        std::cerr << "SdkBuilder::Build() failed: " << built.error().message << '\n';
        return 1;
    }
    const std::shared_ptr<microtel::Provider> provider = std::move(*built);

    const std::shared_ptr<microtel::Tracer> tracer =
        provider->GetTracer("microtel-console-example", "1.0.0");

    const std::string trace_id = EmitRequestTrace(*tracer);
    std::cout << "trace_id: " << trace_id << '\n' << std::flush;

    // The batch processor would export on its own schedule; ForceFlush makes
    // it export now, and the transport prints while this call waits.
    const microtel::Status flush = provider->ForceFlush(kFlushTimeout);
    std::cout << "ForceFlush: " << StatusToString(flush) << '\n';

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    std::cout << "batches_sent=" << health.batches_sent
              << " batches_failed=" << health.batches_failed
              << " rejected=" << PartialSuccessRejected(health) << '\n';

    const microtel::Status shutdown = provider->Shutdown(kShutdownTimeout);
    std::cout << "Shutdown: " << StatusToString(shutdown) << '\n';

    // Completed only means the queues drained; a batch the collector rejected
    // still counts as drained, and so does one it answered with partial
    // success. Success needs no failed batch and no rejected item as well.
    const bool delivered = flush == microtel::Status::Completed && health.batches_failed == 0 &&
                           PartialSuccessRejected(health) == 0;
    return delivered ? 0 : 2;
}
