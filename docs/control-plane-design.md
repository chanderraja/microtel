# microtel Control Plane Design

**Status:** Design complete, implementation DEFERRED — see
"Status and disposition" below. Not a sign-off blocker for any
other milestone.

## Status and disposition

The design below is **accepted as an accurate account of what this codebase
can and cannot support**. It is not being implemented now, and no date is
attached to when it will be.

**Why deferred.** M10 has not shipped. Nothing is packageable, and nothing is
deployed. An operator-facing administrative interface designed with zero
operational feedback — and then frozen under spec §19's compatibility policy —
is a bad trade. The signal that would justify building it is concrete: a real
user reporting that they could not turn up sampling during an incident. That
signal cannot exist yet, because there are no users in production to produce
it.

**What it would cost, stated plainly.** This would be the project's first
inbound socket, a fourth thread, a hand-written parser on attacker-adjacent
input, a signal handler, a permanent threat model, and an administrative
surface frozen under the compatibility policy. That is a lot of new
surface area for a project whose entire pitch is a small, auditable dependency
closure aimed at embedded, edge, CNF, and air-gapped deployments — which are
precisely the environments least likely to permit a listening socket at all.

**What it would buy, not inflated.** After §2's tiering, the user-visible
capability is **four knobs adjustable without a restart**: batch parameters,
metric interval, sampler ratio, and log level. That is genuinely useful during
incident response. It is also all of it.

**Revisit trigger.** Real deployment feedback after v1.0 asking for runtime
reconfiguration. Not a date, and not a milestone slot.

**On the spec and roadmap.** `microtel-spec.md` and `microtel-roadmap.md` are
deliberately **not** amended to match this document. §3 (wire format), §5
(client language), and §9 (fourth thread role) are design-doc decisions whose
ICPs were never filed; with implementation deferred, those ICPs stay unfiled
and the spec keeps saying what it has always said — a Go `microtelctl`,
length-prefixed JSON, three thread roles. **The divergence between this
document and the spec is therefore deliberate and unresolved, not an
oversight.** Anyone reconciling the two later should start by filing the three
ICPs, not by editing the spec to match this file.

This document settles the decisions M15 (control plane + hot reload) cannot be
built without, the same way `docs/metrics-design.md` preceded M12 and
`docs/logs-design.md` preceded M14. Spec §18.1 defers the control plane's
*"scope and threat model"* to "v1.1 design", and `docs/configuration.md:228`
forward-references *"the v1.1 control-plane design doc when written"* — this is
that document.

Every code claim below was verified against `master` at the time of writing;
§0 lists the places where the **existing docs are wrong** and must not be used
as ground truth.

## Scope

In scope for M15 (this doc decides all of these):

- A Unix-domain-socket server inside the microtel process.
- A wire format and framing for that socket.
- Runtime-mutable settings: which ones, and what each actually costs.
- A client (`microtelctl`): single-shot and REPL.
- `SIGHUP`-driven config reload.
- Multi-profile within one process.
- The threat model for all of the above.

Explicitly **out of scope** for M15 (anti-goals):

- Any network-facing (TCP) control surface. UDS only, permanently.
- Remote administration, clustering, or a fleet control protocol.
- Reload of endpoint, protocol, TLS material, `service.name`, or resource
  attributes. `microtel-roadmap.md:85` names these as explicitly **not**
  hot-reloadable in v1.1, and §2 shows the architecture already enforces that
  for free.

## Sign-off checklist

Each item has a proposed **Decision** below. Reviewer approves by checking
every box (and editing any decision they want changed first).

- [x] §0 LOCKED means "needs an ICP to change", never "verified true"
- [x] §1 Design-only until M10; prerequisites split out to their own issues
- [x] §2 Four tiers, not one feature; Tier 4 deferred
- [x] §3 Asymmetric wire: token requests, real-JSON responses (**ICP**)
- [x] §4 Threat model; socket path required, no default; `SIGPIPE` fix
- [x] §5 `microtelctl` in C++, not Go (**ICP**)
- [x] §6 Command surface
- [x] §7 Multi-profile — reloadable-tier only, load-time validation
- [x] §8 `SIGHUP`; `pthread_atfork` split out to its own issue
- [x] §9 Threading — fourth thread role needs an **ICP**
- [x] §10 Testing/fuzzing; `corpus-check.sh` split out to its own issue

---

## §0 The LOCKED marker does not mean what it appears to mean

Three normative documents describe code that is not there. Listing them first
is not throat-clearing: the most important one is marked **LOCKED —
single-source-of-truth**, and understanding *how it got that way* changes how
much any locked document should be trusted as design input.

1. **`docs/threading-model.md` §5.3** (line 167), marked **(LOCKED —
   single-source-of-truth)**, declares that *"A single
   `std::atomic<ShutdownState> m_state` on the `Provider` is the ground truth
   for shutdown progress."*
   **No such member exists.** `SdkProvider` (`src/sdk/sdk_provider.hpp:143-191`)
   has zero atomics; teardown ordering is achieved purely by member
   declaration order. §9 below already routes around it.
2. **`docs/threading-model.md` §2** states v1 has *"exactly three thread
   roles"*. The code has **seven**: I/O, BSP worker, log-processor worker,
   metric reader, and three exporter workers (trace/metric/log).
3. **`pthread_atfork` is normative in two documents** —
   `docs/threading-model.md` §7 (LOCKED) and
   `docs/sequences/fork-survival.md` — and **registered nowhere**. A repo-wide
   grep for `pthread_atfork`, `signal(`, and `sigaction` returns zero hits.

### How a LOCKED doc came to assert a member that never existed

`ShutdownState` has **never existed in any commit in this repository's
history** — `git log -S ShutdownState -- src/ include/` is empty. The claim,
already marked LOCKED, entered in `dd94e85`: *"M0: design docs,
public/internal headers, ICPs 0001 and 0002"*.

That is the whole explanation, and it generalizes. `threading-model.md`,
`interfaces.md`, and `memory-model.md` were **all added by that same M0
commit**, and M0 is by definition the phase with no source code (CLAUDE.md
rule 1). Every LOCKED marker in this project was therefore applied to *design
intent*, at a moment when there was nothing to verify it against — and nothing
since re-checks that the code still matches. **LOCKED means "don't change this
without an ICP." It has never meant "this is true."** Those two readings are
easy to conflate, and an earlier draft of this document did exactly that.

Scale: **58 LOCKED markers** across the design docs — `interfaces.md` 19,
`threading-model.md` 12, `error-model.md` 10, `memory-model.md` 9,
`grpc-wire-protocol.md` 6, plus singles. Four are now known false: the three
above, plus §2.3's I/O-thread creation site (tracked separately). Nobody has
audited the other 54, and the hit rate among the handful this milestone
happened to touch is not reassuring.

**Decision.** M15 cites no locked document without checking it against the
code first — which is what §2, §9, and §10 do throughout. Beyond M15, the
**58-marker audit deserves its own work item** (#134): the failure is not that
one document drifted, it is that the mechanism meant to prevent drift never had
a verification step. Item 3 is additionally a hard prerequisite for M15's own
socket (§8); it is **not** deferred with the rest of M15 and has its own issue
(#132) — see the Increment plan.

---

## §1 Prerequisite: M15's dependency has not shipped

Spec §13 lists M15's "Depends on" as **M10 — v1.0 traces release**. M10 has
not happened: the newest tag is `v0.2.0-m2`, and there are **zero `install()`
rules in the build** — nothing is packageable today. That matters directly,
because `microtel-roadmap.md:91` specifies `microtelctl` ships as
`.deb`/`.rpm`/`.tar.gz` "separate from the core runtime package", and that
packaging machinery does not exist for *any* target (issue #19).

A control plane is an operator-facing surface. Its value is administering a
deployed process; nothing is deployed. Building it now means designing an
administrative interface with zero operational feedback, then supporting it
under the §19 compatibility policy.

**Decision.** M15 proceeds **design-only**: this document is the deliverable,
and implementation waits on either (a) M10 shipping, or (b) an explicit
reviewer decision to run ahead of the dependency, recorded here.

The justification is narrow, and worth stating precisely because an earlier
draft over-reached. It is **not** that §3 and §5 inform M10's scope — they do
not. The control plane is off by default and not in the v1.0 cut, so neither
the wire format nor the client language touches what M10 ships. The actual
reason is the one above: an administrative interface designed with zero
operational feedback, then frozen under §19's compatibility policy, is a bad
trade that gets worse the longer it is supported.

Note this defers the socket work only. The two prerequisites that were
originally staged inside this milestone have no dependency on M15 and are now
tracked as their own issues — see "Status and disposition" and the Increment
plan.

---

## §2 What is actually reloadable

"Hot-reloadable settings (sampler ratio, batch sizes, internal log level)"
reads as one feature. It is **four, with wildly different costs**, because
the settings differ in how they are synchronized today. This is the finding
that should shape M15's scope.

### Tier 1 — free: settings already read under a lock

`BatchSpanProcessor` holds `BatchOptions m_opts` as a **non-const** member
(`src/sdk/batch_span_processor.hpp:85`), never captured into a local or a
thread lambda, and **every read is already inside the processor's own mutex**:

- `src/sdk/batch_span_processor.cpp:54,56,66` — `OnEnd`, under `scoped_lock{m_mu}`
- `src/sdk/batch_span_processor.cpp:118,122,126` — `WaitAndCollect`, under `unique_lock{m_mu}`

The worker re-reads all four knobs on **every wake**. A writer that takes
`m_mu`, assigns, and `notify_all()`s is correct with no structural change.
`BatchLogRecordProcessor` is identical in shape
(`batch_log_record_processor.hpp:84`; reads at `.cpp:49,51,61,113,117,121`).
`PeriodicExportingMetricReader::m_interval`
(`periodic_exporting_metric_reader.hpp:90`) is likewise non-const, read once
per loop at `.cpp:76` under `m_mu`, and already has an early-wake flag
(`m_wake`) to reschedule immediately — though `m_mu` currently documents
itself as guarding only `m_wake`, so its invariant widens.

### Tier 2 — cheap: sampler **ratio**, via an atomic threshold

The roadmap says *"sampler **ratio**"* (`microtel-roadmap.md:85`), not
"sampler". Honoured literally, that is cheap.

`TraceIdRatioSampler` (`src/sdk/sampler_factories.cpp:138`) stores
`double m_ratio` (:204) and `std::uint64_t m_threshold` (:206), and the hot
path reads only the threshold (:174):

```cpp
return (low < m_threshold) ? internal::SamplingDecision::RecordAndSample : ...
```

Making `m_threshold` a `std::atomic<std::uint64_t>` (plus the `m_always_sample`
short-circuit) makes the ratio adjustable with **one relaxed atomic load** on
the hot path — no pointer swap, no lifetime problem, no allocation, and the
sampler object never changes identity. That respects `ISampler`'s LOCKED
no-allocation-on-hot-path rule (`memory-model.md` §8.1).

Two implementation notes: `m_description` is a `std::string` built from the
ratio at construction (:145) and would go stale — it must either be left
describing the *configured* ratio or made immutable-by-design, never mutated
under a hot-path reader. And the knob only exists when a ratio sampler is
actually in play; with `AlwaysOn`/`AlwaysOff` configured there is no ratio,
and the control plane must say so rather than silently no-op (§6).

### Tier 3 — free, because it does not exist: log level

There is **no log level filtering anywhere**. `LogImpl`
(`src/common/log_sink.cpp:96-129`) performs no level check — the `level`
parameter only picks a string tag — and it has **zero production call sites**.
`docs/configuration.md:164-174` documents `logging.level` and
`MICROTEL_LOG_LEVEL`; neither is implemented, and `config::Config` has no
logging member at all.

So M15 would be *introducing* the filter, not adjusting one. That is good
news for concurrency — a `std::atomic<LogLevel>` is race-free by construction
— but it means the deliverable is "build a log-level filter **and** wire up
the emission sites", which is more work than "expose an existing knob". The
only genuinely runtime-mutable thing in the SDK today is the `LogSink`
callback itself (`src/common/log_sink.cpp:66-78`, swapped under a mutex).

### Tier 4 — deferred: swapping the sampler *object*

Replacing the sampler with a different *kind* of sampler is a different
problem. `SdkTracer` caches a **raw borrowed pointer**
(`internal::ISampler* m_sampler`, `src/sdk/sdk_tracer.hpp:56`) handed over at
`GetTracer` time (`sdk_provider.cpp:80-88`), and dereferences it on the hot
path with **no synchronization of any kind** (`src/sdk/sdk_tracer.cpp:99`).
`SdkProvider` owns the sampler as a move-only `unique_ptr`
(`include/microtel/sampler.hpp:27`, `sdk_provider.hpp:177`).

A live swap is therefore both a data race on the pointer and a use-after-free
on the pointee, for every `SdkTracer` the application still holds. Fixing it
needs an atomic pointer *plus* deferred reclamation (RCU/hazard pointers), or
an `atomic<shared_ptr>` load that puts a refcount round-trip on `StartSpan` —
the hot path the project's performance claims rest on.
`docs/interfaces.md:480` already reserves the seam (*"Replaceable in v1.1's
hot-reload path"*) without implementing it.

**Decision.** M15 ships **Tiers 1–3**: batch/queue/delay/drop-policy, metric
interval, sampler **ratio** via atomic threshold, and a new atomic log-level
filter. **Tier 4 — swapping the sampler object — is deferred out of M15** and,
if wanted, gets its own ICP pricing the hot-path cost against the M7 benchmark
numbers. This satisfies the roadmap's literal wording while declining the
thing it did not ask for.

### Why "not reloadable" is free

`config::Config` is a **`Build()`-local `const`** (`sdk_builder.cpp:556`) that
is shredded into member copies and destroyed when `Build()` returns
(`:593-612`); `Build()` is single-shot. There is no `Config` to reload. What
survives on `SdkProvider` is a handful of members; endpoint, TLS, protocol,
`service.name`, and resource attributes are baked into codecs and
`ConnectOptions` and are **unreachable afterwards** — which is exactly the
roadmap's non-reloadable list. The architecture already enforces the policy
at no cost, and the design should say so rather than re-deriving it.

### Alternative: expose the reloadable knobs as ordinary public setters

Tiers 1–3 do not actually require a control plane. They could be exposed as
thread-safe public setters on `Provider` — `SetBatchOptions`,
`SetMetricInterval`, `SetSamplerRatio`, `SetLogLevel` — with **no socket, no
parser, no fourth thread, no signal handling, and no threat model**. The host
application calls them from whatever administrative interface it already has:
its own HTTP admin endpoint, its own config-reload path, its own management
console.

That delivers most of the user-visible value at a small fraction of the cost,
and it moves the socket, parser, and signal-handling surface **into the host**,
where it usually already exists and is already secured — rather than making
every microtel consumer inherit a listening socket they may not want (see the
disposition note above on embedded and air-gapped deployments).

What it does **not** give, stated honestly: **no out-of-process
administration.** An operator cannot change anything unless the host
application has already built its own control surface and wired these setters
into it. For a host with no such surface, the answer under this alternative is
"restart with new config" — which is exactly the gap the control plane exists
to close. It is a smaller feature, not the same feature delivered more cheaply.

**Not decided here.** Recorded as the **leading candidate to evaluate first**
if M15 is ever revisited, ahead of the socket design in §3–§9.

---

## §3 Wire format: JSON needs a parser microtel does not have

Spec §18.1 specifies "length-prefixed JSON wire". **There is no JSON parser in
the dependency closure.** CLAUDE.md rule 12 fixes that closure at nghttp2,
OpenSSL, upb, zlib, and optional spdlog; `third_party/` holds `tl-expected`,
`upb`, `utf8_range`, and toml++. upb's `upb/json/` is explicitly **not
vendored** (`third_party/upb/README.md`: *"v1 only emits binary protobuf, no
text or JSON"*), and is protobuf-shaped regardless.

Rule 12 makes every path an ICP:

| Option | Cost |
|---|---|
| **A. Vendor a header-only JSON library** | New vendored dependency. Grows the closure microtel exists to keep small — the closure claim is the project's whole pitch. Most such libraries are exception- and allocation-heavy. |
| **B. Hand-roll a restricted JSON parser** | No new dependency, but a **parser on an inbound socket** — the exact shape CVEs are made of. Needs its own fuzz harness and a hard pre-parse size cap. |
| **C. Non-JSON framing** both directions | Safest, but gives up `jq`-able output, which is most of why an operator wants JSON at all. |

### The asymmetry these options miss

Requests and responses have **opposite trust properties**, and the table above
prices them as if they were the same problem. Request bytes are
attacker-controlled; response bytes are ours. **Parsing is the dangerous half;
emitting is trivial and safe.** A symmetric design pays the parser cost twice
over for a benefit that only ever applies to one direction.

An earlier draft of this document recommended Option B with a subset so
restricted — string keys to scalar values, no nesting, no arrays, no `\u` —
that it was *key=value wearing JSON punctuation*. That is the worst position
available: it pays the full price of hand-written parsing on an inbound
socket, delivers none of JSON's expressiveness, and does not even earn the
`jq` compatibility that was used to reject Option C, since nothing in that
grammar round-trips meaningfully through real JSON tooling.

**Decision — split the wire by direction.**

- **Requests: a fixed, non-recursive token grammar.** `set batch.max_queue_size
  2048` — verb, dotted key, scalar. Tokenize on whitespace after a length cap;
  there is no nesting, no quoting problem, and no recursive descent. The
  "parser" is a field split, which is about as much attack surface as an
  inbound socket can have while still doing something.
- **Responses: real, unrestricted JSON**, produced by a small serializer.
  Emitting is a formatting problem, not a parsing one: no untrusted input
  reaches it, and correctness is testable by round-tripping through a real
  JSON parser in the test suite. Operators get genuine `jq` on the thing they
  actually pipe to `jq`.

This also resolves §5's client cleanly. If `microtelctl` wants to *parse*
those responses (for the REPL's table rendering, say), it may **vendor a JSON
library freely** — it is a separate binary, outside the runtime closure claim
rule 12 governs, and its only untrusted input is the server's own output. The
dangerous parser, if one is wanted at all, lands where it costs nothing.

**Structural constraint from §10:** the request tokenizer must still be a pure
`ParseControlRequest(std::string_view) -> Expected<Request, Error>` with all
socket I/O strictly outside it — mirroring `config::ParseTomlString`
(`src/common/config/toml_loader.hpp:28`), which is what makes `toml_fuzz` a
six-line harness. It remains fuzzed (§10); the point is that there is now very
little left to find.

---

## §4 Threat model

Spec §19 lists the threat model as *"initial: enumerated for the v1.1 control
plane"* — this is the first time it is written down, and
`microtel-roadmap.md:93` makes *"control plane has a documented threat model"*
a ship gate.

This is the project's **first inbound socket surface**. A repo-wide grep for
`listen(`, `bind(`, `accept(`, `AF_UNIX`, and `sockaddr_un` across `src/`,
`include/`, and `tools/` returns **nothing** — the only hit is a
`SOCK_STREAM` hint for the outbound `getaddrinfo` in the HTTP/2 transport.

**Asset.** The ability to mutate a live process's telemetry configuration and
read back its resolved config and health.

| Threat | Mitigation |
|---|---|
| Any local user connects and reconfigures the process | Socket mode `0600` owned by the process UID, **and** an `SO_PEERCRED` check rejecting any UID that is neither the process UID nor root. Both, not either. |
| Socket path predictable or squatted before bind | **The path is required configuration with no default** — see below. Bind to a temp name and `rename()` into place; refuse to unlink an existing path that is not a socket we own. |
| Secret disclosure via read-back | §12.6 redaction applies unconditionally to every response. There is **no control-plane equivalent of `--show-secrets`** — secrets are never readable over the socket at any privilege level. |
| Malformed frame → parser exploit | §3's restricted grammar, pre-parse size cap, dedicated fuzz harness (§10). |
| Resource exhaustion (connection flood, slowloris) | Single-threaded accept loop, max 4 concurrent connections, per-connection idle timeout, bounded read buffer. Reject rather than queue. |
| **`SIGPIPE` kills the process** | **Live hazard, not hypothetical.** There is no `MSG_NOSIGNAL` and no `SIG_IGN` anywhere in the repo. A UDS write to a client that has hung up — trivially triggered by Ctrl-C'ing `microtelctl` — would terminate the host application. Every control-plane `send` must use `MSG_NOSIGNAL`. Process-wide `SIG_IGN` is the alternative but is a library imposing policy on its host, which needs its own ICP. |
| Control plane as persistence/escalation foothold | No command may execute a path, load a library, or write a file. §6 is a closed enumeration — no passthrough, no eval, no config-file path argument. |

### The socket path has no safe default

An earlier draft defaulted to `$XDG_RUNTIME_DIR/microtel-<pid>.sock`. That
variable is **frequently unset** — system services, containers, and any host
process that did not originate from a user login session. Since the doc also
(correctly) forbids `/tmp`, a default-seeking design is left with no safe
fallback, and every candidate fallback reintroduces the squatting question in
a new location.

microtel is a library embedded in an arbitrary host process; it does not know
where that host's runtime directory is, and guessing is precisely the wrong
move for a security-relevant path.

**Decision.** The socket path is **required configuration with no default**.
Enabling the control plane means naming the path; there is no
"enabled-but-unconfigured" state to reason about. This composes naturally with
off-by-default (§9) and removes an entire class of path-discovery and
squatting questions rather than answering them one at a time.

**Decision (§4 overall).** As tabled. Two properties are invariants, not
defaults: **UDS-only forever** (no TCP option, not even opt-in — off-box
reachability voids this entire model), and **no secret is ever readable over
the socket**. `MSG_NOSIGNAL` on every write is a correctness requirement, not
a hardening nicety.

---

## §5 `microtelctl`: Go or C++

The spec and roadmap say Go in **four places** — `microtel-spec.md:659`,
`:923`, `microtel-roadmap.md:85`, and `:91` — and `:91` further specifies it
ships as a standalone binary in `.deb`/`.rpm`/`.tar.gz` *"separate from the
core runtime package"*. Reversing this is a real decision, not a detail.

**Cost of Go:** a second toolchain in a C++ repository, installed in **all
four CI jobs that build** (`tidy-check`, `compile`, `sanitizers`, `coverage`
— per CLAUDE.md's own warning), with its own CODEOWNERS entry, lint/format
gates, `go.mod` supply chain, and per-platform release artifacts. None of it
shared with anything else the project builds.

**Cost of C++:** loses Go's easy static cross-compilation and REPL ecosystem.

**What C++ buys:** `tools/preflight/` already demonstrates the pattern — a
`microtel_preflight_lib` static library plus a 9-line `main.cpp` trampoline
(`tools/preflight/main.cpp:6-9`), with an injectable-stream overload
(`preflight.hpp:28-31`) that makes it unit-testable under the existing gtest
setup. It reuses the entire existing build/test/lint/sanitizer pipeline at
zero marginal infrastructure cost, and — the real argument — lets the client
**share §3's frame codec with the server**, so the two cannot drift.

One caveat against over-claiming the reuse: `ParseArgs`
(`tools/preflight/preflight.cpp:40-80`) is a single-flag prefix match with no
subcommands and no `--help`. A subcommand-plus-REPL CLI reuses the *structure*
but none of the *parsing*, and no arg-parsing library is vendored.

**Decision — departs from the spec.** Write `microtelctl` in **C++**,
following the `tools/preflight/` pattern, and amend the four spec/roadmap
sites accordingly. Because this reverses a documented decision (including the
separate-package packaging claim), it needs an **ICP**, not just this
document's sign-off. If the reviewer prefers Go, that is defensible — but it
should be chosen knowing it is a four-CI-job toolchain addition for a program
that opens a socket and prints replies.

---

## §6 Command surface

A closed enumeration (§4: no passthrough). One frame in, one frame out.

| Command | Effect | Tier (§2) |
|---|---|---|
| `status` | Resolved config (redacted) + `GetExporterHealth()` | read-only |
| `get <key>` | One resolved setting, redacted | read-only |
| `set batch.<field> <value>` | Batch/queue/delay/drop-policy | 1 |
| `set metric.interval <value>` | Metric reader interval | 1 |
| `set sampler.ratio <value>` | Atomic threshold (§2 Tier 2) | 2 |
| `set log.level <value>` | Atomic level filter (§2 Tier 3) | 3 |
| `reload` | Re-read config file; apply reloadable deltas, **report the rest as requiring restart** | 1–3 |
| `profile list` / `profile switch <name>` | §7 | 1–3 |
| `flush [timeout]` | `ForceFlush` | read-only-ish |

Two rules matter more than the table:

- `set sampler.ratio` **must fail loudly** when the configured sampler is not
  a ratio sampler, rather than accepting the value and doing nothing.
- `reload` **must report** non-reloadable deltas (endpoint, TLS, sampler
  *kind*, resource) as "requires restart". Silently ignoring them is exactly
  the failure mode ICP 0017 was written to fix; this document must not
  reintroduce the pattern one milestone later.

**Decision.** As tabled. Anything not in the table requires amending this doc.

---

## §7 Multi-profile

"Multi-profile within one process" (spec §12.8, §18.1) means several named
config profiles with one active at a time.

This touches a **LOCKED** constraint: `docs/threading-model.md:44` states
*"v1 always has exactly one worker per process. Multi-profile (multiple
`Provider` instances per process) is a v1.1 feature; v1 supports a single
global `Provider` only."* Lifting it is in-scope for v1.1 by that sentence's
own terms, but it is a locked-doc amendment and therefore ICP-worthy.

Given §2, a profile switch can only apply Tiers 1–3. A profile differing in
endpoint, TLS, auth, or sampler *kind* cannot be switched into without
transport teardown and re-establishment — and reconnect is unsolved
(`ConnectionState::Reconnecting` is dead code; ICP 0017 fixed only the
*initial* connect).

**Decision.** M15 ships profiles as a **reloadable-tier-only** overlay.
Profiles may differ in batch/timeout/interval/ratio/log-level settings; a
profile differing in endpoint/TLS/auth/sampler-kind is a **validation error at
load time**, not a runtime surprise. Full profile switching depends on
transport re-establishment and defers to whichever milestone solves reconnect.

---

## §8 `SIGHUP`, and an unimplemented prerequisite

Spec §923 scopes "config reload via SIGHUP" into the control plane.

**microtel installs no signal handlers today** — zero hits for `signal(`,
`sigaction`, `SIGHUP`, or `pthread_atfork` across the whole tree. M15 would be
the first. Consequences:

- **Async-signal-safety.** The handler may not lock, allocate, or read a file.
  Standard shape: `write()` one byte to a self-pipe/`eventfd`; the
  control-plane thread does the work. `src/common/raii/unique_fd.hpp` already
  provides the RAII fd wrapper. Note the handler **must not** call `LogImpl` —
  it copies a `std::function` under a `std::mutex` and is not
  async-signal-safe.
- **`pthread_atfork` is a prerequisite, not an interaction.** §0 item 3: the
  fork-safety behaviour is normative in `docs/threading-model.md` §7 (LOCKED)
  and `docs/sequences/fork-survival.md`, and **is not implemented**. Without
  it, a forked child inherits the control-plane thread's listening socket and
  UDS path with no cleanup — two processes bound to one path. M15 must
  implement the atfork child handler (close the inherited fd, do **not**
  unlink the path — the parent still owns it), which means M15 partly
  discharges M5's outstanding promise.

**Decision.** Self-pipe handler, real work on the control-plane thread. Signal
installation is **opt-in, never automatic** — a library that installs a
`SIGHUP` handler behind its host's back is a bug, and many applications
already use `SIGHUP` themselves. The atfork handler is a blocking prerequisite
and belongs in the increment plan before the socket lands.

---

## §9 Threading & lifecycle

Adds **one thread**: the control-plane thread, owning the listening socket and
all connections, blocking in `epoll_wait` (including the §8 self-pipe). It
never touches SDK internals directly — it applies mutations through the same
lock-protected setters an application could call.

Two constraints from `docs/threading-model.md`:

- §2 frames v1 as *"exactly three thread roles"* (already inaccurate — §0 item
  2 — but it is the written contract). A fourth *role* that takes
  `BatchSpanProcessor`'s mutex from outside the existing roles needs an
  **ICP** to amend that section, even though it does not violate the lock-order
  rules as written (§4, LOCKED: at most one non-leaf lock held at a time —
  the control-plane thread holds exactly one).
- `IReactor`'s threading tag is **LOCKED**: *"All methods other than `Wake`
  are I/O-thread-only"* (`include/microtel/internal/reactor.hpp:51`). The
  control plane therefore needs its **own** `EpollReactor` instance on its own
  thread and **cannot** register its listening fd on the transport's reactor.
  `EpollReactor` currently lives in `src/transport/`, so reusing it means
  moving or re-exporting it.

`UniqueFd` covers the listening socket's *fd* but not its *lifecycle* — no
`unlink()` on close, no path ownership, no umask/permission handling. A
`UniqueUnixListener` (fd + path + unlink-on-destroy) is new work.

Lifecycle: started only when explicitly enabled in config (**off by default** —
a telemetry library must not open a socket unless asked); stopped in
`Provider::Shutdown` before the transport closes, socket path unlinked on
clean shutdown, thread joinable within `Shutdown`'s timeout (CLAUDE.md rule
15). Because teardown ordering is by member declaration order
(`sdk_provider.hpp:143-174`) and not by the `m_state` the docs claim (§0),
the control-plane member must be declared so it is destroyed **first**.

**Decision.** As above: one thread, own reactor, off by default, no direct
SDK-internal access, ICP for the threading-model amendment.

---

## §10 Testing, fuzzing, and CI

- **Unit:** frame codec (round-trip, truncation, oversize rejection, every
  malformed-grammar case), command dispatch, each setter.
- **Fuzz:** `control_plane_frame_fuzz.cpp` under `tests/fuzz/`, following the
  established `microtel_add_fuzz_target` pattern
  (`tests/fuzz/CMakeLists.txt:26-50`) and the six-line `toml_fuzz.cpp`
  template. **Ship-blocker, not follow-up** —
  `microtel-roadmap.md:93` gates v1.1 on *"hot reload is fuzz-tested"*.
- **Integration:** real socket, real client, real UDS — including the §4
  rejection cases (wrong UID via `SO_PEERCRED`, oversize frame, connection
  cap, and a client that disconnects mid-write to prove the `SIGPIPE` fix).
- **TSAN:** mandatory. New thread mutating state other threads read; per
  CLAUDE.md's threading-discipline note, TSAN is the gate. Tier 2's atomic
  threshold specifically needs it.
- **Coverage:** existing diff-coverage thresholds apply unchanged.

**Gap to close first:** `tests/fuzz/CMakeLists.txt:9` and
`tests/fuzz/README.md:38-43` both assert that crash-corpus replay is a hard PR
gate via `ci/scripts/corpus-check.sh`. **That script does not exist** —
`ci/scripts/` contains only `baseline-update.sh`, `coverage.sh`,
`format-check.sh`, `regen-protos.sh`, `symbol-scan.sh`, and `tidy-check.sh`.
The gate M15's ship criterion depends on is currently vapor.

**Decision.** As above, with the fuzz harness a ship-blocker and
`corpus-check.sh` written before M15 claims to satisfy its own gate.

---

## Resolved at review

All six open items were answered in review; recorded here so the decisions
live with the design rather than in a thread.

1. **§1 — stop at this document** until M10 ships, on the operational-feedback
   grounds only (the "informs M10's scope" argument was withdrawn as wrong).
   Defers L1–L8; **the two prerequisites are decoupled into their own
   issues.**
2. **§2 — tiering accepted**, including Tier 4's deferral: a refcount
   round-trip on `StartSpan` for a feature the roadmap never asked for is
   backwards given what the project's pitch rests on.
3. **§3 — asymmetric wire**, overturning the original Option B. Requests in a
   fixed token grammar; responses in real JSON. See §3.
4. **§5 — C++ client accepted.** Shared framing between client and server is
   worth more here than Go's cross-compilation, and the four-CI-job toolchain
   cost is not close for a program that opens a socket and prints replies.
5. **§8 — confirmed**, and pulled out of M15 entirely into its own issue.
6. **Spec/roadmap amendments land *after* the ICPs**, not before, so the ICP
   is the record and the spec follows it. Amending both in parallel risks a
   third version of the same decision.

Also accepted: **§7** (validation error at load time, not runtime surprise)
and **§6's two rules** — loud failure on `set sampler.ratio` against a
non-ratio sampler is the ICP 0017 pattern deliberately not repeating.

## Increment plan

Retained as the implementation record if M15 is revisited. Everything below is
**deferred** — see "Status and disposition" at the top of this document.

**L0 has been removed from this plan entirely.** Its two items were never
really M15 work: both are outstanding debts asserted elsewhere in the
repository, and parking them behind a milestone is how they were orphaned in
the first place. They now have their own GitHub issues and are **not gated on
M15**:

- **`pthread_atfork` handlers** (#132) — normative in
  `docs/threading-model.md` §7 (LOCKED) and `docs/sequences/fork-survival.md`,
  unimplemented since M5.
- **`ci/scripts/corpus-check.sh`** (#133) — asserted as a hard PR gate by
  `tests/fuzz/CMakeLists.txt:9` and `tests/fuzz/README.md:38-43`, does not
  exist.

| Increment | Scope | State |
|---|---|---|
| L1 | ICPs: §3 wire, §5 client language, §9 threading-model amendment, §12.8 anti-goal lift | deferred |
| L2 | Request tokenizer + response serializer + fuzz harness. No socket. | deferred |
| L3 | `UniqueUnixListener`, UDS server, §4 controls, required-path config | deferred |
| L4 | Tier-1 setters (batch/interval) + `status`/`get`/`set`/`flush` dispatch | deferred |
| L5 | Tier-2 atomic ratio threshold + Tier-3 log-level filter and emission sites | deferred |
| L6 | `microtelctl` single-shot, sharing L2's framing | deferred |
| L7 | REPL, `reload` + `SIGHUP` (§8), profiles (§7) | deferred |
| L8 | Integration tests over a real socket; TSAN; `docs/threading-model.md` update | deferred |

Separately tracked, out of M15's scope but surfaced by it: the **58-marker
LOCKED audit** (§0).

## References

- `microtel-spec.md` §12.8, §18.1, §19, §13; `microtel-roadmap.md:85,91,93`
- `docs/metrics-design.md`, `docs/logs-design.md` — the precedent this follows
- `docs/configuration.md:228` — forward-reference to this document
- `docs/icps/0017-lazy-transport-connect.md` — the silent-no-op failure mode §6 avoids repeating
- §2 Tier 1: `batch_span_processor.hpp:85`, `.cpp:54,118`; `periodic_exporting_metric_reader.cpp:76`
- §2 Tier 2: `sampler_factories.cpp:138,174,204,206`
- §2 Tier 3: `src/common/log_sink.cpp:96-129`; `docs/configuration.md:164-174`
- §2 Tier 4: `sdk_tracer.hpp:56`, `sdk_tracer.cpp:99`, `sampler.hpp:27`, `docs/interfaces.md:480`
- §2 frozen config: `sdk_builder.cpp:556,593-612`
- §9: `include/microtel/internal/reactor.hpp:51`, `src/common/raii/unique_fd.hpp`
- §10: `tests/fuzz/CMakeLists.txt:26-50`, `src/common/config/toml_loader.hpp:28`
