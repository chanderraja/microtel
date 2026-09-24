# `sugar_tour`

Every helper in `microtel::sugar`, inside one small order pipeline.

The sugar layer is header-only and adds nothing to your link closure. Each
helper compiles down to the two or three public calls you would otherwise have
written by hand, so the sugar gives you no new capability. What this example
shows is how the call sites look with it, and a trace that reads like a
sequence of events instead of a list of API names.

Here's the sequence: an order is submitted, its cart is validated, inventory
is reserved, the card is declined, and the reservation is released.

```
order.submit                                       Server, status=Error
├── void (anonymous namespace)::ValidateCart(…)    MICROTEL_TRACE_FUNCTION
│   └── cart.items.check                           mt::Span
├── inventory.reserve                              mt::Traced → int64
│   └── warehouse.lookup                           mt::Span
├── payment.charge                                 Client, `exception` event
└── inventory.release                              mt::Span
```

**No function passes a span or a parent to the function it calls.** Every
sugar helper calls `StartAsCurrentSpan` underneath; see
[`context_propagation/`](../context_propagation/) for how that works.

## Run it

```bash
examples/stack/up.sh

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build --target microtel_example_sugar_tour

./build/examples/microtel_example_sugar_tour
```

```
trace_id: 04e2b1fa78ab325c22156b42e4c99921
ForceFlush: Completed
batches_sent=1 batches_failed=0 queue_depth=0
Shutdown: Completed

view it:  http://localhost:3000  (home dashboard: microtel — recent traces)
     or:  curl -s http://localhost:3200/api/traces/04e2b1fa78ab325c22156b42e4c99921
```

To export to a different collector, pass its endpoint as the first argument.

## The alias nobody declares

```cpp
namespace mt = microtel::sugar;   // in your code, never in ours
```

The namespace is `microtel::sugar`. No microtel header declares `mt`, because
a library that claims a two-character global name takes it away from every
program that includes it. Declaring the alias yourself costs one line per file,
and then your call sites look the way
[`microtel-roadmap.md`](../../microtel-roadmap.md) §5 writes them.

One thing to be aware of: `microtel::sugar::Span` is a function, so inside the
sugar headers the core types are written fully qualified (`::microtel::Span`).
In your own code the two don't collide. `mt::Span(...)` is the factory and
`microtel::Span` is the class.

## The five forms

### `MICROTEL_TRACE_FUNCTION(tracer)`: trace this function

```cpp
void ValidateCart(microtel::Tracer& tracer)
{
    MICROTEL_TRACE_FUNCTION(tracer);
    ...
}
```

It's a macro because only a declaration can create a variable whose lifetime
is the enclosing scope. Two things to expect.

It takes the tracer as an argument. `microtel-roadmap.md` §5 writes it with no
argument, but that form needs a process-global tracer, and microtel doesn't
have one. The project chose not to add a mutable global, with its
static-destruction-order problems, just to support a convenience macro. This
is recorded as Discrepancy 1 in
[ICP 0028](../../docs/icps/0028-sugar-surface.md).

The span name is the full function signature. It comes from
`std::source_location::current().function_name()`, so in Grafana the span
above is named `void (anonymous namespace)::ValidateCart(microtel::Tracer &)`
and not just `ValidateCart`. That's the price of a name you didn't have to
type. When you want a short, stable name of your own choosing, use `mt::Span`,
which takes one.

The variable the macro declares gets a `__LINE__`-based unique name, so you
can't refer to it, and it is `const`. Both are intentional: a scope you can't
move out of can't be destroyed out of order, which is the one programming
error [ICP 0025](../../docs/icps/0025-propagation-core.md) §3 documents but
doesn't check for.

### `mt::Span(tracer, name, {attrs}, kind)`: a scoped span with inline attributes

```cpp
const auto charge = mt::Span(tracer, "payment.charge",
                             {kOrderTotal(kOrderTotalCents)},
                             microtel::SpanKind::Client);
```

`mt::Span` only views the attributes you pass; it doesn't take ownership. The
`std::initializer_list`'s backing array lives until the end of the enclosing
full-expression, and the attributes are copied into the span record during the
call. The returned scope holds no reference to them, so you have nothing to
keep alive.

### `mt::Traced(tracer, name, callable)`: run something inside a span and keep its result

```cpp
return mt::Traced(tracer, "inventory.reserve",
                  [&tracer]() -> std::int64_t { ...; return units; });
```

The return type is forwarded exactly (`decltype(auto)`). A value stays a value,
a reference stays a reference, `void` stays `void`, and the span ends after the
return value has been initialised. `Traced` is `noexcept` exactly when invoking
the callable is, so a `noexcept` caller keeps its guarantee.

**If the callable throws, the exception propagates without being recorded.**
`ScopedSpan`'s destructor ends the span during unwinding, and nothing else
happens. Recording exceptions is the job of `mt::TryCatch`, planned for v1.4
(roadmap §5). If `Traced` caught and rethrew, `TryCatch` would be redundant and
`Traced` would behave surprisingly. To record an exception today, catch it and
call `RecordException`, as shown next.

### `mt::RecordException(span, e)`: `Error` status plus the `exception` event

```cpp
catch (const PaymentDeclined& declined)
{
    mt::RecordException(*charge, declined);
}
```

It makes two calls for you: `SetStatus(Error, e.what())` and
`AddEvent("exception", {exception.type, exception.message})`. A few details:

`exception.type` is the mangled name. It comes from `typeid(e).name()`, so the
event carries `N12_GLOBAL__N_115PaymentDeclinedE` rather than
`PaymentDeclined`. That was a deliberate choice: demangling needs
`abi::__cxa_demangle`, which allocates on an error path and is ABI-specific,
while the mangled form is stable and easy to grep for. If you want a readable
name, use the `RecordException(span, type, message)` overload. That overload is
also the one to use in `-fno-rtti` builds, because the other one needs
`typeid` and is only declared when RTTI is enabled.

Setting `Error` status is a difference from opentelemetry-cpp, whose
similarly named `Span::RecordException` leaves the status alone. Roadmap §5
v1.1 specifies both the status and the event, and spec §18.1 excludes sugar
from conformance testing, so this helper isn't measured against the OTel API.

`exception.stacktrace` and `exception.escaped` are left out. microtel doesn't
capture stack traces, and the helper has no way to know whether the exception
escaped the span's scope.

### `mt::AttrKey`: write a key once

```cpp
constexpr mt::AttrKey kOrderId{"order.id"};

kOrderId.Set(*charge, std::string{order_id});                  // on a live span
mt::Span(tracer, "order.submit", {kOrderId(std::string{id})}); // as a KeyValue
```

A `constexpr` key at namespace scope is constant-initialised. The type never
allocates, there's no static-initialisation order to think about, and the
`string_view` length is fixed at compile time, so there's no `strlen` at call
sites where the compiler can't see the literal. It is also a distinct type, so
you can't pass a key where a value is expected.

The key string is borrowed and must outlive the `AttrKey`. Pass a string
literal.

In v1.1, `AttrKey` does not pre-encode wire bytes. Doing that would change the
ABI of `attribute.hpp` and put wire encoding in a public header, which the
project's dependency rules (rule 13 in [`CLAUDE.md`](../../CLAUDE.md)) forbid.
Roadmap §4 schedules the optimisation for v1.5, and because the plain binder
ships now, callers' source won't need to change when it lands.

## The same span without sugar

`mt::Span` is the helper worth expanding by hand, because that's where the
lifetime rule comes in. This:

```cpp
const auto charge = mt::Span(tracer, "payment.charge",
                             {kOrderTotal(kOrderTotalCents)},
                             microtel::SpanKind::Client);
```

is exactly this:

```cpp
const std::initializer_list<microtel::KeyValue> attrs{kOrderTotal(kOrderTotalCents)};

const microtel::ScopedSpan charge = tracer.StartAsCurrentSpan(
    "payment.charge",
    {.kind       = microtel::SpanKind::Client,
     .parent     = {},                     // unset → the current context
     .start_time = {},                     // zero → "now"
     .attributes = microtel::AttributeSpan{attrs.begin(), attrs.size()}});
```

Here is what the sugar saves you, starting with what trips people up most
often:

1. Writing all four fields of `StartSpanOptions` every time. A partial
   designated initialiser triggers `-Wmissing-field-initializers`, which these
   examples build with as an error (`-Werror`).
2. The `initializer_list` → `AttributeSpan` conversion. `AttributeSpan` is a
   borrowed `std::span`, so you have to think about lifetime. Within one
   full-expression it's fine; once you hoist the list into a named variable, as
   above, you have to keep `attrs` alive for at least as long as the call.
3. Writing `.parent = {}` to mean "inherit". It looks a lot like
   `.parent = SpanContext{}`, which means the opposite: a set-but-invalid parent
   makes an explicit root with a fresh trace ID and ignores the current context
   entirely.

Everything else is identical, including the type. The sugar defines no RAII
type of its own, so the end-then-restore ordering is defined in exactly one
place: the declaration order of `ScopedSpan`'s members (ICP 0025 §3).

## What you will see in Grafana

Filter to this run:

```
{ resource.service.name = "microtel-sugar-tour" }
```

You'll see seven spans in one trace. `order.submit` and `payment.charge` are
red: `payment.charge` because `RecordException` set `Error`, and `order.submit`
because the pipeline set it after releasing the reservation. Open
`payment.charge` to find the `exception` event with the mangled
`exception.type`.

The card is always declined, so you always see the error path. An all-green
trace wouldn't show anything that [`basic_trace`](../basic_trace/) doesn't
already.
