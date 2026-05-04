# Sequence: Fork Survival

**Status:** M0 deliverable. Normative timeline for `fork()` in a process running microtel.
**See also:** `threading-model.md` §7, `microtel-spec.md` §5.3.

---

## Participants

- **Parent forking thread** — calls `fork()`.
- **Other parent threads** — exporter worker, I/O thread; not present in the child.
- **Child process** — only the forking thread survives.
- **`pthread_atfork` handlers** — registered at first `Provider::Build`.
- **`m_state`** — atomic shutdown state, replicated by `fork()` into the child.

---

## Happy path

```
PARENT
  Forking thread       Worker thread       I/O thread       atfork handlers
       |                     |                  |                   |
       | (active span work)                                         |
       |                                                             |
       | fork()                                                      |
       |--> kernel: prepare handler runs (parent context)            |
       |     |                                                      |
       |     | record diagnostic("fork observed")                    |
       |     | flush internal log if synchronous                     |
       |     | (no locks acquired)                                   |
       |     |                                                      |
       |--> kernel: parent handler runs after fork                   |
       |     |     - parent state continues unchanged                |
       |     |     - worker and I/O threads still running            |
       |--> child process spawned                                    |

CHILD
  Forking thread (the only thread) -- the child handler runs here
       |
       | child handler:
       |   for each live Provider:
       |     CAS m_state from any value to Closed
       |   replace global LogSink with stderr fallback
       |   (avoid relying on external logger that may have shared fds)
       |
       | (application code in child resumes)
       |
       | API calls observe m_state == Closed -> drop with post_shutdown
       |
       | option A: child re-initialises microtel
       |   SdkBuilder().With...().Build()
       |   -> new Provider, new threads, new sockets
       |
       | option B: child does not re-init
       |   -> all microtel API calls drop silently
       |
       | option C: child execve()
       |   -> microtel state irrelevant after exec replaces the image
```

---

## Annotations

1. **`fork()` produces a child with only one thread alive: the forking thread.** All other threads (exporter worker, I/O thread) are missing in the child. Any locks they were holding remain "held" in the child's memory but with no thread to release them. Any half-written socket buffers are inherited but not safe to use — the parent and child would interfere with each other's TCP state.
2. **The atfork prepare handler in the parent records a diagnostic.** It does not acquire microtel locks because doing so risks deadlocking the fork itself. (`pthread_atfork(3)` warns extensively about this.) The diagnostic is emitted via the configured log sink synchronously where possible.
3. **The child handler immediately CAS-flips every live `Provider` to `Closed`.** This is the entire mitigation — no attempt is made to "fix up" the orphaned threads, sockets, or nghttp2 sessions. The child observes a fully-shut-down microtel from its first instruction.
4. **The child must re-initialise microtel explicitly** if it wants to send telemetry. Calling `SdkBuilder().Build()` from the child constructs a fresh `Provider` with fresh threads, fresh sockets, and a fresh nghttp2 session. The parent's I/O state is **not shared.** (LOCKED.)
5. **No automatic re-initialisation in v1.** The application is responsible for the policy. Some applications fork-and-exec (no microtel needed in the brief gap); others fork to spawn workers (each worker re-inits). v1 does not pick a default.

---

## What about API calls after fork without re-init?

The child's API calls observe `m_state == Closed` immediately. Each call drops with `post_shutdown`:

- `Tracer::StartSpan` returns a no-op `Span` (the unsampled-fast-path object).
- `Span::End` is a no-op.
- `Provider::ForceFlush`, `Provider::Shutdown` return `Status::AlreadyShutDown`.
- `Provider::GetExporterHealth` returns the **child's snapshot** — counters that were valid at the moment of fork (since `m_state` flipped in the atfork handler, the snapshot reflects post-shutdown state).

The application sees no errors. Telemetry is silently dropped. This matches the `noexcept` guarantee on the API.

---

## Variant — `vfork()` or `posix_spawn()`

`vfork()` semantics permit only `_exit` or `execve` in the child before the parent resumes. microtel's atfork handlers do not run during `vfork()` (handlers are only invoked for `fork()`). The child must `_exit`/`execve` immediately and microtel state in the child is irrelevant.

`posix_spawn()` is implemented in terms of `vfork()`+`execve()` on Linux; same handling.

If the application's `posix_spawn`/`vfork` flow ever does work between the spawn and the exec (some libcs do), that work must not call into microtel. Applications using `vfork` are expected to know this; no microtel mitigation is added in v1.

---

## Variant — daemonisation (double fork)

The `daemonize` pattern (fork → setsid → fork → exec or run main) ends with a fresh process. Each fork triggers the atfork handlers; in the second fork, the intermediate process has already-closed `Provider`s, so nothing escapes to the final daemon. The daemon must call `SdkBuilder::Build()` itself.

Common gotcha: applications that initialise microtel **before** daemonising will end up with a closed `Provider` in the daemon. Applications should daemonise first, then build microtel. The atfork handler ensures correctness even if they don't, but at the cost of telemetry until they re-init.

---

## Edge cases captured by tests

- `fork()` followed by re-init in the child — telemetry resumes, parent and child are isolated.
- `fork()` followed by API use without re-init — no crashes; counters reflect post-shutdown drops.
- `fork()` mid-export — parent's in-flight batch completes in the parent; child drops the inherited batch state.
- `fork()` from the I/O thread — undefined behaviour at the application level (you don't fork from a non-main thread on Linux without exec); not tested.
- Sequential `fork()` calls in the parent — each one runs the prepare and parent-handler; only handler invariants matter, both are idempotent.

These live in `tests/integration/fork/` (M3+).

---

## What v1 does **not** do

- No fork detection that automatically re-initialises microtel in the child. The application opts in.
- No special handling for `clone()` syscalls. Linux's `clone()` with `CLONE_THREAD` is a thread, not a fork; it does not invoke atfork handlers. `clone()` without `CLONE_THREAD` is a fork; atfork handlers do run.
- No FD-cloexec on microtel-owned sockets. The socket inherits across `fork`+`execve` by default. The atfork handler closing `m_state` is sufficient mitigation; if a hostile child binary inherits the FD, the parent's nghttp2 session continues to use it on its own thread, but the child cannot speak HTTP/2 on the inherited FD without reproducing the entire microtel state.

The full design here is "the child must re-init or not use microtel." v1 commits to that and nothing more.
