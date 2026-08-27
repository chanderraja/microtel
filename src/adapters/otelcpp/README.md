# `src/adapters/otelcpp/` — opentelemetry-cpp API-adapter shim

Implements the **opentelemetry-cpp API** on top of microtel's SDK so a codebase
already instrumented against `opentelemetry-cpp` can swap in microtel **without
editing call sites** — dropping the gRPC / protobuf / abseil dependency tree.

Milestone **M17**. Design and rationale: [ICP 0014](../../../docs/icps/0014-otelcpp-shim-and-rule-13.md).

## Status

| Increment | Scope | State |
|---|---|---|
| L1 | Build scaffolding, pinned API headers, configuration assertions | **done** |
| L2 | Traces — `TracerProvider` / `Tracer` / `Span` | **done** (#110, #113, #114, #115) |
| L3 | Metrics — `MeterProvider` / `Meter` / instruments | **done** — `MeterShim` (all 12 ABI-v1 creates), sync instrument shims, observable callback-registry bridging, `MakeMeterProvider` |
| L4 | Logs — `LoggerProvider` / `Logger` | **done** — `LogRecordShim`, `LoggerShim`, `LoggerProviderShim`, `MakeLoggerProvider` |
| L5 | Global provider registration | **partial** — `RegisterGlobally`/`UnregisterGlobally` (one call, all three signals) done; wire-conformance end-to-end tracked separately, see below |
| L6 | `docs/migration-from-otel-cpp.md` written against the working shim | not started |

## Two rules that are easy to break

**1. Source-only. This is never shipped as a prebuilt binary.**

The opentelemetry-cpp API wraps everything in `inline namespace v<ABI_VERSION_NO>`
and puts `nostd::` types in every signature, so the same shim source compiled
under two configurations produces link-incompatible symbols:

```
default config:  ShimEntry(opentelemetry::v1::nostd::string_view,
                           absl::otel_v1::variant<bool, long, double>)
STL=CXX20:       ShimEntry(std::basic_string_view<char, ...>,
                           std::variant<bool, long, double>)
```

A prebuilt shim's ABI would therefore depend on choices the *consumer* made when
they built opentelemetry-cpp. Consumers build the shim inside their own tree,
where exactly one configuration is in play. The in-tree target here exists to
build and test the shim, not to produce a shipped artifact.

**2. `OPENTELEMETRY_STL_VERSION=2020` is load-bearing.**

Under opentelemetry-cpp's default (`WITH_STL=OFF`), `nostd::` resolves to a
**vendored abseil snapshot bundled inside its own API headers**. Setting
`WITH_ABSEIL=OFF` does not prevent this — only the STL mode does. Measured on the
header set this shim needs: 30 vendored-absl headers enter the include graph
under the default, 0 under `STL=CXX20`.

The definition is applied by the `microtel_otelcpp_api` target in the root
`CMakeLists.txt` and asserted in
[`tests/unit/adapters/otelcpp_config_test.cpp`](../../../tests/unit/adapters/otelcpp_config_test.cpp).
Change it and that file stops compiling, which is deliberate — the failure is
otherwise invisible at runtime.

## Attribute-value conversion (L2)

otel-cpp's `common::AttributeValue` has **16** alternatives; microtel's has
**8**, matching what the OTel data model specifies. Thirteen map exactly or
widen losslessly. The three with no faithful *type* mapping are preserved with
a **degraded type** — never dropped, never clamped — per
[ICP 0015](../../../docs/icps/0015-unrepresentable-attribute-policy.md):

| otel-cpp value | becomes | fidelity |
|---|---|---|
| `uint64_t` > `INT64_MAX` | its exact decimal digits as `string` | value exact, type changed |
| `span<const uint64_t>` with any such element | every element rendered decimally (`vector<string>`) | value exact, type changed |
| `span<const uint8_t>` | lowercase hex `string`, no separators | value exact, type changed |

If one element of a `uint64_t` array overflows, **all** elements render as
strings: microtel's array alternatives cannot hold mixed types, and dropping
just the offending element would change the array's length, silently breaking
index correlation with a parallel attribute. The conversion is total — every
alternative produces a value — so the shim needs no drop accounting and no
diagnostics path. Implementation:
[`attribute_conversion.hpp`](attribute_conversion.hpp).

## Dependency status

The opentelemetry-cpp API headers are a **build-time dependency of this package
only**. microtel's runtime closure (CLAUDE.md rule 12: nghttp2, OpenSSL, upb,
zlib, optional spdlog) is unchanged, and the shim is excluded from every
dependency-closure claim in the README, spec, and M7 footprint numbers.

Per rule 13, optional source-distributed adapters may compile against
third-party headers already present in the consumer's build provided they add
nothing to the consumer's link closure. On a default-config consumer, adding
this shim is a no-op with respect to abseil: that abseil is inline header code
already being compiled into their translation units at every site that includes
an otel header.

## Building

```bash
cmake -S . -B build -DMICROTEL_BUILD_OTELCPP_SHIM=ON -DMICROTEL_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build -R otelcpp
```

Off by default: a user who does not opt in never fetches opentelemetry-cpp.

## Why the API only

Only opentelemetry-cpp's header-only **API** is consumed — never its SDK or
exporters, which is where protobuf and libcurl live. The root `CMakeLists.txt`
uses `FetchContent` with `SOURCE_SUBDIR api/include` (a directory with no
`CMakeLists.txt`) so the sources are populated *without* adding otel-cpp's own
CMake project, and asserts that the `opentelemetry_api` target does not exist
afterwards.

## Tests

- [`tests/unit/adapters/otelcpp_config_test.cpp`](../../../tests/unit/adapters/otelcpp_config_test.cpp)
  — configuration assertions (L1).
- [`tests/unit/adapters/otelcpp_attribute_conversion_test.cpp`](../../../tests/unit/adapters/otelcpp_attribute_conversion_test.cpp)
  — every variant alternative, including the exact rendered strings for the
  three degraded cases (L2).
- [`tests/unit/adapters/otelcpp_context_conversion_test.cpp`](../../../tests/unit/adapters/otelcpp_context_conversion_test.cpp)
  — TraceId / SpanId / SpanContext bridging round-trips (L2).
- [`tests/unit/adapters/otelcpp_span_shim_test.cpp`](../../../tests/unit/adapters/otelcpp_span_shim_test.cpp)
  — every `trace::Span` pure virtual forwarded onto a recording fake (L2).
- [`tests/unit/adapters/otelcpp_tracer_shim_test.cpp`](../../../tests/unit/adapters/otelcpp_tracer_shim_test.cpp)
  — `StartSpan` option mapping (kind, parent variant, time, links),
  flush/close delegation, provider scope pass-through (L2).
- [`tests/unit/adapters/otelcpp_meter_shim_test.cpp`](../../../tests/unit/adapters/otelcpp_meter_shim_test.cpp)
  — every sync instrument's forwarding, the uint64 omit rule, observable
  callback-registry bridging, global registration (L3).
- [`tests/unit/adapters/otelcpp_log_record_shim_test.cpp`](../../../tests/unit/adapters/otelcpp_log_record_shim_test.cpp)
  — every `LogRecord` setter, including the event-id name/id split (L4).
- [`tests/unit/adapters/otelcpp_logger_shim_test.cpp`](../../../tests/unit/adapters/otelcpp_logger_shim_test.cpp)
  — create/emit round trip, `logger_name`→`name` fallback, global
  registration (L4).
- [`tests/unit/adapters/otelcpp_global_registration_test.cpp`](../../../tests/unit/adapters/otelcpp_global_registration_test.cpp)
  — `RegisterGlobally` wires all three signals from one call;
  `UnregisterGlobally` restores the noop defaults (L5).
