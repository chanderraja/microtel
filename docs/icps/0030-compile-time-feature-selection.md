# ICP 0030: compile-time feature selection

**Status:** Draft.
**Affected interfaces / docs:** `CMakeLists.txt` (new `MICROTEL_WITH_*`
options, per-feature source sets); `cmake/microtelConfig.cmake.in` (recorded
feature set, conditional `find_dependency`, `COMPONENTS`);
[`include/microtel/error.hpp`](../../include/microtel/error.hpp)
(`ConfigError::Kind`); [`include/microtel/provider.hpp`](../../include/microtel/provider.hpp)
(`DropReason`); `CLAUDE.md` rule 12 (wording only); ICP 0020 (the exported
set and its `find_dependency` calls become conditional); `.github/workflows/ci.yml`.
**Affected tracks:** build / packaging, SDK (`SdkBuilder`, `Provider`), CI.

## Summary

Let a consumer compile out whole features — metrics, logs, one of the two OTLP
protocols, gzip, TOML file config — to shrink the linked binary, without making
any public type's layout depend on the choice.

## Motivation

Spec §10.5 sets a "library binary size" footprint target, and small static binaries are part
of why microtel exists. Today nothing can be removed:

- `MICROTEL_USE_SPDLOG=OFF` only skips building the spdlog adapter. The library
  itself stays the same size.
- `Provider::GetMeter` and `GetLogger` are virtual, so the vtable keeps the
  whole metrics and logs SDK live even if the application never calls them.
- `SdkBuilder`'s `BuildExporters` always builds all three signal exporters, and
  its `BuildWireCodec` can build either codec, so both protocols are always
  linked.

Rough `.text` sizes from a clang Release build, out of about 880 KB in total:

| Feature | Approx. `.text` | Also removes |
|---|---|---|
| Metrics SDK + exporter + encoder half | ~300 KB | — |
| TOML file config (toml++) | ~140–170 KB | toml++ fetch |
| Logs SDK + exporter | ~35 KB | — |
| HTTP-protobuf codec | ~25 KB | — |
| gRPC codec | ~22 KB | — |
| gzip | small | **zlib** from the link |

The PR that adds the size-report job (step 2 below) replaces these estimates
with measured numbers.

## Decision 1 — the options

| Option | Default | When OFF |
|---|---|---|
| `MICROTEL_WITH_METRICS` | ON | the metric SDK, readers, views, metric exporter and metric encoding are not built |
| `MICROTEL_WITH_LOGS` | ON | the log SDK, processors, log exporter and log encoding are not built |
| `MICROTEL_WITH_GRPC` | ON | `src/wire/grpc` is not built |
| `MICROTEL_WITH_HTTP_PROTOBUF` | ON | `src/wire/http` is not built |
| `MICROTEL_WITH_GZIP` | ON | `src/wire/gzip*` and the zlib link are dropped |
| `MICROTEL_WITH_TOML` | ON | `toml_loader` and the toml++ fetch are dropped |
| `MICROTEL_MINIMAL` | OFF | a convenience switch: before the options above are declared, it sets every one of them OFF except `MICROTEL_WITH_HTTP_PROTOBUF` |

Rules:

- **Configure fails if both protocols are OFF.**
- **With every option at its default, the build behaves exactly as it does today.**
- **TLS cannot be compiled out.** OpenSSL is the largest item in the static
  closure, but a build that cannot verify peers conflicts with ICP 0022 and
  `MICROTEL_FORBID_INSECURE_TLS`. That was considered and rejected; see
  Rationale.

**Mechanism.** A feature is removed by leaving its `.cpp` files out of the
build (conditional `target_sources`, or a separate component library), **not**
by wrapping function bodies in `#if`. `#if` appears only where a feature is
selected:

- `BuildWireCodec` and `BuildExporters` in `src/sdk/sdk_builder.cpp`
- the bodies of `GetMeter` and `GetLogger`
- the config validator
- `accept-encoding` / `grpc-accept-encoding` advertisement, which is dropped
  when gzip is OFF
- the metric and log halves of the encoder

## Decision 2 — how the feature set is published: compile definitions, no generated header

Each option becomes an INTERFACE compile definition on `microtel_headers`,
always present and always `0` or `1`:

```cmake
target_compile_definitions(microtel_headers INTERFACE
    MICROTEL_HAS_METRICS=$<BOOL:${MICROTEL_WITH_METRICS}> ...)
```

Every microtel archive links `microtel_headers`, so the library and a CMake
consumer see the same values from one source. Consumer code can use
`#if MICROTEL_HAS_METRICS` to skip its own instrumentation.

There is no generated `include/microtel/config.hpp`. That keeps RELEASING.md's
rule that no public header is generated. microtel ships no pkg-config file, so
consumers who don't use CMake are not a supported install path today.

## Decision 3 — public headers do not change shape

**No public type layout, vtable, or enumerator depends on `MICROTEL_HAS_*`.**

- `Provider`, `Meter`, `Logger` and every other public type are identical in
  every configuration.
- `GetMeter` and `GetLogger` keep their signatures.
- New enumerators (Decision 4) are added unconditionally.

Because of this, a consumer building with stale or missing `MICROTEL_HAS_*`
values cannot hit an ODR violation. The worst outcome is a runtime error or a
no-op object, as described in Decision 4.

This is enforced mechanically: `ci/header_check.cpp` builds against the
minimal configuration, and a grep in the `header-check` job fails if
`MICROTEL_HAS_` appears anywhere under `include/microtel/`.

## Decision 4 — what happens when configuration asks for a compiled-out feature

**Configuration errors.** `ConfigError::Kind` gains
`FeatureNotCompiled = 13`. Validation and `SdkBuilder::Build()` return it when:

- the resolved protocol is compiled out
- `compression = gzip` is set and gzip is compiled out
- `SdkBuilder::FromFile` is used (or `MICROTEL_CONFIG_FILE` is set) and TOML is
  compiled out

In each case `field` names the setting and `message` names the CMake option.
This check has to happen at runtime because configuration arrives from env and
files; a `static_assert` cannot see it.

**Default protocol.** If `protocol` is not set explicitly, the default (gRPC
today) becomes whichever protocol is compiled in. An explicit request for a
compiled-out protocol is still an error. Without this, a minimal build with
default configuration would fail out of the box.

**Signals.** When metrics or logs are compiled out:

- `GetMeter` and `GetLogger` return a no-op `Meter` or `Logger`. A no-op
  logger already exists; this ICP adds a matching no-op meter.
- `DropReason` gains `SignalNotCompiled = 23`. Its counter goes up by one each
  time `GetMeter` or `GetLogger` hands out a no-op object.
- One warning goes to the `LogSink` the first time it happens.

`HealthSnapshot::drop_counters` grows by one element. ICPs 0008 and 0011 set
the precedent for adding values, and the ABI policy (spec §19) allows it
within a minor release.

## Decision 5 — package config

- `microtelConfig.cmake` records `microtel_WITH_<X>` for every option, next to
  the existing `microtel_WITH_SPDLOG`.
- `find_dependency(ZLIB)` runs only when gzip is ON.
- `find_package(microtel COMPONENTS metrics logs grpc http_protobuf gzip toml)`
  sets `microtel_<comp>_FOUND` before `check_required_components`, so a
  consumer that needs gRPC fails at configure time rather than at runtime.
- Targets that disappear because their feature is OFF (`microtel::gzip`,
  `microtel::grpc_wire`, `microtel::http_wire`) are left out of
  `MICROTEL_EXPORTED_TARGETS`.
- The aggregate `microtel::microtel` is always present. It is the only target
  consumers are told to link, so omitting the others breaks nobody who
  followed ICP 0020.

## Decision 6 — rule 12 wording

`CLAUDE.md` rule 12 changes from "the runtime dependency closure for v1 is
fixed" to "the runtime dependency closure for v1 is **at most**: nghttp2,
OpenSSL, upb (vendored), zlib, plus optional spdlog". Compile-time selection
may only remove members from the closure. Adding one still needs an ICP.

## CI

- **New `feature-matrix` job.** Release, gcc. Each entry builds, runs
  `ctest -L unit`, and runs `symbol-scan`. Entries:
  - `MICROTEL_MINIMAL=ON`
  - gRPC-only
  - metrics and logs OFF
  - gzip OFF

  The minimal entry also runs a negative check: no `U deflate*` / `U inflate*`
  in any archive.
- **consumer-smoke** also installs and consumes the minimal build. This catches
  a `find_dependency` that should be conditional, and a missing component.
- **Tests.** Every feature-OFF path has a test that runs in the matrix, for
  example `Build()` with `Protocol::Grpc` in a gRPC-OFF build returns
  `FeatureNotCompiled`. Feature-specific test targets are added under
  `if(MICROTEL_WITH_<X>)` in `tests/**/CMakeLists.txt`.
- **Coverage.** The `#if` branches that only compile in the minimal
  configuration are covered by uploading lcov data from the minimal matrix
  entry and merging it before diff-cover runs. The 80/90% thresholds are
  unchanged.
- **tidy.** `tidy-check.sh` runs a second time over the minimal build's
  `compile_commands.json`.
- **Size report.** A new step builds `examples/basic_trace` statically, in full
  and minimal configurations, strips it, and compares its size against a
  checked-in baseline. It warns above +2% and fails above +5%.
  A script modelled on `ci/scripts/baseline-update.sh` refreshes the baseline.

## Migration

- **Default builds:** nothing to do.
- **Code that switches over `ConfigError::Kind` or `DropReason` exhaustively:**
  add the new values.
- **Contributors adding a new feature:** decide whether it gets a
  `MICROTEL_WITH_*` option, and keep it out of public type layout (Decision 3).

## Implementation order

Each step is its own PR, green on its own:

1. This ICP.
2. Size-report job and baseline. Gives measured numbers before anything is
   removed.
3. `FeatureNotCompiled`, `SignalNotCompiled`, the no-op meter, compile
   definitions, package variables and the `feature-matrix` job, with every
   option still forced ON.
4. gzip.
5. gRPC and HTTP-protobuf, including the default-protocol rule.
6. TOML.
7. Metrics.
8. Logs.
9. `MICROTEL_MINIMAL`, a README build-options section, and a size table.

## Rationale & alternatives

- **A generated `microtel/config.hpp`.** It would describe the build even
  without CMake, but it reverses RELEASING.md's decision not to generate public
  headers. It would only help pkg-config and vendored consumers, and microtel
  supports neither today. Rejected.
- **A link-time mismatch marker** (a per-configuration symbol that the headers
  reference). Unnecessary under Decision 3: with layout-invariant headers a
  mismatch cannot corrupt anything, and it would need a conditional reference
  in a public header. Rejected.
- **`MICROTEL_WITH_TLS`.** It is the largest possible saving: libssl and
  libcrypto are several MB in a static build. It was rejected because it
  would produce a library that cannot verify peers at all, which goes against
  ICP 0022 and `MICROTEL_FORBID_INSECURE_TLS`. Plaintext-only deployments can
  already use `http://` endpoints; they keep the OpenSSL link.
- **Failing hard from `GetMeter`/`GetLogger`.** This would need a new return
  type, which is an API break. Returning a no-op object matches what the OTel
  API specifies for a disabled SDK.
- **Making samplers and propagators optional** (~77 KB of samplers). These are
  locked public API from ICPs 0025 and 0028, and removing them would change
  behaviour, not just size. They stay; any slimming is left to ordinary
  refactoring.
