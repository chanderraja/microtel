# `context_propagation`

Implicit parenting inside one process. This example shows how a three-level
trace comes out of functions that never mention each other:

```cpp
void QueryDb(microtel::Tracer& tracer);   // <- the whole signature.
void LoadUser(microtel::Tracer& tracer);  //    No span. No parent. No context.
```

It then moves to a thread boundary, where implicit parenting stops, and shows
the one line you need on each side to carry the context across.

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

After this block the program prints the always-off provider's `Shutdown`
status, the main provider's `ForceFlush`/health/`Shutdown` summary, and the
Grafana and `curl` commands for the two exported traces. To export to a
different collector, pass its endpoint as the first argument.

## The slot

`microtel::CurrentContext()` gives you one `Context` per thread. It is never
null: a thread that hasn't installed anything sees a default-constructed
`Context` whose `active_span_context` is all zeros. That's what the first and
last lines of the output above show.

A `Context` holds exactly two things, both named and typed:

```cpp
class Context
{
public:
    SpanContext active_span_context;   // what a child parents to
    Baggage     baggage;               // what the request carries
};
```

There is no opaque key/value map. The two members that can grow,
`SpanContext::trace_state` and `baggage`, keep their entries behind a
`shared_ptr`, so copying a `Context` costs a couple of refcount bumps. The copy
is `noexcept` and allocation-free, cheap enough to put in a lambda capture. The
design depends on this: `Span::GetContext()` returns a `SpanContext` by value
and, like every hot-path method, is `noexcept`, so nothing inside a
`SpanContext` may have an allocating copy.

`CurrentContext()` returns a borrowed reference to the thread's slot. The next
scope construction or destruction on that thread invalidates it, so read it
and copy it if you need to keep it.

## The RAII restore rule

`StartAsCurrentSpan` returns a `ScopedSpan`, which owns a `ScopedContext`. On
construction it installs the new context; on destruction it writes back the
context it displaced.

The per-thread stack is your C++ call stack. There is no thread-local
container. The slot holds a single `Context` by value, and each live scope
holds the value it displaced, so nesting is a chain through your own stack
frames. That means no heap and no depth limit, and installing a context costs
a refcount bump and a thread-local store.

The cost of that design is that restore is positional: **scopes must be
destroyed in reverse order of creation on a thread.** Destroying them out of
order writes back a stale context. microtel treats that as a programming error
and makes no attempt to detect it. Ordinary block scope and member lifetimes
give you the right order automatically. That's why `ScopedContext` and
`ScopedSpan` are move-constructible but neither copyable nor move-assignable;
either operation would let a restore happen out of order.

Both types are thread-confined. Construct and destroy each one on a single
thread, and don't share them.

The order of `ScopedSpan`'s members matters:

```cpp
private:
    ScopedContext m_scope;   // declared first, destroyed last
    SpanHandle    m_span;
```

Members are destroyed in reverse declaration order, so the span ends while it
is still the current one, and the caller's context is restored after that.

### Three states of `StartSpanOptions::parent`

Only the first one reads the slot:

| `opts.parent` | result |
|---|---|
| **unset** (`{}`) | parent is `CurrentContext().active_span_context` |
| **set and valid** | that context is the parent; the slot is ignored |
| **set but invalid** | an explicit **root**, fresh trace ID; the slot is *not* consulted |

The third row is easy to write by accident. `.parent = SpanContext{}` and
`.parent = {}` look alike but mean opposite things.

`StartSpan` and `StartAsCurrentSpan` resolve the parent identically. The only
difference is that `StartAsCurrentSpan` also installs.

## The thread boundary

**A new thread does not inherit its creator's context, and microtel won't
add that** ([ICP 0025](../../docs/icps/0025-propagation-core.md) §3, contract
4). A newly created thread starts from a default-constructed `Context`. A
library can't portably hook thread creation, and the ways of faking it
(wrapping `std::thread`, interposing `pthread_create`) surprise whoever owns
the rest of the process. opentelemetry-cpp's `RuntimeContext` is per-thread
for the same reason.

So a bare worker produces its own trace:

```cpp
std::thread worker([&tracer] {
    const auto scope = tracer.StartAsCurrentSpan("worker.orphan");  // new trace
});
```

That's the `orphan trace_id` in the output. In a real service this is the bug
where half the work vanishes from the request's trace.

The fix is one line on each side of the boundary:

```cpp
const microtel::Context carried = microtel::CurrentContext();     // copy out

std::thread worker([&tracer, carried] {
    const microtel::ScopedContext installed{carried};             // install
    const auto scope = tracer.StartAsCurrentSpan("worker.carried");
});
```

The copy is the `noexcept`, allocation-free one described above, so
capturing it by value in a lambda or a queued task is cheap. Once `installed`
exists, everything the worker starts parents into the caller's trace without a
parent argument anywhere. That's the `carried trace_id` in the output, which
matches the request's.

`installed` has to be destroyed on the worker thread. Making it a local inside
the lambda takes care of that.

## The unsampled path

When the sampler drops a span, `StartAsCurrentSpan` still installs a context
(ICP 0025 §3, contract 3), and the handle and the slot deliberately disagree:

```
span->IsSampled()                = false
span->GetContext().IsValid()     = false     <- the handle
CurrentContext() trace_id        = a45776f9…  <- the slot
CurrentContext() sampled flag    = false
```

`span->GetContext()` is invalid because the handle for a dropped span is a
process-wide no-op singleton. Giving it per-span state would break the
zero-allocation guarantee for unsampled spans
([`docs/memory-model.md`](../../docs/memory-model.md) §8.1).
`CurrentContext().active_span_context`, on the other hand, carries the real
trace and span IDs with the sampled flag cleared.

That keeps the children of an unsampled span in the same trace, and lets a
propagator emit a coherent `traceparent` across a hop. **On the unsampled path,
read `CurrentContext()`, not the handle.** This matters for
[`distributed_handoff/`](../distributed_handoff/), where whatever goes on the
wire has to be correct whether or not this process is recording.

The example builds a second provider with `MakeAlwaysOffSampler()` to show
this. It also has to pass `WithProfileName("unsampled")`. Profile names
identify live providers within the process and an unnamed profile is
`"default"`, so a second `Build()` without a distinct name fails with
`DuplicateProfileName` while the first provider is alive.

## What you will see in Grafana

Two traces, and it's the contrast between them that matters:

1. The request trace: `http.handler` with `user.load` → `db.query`,
   `page.render`, and `worker.carried` under it. That's five spans from four
   functions and one worker thread, none of which was handed a parent.
2. A single-span trace containing only `worker.orphan`.

```
{ resource.service.name = "microtel-context-propagation" }
```

The always-off provider's trace ID is in neither. It exports nothing, and
`curl http://localhost:3200/api/traces/<that id>` returns a 404. That's the
correct result, since a dropped span is never sent. Check it once so
you know what "unsampled" looks like and don't mistake it for data loss.
