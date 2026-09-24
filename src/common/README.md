# `src/common/`

## What lives here

The top level of this directory holds microtel's internal logging route and
nothing else: [`internal_log.hpp`](internal_log.hpp) declares it and
[`log_sink.cpp`](log_sink.cpp) implements it, built as `microtel_common`. Two
subdirectories carry work that other tracks own:

- [`config/`](config/): Track E. TOML parser, env-var resolution, validation,
  the resolved `Config` value type, and the auth providers.
- [`raii/`](raii/): header-only RAII wrappers for C resources (`UniqueFd`,
  `SslCtx`, `SslSession`, `Nghttp2Session`, `BioMethod`, and the zlib
  streams). Track D reviews them; every track that touches a C resource
  uses them.

Several things once planned for this directory live elsewhere:

- The production `internal::IDiagnosticsSink` is `DiagnosticsCounters` in
  [`src/sdk/diagnostics_counters.hpp`](../sdk/diagnostics_counters.hpp), which
  keeps the per-reason atomic counters and builds the `HealthSnapshot`
  (`docs/error-model.md` §3, §9.1).
- `microtel::Error` and `microtel::ConfigError` are header-only, in
  [`include/microtel/error.hpp`](../../include/microtel/error.hpp).
- The byte-budget defaults from `microtel-spec.md` §5.5 are the
  `MemoryLimitOptions` defaults in
  [`include/microtel/sdk_builder.hpp`](../../include/microtel/sdk_builder.hpp).
- `internal::IClock` and `internal::ISteadyClock`
  ([`include/microtel/internal/clock.hpp`](../../include/microtel/internal/clock.hpp))
  have no production realisation yet. Components that take one accept
  `nullptr`, and only the test fakes implement them.

## Owner

The top-level files are shared, with no single track owner. The subdirectories
name their own owners in their READMEs.

## What it implements

Internal logging routing:

- the `LogSink` injection hook declared in
  [`include/microtel/log_sink.hpp`](../../include/microtel/log_sink.hpp)
  (`SetLogSink` / `ResetLogSink`);
- the minimum-level filter behind `internal::SetMinLogLevel` /
  `MinLogLevel` ([ICP 0026](../../docs/icps/0026-provider-setters.md) §6),
  one process-wide atomic that `Provider::SetLogLevel` forwards to;
- `internal::LogImpl`, which every component calls, and a minimal stderr
  fallback for when no sink is installed.

There is no spdlog route inside microtel, and there will not be one (option (1)
of issue #190). An application that wants spdlog installs
`microtel_spdlog_bridge` ([`../adapters/spdlog/`](../adapters/spdlog/)) as its
sink inside its own build, so `libmicrotel_common.a` never acquires an
undefined spdlog reference. Per-`(level, reason)` rate limiting is not built
yet.

## Dependencies

Nothing outside the standard library and `include/microtel/`.
`MICROTEL_USE_SPDLOG=ON` does not put spdlog on this library's link line; it
gates the bridge adapter in [`../adapters/spdlog/`](../adapters/spdlog/) and its
tests (issue #190).

## Tests

- [`tests/unit/log_sink_test.cpp`](../../tests/unit/log_sink_test.cpp): the
  logging route, sink injection and the level filter.
- `tests/unit/common/config/`, `tests/unit/common/auth/` and
  `tests/unit/common/raii/` cover the subdirectories (see their READMEs).
- The diagnostics sink is tested with the SDK, in
  `tests/unit/sdk/diagnostics_counters_test.cpp`.

## Style notes

- **Diagnostics sink is the leaf-lock** (LOCKED — `threading-model.md`
  §4). No other lock is acquired while `DiagnosticsCounters::m_error_mu`
  (in `src/sdk/`) is held; it guards only the last-error timestamp and
  message. Per-reason counters are `std::atomic<uint64_t>`, so the
  increment path takes no lock at all.
- **Log sink is invoked under no microtel-held lock**, and may run on any
  internal thread. The application is responsible for the thread-safety of
  any state the sink touches (LOCKED — `error-model.md` §9.3).
- **Internal logs are never recursively exported** through microtel's
  own OTLP exporter (LOCKED — `error-model.md` §9.4). A user who wants
  microtel's logs in their OTel logs pipeline bridges them explicitly
  with `SetLogSink` pointing at a separate logger.
