# `sugar_tour`

Every helper in `microtel::sugar`, inside one small order pipeline.

The sugar layer is **header-only** and adds nothing to your link closure: each
helper compiles to the two or three public calls you would otherwise have
written. So this example is not about capability — it is about what the call
sites look like, and about a trace that reads as a story rather than a
checklist of API names.

The story: an order is submitted, its cart validated, inventory reserved, the
card declined, and the reservation released.

```
order.submit                                       Server, status=Error
├── void (anonymous namespace)::ValidateCart(…)    MICROTEL_TRACE_FUNCTION
│   └── cart.items.check                           mt::Span
├── inventory.reserve                              mt::Traced → int64
│   └── warehouse.lookup                           mt::Span
├── payment.charge                                 Client, `exception` event
└── inventory.release                              mt::Span
```

**Not one function passes a span or a parent to the function it calls.** Every
sugar helper is `StartAsCurrentSpan` underneath; the mechanism is
[`context_propagation/`](../context_propagation/)'s subject.

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

A different collector is `argv[1]`.

## The alias nobody declares

```cpp
namespace mt = microtel::sugar;   // in your code, never in ours
```

The namespace is `microtel::sugar`. No microtel header declares `mt`: a library
that squats a two-character global name has taken something it cannot give
back. It costs you one line per file, and the call sites then read the way
`microtel-roadmap.md` §5 writes them.

One consequence to know about: `microtel::sugar::Span` is a *function*, so
inside the sugar headers the core types are spelled fully-qualified
(`::microtel::Span`). In your code the two never collide — `mt::Span(...)` is
the factory, `microtel::Span` is the class.

## The five forms

### `MICROTEL_TRACE_FUNCTION(tracer)` — "just trace this function"

```cpp
void ValidateCart(microtel::Tracer& tracer)
{
    MICROTEL_TRACE_FUNCTION(tracer);
    ...
}
```

A macro rather than a function because only a *declaration* can create the
variable whose lifetime is the scope. Two things to expect:

- **It takes the tracer.** `microtel-roadmap.md` §5 writes it with no argument;
  that shape needs a process-global tracer, and microtel has none. Adding a
  mutable global with static-destruction-order problems underneath a
  convenience macro is not a trade this project makes — recorded as
  Discrepancy 1 in [ICP 0028](../../docs/icps/0028-sugar-surface.md).
- **The span name is the full function signature.** It is
  `std::source_location::current().function_name()`, so in Grafana the span
  above is named `void (anonymous namespace)::ValidateCart(microtel::Tracer &)`,
  not `ValidateCart`. That is the honest cost of a name you did not have to
  type. When you want a short, stable, hand-picked one, `mt::Span` is the
  helper that takes it.

The declared variable is `__LINE__`-uniqued and `const`, so you cannot name it
— which is the point. `const` is deliberate too: a scope that cannot be moved
out of cannot be destroyed out of order, which is the one programming error
[ICP 0025](../../docs/icps/0025-propagation-core.md) §3 documents and does not
check.

### `mt::Span(tracer, name, {attrs}, kind)` — a scoped span with inline attributes

```cpp
const auto charge = mt::Span(tracer, "payment.charge",
                             {kOrderTotal(kOrderTotalCents)},
                             microtel::SpanKind::Client);
```

`attributes` is **viewed, not owned**: the `std::initializer_list`'s backing
array lives to the end of the enclosing full-expression, and the attributes are
copied into the span record inside the call. The returned scope holds no
reference to them, so there is nothing to keep alive.

### `mt::Traced(tracer, name, callable)` — run something inside a span, keep its result

```cpp
return mt::Traced(tracer, "inventory.reserve",
                  [&tracer]() -> std::int64_t { ...; return units; });
```

The return type is forwarded exactly (`decltype(auto)`): a value stays a value,
a reference stays a reference, `void` stays `void`, and the span ends after the
return value has been initialised. `Traced` is conditionally `noexcept` —
`noexcept` iff invoking the callable is — so a `noexcept` caller keeps its
guarantee.

**If the callable throws, the exception propagates *unrecorded*.** The span is
ended by `ScopedSpan`'s destructor during unwinding, and that is all. Recording
it is `mt::TryCatch`, roadmap §5 v1.4; a `Traced` that swallowed-and-rethrew
would make that helper redundant and this one surprising. To record an
exception today, catch it and call `RecordException` — as below.

### `mt::RecordException(span, e)` — `Error` status *and* the `exception` event

```cpp
catch (const PaymentDeclined& declined)
{
    mt::RecordException(*charge, declined);
}
```

Two calls in one: `SetStatus(Error, e.what())` and
`AddEvent("exception", {exception.type, exception.message})`.

- **`exception.type` is mangled.** It is `typeid(e).name()`, so the event
  carries `N12_GLOBAL__N_115PaymentDeclinedE`, not `PaymentDeclined`.
  Deliberate: demangling needs `abi::__cxa_demangle`, which allocates on an
  error path and is ABI-specific, while the mangled form is stable and
  greppable. Use the `RecordException(span, type, message)` overload when you
  want a pretty name — it is also the overload for `-fno-rtti` builds, since
  `typeid` is what the other one costs.
- **Setting `Error` status diverges from opentelemetry-cpp**, whose
  similarly-named `Span::RecordException` does not. Roadmap §5 v1.1 specifies
  both halves, and spec §18.1 excludes sugar from conformance testing, so this
  helper is not measured against the OTel API surface.
- `exception.stacktrace` and `exception.escaped` are omitted: microtel captures
  no stack traces, and whether an exception escaped the span's scope is not
  knowable from inside the helper.

### `mt::AttrKey` — spell a key once

```cpp
constexpr mt::AttrKey kOrderId{"order.id"};

kOrderId.Set(*charge, std::string{order_id});                  // on a live span
mt::Span(tracer, "order.submit", {kOrderId(std::string{id})}); // as a KeyValue
```

`constexpr` at namespace scope is constant-initialised: no allocation anywhere
in the type, no static-initialisation order to reason about, and a
`string_view` whose length is fixed at compile time — no `strlen` at each of N
call sites where the compiler cannot see the literal. Plus a distinct type, so
a key cannot be passed where a value is expected.

The key is **borrowed** and must outlive the object — a string literal is the
intended argument.

What `AttrKey` deliberately does *not* buy in v1.1 is pre-encoded wire bytes.
That would be an ABI change on `attribute.hpp` and would put wire encoding in a
public header, against CLAUDE.md rule 13. Roadmap §4 v1.5 schedules the
optimisation; shipping the plain binder now is what makes it
source-compatible for callers when it lands.

## The same span without sugar

`mt::Span` is the one worth desugaring, because it is where the lifetime rule
lives. This:

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

Three things the sugar is saving you, in order of how often they bite:

1. **All four fields of `StartSpanOptions`**, every time. A partial designated
   initialiser trips `-Wmissing-field-initializers` under the `-Werror` these
   examples build with.
2. **The `initializer_list` → `AttributeSpan` conversion**, which is a borrowed
   `std::span` and therefore a lifetime question you have to answer. Inside one
   full-expression the answer is "fine"; hoisted to a named variable, as above,
   you now have to keep `attrs` alive at least as long as the call.
3. **The spelling of `.parent = {}`** to mean "inherit", which is not obviously
   different from `.parent = SpanContext{}` — and that one means the *opposite*:
   a set-but-invalid parent is an explicit root with a fresh trace ID, and the
   current context is not consulted at all.

Everything else is identical, including the type: sugar defines no RAII type of
its own, so the end-then-restore ordering lives in exactly one place
(`ScopedSpan`'s member declaration order, ICP 0025 §3).

## What you will see in Grafana

Filter to this run:

```
{ resource.service.name = "microtel-sugar-tour" }
```

Seven spans in one trace. `order.submit` and `payment.charge` are red —
`payment.charge` because `RecordException` set `Error`, `order.submit` because
the pipeline set it after compensating. Open `payment.charge` and the
`exception` event is there with the mangled `exception.type`.

The example always declines, so the error path is always the one you see. That
is on purpose: a green trace teaches nothing that `basic_trace` does not
already show.
