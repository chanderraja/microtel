# `multi_profile`

Two independent named providers in one process, and the registry that lets
code find one without being handed a pointer.

The example builds a `"frontend"` and a `"backend"` profile with different
service names and different samplers, emits from both, looks the backend up by
name from a function that received nothing, and then walks the three registry
rules that trip people up: a duplicate name is refused, `Shutdown` does not
release a name, destruction does.

## Run it

```bash
examples/stack/up.sh

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build --target microtel_example_multi_profile

./build/examples/microtel_example_multi_profile
```

```
endpoint: http://localhost:4317

emitting
  microtel-frontend-example trace_id: c2d15d47411fdb4ff46217b1f6686701
  backend.healthz sampled: no  (the backend sampler drops it)
  microtel-backend-example trace_id: 9f24b41fb453eaf8e67783a59e3bc3af

the registry
  GetProvider("frontend"): live
  GetProvider("backend"):  live
  GetProvider() ["default"]: nullptr  (nothing was built under it)
  Build() with WithProfileName("backend") refused: a live provider is already registered under profile name 'backend'; shutting a provider down does not release its name, destroying it does
  kind is DuplicateProfileName: yes  (the live provider keeps the name; this is never last-wins)

shutting down
  frontend: ForceFlush=Completed batches_sent=1 batches_failed=0 queue_depth=0 Shutdown=Completed
  backend: ForceFlush=Completed batches_sent=1 batches_failed=0 queue_depth=0 Shutdown=Completed
  after Shutdown, GetProvider("frontend"): live  (a name is freed by destruction, not by Shutdown)
  after destruction, GetProvider("frontend"): nullptr
  GetProvider("backend"):  live  (still owned here)
```

## Two profiles, nothing shared

```cpp
auto frontend = microtel::SdkBuilder{}
                    .WithServiceName("microtel-frontend-example")
                    .WithProfileName("frontend")
                    .WithSampler(microtel::MakeAlwaysOnSampler())
                    .Build();

auto backend = microtel::SdkBuilder{}
                   .WithServiceName("microtel-backend-example")
                   .WithProfileName("backend")
                   .WithSampler(microtel::MakeSpanNameRuleSampler(
                       "backend.healthz",
                       microtel::MakeAlwaysOffSampler(),    // on match
                       microtel::MakeAlwaysOnSampler()))    // on everything else
                   .Build();
```

A profile is **not** an overlay on a shared pipeline. Each one owns its
endpoint, protocol, TLS material, sampler, `Resource`, processors, worker
threads, transport and I/O thread, and shares none of them
([ICP 0027](../../docs/icps/0027-multi-profile-threading.md)). Both export to
the same collector here, which is the least interesting thing they could do —
the shape this exists for is one profile to your own collector and another to a
vendor, or a high-volume profile sampled hard next to an audit profile sampled
not at all.

The samplers differ to make the independence visible rather than asserted: the
frontend keeps everything it starts, the backend keeps everything except its
health check. `backend.healthz` is dropped at `StartSpan` and never reaches a
queue — the console line says so, and Tempo agrees.

`WithProfileName` is compared **byte for byte**. No normalisation, no case
folding: `"backend"` and `"Backend"` are two profiles.

## Finding a profile from code that was handed nothing

```cpp
std::string RecordBackendWork()
{
    microtel::Provider* const provider = microtel::GetProvider("backend");
    if (provider == nullptr)
    {
        return {};   // nothing live under that name
    }
    const auto tracer = provider->GetTracer("backend.library", "1.0.0");
    ...
}
```

This is the whole reason the registry exists. A host that builds its profiles
in one place can pass the `shared_ptr<Provider>` around and never call
`GetProvider` — and should. The call is for instrumentation that cannot be
reached that way: library code, a plugin, a signal-handler-adjacent callback,
anything whose signature you do not control.

`GetProvider()` with no argument is the default-profile accessor — it looks up
`"default"`, the name a provider built without `WithProfileName` carries. This
example names both of its profiles, so the default slot is empty and the call
returns `nullptr`, which the run prints.

## Registry semantics

`GetProvider` returns a **borrowed, non-owning** pointer. It never transfers
ownership and a lookup never extends a lifetime — the owner is whoever holds
the `shared_ptr<Provider>` that `Build()` returned.

| Event | `GetProvider(name)` | Why |
|---|---|---|
| After `Build()` | the provider | registered by `Build`, under that name |
| After `Shutdown()` | **still the provider** | `Shutdown` does not deregister; every method on it now returns `Status::AlreadyShutDown` |
| After destruction | `nullptr` | the destructor releases the slot, before it tears anything down |
| In a `fork()` child | `nullptr` | every slot is emptied in the child, so it can re-`Build()` under the same names |

The run demonstrates rows two and three back to back: `live` after both
providers are shut down, `nullptr` for the frontend the moment its last
`shared_ptr` is dropped, and `live` still for the backend, which `main` is
still holding.

The pointer **dangles across destruction**, like every non-owning pointer in
this API. What is guaranteed is that a *subsequent* lookup returns `nullptr`
rather than the corpse. A program that destroys providers while other threads
look them up has to synchronise that itself; the documented usage — look
profiles up after building them, hold the pointer, destroy at process teardown
— never meets the race.

One more lifetime rule the example has to respect, and which the header
currently gets wrong ([#285](https://github.com/chanderraja/microtel/issues/285)):
a `Tracer` borrows its provider's pipeline through raw pointers and **must not
outlive it**. The frontend's tracer is scoped away before `frontend.reset()`.

### Duplicate names fail loudly

```
Build() with WithProfileName("backend") refused: a live provider is already registered
under profile name 'backend'; shutting a provider down does not release its name,
destroying it does
```

Never last-wins. The live provider keeps the name and the *new* `Build()` is
the one that fails, with `ConfigError::Kind::DuplicateProfileName`. Two
concurrent `Build()`s of the same name produce exactly one provider and exactly
one error — the registry resolves the race by lowest slot index, so the outcome
is deterministic rather than whichever thread got there first.

The sibling failure is `ConfigError::Kind::ProfileLimitExceeded`: the process
holds a fixed maximum number of live providers, and the number is carried in
the error message rather than in any header you include, because it is internal
and raising it is a rebuild, not an ICP.

## What Grafana shows

Both services appear in the service list, side by side:

```
microtel-backend-example
microtel-frontend-example
```

In **Explore → Tempo**, one query each:

```
{ resource.service.name = "microtel-frontend-example" }   # 1 trace: frontend.request
{ resource.service.name = "microtel-backend-example" }    # 1 trace: backend.query
{ name = "backend.healthz" }                              # 0 — dropped at StartSpan
```

Two service names from one process is the observable difference between a
profile and a knob. There is no way to get this by reconfiguring a single
provider: `service.name` is part of the `Resource`, the `Resource` is fixed at
`Build()`, and none of the four hot-reload setters touches it.

## What to notice in the code

- **`GetProvider` returns a raw pointer, and the null check is not optional.**
  A name that was never built, a provider already destroyed, and a
  `fork()`ed child all return `nullptr`.
- **Each provider is shut down explicitly**, in the order the program chooses.
  Destructors would do it, with a small finite timeout, but then the teardown
  order is member-destruction order and the flush statuses are not observable.
- **The duplicate-name attempt is real code, not a comment.** It builds, it
  fails, and the program prints the failure — an example that only *claims* an
  error case is an example that stops being true the day the error changes.
