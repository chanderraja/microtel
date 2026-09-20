// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// sugar_tour — every helper in `microtel::sugar`, inside one small order
// pipeline.
//
// The sugar layer is header-only and adds nothing to a consumer's link
// closure: every helper compiles to the two or three public calls you would
// have written by hand. So this example is not about capability, it is about
// what the call sites look like — and the trace it emits is meant to read as
// a story in Grafana, not as a checklist of API names.
//
// The story: an order is submitted, its cart validated, inventory reserved,
// the card declined, and the reservation released. One trace, five spans, one
// exception event.
//
//   order.submit                                      Server, status=Error
//   |- <ValidateCart's function signature>            MICROTEL_TRACE_FUNCTION
//   |  \- cart.items.check                            mt::Span
//   |- inventory.reserve                              mt::Traced -> int64
//   |  \- warehouse.lookup                            mt::Span
//   |- payment.charge                                 Client, exception event
//   \- inventory.release                              mt::Span
//
// Usage:
//   sugar_tour [endpoint]
//
// where [endpoint] defaults to http://localhost:4317 — the OTLP/gRPC receiver
// of the shared examples stack, started with:
//   examples/stack/up.sh
//
// Not one function below passes a span or a parent to the function it calls.
// That is `StartAsCurrentSpan` underneath every sugar helper; the mechanism
// is examples/context_propagation/'s subject.

#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/sugar.hpp"
#include "microtel/tracer.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

// The `mt` alias is **consumer-side**. No microtel header declares it: a
// library that squats a two-character global name has taken something it
// cannot give back. One line per file that wants it, and the call sites below
// read the way microtel-roadmap.md §5 writes them.
namespace mt = microtel::sugar;

namespace
{

constexpr std::chrono::seconds kFlushTimeout{5};
constexpr std::chrono::seconds kShutdownTimeout{5};
constexpr const char* kDefaultEndpoint{"http://localhost:4317"};

// ---------------------------------------------------------------------------
// mt::AttrKey — each attribute key is spelled exactly once, here.
// ---------------------------------------------------------------------------
//
// `constexpr` at namespace scope means constant-initialised: no allocation
// anywhere in the type, and no static-initialisation order to reason about.
// The key is a borrowed `std::string_view` over a string literal, whose length
// is therefore fixed at compile time — no `strlen` at each call site where the
// compiler cannot see the literal.
//
// What it buys beyond that is a distinct type: a key cannot be passed where a
// value is expected. What it deliberately does not buy in v1.1 is pre-encoded
// wire bytes — see ICP 0028 §2, which schedules that for v1.5 and explains why
// shipping the plain binder now keeps the later change source-compatible.
constexpr mt::AttrKey kOrderId{"order.id"};
constexpr mt::AttrKey kOrderTotal{"order.total_cents"};
constexpr mt::AttrKey kSku{"inventory.sku"};
constexpr mt::AttrKey kUnits{"inventory.units"};

constexpr std::int64_t kOrderTotalCents{4299};
constexpr std::int64_t kRequestedUnits{3};
constexpr const char* kSkuValue{"WIDGET-7"};
constexpr const char* kOrderIdValue{"ord-20260919-0042"};

const char* StatusToString(microtel::Status status) noexcept
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
        // Setter-only outcomes. ForceFlush and Shutdown never return these,
        // but the switch is exhaustive so -Wswitch keeps this honest if the
        // enum grows again — which is exactly what happened in v1.1.
        case microtel::Status::InvalidArgument:
            return "InvalidArgument";
        case microtel::Status::Unsupported:
            return "Unsupported";
    }
    return "Unknown";
}

/// @brief What the fake payment gateway throws. Any `std::exception` works.
class PaymentDeclined : public std::runtime_error
{
public:
    explicit PaymentDeclined(const std::string& what) : std::runtime_error(what) {}
};

/// @brief The fake gateway. Always declines, so the example always has an
///        exception to record.
void ContactGateway(std::int64_t amount_cents)
{
    throw PaymentDeclined{"card declined: insufficient funds for " + std::to_string(amount_cents) +
                          " cents"};
}

// ---------------------------------------------------------------------------
// 1. MICROTEL_TRACE_FUNCTION — a span named for the enclosing function.
// ---------------------------------------------------------------------------
//
// A macro rather than a function because only a declaration can create the
// variable whose lifetime *is* the scope. It takes the tracer because microtel
// has no global provider, and adding a mutable global with
// static-destruction-order problems for a convenience macro's benefit is not a
// trade this project makes (ICP 0028, Discrepancies §1).
//
// The span name is `std::source_location::current().function_name()`, which on
// clang and gcc is the **full signature**, not the bare identifier — look for
// `void (anonymous namespace)::ValidateCart(microtel::Tracer &)` in Grafana.
// That is the honest cost of a zero-argument name: when you want a short,
// stable, hand-picked span name, `mt::Span` is the helper that takes one.
//
// The declared variable is `__LINE__`-uniqued and `const`, so it is not
// nameable here — which is the point. This form is for "just trace this
// function"; reach for `mt::Span` the moment you want to touch the span.
void ValidateCart(microtel::Tracer& tracer)
{
    MICROTEL_TRACE_FUNCTION(tracer);

    // Nothing below mentions a parent. The macro's scope is the current span
    // for the rest of this function, so this child parents to it implicitly.
    const auto items = mt::Span(tracer, "cart.items.check", {kSku(std::string{kSkuValue})});
    items->AddEvent("cart.line_items.counted");
}

// ---------------------------------------------------------------------------
// 2. mt::Traced — run something inside a span and keep its return value.
// ---------------------------------------------------------------------------
//
// The return type is forwarded exactly (`decltype(auto)`): a value stays a
// value, a reference stays a reference, `void` stays `void`, and the span ends
// after the return value has been initialised. `Traced` is conditionally
// `noexcept` — `noexcept` iff invoking the callable is — so a `noexcept`
// caller keeps its guarantee.
//
// If the callable throws, the span is ended by `ScopedSpan`'s destructor
// during unwinding and the exception propagates **unrecorded**. That is
// deliberate: recording it is `mt::TryCatch`, roadmap §5 v1.4, and a `Traced`
// that swallowed-and-rethrew would make that helper redundant. To record an
// exception today, catch it and call `mt::RecordException` — as ChargeCard
// does below.
std::int64_t ReserveInventory(microtel::Tracer& tracer)
{
    return mt::Traced(tracer,
                      "inventory.reserve",
                      [&tracer]() -> std::int64_t
                      {
                          const auto lookup =
                              mt::Span(tracer, "warehouse.lookup", {kSku(std::string{kSkuValue})});
                          lookup->SetAttribute("warehouse.region", std::string{"us-east-1"});
                          return kRequestedUnits;
                      });
}

// ---------------------------------------------------------------------------
// 3. mt::RecordException — Error status and the OTel `exception` event.
// ---------------------------------------------------------------------------
//
// Two calls on the public `Span` in one: `SetStatus(Error, e.what())` and
// `AddEvent("exception", {exception.type, exception.message})`.
//
// `exception.type` is `typeid(e).name()` — the implementation-**mangled** name,
// left that way on purpose. Demangling needs `abi::__cxa_demangle`, which
// allocates on an error path and is ABI-specific, while the mangled form is
// stable and greppable. Expect `N12_GLOBAL__N_115PaymentDeclinedE` in the
// event, not `PaymentDeclined`. The two-argument overload
// `RecordException(span, type, message)` is the answer when you want a pretty
// name — or when you are compiling with `-fno-rtti`, which is what the
// `typeid` overload costs.
//
// Setting `Error` status is a deliberate divergence from opentelemetry-cpp's
// similarly-named `Span::RecordException`, which does not: roadmap §5 v1.1
// specifies both halves, and spec §18.1 excludes sugar from conformance
// testing, so this helper is not measured against the OTel API surface.
bool ChargeCard(microtel::Tracer& tracer, std::string_view order_id)
{
    // mt::Span's inline attributes. `attributes` is viewed, not owned: the
    // initializer_list's backing array lives to the end of this
    // full-expression, and StartAsCurrentSpan copies the attributes into the
    // span record inside the call. The returned scope holds no reference.
    const auto charge = mt::Span(
        tracer, "payment.charge", {kOrderTotal(kOrderTotalCents)}, microtel::SpanKind::Client);

    // AttrKey's other form: Set() on a live span, rather than operator()
    // building a KeyValue for the initializer_list paths above. Same key, one
    // spelling.
    kOrderId.Set(*charge, std::string{order_id});

    try
    {
        ContactGateway(kOrderTotalCents);
        return true;
    }
    catch (const PaymentDeclined& declined)
    {
        // `*charge` is the `Span&` — ScopedSpan's operator* borrows it; the
        // scope still owns it and still ends it.
        mt::RecordException(*charge, declined);
        return false;
    }
}

/// @brief The compensating step when the charge fails.
void ReleaseInventory(microtel::Tracer& tracer, std::int64_t units)
{
    const auto release = mt::Span(tracer, "inventory.release", {kUnits(units)});
    release->AddEvent("inventory.reservation.released");
}

/// @brief The whole pipeline. Returns the trace ID as lowercase hex, which is
///        what a backend indexes the trace under.
std::string SubmitOrder(microtel::Tracer& tracer, std::string_view order_id)
{
    const auto order = mt::Span(tracer,
                                "order.submit",
                                {kOrderId(std::string{order_id}), kOrderTotal(kOrderTotalCents)},
                                microtel::SpanKind::Server);
    const std::string trace_id = order->GetContext().trace_id.ToHex();

    ValidateCart(tracer);

    const std::int64_t units = ReserveInventory(tracer);
    kUnits.Set(*order, units);

    if (ChargeCard(tracer, order_id))
    {
        order->SetStatus(microtel::StatusCode::Ok);
        return trace_id;
    }

    ReleaseInventory(tracer, units);
    order->SetStatus(microtel::StatusCode::Error, "payment declined");
    return trace_id;
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string endpoint{(argc > 1) ? argv[1] : kDefaultEndpoint};

    auto built = microtel::SdkBuilder{}
                     .WithEndpoint(endpoint)
                     .WithProtocol(microtel::Protocol::Grpc)
                     .WithServiceName("microtel-sugar-tour")
                     .WithServiceVersion("1.0.0")
                     .Build();

    if (!built)
    {
        std::cerr << "SdkBuilder::Build() failed: " << built.error().message << '\n';
        return 1;
    }

    const std::shared_ptr<microtel::Provider> provider = std::move(*built);

    if (auto connected = provider->Connect(); !connected)
    {
        std::cerr << "warning: Connect() to " << endpoint
                  << " failed: " << connected.error().message
                  << "\n         is an OTLP/gRPC collector listening there? "
                     "continuing — export will be retried on flush.\n";
    }

    const std::shared_ptr<microtel::Tracer> tracer =
        provider->GetTracer("microtel-sugar-tour", "1.0.0");

    const std::string trace_id = SubmitOrder(*tracer, kOrderIdValue);
    std::cout << "trace_id: " << trace_id << '\n';

    const microtel::Status flush = provider->ForceFlush(kFlushTimeout);
    std::cout << "ForceFlush: " << StatusToString(flush) << '\n';

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    std::cout << "batches_sent=" << health.batches_sent
              << " batches_failed=" << health.batches_failed
              << " queue_depth=" << health.queue_depth_now << '\n';
    if (!health.last_error_message.empty())
    {
        std::cout << "last_error: " << health.last_error_message << '\n';
    }

    const microtel::Status shutdown = provider->Shutdown(kShutdownTimeout);
    std::cout << "Shutdown: " << StatusToString(shutdown) << '\n';

    std::cout << "\nview it:  http://localhost:3000  (home dashboard: microtel — recent traces)\n"
              << "     or:  curl -s http://localhost:3200/api/traces/" << trace_id << '\n';

    return (flush == microtel::Status::Completed) ? 0 : 2;
}
