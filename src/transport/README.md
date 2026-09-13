# `src/transport/`

## Purpose

The HTTP/2 transport implementation: nghttp2 session, OpenSSL TLS,
epoll/kqueue I/O loop, per-stream in-flight state, reconnect with
backoff. The seam where an HTTP/3 transport could drop in for v1.5+
without changing `IWireCodec`.

## Owner

Track D — Transport. Foundational track (finishes before A/B/C/E
unblock).

## Implements

- `internal::ITransport` (declared in [`include/microtel/internal/transport.hpp`](../../include/microtel/internal/transport.hpp))
- `internal::IReactor`   (declared in [`include/microtel/internal/reactor.hpp`](../../include/microtel/internal/reactor.hpp))
- The connection state machine: `Disconnected` → `Connecting` →
  `Connected` → `Reconnecting` → `Closed` (per `provider.hpp`)
- The I/O thread loop (`docs/threading-model.md` §2.3)

## Depends on

- nghttp2 (system `libnghttp2.so`, ≥ 1.50 per spec §9.1)
- OpenSSL (system, ≥ 1.1.1 per spec §9.1)
- The RAII wrappers in [`src/common/raii/`](../common/raii/) — `Socket`,
  `SslCtx`, `SslSession`, `Nghttp2Session` (per ICP 0003 §3.1, `SslCtx`
  is per-`Transport`, not process-shared)

## Test entry points

- `tests/unit/transport/` — `ITransport` and `IReactor` against a fake
  reactor.
- [`tests/integration/transport/`](../../tests/integration/transport/) —
  runs against a small in-process nghttp2 server over loopback:
  - `http2_connect_test.cpp` — connect, drop, reconnect, HTTP/1.1 peer.
  - `http2_send_test.cpp` — request/response, concurrent `Send`, peer
    TCP reset, and the frame-level peer behaviour from
    `docs/sequences/goaway-handling.md` and
    `docs/grpc-wire-protocol.md` §2.6: `Send_PeerGoaway*`,
    `Send_PeerRstStream_*`, `GrpcResponse_Split*`.
  - `http2_tls_connect_test.cpp` — TLS, ALPN, certificate handling.
- `tests/fuzz/` — response-size and trailer-parser fuzzers (used by
  Track C too).

## Style notes

- **Single-threaded `Send`** (LOCKED — `interfaces.md` §4.1). Only the
  exporter worker calls `Send`. Concurrent calls are a contract violation.
- **The bytes referenced by `RequestSpec.payload` are borrowed.** The
  caller (the wire codec) retains ownership and guarantees the buffer is
  stable until the per-request future completes (LOCKED —
  `memory-model.md` §3.3).
- **One transport == one socket == one nghttp2 session.** Reconnect is
  internal; clients see it only via `ConnectionState`.
- **Every socket write goes through `nosignal_io.hpp`** — `SendNoSignal` for
  plaintext, and the custom `BIO` it builds for TLS, which is why the TLS
  session is wired with `SSL_set_bio` and never `SSL_set_fd`. A bare `write`
  or `SSL_set_fd` here hands a peer the ability to kill the host process with
  `SIGPIPE` (issue #177, `threading-model.md` §7.1).
- **`SslCtx` is per-`Transport`** (per ICP 0003 §3.1), not process-shared.
  v1.1 multi-`Provider` keeps the same shape: each `Provider` builds its
  own `Transport` with its own `SslCtx`.
- **No exceptions cross thread boundaries** (LOCKED —
  `error-model.md` §5). I/O thread internal failures are caught at the
  reactor's top level, recorded as `connect_failure`, the loop continues.
- **`pthread_atfork` registration** for fork survival (per
  `docs/sequences/fork-survival.md`). The atfork handler in the parent
  records a diagnostic; the child handler flips `m_state = Closed` on
  every live `Provider`.
