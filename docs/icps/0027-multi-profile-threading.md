# ICP 0027: multi-profile — named `Provider`s, and the threading model that allows them

**Status:** Proposed — **two decisions below await maintainer confirmation**
(the registry capacity, and the shape of the lookup accessor). The
`docs/threading-model.md` amendments in §1 are applied by this PR because
amending LOCKED text is what an ICP is for; everything else is scheduled work
landing in v1.1 packet 3.1.
**Affected interfaces / docs:** [`docs/threading-model.md`](../threading-model.md)
§2.2 and §2.3 — both **LOCKED**, amended here;
[`include/microtel/sdk_builder.hpp`](../../include/microtel/sdk_builder.hpp)
(`WithProfileName`), [`include/microtel/provider.hpp`](../../include/microtel/provider.hpp)
(one new free function — no new virtual, so **no vtable change**),
[`include/microtel/error.hpp`](../../include/microtel/error.hpp)
(`ConfigError::Kind` gains two enumerators), a new internal
`src/sdk/provider_registry.{hpp,cpp}`, and `src/sdk/sdk_provider.cpp`'s
`g_live_provider` / `ForkChildHandler`. Downstream doc edits are listed under
Migration. No CI, no runtime dependency, no wire-format change.
**Affected tracks:** Track A — SDK (`src/sdk/`). Tracks B–F are untouched:
each profile builds its own transport, encoder, and codecs through the
existing `SdkBuilder::Build` path, which those tracks cannot tell apart from
being built once.

## Summary

Let a process hold several named `Provider`s at **full independence** — each
with its own endpoint, protocol, TLS material, sampler, `Resource`, pipelines,
worker threads, transport, and I/O thread — selected at build time with
`SdkBuilder::WithProfileName(name)` and found at runtime with
`microtel::GetProvider(name)`. The mechanism is a fixed-capacity array of
atomic slots, because the `pthread_atfork` child handler has to walk it
without taking a lock.

## Motivation

**The roadmap promises independence, not an overlay.** `microtel-roadmap.md`
§4 v1.1: *"Multi-profile within one process. Named providers with independent
endpoints, samplers, and Resources."* [`docs/control-plane-design.md`](../control-plane-design.md)
§7 proposed something much smaller — profiles as a **reloadable-tier-only**
overlay, one active at a time, with any endpoint/TLS/sampler-kind difference a
load-time validation error — because a *switch* between profiles needs
transport teardown and re-establishment, which was unsolved when that document
was written. Several `Provider`s side by side need none of that: each one
builds a transport it never hands to anyone else. The maintainer decision is
full independence, and this ICP specifies that, not §7's overlay. §7's
constraint is not wrong; it is a constraint on *switching*, which this design
does not do.

**One LOCKED sentence is the only thing standing in the way, and it is not
standing in the way of much.** [ICP 0021](0021-threading-model-reconciliation.md)
already rescoped the worker claim from "per process" to "per pipeline",
leaving the per-process language in exactly one LOCKED sentence — §2.3's I/O
thread — plus one trailing non-LOCKED sentence in §2.2. Nothing else in the
threading model assumes a single `Provider`: every lock is a member of an
object a `Provider` owns, and §4's lock-ordering rules are per-thread claims
that N independent provider graphs satisfy exactly as one does.

**The fork handler is what actually shapes the design.** `g_live_provider`
(`src/sdk/sdk_provider.cpp`) is one `std::atomic<SdkProvider*>`, and its own
comment says why: *"a child-side handler must not take a lock, because a lock
held at `fork()` time by a thread that does not exist in the child is never
released. An atomic has no such hazard … Multi-profile (v1.1) turns this into
a real registry, and that design has to solve the locking problem this one
sidesteps."* This ICP is that design. It is also worth noting that
[`docs/sequences/fork-survival.md`](../sequences/fork-survival.md) has said
*"the child handler immediately CAS-flips **every** live `Provider`"* since M0,
and `tests/unit/sdk/fork_safety_test.cpp`'s header comment repeats it: the
plural was always the intent, and the single slot is the narrowing.

**The single slot is already slightly wrong.** Build A, build B, destroy A:
`~SdkProvider`'s `compare_exchange_strong(self, nullptr)` correctly declines to
unhook B — but A stopped being reachable the moment B's constructor overwrote
the slot. A fork in between marks B and leaves A, still live, unmarked;
a child touching A's `Span::End()` can deadlock on `BatchSpanProcessor`'s mutex
(the hazard §7 already documents). Nothing in-tree builds two providers at
once, so the bug is latent. Multi-profile makes it routine.

## Proposed change

### 1. The two LOCKED amendments (applied in this PR)

Both are rescopings from *per process* to *per `Provider`*, which is the same
claim while a process has one `Provider` — so the amended text is true of
`master` as it stands, and nothing here asserts code that does not exist. Only
the forward-looking clauses name packet 3.1, and they say so. §2.3's marker is
the correction; §2.2's carried no per-process language after ICP 0021 and is
amended only to make its scope unambiguous once several providers exist.

| `threading-model.md` | Before | After |
|---|---|---|
| §2.2 | "**One worker per pipeline** (LOCKED — …), of which a fully-configured single `Provider` has **five**: …" | "**One worker per pipeline, and every pipeline belongs to one `Provider`** (LOCKED — same cites), of which a fully-configured `Provider` has **five**: … The count is **per `Provider`, not per process**: a process running N named profiles runs N such sets, sharing no thread, queue, or transport between them." |
| §2.2 trailing | "One `Provider` is still the v1 supported configuration." | "One `Provider` was v1.0's supported configuration; from v1.1 a process may run several named ones (ICP 0027, taking effect with packet 3.1). Neither fact touches the per-pipeline rule, which was always a statement about one `Provider`'s internals." |
| §2.3 heading | "I/O thread (one per process)" | "I/O thread (one per `Provider`)" |
| §2.3 | "**v1 always has exactly one I/O thread per process** (LOCKED — cites `src/transport/http2_transport.hpp:m_io_thread`, `src/sdk/sdk_builder.cpp:Build`). One nghttp2 session, one socket, one reactor: … `SdkBuilder::Build` constructs one transport, shared by every pipeline." | "**Exactly one I/O thread per `Provider`** (LOCKED — same cites). … **each** `SdkBuilder::Build` constructs one transport, shared by every pipeline **of the `Provider` it builds**." Plus a paragraph recording the rescoping, its packet-3.1 effective date, and that each profile owns its own socket, reactor, and `SslCtx` — the last per [ICP 0003](0003-m0-deferred-decisions.md) §3.1, which put `SslCtx` on the `Transport` and named multi-`Provider` as the case that might one day revisit it. |

Citations are unchanged and still resolve; `ci/scripts/citation-check.py` is
green. **Only these two markers are amended by this PR.** §7's fork bullet
("marks the live `Provider`", singular) and §10's type table are packet 3.1's
to amend, per the [ICP 0025](0025-propagation-core.md) /
[ICP 0026](0026-provider-setters.md) precedent that a normative document is not
told about a method before the method exists.

### 2. The registry: a fixed array of atomic slots

`src/sdk/provider_registry.{hpp,cpp}`, internal to `microtel_sdk`.

```cpp
/// PROPOSED capacity — see "Decisions awaiting confirmation". One edit here
/// changes it; nothing else in the tree hard-codes a profile count.
inline constexpr std::size_t kMaxProfiles = 8;

/// Slots are plain atomic pointers and nothing else. The name lives in the
/// provider (`SdkProvider::m_profile_name`, immutable from before the slot is
/// published until the provider is destroyed), so the fork handler never
/// dereferences a `std::string` and the registry never allocates.
using Registry = std::array<std::atomic<SdkProvider*>, kMaxProfiles>;
```

**Every operation is lock-free.** Not "lock-free where convenient" — the child
of `fork()` must be able to call `SdkBuilder::Build()` (fork-survival option A,
the supported path), and a registry mutex held by a thread that did not survive
the fork would hang exactly that call.

- **Register** (`SdkBuilder::Build`, after construction): scan for a live slot
  whose provider carries the same name → `ConfigError`. Otherwise
  `compare_exchange_strong(nullptr, provider)` on the first empty slot; no
  empty slot → `ConfigError`. Then **re-scan**: if another slot now holds the
  same name at a *lower index*, store `nullptr` back and fail. Lowest index
  wins, deterministically, so two concurrent `Build()`s of one name produce
  exactly one provider and one `ConfigError` — with no lock and no ABA, because
  a slot only ever goes null → provider → null and a provider address is
  claimed once.
- **Deregister** (`~SdkProvider`): find the slot holding `this`, CAS it to
  `nullptr`. This stays **at the top of the destructor, before `Shutdown`**,
  which is where the existing single-slot clear already is, and it is what
  makes the fork handler safe: a slot is emptied before its provider begins
  tearing down, so a handler running in the child sees either a provider that
  is entirely intact or nothing at all.
- **Look up** (`microtel::GetProvider`): acquire-load each slot, compare
  `m_profile_name`, return the first match. `kMaxProfiles` loads and at most
  `kMaxProfiles` short string compares; not a hot path, and callers are
  expected to look a provider up once and keep the pointer.
- **Fork child handler:** §3.

Registration moves out of `SdkProvider`'s constructor (where
`g_live_provider.store(this)` is today) into `SdkBuilder::Build`, because it can
now fail and the constructor is `noexcept`. `Build` pre-checks the name before
constructing anything, so the ordinary sequential duplicate costs nothing; the
CAS claim after construction is what makes it correct under concurrency. A
duplicate detected only by that claim costs one provider construction and
immediate teardown — a startup-path cost, paid once, by a program with a bug.

**Why a fixed array rather than a map.** A `std::map`/`std::vector` cannot be
walked from a fork child handler: it is mutated under a lock, its nodes are
heap-allocated by an allocator that is itself mid-operation at `fork()` time,
and its iteration is not a bounded sequence of atomic loads. The array gives
the handler N loads and N stores with no allocation, no lock, and a compile-time
bound — which is the entire reason to accept a capacity limit at all.

`kMaxProfiles` is **internal, not public API**: no header a consumer includes
names it, and the `ConfigError` message carries the number. Raising it later is
a rebuild, not an ICP.

### 3. The fork handler marks — and clears — every slot

```
for each slot:
    p = slot.load(acquire)
    if p: p->MarkForkedChild();  slot.store(nullptr, release)
```

Async-signal-safe by the same argument the single-slot version already makes
(`SdkProvider::MarkForkedChild`'s Doxygen: *"this does nothing but a
relaxed-release store to an atomic. It takes no lock and allocates nothing"*),
now bounded at `kMaxProfiles` iterations. No name is read, so no `std::string`
is touched in the child.

**Clearing the slots is a deliberate addition, and it is a trade.** The child's
supported move is to re-`Build()` (fork-survival option A) — and it will
naturally re-build with the *same* profile names, which would collide with the
stale parent-era entries and fail with a duplicate-name `ConfigError`. Clearing
makes the child's registry empty, exactly as a fresh process's is. The cost is
that `GetProvider(name)` in a child returns `nullptr` rather than a
shut-down provider. That is the better failure: `nullptr` is a defined,
checkable outcome the caller can branch on, while a name collision at re-init
is a confusing one. The application's own `shared_ptr<Provider>`s from before
the fork are unaffected and still observe `AlreadyShutDown` on every call,
which is what §7's rule actually promises.

### 4. `WithProfileName` and `GetProvider`

```cpp
// include/microtel/sdk_builder.hpp, beside WithServiceName
/// @brief Name this provider's profile. Defaults to "default".
///
/// Names identify live providers within the process: `Build()` fails with
/// `ConfigError::Kind::DuplicateProfileName` if another live provider already
/// has this one. Compared byte-for-byte; no normalisation, no case folding.
SdkBuilder& WithProfileName(std::string name);

// include/microtel/provider.hpp
inline constexpr std::string_view kDefaultProfileName = "default";

/// @brief Find the live provider registered under @p name.
///
/// @return a **borrowed, non-owning** pointer, or `nullptr` if no live
///         provider carries that name. Never transfers ownership.
[[nodiscard]] Provider* GetProvider(std::string_view name = kDefaultProfileName) noexcept;
```

One function with a default argument, so `GetProvider()` is the default-profile
accessor and `GetProvider("audit")` is the named one. Defined in
`src/sdk/provider_registry.cpp` (`microtel_sdk`), not in `microtel_api` — the
registry is an SDK concept and `microtel_api` links nothing from the SDK.

**Lifetime, in full.** The returned pointer is borrowed under hard rule 7, and
ownership stays exactly where `Build()` put it: in the caller's
`std::shared_ptr<Provider>`.

- **The registry does not own and does not extend.** A slot holds a raw
  pointer, so registration keeps nothing alive. The slot's occupancy is
  strictly *shorter* than the provider's life — cleared at the top of the
  destructor, before teardown — never longer. So a lookup that loads a slot
  after that clear cannot see a destructing provider; one that loads it just
  before can, which is the inherent race of any non-owning accessor and is the
  next bullet's subject.
- **Across `Shutdown`: the pointer stays valid.** `Shutdown` does not
  deregister; only destruction does. A caller holding a `Provider*` across
  `Shutdown(timeout)` keeps a valid pointer to a live object on which every
  lifecycle method returns `AlreadyShutDown` and spans from a tracer it hands
  out drop with `post_shutdown` — the same contract a `shared_ptr` holder
  gets. `GetProvider`
  also keeps *finding* a shut-down provider — shut down is not gone, and a name
  is freed for reuse by destruction, not by `Shutdown`.
- **Across destruction: the pointer dangles**, like every non-owning pointer in
  this codebase. The registry cannot prevent that; what it guarantees is that a
  *subsequent* `GetProvider(name)` returns `nullptr` rather than the corpse.
  The documented usage is the one that makes this a non-issue: look profiles up
  after building them, hold the pointer, destroy providers at process teardown.
  A program that destroys providers while other threads look them up must
  synchronise that itself, and the Doxygen says so.
- **Across `fork()`:** in the child every slot is empty (§3), so `GetProvider`
  returns `nullptr` until the child re-builds. A pointer obtained *before* the
  fork is still valid in the child and answers `AlreadyShutDown`.

### 5. Duplicate names fail the build, loudly

`ConfigError::Kind` gains two enumerators, additive, in the same v1.0 → v1.1
recompile window [ICP 0025](0025-propagation-core.md) and
[ICP 0026](0026-provider-setters.md) already spend:

```cpp
    DuplicateProfileName = 11,  ///< another live provider already has this name
    ProfileLimitExceeded = 12,  ///< the process already holds the maximum
                                ///< number of live providers
```

**Never last-wins.** A second `Build("default")` that silently displaced the
first would leave the displaced provider live, exporting, and unreachable by
name, while every later `GetProvider("default")` returned the other one —
telemetry quietly redirected by a typo. `Build()` already returns
`Expected<std::shared_ptr<Provider>, ConfigError>` and already fails for
`BuildAlreadyConsumed`; a duplicate name is the same kind of programming error
and gets the same treatment. The name is also rejected when empty
(`Kind::InvalidValue`) — an unnamed profile is `"default"`, not `""`.

No enumerator is removed and nothing in-tree switches exhaustively over
`ConfigError::Kind`, so no in-tree consumer changes.

### 6. What is *not* independent, stated plainly

Default-profile semantics are unchanged for every v1.0 program: a builder with
no `WithProfileName` gets `"default"`, `Build()` behaves exactly as today, and a
program that never calls `GetProvider` cannot tell that a registry exists.

Three things stay process-wide, and full independence does not reach them:

1. **The internal log level and log sink.** Already decided:
   [ICP 0026](0026-provider-setters.md) §6 — *"because the knob is
   process-global, v1.1's multi-profile providers share it: last writer
   wins"* — because `LogImpl` is a free function with no provider in scope.
   `g_live_provider` is the only process-global mutable state that names a
   `Provider`; the log sink (`src/common/log_sink.cpp`'s `SinkState`) is the
   other piece of process-global mutable state, and it is shared by design.
2. **The current context.** [ICP 0025](0025-propagation-core.md)'s
   `CurrentContext()` is one slot **per thread**, not per profile: a span
   started through profile A's tracer is the current span that profile B's
   tracer will parent from on the same thread. This is correct — context is a
   property of the request, not of the exporter it is destined for — but it
   means profiles are independent in where telemetry *goes*, not in what the
   thread is currently doing.
3. **The otel-cpp shim binds one profile.**
   `src/adapters/otelcpp/global_registration.hpp`'s `RegisterGlobally` takes
   the `std::shared_ptr<microtel::Provider>` to bind as a parameter and never
   consults any global of ours, so it needs **no change**: it binds whichever
   provider it is handed, which for existing code is the default profile.
   otel-cpp's own API holds exactly one provider per signal, so only that one
   profile is reachable through instrumentation written against otel-cpp. Other
   profiles are reached through microtel's own API.

## Decisions awaiting confirmation

Both are marked **PROPOSED, maintainer confirmation pending**; the status line
above stays `Proposed` until they are settled.

1. **`kMaxProfiles = 8.`** A fixed capacity is forced by §2 (a fork child
   handler cannot walk a container). The *number* is a guess with one datum
   behind it: the deployments this feature is for — an application splitting
   audit telemetry from operational telemetry, or fanning to a local collector
   and a vendor endpoint — want two or three, and 8 leaves room without making
   the child handler's loop or the eight-pointer array worth a thought.
   Guessing low costs a `ProfileLimitExceeded` at startup; guessing high costs
   eight pointers of BSS. Flipping it is a **one-line edit** to
   `kMaxProfiles` in `src/sdk/provider_registry.hpp`, and because the constant
   is internal, no consumer recompile is implied by changing it later.
2. **`Provider* GetProvider(std::string_view name = kDefaultProfileName)`** as
   a free function returning a borrowed pointer. Recommended: it matches hard
   rule 7 (a returned `T*` is borrowed), keeps `shared_ptr` out of a path that
   does not need it (rule 8), needs no vtable slot, and reads the same way as
   `CurrentContext()`, the other free-function accessor for process-scoped
   state ([ICP 0025](0025-propagation-core.md)) — **microtel has no
   provider-returning global accessor today**, so this defines the shape rather
   than matching one. The alternative is
   `std::shared_ptr<Provider> GetProvider(...)` with `weak_ptr`s in the
   registry, which would make the dangling case in §4 impossible. It is
   rejected because `std::atomic<std::weak_ptr<T>>` is not lock-free — it
   spins on an internal lock that a `fork()` can strand — which puts a lock
   back into the one data structure that must not have one. A registry holding
   *both* a raw pointer for the handler and a `weak_ptr` for lookups would
   work, at the price of two parallel structures and a second consistency
   invariant, for a guarantee the documented usage does not need.

## Discrepancies found while writing this

Recorded rather than silently designed around.

1. **`memory-model.md` contradicts itself about `SslCtx`.** §4.2's table says
   *"Owned by `Transport` (one per process in v1; multi-`Provider` in v1.1
   gives each `Transport` its own)"*, while §5.3's diagram says
   *"`SslCtx` ◄── shared, single instance per process"* and its prose repeats
   *"shared across reconnects (one per process)"*. The code has
   `Http2Transport::m_ssl_ctx`, a per-transport member, which is what
   [ICP 0003](0003-m0-deferred-decisions.md) §3.1 decided. Pre-existing; packet
   3.1 fixes §5.3's wording, which is the only edit multi-profile needs there.
2. **`architecture.md` §8's glossary still says "single thread per process"**
   for both the exporter worker and the I/O thread. The first has been wrong
   since M12 (ICP 0021 corrected `threading-model.md` but not this glossary);
   the second becomes wrong with packet 3.1. Packet 3.1 amends both, along with
   `microtel-spec.md` §5.1's identical phrasing.
3. **`control-plane-design.md` §7 quotes threading-model text that no longer
   exists**, by line number (`docs/threading-model.md:44`), and reaches the
   opposite scope decision to this one. [ICP 0024](0024-v1.1-rescope.md) chose
   not to amend that document, and this ICP does not either; §7 is a decision
   about a socket that is now v1.2 work, and it is re-opened with the socket.
4. **`InstallForkHandlersOnce` can strand a forked child.** `std::call_once`
   blocks if another thread is inside the callable, and a `fork()` during that
   window leaves the flag permanently "in progress" for the child — so a child
   calling `Build()` hangs. Pre-existing, entirely independent of
   multi-profile, and the reason §2 insists the registry itself stay lock-free
   rather than adding a second instance of the same hazard. Worth its own issue;
   not fixed here.

## Migration

Nothing to do today; this ICP amends two sentences and schedules the rest.

- **Packet 3.1 — multi-profile.** `provider_registry.{hpp,cpp}`,
  `sdk_provider.{hpp,cpp}` (replace `g_live_provider`, add `m_profile_name`,
  rewrite the comment block that cites §2.2 as fixing v1 at one provider),
  `sdk_builder.{hpp,cpp}`, `provider.hpp`, `error.hpp`, and
  `tests/unit/sdk/fork_safety_test.cpp` (a two-provider fork test — the
  scenario `docs/sequences/fork-survival.md` has specified since M0 and nothing
  has ever exercised). Amends `threading-model.md` §7 (the fork bullet's
  singular "the live `Provider`") and §10 (a row for the registry accessor),
  `memory-model.md` §5.3, `architecture.md` §8, and `microtel-spec.md` §5.1,
  §12.8 and §18.1. Adds a TSAN test racing concurrent `Build()`s of the same
  and different names against `GetProvider`.
- **Consumers:** recompile against v1.1 headers — the same recompile ICPs 0025
  and 0026 already require. No source change. A program that never names a
  profile is unaffected in every observable way.

## Rationale & alternatives

- **`control-plane-design.md` §7's reloadable-tier overlay** — rejected as the
  answer to *this* question. It solves profile *switching* (one active profile,
  swapped at runtime), which needs transport re-establishment. Independent
  concurrent providers need no switch, no teardown, and no validation rule
  about which fields may differ: they may all differ, because nothing is
  shared.
- **A `std::map<std::string, SdkProvider*>` under a mutex** — rejected; the
  fork handler cannot take the mutex, and cannot safely walk the map without
  it.
- **Keep `g_live_provider` and register only the default profile** — rejected.
  It preserves today's latent bug as a designed-in one: every non-default
  profile would be invisible to the fork handler, and a child touching one
  could deadlock in `BatchSpanProcessor::OnEnd`.
- **Last-wins on a duplicate name** — rejected; §5.
- **No lookup function at all** (the host keeps its own `shared_ptr`s) —
  rejected, but it is the closest alternative and it is why `GetProvider`'s
  shape is still open. A host that builds its profiles in one place can always
  pass the `shared_ptr` where it is needed, and would never call `GetProvider`.
  The lookup earns its place for instrumentation that cannot reach the host's
  wiring — library code, a plugin, a callback — which is the same argument
  otel-cpp's global provider rests on. The registry itself is **not** optional
  either way: the fork handler needs it whether or not anything looks a
  provider up by name.
- **A per-profile internal log level** — rejected by
  [ICP 0026](0026-provider-setters.md) §6 and not reopened here.
