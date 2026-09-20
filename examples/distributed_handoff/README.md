# `distributed_handoff`

Two processes, one trace.

A sender starts a `Client` span and writes its identity into an outgoing
message with the W3C propagators. A receiver reads them back, starts a `Server`
span, and — because the extracted context is installed as current — that span
parents to the sender's **without a parent argument**. Both export to the same
collector under different service names, and Tempo joins them because the trace
ID on the wire is the trace ID in both.

This is the example the other two build up to.
[`sugar_tour/`](../sugar_tour/) shows the call sites,
[`context_propagation/`](../context_propagation/) shows the thread-local slot
that makes implicit parenting work; this one puts a socket in the middle and
shows that nothing about the mechanism changes.

## Run it

Two terminals. **Start the receiver first** — it binds the port and blocks.

```bash
examples/stack/up.sh

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build --target microtel_example_handoff_receiver \
                    --target microtel_example_handoff_sender
```

```bash
# terminal 1
./build/examples/microtel_example_handoff_receiver
```
```bash
# terminal 2
./build/examples/microtel_example_handoff_sender
```

Both take the same two optional arguments:

```
<binary> [endpoint] [port]

  [endpoint]  OTLP/gRPC collector, default http://localhost:4317
  [port]      the loopback port the two halves meet on, default 9099
```

The sender retries the connect for five seconds, so you have time to switch
terminals. The receiver handles exactly one message and exits — it needs to
flush its trace and shut down cleanly, and a loop would only obscure that. A
real server would loop.

### What they print

```
$ ./build/examples/microtel_example_handoff_sender
headers written:
  baggage: tenant=acme,order.priority=high
  traceparent: 00-6b6fbe127f904dffdd16cf9e16c823c0-add1851f35ba998f-01
  tracestate: microtel=e1
reply: 200 OK

sender trace_id: 6b6fbe127f904dffdd16cf9e16c823c0
sender span_id:  add1851f35ba998f   (the receiver's server span should name this as its parent)
ForceFlush: Completed
batches_sent=1 batches_failed=0 queue_depth=0
Shutdown: Completed
```

```
$ ./build/examples/microtel_example_handoff_receiver
receiver: waiting for one message on 127.0.0.1:9099
request: PLACE /orders/ord-20260919-0042
headers read:
  baggage: tenant=acme,order.priority=high
  traceparent: 00-6b6fbe127f904dffdd16cf9e16c823c0-add1851f35ba998f-01
  tracestate: microtel=e1
extracted: remote=true sampled=true tracestate="microtel=e1" baggage_entries=2

receiver trace_id:      6b6fbe127f904dffdd16cf9e16c823c0
receiver span_id:       3b3c3936ccd0042c
receiver parent span:   add1851f35ba998f   (the sender's client span)
ForceFlush: Completed
batches_sent=1 batches_failed=0 queue_depth=0
Shutdown: Completed
```

Three things to read off that pair:

| sender | receiver | |
|---|---|---|
| `trace_id: 6b6fbe12…` | `trace_id: 6b6fbe12…` | **equal** — one trace |
| `span_id: add1851f…` | `parent span: add1851f…` | the join |
| `traceparent: …-01` | `sampled=true` | the sampling decision crossed too |

## Confirming it is really one trace

The ID is the same in both terminals, which is most of the proof. The rest is
that the backend agrees — a single trace document containing spans from two
*different* services:

```bash
$ curl -s http://localhost:3200/api/traces/6b6fbe127f904dffdd16cf9e16c823c0 | jq -r '
    .batches[] as $b
    | ($b.resource.attributes[] | select(.key=="service.name") | .value.stringValue) as $svc
    | $b.scopeSpans[].spans[]
    | "\($svc)\t\(.name)\t\(.kind)"'

microtel-handoff-sender      order.place     SPAN_KIND_CLIENT
microtel-handoff-receiver    order.receive   SPAN_KIND_SERVER
```

(The two resource batches come back in whatever order Tempo stored them; only
the pairing matters.)

Two resource batches, one trace. In Grafana, open
<http://localhost:3000> — the trace is on the **microtel — recent traces**
dashboard within about ten seconds — and the flame graph shows `order.receive`
nested under `order.place` with the service name changing between them. That
colour change at the boundary is the whole feature.

TraceQL for either half:

```
{ resource.service.name =~ "microtel-handoff-.*" }
```

## What actually crosses

### 1. `traceparent` — the trace context

```cpp
microtel::SpanContext outgoing = client->GetContext();
outgoing.trace_state = outgoing.trace_state.Set("microtel", "e1");

microtel::W3CTraceContextPropagator{}.Inject(outgoing, setter);
```

`Inject` writes `traceparent` and, if non-empty, `tracestate`. If the context
is invalid it writes nothing at all — which is why the receiver checks.

Note that the sender mutates a **local copy** of the context, not the span.
`TraceState::Set` is copy-on-write: it builds a new list and returns a new
`TraceState`, leaving every other copy untouched. That is not a convenience,
it is what lets `Span::GetContext()` stay `noexcept` and return by value —
`TraceState` holds its entries behind a `shared_ptr`, so copying a
`SpanContext` is a refcount bump rather than an allocation
([ICP 0025](../../docs/icps/0025-propagation-core.md) §1).

`tracestate` carrying a value across the hop is new in v1.1. Before it,
`TraceState` had no storage, `FromHeader` returned the empty state for every
input, and a vendor's `tracestate` was silently dropped by a microtel hop
(issue #208). The receiver prints what arrived so you can see it survive.

### 2. `baggage` — what the request carries

```cpp
const microtel::Baggage bag =
    microtel::Baggage{}.Set("tenant", "acme").Set("order.priority", "high");

microtel::W3CBaggagePropagator{}.Inject(bag, setter);
```

**Baggage rides `Context`, never `SpanContext`.** It is per-request, not
per-span: a request carries baggage whether or not a span is active, and
baggage set inside a span must outlive that span within the enclosing scope.
Parking it on `SpanContext` would also put a second growable member inside that
`noexcept` by-value accessor — the trap `TraceState` is behind a `shared_ptr`
to escape. ICP 0025 §2 records the decision so it is not re-litigated.

Baggage never influences sampling or parenting (§3 contract 6). It exists so
that a value known at the edge — the tenant, here — is available to every
service downstream, and the receiver does the usual thing with it: promotes one
entry to a span attribute.

Every mutator is copy-on-write, so the chain above builds three immutable
values and keeps the last. Limits are W3C's: 180 members, 4096 bytes per
member, 8192 total, enforced against the **canonical serialised** form so that
anything which fits once fits forever.

### 3. Nothing else

No SDK state, no resource attributes, no configuration. A hop carries the trace
context and the baggage, and that is the whole contract.

## The two sides

### Inject: a `HeaderSetter`

```cpp
handoff::Headers headers;
const microtel::HeaderSetter setter = [&headers](std::string_view name, std::string_view value)
{ headers.insert_or_assign(std::string{name}, std::string{value}); };

microtel::W3CTraceContextPropagator{}.Inject(outgoing, setter);
microtel::W3CBaggagePropagator{}.Inject(bag, setter);
```

The propagators are **carrier-agnostic**: they write through a callback and
never learn what the carrier is. Two propagators, two disjoint sets of headers,
no coordination between them — use either, or both. Both are stateless and
thread-safe, which is why they are constructed inline rather than kept.

### Extract: a `HeaderGetter`, and then install

```cpp
const microtel::SpanContext remote = microtel::W3CTraceContextPropagator{}.Extract(getter);
const microtel::Baggage     bag    = microtel::W3CBaggagePropagator{}.Extract(getter);

const microtel::ScopedContext incoming{microtel::Context{remote, bag}};

const auto server = tracer.StartAsCurrentSpan("order.receive",
    {.kind = microtel::SpanKind::Server, .parent = {}, .start_time = {}, .attributes = {}});
```

**`.parent` is unset.** That is the point of the example. Installing the
extracted context and then starting a span with no parent is the same implicit
parenting as inside one process — the parent is
`CurrentContext().active_span_context`, which happens to have arrived over a
socket. Passing `.parent = remote` would also work and would produce the same
tree; installing is better because it also puts the baggage where the rest of
the handler can read it without threading an argument through every call.

`Extract` reports failure by returning an **invalid** `SpanContext` rather than
throwing — there is no error channel on a hot path. An invalid parent makes the
server span an ordinary root, so the service still produces a trace; it just is
not joined to anyone's. The receiver says so on stderr rather than pretending:

```
receiver: no usable traceparent arrived — this span will start a new trace
          instead of joining the sender's.
```

Try it: `printf 'PLACE /x\r\n\r\n' | nc localhost 9099` while the receiver is
waiting.

On successful extraction `remote` is `true` on the returned context, which is
what distinguishes "this parent came from the wire" from "this parent is a span
in my own process".

### Unsampled senders

If the sender's span is dropped by its sampler, `traceparent` still goes out —
with the sampled flag clear, and with real IDs, because `StartAsCurrentSpan`
installs a valid context on the drop path too. The receiver then joins a trace
whose sender half was never exported. That is correct W3C behaviour, and it is
why the sender reads what to inject from the span context rather than from
anything that knows about sampling. See
[`context_propagation/`](../context_propagation/)'s unsampled section.

## The plumbing, and why it is in its own file

`wire.hpp` holds the socket and a text protocol that is HTTP's
start-line-plus-headers shape and nothing else:

```
PLACE /orders/ord-20260919-0042 CRLF
traceparent: 00-<32 hex>-<16 hex>-01 CRLF
tracestate: microtel=e1 CRLF
baggage: tenant=acme,order.priority=high CRLF
CRLF
```

It is deliberately boring, and it is quarantined so that `sender.cpp` and
`receiver.cpp` contain only telemetry. **Nothing in `wire.hpp` includes a
microtel header** — that is the demonstration. Because the propagators speak
through `HeaderGetter` / `HeaderSetter` callbacks, the carrier never has to
know what is being carried. Replace `wire.hpp` with HTTP/2, gRPC metadata, a
Kafka record header, or a JSON envelope and neither `main` changes.

Two things in there are real rather than illustrative:

- **`handoff::Fd`** is a move-only RAII owner for the file descriptor.
  CLAUDE.md rule 5 applies to example code too, and the project's own
  `UniqueFd` lives in `src/common/raii/` and is not public API — so an example
  meant to be copied into your tree carries its own five lines instead of
  reaching into `src/`.
- **Header names are matched exactly**, not case-folded. Both halves write them
  the way the propagators do (lowercase). A real HTTP carrier must case-fold in
  its `HeaderGetter`; HTTP/1.1 field names are case-insensitive and HTTP/2
  requires lowercase on the wire.

## One rough edge

The receiver reads the baggage value **before** opening the span scope:

```cpp
const microtel::ScopedContext incoming{microtel::Context{remote, bag}};

const std::optional<std::string_view> tenant =
    microtel::CurrentContext().baggage.Get("tenant");     // <- here

const auto server = tracer.StartAsCurrentSpan("order.receive", {...});
```

That ordering is correct in any case, but today it is also necessary:
`StartAsCurrentSpan` installs a `Context` built from the span context alone, so
the thread's baggage is empty for the span's entire scope. Reading it after the
span starts returns `nullopt`. Filed as
[#283](https://github.com/chanderraja/microtel/issues/283); when it is fixed,
the read can move down beside the `SetAttribute` call where it reads more
naturally. It is called out here rather than silently worked around, because
from the outside the ordering looks like a style choice.

## Exit codes

Both binaries return `0` on success, `2` if `ForceFlush` did not complete, and
`3` if the hand-off itself did not happen — nothing accepted the connection,
the peer hung up, or no reply arrived. A failed hand-off still produces a trace
with the client span marked `Error`, which is the useful outcome: the API
reports through its own channels rather than crashing, and you can see the
failure in Grafana.
