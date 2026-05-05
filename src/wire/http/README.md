# `src/wire/http/`

## Purpose

The OTLP/HTTP-protobuf implementation of `IWireCodec`. Owns the HTTP/2
header construction (request and response), the `Retry-After` parsing,
status-code interpretation per the matrix in `error-model.md` §7.1, and
response-body capture for diagnostics.

Validated end-to-end during the M1 spike against
`otel/opentelemetry-collector:0.151.0`; zero ICPs from that work.

## Owner

Track B — OTLP/HTTP wire codec.

## Implements

- The HTTP implementation of `internal::IWireCodec` (declared in
  [`include/microtel/internal/wire_codec.hpp`](../../../include/microtel/internal/wire_codec.hpp))

## Depends on

- `EncodedPayload` (Track F; input only — bytes plus size)
- `ITransport`     (Track D; mock at [`tests/mocks/mock_transport.hpp`](../../../tests/mocks/))
- `IAuthProvider`  (Track E; fake at [`tests/fakes/fake_auth_provider.hpp`](../../../tests/fakes/))
- `IDiagnosticsSink` (fake at [`tests/fakes/`](../../../tests/fakes/))

## Test entry points

- `tests/unit/wire/http/` — every status-code row from `error-model.md` §7.1.
- `tests/wire/http/` — byte-level fixtures from real collector responses.
- `tests/conformance/http/` — end-to-end against a real collector
  (shared with Track C's gRPC).

## Style notes

- **Codec owns retry classification** (LOCKED — `interfaces.md` §4.3).
  The exporter respects `WireResult.retryable` and `retry_after` as
  returned; HTTP-specific knowledge (the `Retry-After` header) stays
  inside this directory.
- **Single-threaded.** Only the exporter worker calls `Send`. Concurrent
  calls are a contract violation (LOCKED — `interfaces.md` §4.3).
- **Endpoint path semantics** per `docs/configuration.md` §3.3: empty
  path or `/` becomes `/v1/traces`; non-empty path is treated as a base
  with `/v1/traces` appended.
- **Compression:** request `Content-Encoding: gzip` per spec §7.1; off
  by default for the low-CPU profile.
- **Response handling:** capture body up to `max_response_bytes` for
  diagnostics; `415` and `404` are non-retryable; `429`/`502`/`503`/`504`
  are retryable and respect `Retry-After`.
