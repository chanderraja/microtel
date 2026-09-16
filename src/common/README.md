# `src/common/`

## Purpose

Shared, layer-independent services consumed by every track:
diagnostics sink, internal logging (the `LogSink` hook, with a stderr
fallback), error types, byte-budget constants, time abstraction.

The top-level files in this directory are the layer-independent ones;
two subdirectories carry track-specific work:

- [`config/`](config/) — Track E (TOML parser, env-var resolution,
  validation, the resolved `Config` value type, auth providers).
- [`raii/`](raii/) — Custom RAII wrappers for C resources (Socket,
  SslCtx, SslSession, Nghttp2Session). Owned by Track D's review but
  used by every track that touches a C resource.

## Owner

Common files: shared (no single track owner). Subdirectory ownership
per their own READMEs.

## Implements

- `internal::IDiagnosticsSink` and the concrete `DiagnosticsSink` —
  atomic per-reason counters, the `HealthSnapshot` builder (per
  `docs/error-model.md` §3, §9.1)
- Internal logging routing — the `LogSink` injection hook (declared in
  [`include/microtel/log_sink.hpp`](../../include/microtel/log_sink.hpp)),
  the minimum-level filter behind `internal::SetMinLogLevel` /
  `MinLogLevel` ([ICP 0026](../../docs/icps/0026-provider-setters.md) §6),
  and a minimal stderr fallback when no sink is installed. **There is no
  spdlog route inside microtel** and there will not be one — issue #190's
  option (1). An application that wants spdlog installs
  `microtel_spdlog_bridge` ([`../adapters/spdlog/`](../adapters/spdlog/))
  as its sink, inside its own build, so `libmicrotel_common.a` never
  acquires an undefined spdlog reference.
- `internal::IClock` and `internal::ISteadyClock` realisations backed by
  `std::chrono::system_clock` and `std::chrono::steady_clock`
- The byte-budget constants from `microtel-spec.md` §5.5
- The `microtel::Error` and `microtel::ConfigError` value types
  (declared in [`include/microtel/error.hpp`](../../include/microtel/error.hpp))

## Depends on

Nothing outside the standard library and `include/microtel/`.
`MICROTEL_USE_SPDLOG=ON` no longer puts spdlog on this library's link line;
it gates the bridge adapter in [`../adapters/spdlog/`](../adapters/spdlog/)
and its tests (see the logging bullet above, and issue #190).

## Test entry points

- `tests/unit/common/` — tests for the diagnostic sink, the byte-budget
  constants, the clock realisations, the logging routing.
- `tests/unit/common/logging/` — the rate-limiter and the sink-injection
  contract (no recursive export per `error-model.md` §9.4).

## Style notes

- **Diagnostics sink is the leaf-lock** (LOCKED — `threading-model.md`
  §4). No other lock is acquired while `m_diag` is held. Per-reason
  counters are `std::atomic<uint64_t>`; the increment path takes no
  lock at all.
- **Log sink is invoked under no microtel-held lock**, may run on any
  internal thread. Application is responsible for thread-safety of any
  state the sink touches (LOCKED — `error-model.md` §9.3).
- **Internal logs are never recursively exported** through microtel's
  own OTLP exporter (LOCKED — `error-model.md` §9.4). If a user wants
  microtel's logs in their OTel logs pipeline, they bridge it explicitly
  via `SetLogSink` pointing at a separate logger.
