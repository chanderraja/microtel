# microtel Control Plane Design

**Status:** Draft — M15 design milestone. **Not implemented.** Requires
reviewer sign-off before any implementation begins.

This document settles the decisions M15 (control plane + hot reload) cannot be
built without, the same way `docs/metrics-design.md` preceded M12 and
`docs/logs-design.md` preceded M14. Spec §18.1 defers the control plane's
*"scope and threat model"* to "v1.1 design" — this is that design.

Three of the decisions below (§3 JSON, §5 client language, §4 threat model)
are genuinely open, not rubber stamps: the spec names a shape for each, but
each carries a cost the spec did not price. Where this document recommends
departing from the spec's stated shape, it says so explicitly.

## Scope

In scope for M15 (this doc decides all of these):

- A Unix-domain-socket server inside the microtel process, accepting
  operator commands.
- A wire format and framing for that socket.
- Runtime-mutable settings: which ones, and what each costs to make mutable.
- A client (`microtelctl`): single-shot and REPL.
- `SIGHUP`-driven config reload.
- Multi-profile within one process.
- The threat model for all of the above.

Explicitly **out of scope** for M15 (anti-goals):

- Any network-facing (TCP) control surface. UDS only, permanently.
- Remote administration, clustering, or a fleet control protocol.
- Mutating anything that would require breaking a locked interface — such
  changes go through the ICP process on their own merits, not bundled here.
- Reload of endpoint/TLS/auth settings that would require tearing down and
  re-establishing the transport (see §2, deferred to the reconnect work).

## Sign-off checklist

Each item has a proposed **Decision** below. Reviewer approves by checking
every box (and editing any decision they want changed first).

- [ ] §1 Prerequisite — M15's stated dependency (M10) has not shipped
- [ ] §2 What is actually hot-reloadable — **the finding that reshapes M15**
- [ ] §3 Wire format — JSON needs a parser microtel does not have (**ICP**)
- [ ] §4 Threat model — UDS permissions, peer credentials, mutation surface
- [ ] §5 `microtelctl` language — **recommend C++, not Go** (departs from spec)
- [ ] §6 Command surface
- [ ] §7 Multi-profile
- [ ] §8 `SIGHUP` reload and fork-safety interaction
- [ ] §9 Threading & lifecycle
- [ ] §10 Testing, fuzzing, and CI cost

---

## §1 Prerequisite: M15's dependency has not shipped

Spec §13 lists M15's "Depends on" as **M10 — v1.0 traces release**. M10 has
not happened: the newest tag in the repository is `v0.2.0-m2`, and no release
commit exists. Issue #19 (`install(TARGETS ... EXPORT ...)`, needed for
`find_package(microtel)`) is itself gated on that packaging work.

This matters concretely, not just procedurally. A control plane is an
*operator-facing* surface: its value is administering a deployed process, and
nothing is deployed yet. Shipping it before v1.0 means designing an
administrative interface against zero operational feedback, then supporting it
under the compatibility policy (§19: "the public C++ API is stable within a
major version").

**Decision.** M15 proceeds **design-only** for now: this document is the
deliverable, and implementation waits on either (a) M10 shipping, or (b) an
explicit reviewer decision to run M15 ahead of its dependency, recorded here.
The design work is not wasted either way — it is what makes M10's scope
decision informed, since §3 and §5 below add dependencies and toolchains that
a v1.0 release would have to carry.

---

## §2 What is actually hot-reloadable

This is the finding that should shape M15's scope, and it is not visible from
the spec.

"Hot-reloadable settings (sampler, batch sizes, log level)" reads as one
feature. In this codebase it is **two features with an order-of-magnitude cost
difference between them**, because the settings differ in how they are
synchronized today.

### Cheap: settings already read under a lock

`BatchSpanProcessor` holds `BatchOptions m_opts` as a plain non-const member
(`src/sdk/batch_span_processor.hpp:85`), and **every read of it is already
inside the processor's own mutex**:

- `src/sdk/batch_span_processor.cpp:54,56,66` — `OnEnd`, under `scoped_lock{m_mu}`
- `src/sdk/batch_span_processor.cpp:118,122,126` — `WaitAndCollect`, under `unique_lock{m_mu}`

Mutating `max_queue_size`, `max_export_batch_size`, `schedule_delay`, or
`drop_policy` at runtime therefore costs: take `m_mu`, assign, `notify_one()`.
No new synchronization, no lifetime problem, no hot-path tax. The same shape
applies to the log processor and the metric reader interval.

### Expensive: the sampler is read unsynchronized on the hot path

`SdkTracer` borrows a **raw non-owning pointer** —
`internal::ISampler* m_sampler` (`src/sdk/sdk_tracer.hpp:56`) — and calls
through it in `StartSpan` with no lock of any kind:

```cpp
const internal::SamplingResult result = m_sampler->ShouldSample(sctx);
```
(`src/sdk/sdk_tracer.cpp:99`)

Every application thread that starts a span reads that pointer concurrently.
Swapping the sampler at runtime therefore requires **both**:

1. Making the pointer itself atomic — a plain write racing those reads is a
   data race, i.e. undefined behavior, not merely a stale read.
2. A lifetime scheme for the outgoing sampler. `SdkProvider` owns it as
   `SamplerHandle m_sampler` (`src/sdk/sdk_provider.hpp:177`), which is a
   move-only `std::unique_ptr<ISampler>` owner
   (`include/microtel/sampler.hpp:27`). Destroying the old sampler while a
   reader is inside `ShouldSample()` is a use-after-free. Correct options are
   RCU/hazard-pointer style deferred reclamation, an atomic `shared_ptr` load
   on every `StartSpan`, or never freeing the old sampler.

Option (2) is the problem. An atomic `shared_ptr` load per `StartSpan` puts a
refcount round-trip on the hot path that microtel's entire performance claim
(§10 footprint/CPU metrics, validated by the M7 harness) is built on
*avoiding*. Deferred reclamation avoids the hot-path cost but is genuinely
hard concurrent code, and it would be the most delicate code in the project —
added for an operator convenience, in a telemetry library.

**Decision.** M15 ships hot-reload for the **lock-protected tier only**: batch
sizes, queue sizes, schedule delay, drop policy, export/retry timeouts, and
log level. **Sampler hot-swap is deferred out of M15** and, if wanted later,
gets its own ICP that prices the hot-path cost against the M7 benchmark
numbers rather than assuming it is free.

This is a real scope reduction against the spec's wording, and it is the main
thing this document asks the reviewer to accept. The alternative — taxing
`StartSpan` for every user in every deployment so that an operator can change
sampling without a restart — inverts the project's stated priorities. Note
that sampling ratio remains changeable the ordinary way: edit config, restart.

---

## §3 Wire format: JSON needs a parser microtel does not have

Spec §18.1 specifies "length-prefixed JSON wire". **There is no JSON
parser in the dependency closure.** CLAUDE.md rule 12 fixes that closure at
nghttp2, OpenSSL, upb (vendored), zlib, and optional spdlog; `third_party/`
contains exactly `tl-expected`, `upb`, and `utf8_range`. upb's `upb/json/` is
pruned from the vendored subset (`third_party/upb/README.md`), and is
protobuf-shaped anyway.

So JSON is not free here, and rule 12 makes every path an ICP:

| Option | Cost |
|---|---|
| **A. Vendor a header-only JSON library** | New vendored dependency (**ICP**). Adds to the closure microtel exists to keep small; the closure claim is the project's whole pitch. Most such libraries are exception-heavy and allocation-heavy. |
| **B. Hand-roll a minimal parser** | No new dependency, but it is a **parser on an inbound socket** — the exact shape of bug that CVEs are made of. Needs its own fuzz harness (§10) and a hard input-size cap. Realistically 300–500 lines of security-relevant code. |
| **C. Non-JSON framing** (length-prefixed key=value or a small binary form) | Cheapest and safest: a trivially parseable, fixed-grammar format with no nesting and no recursion. Departs from the spec's stated wire. Slightly worse for ad-hoc scripting (`jq` no longer works against it). |

**Decision.** **Option B**, with constraints, and only because the client
(§5) is ours: a strict, non-recursive subset parser — objects of string keys
to scalar values, no nested objects, no arrays, no `\u` escapes — with a hard
frame cap (proposed: 64 KiB) enforced *before* parsing. That subset is
adequate for every command in §6, is small enough to fuzz to exhaustion, and
keeps the closure claim intact. It is JSON on the wire (so `jq` and ad-hoc
tooling still work) without accepting arbitrary JSON.

Rejecting A is a judgment call the reviewer may want to overturn: vendoring a
mature parser trades a closure-size increase for not writing security-relevant
parsing code by hand. If the reviewer prefers A, the ICP should name the
specific library and measure the closure delta.

**This decision requires an ICP regardless of which option is chosen**, since
rule 12 governs the closure and §12.8's "no long-running control-plane socket"
is a v1 anti-goal being lifted.

---

## §4 Threat model

Spec §19 lists the threat model as *"initial: enumerated for the v1.1 control
plane"* — i.e. this section is the first time it is written down. This is the
project's **first inbound socket surface**: a grep for `listen(`, `accept(`,
`AF_UNIX`, and `sockaddr_un` across `src/` and `tools/` returns nothing.
Everything today is outbound client traffic.

**Asset.** The ability to mutate a live process's telemetry configuration, and
to read back its resolved config and health.

**Threats and mitigations:**

| Threat | Mitigation |
|---|---|
| Any local user connects and reconfigures the process | Socket mode `0600`, owned by the process UID. Additionally verify peer identity with `SO_PEERCRED` and reject any UID that is neither the process UID nor root. Both, not either. |
| Socket path predictable / squatted before bind | Default the path under a runtime dir owned by the user (`$XDG_RUNTIME_DIR/microtel-<pid>.sock`), never `/tmp`. Bind to a temp name and `rename()` into place; refuse to unlink an existing path that is not a socket we own. |
| Secret disclosure via read-back commands | The existing redaction rule (§12.6) applies unconditionally to every control-plane response. `--show-secrets` has **no control-plane equivalent** — secrets are never readable over the socket, at any privilege level. |
| Malformed frame → parser exploit | §3's restricted grammar, hard size cap enforced pre-parse, plus a dedicated fuzz harness (§10). |
| Resource exhaustion (connection flood, slowloris) | Single-threaded accept loop, max 4 concurrent connections, per-connection idle timeout, bounded read buffer. Reject rather than queue. |
| Control plane as a persistence/escalation foothold | No command may execute a path, load a library, or write a file. The command surface (§6) is a closed enumeration of setting mutations and reads — **no passthrough, no eval, no config-file path argument**. |

**Decision.** The above is the M15 threat model. Two properties are
load-bearing and should be treated as invariants rather than defaults:
**UDS-only forever** (no TCP option, not even opt-in — the moment it is
reachable off-box the model above is void), and **no secret is ever readable
over the socket**.

---

## §5 `microtelctl`: Go or C++

Spec §18.1 says "`microtelctl` Go binary". That was written before the repo
had a CLI of its own; it now has one.

Cost of Go: a second toolchain in a C++ repository. It must be installed in
**all four CI jobs that build** (`tidy-check`, `compile`, `sanitizers`,
`coverage` — per CLAUDE.md's own warning about adding dependencies to all
four), gain its own CODEOWNERS entry, its own lint/format/test gates, its own
supply chain (`go.mod`), and its own release artifacts per platform. None of
that is shared with anything else the project builds.

Benefit of Go: fast static binaries, easy cross-compilation, a pleasant REPL
ecosystem. Real, but the client is a thin socket-and-print program.

The alternative is already demonstrated in-tree: `tools/preflight/` builds
`microtel-preflight` as a `libmicrotel_preflight_lib.a` plus a thin `main.cpp`
— library-plus-shim, which makes the logic unit-testable with the existing
gtest setup. A `microtelctl` following that pattern reuses the whole existing
build, test, lint, sanitizer, and packaging pipeline at zero marginal
infrastructure cost, and can share the §3 frame codec with the server
directly, guaranteeing client and server cannot drift.

**Decision — departs from the spec.** Write `microtelctl` in **C++**,
following the `tools/preflight/` pattern. Recommend amending spec §18.1 to
drop "Go binary". If the reviewer wants Go anyway, that is a legitimate call —
but it should be made knowing it is a four-CI-job toolchain addition for a
program that opens a socket and prints replies.

---

## §6 Command surface

A closed enumeration (§4: no passthrough). Every command is one frame in, one
frame out.

| Command | Effect | Tier (§2) |
|---|---|---|
| `status` | Resolved config (redacted) + `GetExporterHealth()` snapshot | read-only |
| `get <key>` | One resolved setting, redacted | read-only |
| `set batch.<field> <value>` | Mutate batch/queue/delay/drop-policy | cheap |
| `set timeout.<field> <value>` | Mutate export/retry timeouts | cheap |
| `set log.level <value>` | Mutate diagnostic verbosity | cheap |
| `reload` | Re-read the config file; apply only cheap-tier deltas, report the rest as requiring restart | cheap |
| `profile list` / `profile switch <name>` | §7 | cheap |
| `flush [timeout]` | `ForceFlush` | read-only-ish |

`set sampler.*` is **absent** by §2's decision, and `reload` must report
sampler changes as "requires restart" rather than silently ignoring them —
silent no-ops are the failure mode ICP 0017 was written to fix, and this
document should not reintroduce the pattern.

**Decision.** As tabled. Any command not in this table requires amending this
document.

---

## §7 Multi-profile

"Multi-profile within one process" (spec §12.8, §18.1) means several named
config profiles resolvable at runtime with one active at a time.

Given §2, a profile switch can only apply the cheap tier. A profile that
differs in endpoint, TLS, auth, or sampler cannot be switched into without a
transport teardown — which is the reconnect problem, currently unsolved (see
`ConnectionState::Reconnecting`, dead code today; ICP 0017 addressed only the
*initial* connect).

**Decision.** M15 ships profiles as a **cheap-tier-only** overlay: profiles
may differ in batch/timeout/log-level settings; a profile differing in
endpoint/TLS/auth/sampler is a **validation error at load time**, not a
runtime surprise. Full profile switching depends on transport re-establishment
and is deferred to whichever milestone solves reconnect.

---

## §8 `SIGHUP` reload and fork-safety

Spec §18.1 wants "config reload via SIGHUP".

Constraint: async-signal-safety. A handler may not lock a mutex, allocate, or
re-read a TOML file. The standard shape applies — the handler does nothing but
`write()` a byte to a self-pipe (or `eventfd`); the control-plane thread does
the real work. `src/common/raii/unique_fd.hpp` already provides the RAII fd
wrapper for it.

Interaction to verify during implementation: M5 added fork-safety handling,
and the control-plane thread and its listening socket must not survive into a
forked child in a broken state — the child must not inherit a bound socket it
does not own. Expected shape: `pthread_atfork` child handler closes the
inherited fd without unlinking the path (the parent still owns it).

**Decision.** Self-pipe handler, real work on the control-plane thread. Signal
installation is **opt-in**, not automatic — a library that installs a `SIGHUP`
handler behind an application's back is a bug, and many applications already
use `SIGHUP` for their own reload.

---

## §9 Threading & lifecycle

Adds **one** thread: the control-plane thread, owning the listening socket and
all connections, blocking in `poll()`/`epoll_wait` (including the §8
self-pipe). It never touches SDK internals directly; it applies mutations by
calling the same lock-protected setters an application could call.

Thread inventory becomes: I/O thread, exporter worker(s), metric reader
thread, control-plane thread. `docs/threading-model.md` gains a section.

Lifecycle: started only when explicitly enabled in config (**off by default** —
a telemetry library must not open a socket unless asked); stopped in
`Provider::Shutdown` before the transport closes, with the socket path
unlinked on clean shutdown. The thread must be joinable within `Shutdown`'s
timeout, per CLAUDE.md rule 15.

**Decision.** As above. Off by default, one thread, no direct SDK-internal
access.

---

## §10 Testing, fuzzing, and CI cost

- **Unit:** frame codec (round-trip, truncation, oversize rejection, every
  malformed-grammar case), command dispatch, each cheap-tier setter.
- **Fuzz:** a `control_plane_frame_fuzz.cpp` under `tests/fuzz/`, following
  the established pattern there (`toml_fuzz.cpp`, `grpc_codec_fuzz.cpp`,
  `otlp_response_fuzz.cpp`, `response_decompression_fuzz.cpp` + `corpus/`).
  **Non-optional** given §3's decision to hand-roll the parser.
- **Integration:** real socket, real client binary, over a real UDS —
  including the §4 rejection cases (wrong UID via `SO_PEERCRED`, oversize
  frame, connection-count cap).
- **TSAN:** mandatory. This adds a thread that mutates state read by other
  threads; per CLAUDE.md's threading-discipline note, TSAN is the gate.
- **Coverage:** the diff-coverage thresholds apply unchanged.

**Decision.** As above, with the fuzz harness treated as a ship-blocker rather
than a follow-up.

---

## Open items flagged for the reviewer

1. **§1** — Does M15 implement now, ahead of M10, or stop at this document?
   Recommend stopping here until M10 ships.
2. **§2** — Accept dropping sampler hot-swap from M15? This is the largest
   departure from the spec's wording and the one most worth arguing about.
3. **§3** — Hand-rolled restricted parser (recommended) vs. vendoring a JSON
   library. Either way, an ICP.
4. **§5** — C++ client (recommended) vs. the spec's Go binary.
5. Should spec §18.1 be amended to match whatever is decided in §2/§3/§5, so
   the roadmap stops describing a shape the project has decided against?

## Increment plan

Assumes sign-off, and assumes §1 resolves in favour of proceeding.

| Increment | Scope |
|---|---|
| L1 | ICP for §3 (closure/wire) and the §12.8 anti-goal lift; spec §18.1 amendments from §2/§5 |
| L2 | Frame codec + fuzz harness, no socket yet |
| L3 | UDS server, threat-model controls (§4), off-by-default config plumbing |
| L4 | Cheap-tier setters + `status`/`get`/`set`/`flush` dispatch |
| L5 | `microtelctl` single-shot, sharing L2's codec |
| L6 | REPL, `reload` + `SIGHUP` (§8), profiles (§7) |
| L7 | Integration tests over a real socket; TSAN; `docs/threading-model.md` update |

## References

- `microtel-spec.md` §12.8 (v1 anti-goals), §18.1 (v1.1 control plane), §19 (governance / threat model), §13 (milestone table)
- `docs/metrics-design.md`, `docs/logs-design.md` — the design-doc precedent this follows
- `docs/icps/0017-lazy-transport-connect.md` — the silent-no-op failure mode §6 avoids repeating
- `src/sdk/sdk_tracer.cpp:99`, `src/sdk/sdk_tracer.hpp:56`, `include/microtel/sampler.hpp:27` — §2's hot-path finding
- `src/sdk/batch_span_processor.cpp:54,118` — §2's lock-protected tier
- `tools/preflight/` — §5's client pattern
- `tests/fuzz/` — §10's harness pattern
