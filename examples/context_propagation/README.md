# `context_propagation`

Implicit parenting inside one process: how a three-level trace comes out of
functions that never mention each other.

```cpp
void QueryDb(microtel::Tracer& tracer);   // <- the whole signature.
void LoadUser(microtel::Tracer& tracer);  //    No span. No parent. No context.
```

Then the thread boundary, which is where the mechanism stops — and the one line
on each side that gets you across it.

## Run it

```bash
examples/stack/up.sh

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build --target microtel_example_context_propagation

./build/examples/microtel_example_context_propagation
```

```
the current-span slot, as the call stack moves:
  main            (level 0, nothing installed)
      current span_id = 0000000000000000  valid=no
  http.handler    (level 1)
      current span_id = 801dacdcaaccf0e0  valid=yes
  user.load       (level 2)
      current span_id = 7d2c1606c072ebf7  valid=yes
  db.query        (level 3)
      current span_id = 6e67f14cccc373c6  valid=yes
  page.render     (level 2)
      current span_id = 77da72d3aef7d4bf  valid=yes
  -- inside a bare std::thread --
  worker thread, before starting a span
      current span_id = 0000000000000000  valid=no
  -- inside a std::thread handed a context --
  worker thread, after ScopedContext
      current span_id = 801dacdcaaccf0e0  valid=yes
  main            (level 0, every scope destroyed)
      current span_id = 0000000000000000  valid=no

under a sampler that drops everything:
  span->IsSampled()                = false
  span->GetContext().IsValid()     = false
  CurrentContext() trace_id        = a45776f923917bcb2f172132647e3f94
  CurrentContext() sampled flag    = false

request trace_id: 12a0fc5d839c49948298dffde504fde8
orphan  trace_id: 7416ffaee7086a89d78391d87b5523bb   (different — a new thread starts at the root context)
carried trace_id: 12a0fc5d839c49948298dffde504fde8   (same as the request — context handed over)
```

A different collector is `argv[1]`.

## The slot

`microtel::CurrentContext()` is one `Context` **per thread**. It is never null:
a thread that has installed nothing sees a default-constructed `Context` whose
`active_span_context` is all zeros — the first and last lines of the output
above.

A `Context` holds exactly two things, both named and typed:

```cpp
class Context
{
public:
    SpanContext active_span_context;   // what a child parents to
    Baggage     baggage;               // what the request carries
};
```

Two slots, not an opaque key/value map. Both of the growable members —
`SpanContext::trace_state` and `baggage` — hold their entries behind a
`shared_ptr`, so copying a `Context` is a couple of refcount bumps: `noexcept`,
allocation-free, and cheap enough to put in a lambda capture. That is not a
detail, it is the reason the design works — `Span::GetContext()` returns a
`SpanContext` **by value** and is `noexcept` by hard rule 14, so nothing inside
one may have an allocating copy.

`CurrentContext()` returns a **borrowed** reference to the thread's slot,
invalidated by the next scope construction or destruction on that thread. Read
it; copy it if you need to keep it.

## The RAII restore rule

`StartAsCurrentSpan` returns a `ScopedSpan`, which owns a `ScopedContext`. On
construction the new context is installed; on destruction the **displaced** one
is written back.

**The per-thread stack is the C++ stack.** There is no thread-local container:
the slot holds a single `Context` by value, and each live scope holds the value
it displaced. Nesting is a chain through your own stack frames — no heap, no
depth limit, and installing a context costs a refcount bump and a thread-local
store.

The price of that shape is that **restore is positional: scopes must be
destroyed in reverse order of creation on a thread.** Destroying out of order
writes back a stale context. It is a programming error, not a diagnosed
condition — microtel does not detect it, deliberately. Ordinary block scope and
ordinary member lifetimes give you the right order for free, which is why
`ScopedContext` and `ScopedSpan` are move-constructible but **not** copyable
and **not** move-assignable: either would let a restore land out of order.

Both types are **thread-confined**. Construct and destroy on one thread; do not
share one.

`ScopedSpan`'s member order is load-bearing:

```cpp
private:
    ScopedContext m_scope;   // declared first, destroyed last
    SpanHandle    m_span;
```

Members are destroyed in reverse declaration order, so the span is ended
*while it is still the current one*, and the caller's context is restored after
that.

### Three states of `StartSpanOptions::parent`

Only the first consults the slot:

| `opts.parent` | result |
|---|---|
| **unset** (`{}`) | parent is `CurrentContext().active_span_context` |
| **set and valid** | that context is the parent; the slot is ignored |
| **set but invalid** | an explicit **root**, fresh trace ID; the slot is *not* consulted |

The third row is easy to write by accident — `.parent = SpanContext{}` is not
`.parent = {}`, and it means the opposite.

`StartSpan` and `StartAsCurrentSpan` resolve the parent identically. The only
difference is that `StartAsCurrentSpan` also installs.

## The thread boundary

**There is no cross-thread inheritance, and there will not be** (ICP 0025 §3
contract 4). A newly created thread starts from a default-constructed
`Context`. Installing a hook at thread creation is not something a library can
do portably, and every mechanism that fakes it — wrapping `std::thread`,
interposing `pthread_create` — is a surprise in someone else's process. This
matches opentelemetry-cpp's `RuntimeContext`, which is also per-thread.

So a bare worker produces its own trace:

```cpp
std::thread worker([&tracer] {
    const auto scope = tracer.StartAsCurrentSpan("worker.orphan");  // new trace
});
```

That is the `orphan trace_id` in the output — a different trace, which in a
real service is the bug where half your work vanishes from the request.

The documented pattern is one line on each side:

```cpp
const microtel::Context carried = microtel::CurrentContext();     // copy out

std::thread worker([&tracer, carried] {
    const microtel::ScopedContext installed{carried};             // install
    const auto scope = tracer.StartAsCurrentSpan("worker.carried");
});
```

The copy is the `noexcept`, allocation-free one described above, so capturing
it by value in a lambda or a queued task costs nothing worth thinking about.
After `installed`, everything the worker starts parents into the caller's
trace with no parent argument anywhere — which is the `carried trace_id` in the
output, equal to the request's.

`installed` must be destroyed on the worker thread, which a lambda-local gives
you automatically.

## The unsampled path

When the sampler drops a span, `StartAsCurrentSpan` **still installs** (ICP
0025 §3 contract 3), and the two sources of truth disagree on purpose:

```
span->IsSampled()                = false
span->GetContext().IsValid()     = false     <- the handle
CurrentContext() trace_id        = a45776f9…  <- the slot
CurrentContext() sampled flag    = false
```

- `span->GetContext()` is **invalid**, because the handle is a process-wide
  no-op singleton. It cannot hold per-span state without breaking the
  zero-allocation guarantee for unsampled spans (`docs/memory-model.md` §8.1).
- `CurrentContext().active_span_context` carries the **real** trace and span
  IDs with the sampled flag cleared.

That is what keeps children of an unsampled span in the same trace, and what
lets a propagator still emit a coherent `traceparent` across a hop. **On the
unsampled path, `CurrentContext()` is the accurate source** — which matters
directly for [`distributed_handoff/`](../distributed_handoff/), where the thing
going on the wire has to be right whether or not this process is recording.

The example builds a second provider with `MakeAlwaysOffSampler()` to show
this. It also passes `WithProfileName("unsampled")`, and that is not
decoration: profile names identify live providers within the process, an
unnamed profile is `"default"`, and a second `Build()` without a distinct name
fails with `DuplicateProfileName` while the first provider is alive.

## What you will see in Grafana

**Two** traces, and the pair is the point.

1. The request trace — `http.handler` with `user.load` → `db.query`,
   `page.render`, and `worker.carried` under it. Five spans produced by four
   functions and one worker thread, none of which was handed a parent.
2. A single-span trace containing only `worker.orphan`.

```
{ resource.service.name = "microtel-context-propagation" }
```

The always-off provider's trace ID appears in neither: it exports nothing, and
`curl http://localhost:3200/api/traces/<that id>` is a 404. That is the correct
result — dropped means dropped — and it is worth checking once, because it is
the difference between "unsampled" and "lost".
