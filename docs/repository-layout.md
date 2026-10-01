# microtel Repository Layout

A map of the tree as it is today: what each top-level directory holds, who
it is for, which CMake options build which parts, and where each directory's
own README lives. For how the project got here, see
[`microtel-roadmap.md`](../microtel-roadmap.md).

**Who reads what.** If you are *using* microtel, the parts you need are
`include/microtel/` (the API), `examples/`, `leaf/` (for microcontrollers),
`tools/preflight/` and the user documents indexed in
[`docs/README.md`](README.md). Everything else is for contributors.

---

## 1. Top level

```
microtel/
├── include/microtel/   public C++ headers (the API)                       user
├── src/                C++ runtime implementation                         contributor
├── leaf/               C11 leaf library for microcontrollers              user
├── examples/           runnable example programs                          user
├── tools/              operator CLI (preflight) and diagram generators    user / contributor
├── docs/               documentation; index in docs/README.md             both
├── tests/              all tests                                          contributor
├── bench/              benchmark harness (Docker)                         contributor
├── ci/                 CI scripts called by .github/workflows/            contributor
├── cmake/              package-config template and cross toolchains      contributor
├── gen/                generated protobuf C code (committed)              contributor
├── proto/              vendored OpenTelemetry .proto files                contributor
├── third_party/        vendored upb, utf8_range, nanopb, tl-expected      contributor
├── .github/            workflows, issue and PR templates                  contributor
└── .claude/            Claude Code project settings                       contributor
```

Top-level files:

| File | What |
|---|---|
| `README.md` | Project overview, quick start, build and install. |
| `CMakeLists.txt` | The build entry point; every `MICROTEL_*` option in §3 is declared here. |
| `CONTRIBUTING.md` | How to contribute: TDD gates, ICP process. |
| `CLAUDE.md`, `.github/copilot-instructions.md` | Rules for AI coding agents. |
| `RELEASING.md` | The release procedure. |
| `SECURITY.md` | Vulnerability disclosure policy. |
| `CODEOWNERS` | Review routing. |
| `LICENSE`, `NOTICE`, `THIRD_PARTY_NOTICES.md` | Apache-2.0, attributions, and the licenses of vendored code. |
| `microtel-spec.md` | The original v1.0 design specification. |
| `microtel-roadmap.md` | The roadmap, v1.0 onward. |
| `sonar-project.properties` | SonarQube Cloud configuration. |
| `.clang-format`, `.clang-tidy`, `.clangd`, `.editorconfig` | Formatting, lint and editor settings. |
| `.gitignore`, `.gitattributes`, `.dockerignore`, `.graphifyignore` | Ignore and attribute rules. |

There is no `CMakePresets.json`, no `CHANGELOG.md`, and no Python, packaging
or shim directory at the top level.

---

## 2. Directory by directory

### `include/microtel/` — the public API

The headers a C++ consumer includes. The top level (`provider.hpp`,
`sdk_builder.hpp`, `tracer.hpp`, `span.hpp`, `meter.hpp`, `logger.hpp`,
`propagator.hpp`, `sugar.hpp`, `expected.hpp`, `version.hpp` and the rest) is
the supported surface.

- `include/microtel/sugar/` — the header-only sugar layer behind `sugar.hpp`.
- `include/microtel/adapters/` — header-only log bridges (spdlog, glog,
  log4cxx) and `code_attributes.hpp`.
- `include/microtel/internal/` — the internal interfaces (`transport.hpp`,
  `wire_codec.hpp`, `otlp_encoder.hpp`, `exporter.hpp`, `processor.hpp`,
  `sampler.hpp`, `reactor.hpp`, …). Installed because public headers include
  them, but not public API; their contracts are in
  [`interfaces.md`](interfaces.md).

`version.hpp` is hand-maintained, not generated;
`ci/scripts/version-drift-check.sh` checks it agrees with `CMakeLists.txt`.

### `src/` — the runtime implementation

Each directory has a README covering what lives there, which interfaces it
implements, what it depends on, and its tests. Read it before editing.

| Directory | CMake target(s) | README |
|---|---|---|
| `src/api/` | `microtel_api` | [README](../src/api/README.md) |
| `src/sdk/` | `microtel_sdk` | [README](../src/sdk/README.md) |
| `src/exporter/` | `microtel_exporter` | [README](../src/exporter/README.md) |
| `src/transport/` | `microtel_transport` | [README](../src/transport/README.md) |
| `src/wire/` | `microtel_otlp_response`, `microtel_gzip` | — |
| `src/wire/encoder/` | `microtel_encoder` (the only runtime code that touches upb) | [README](../src/wire/encoder/README.md) |
| `src/wire/http/` | `microtel_http_wire` | [README](../src/wire/http/README.md) |
| `src/wire/grpc/` | `microtel_grpc_wire` | [README](../src/wire/grpc/README.md) |
| `src/wire/custom/` | `microtel_custom_wire` | [README](../src/wire/custom/README.md) |
| `src/common/` | `microtel_common` | [README](../src/common/README.md) |
| `src/common/config/` | `microtel_config` | [README](../src/common/config/README.md) |
| `src/common/raii/` | header-only | [README](../src/common/raii/README.md) |
| `src/adapters/spdlog/` | `microtel_spdlog_bridge` | [README](../src/adapters/spdlog/README.md) |
| `src/adapters/glog/` | `microtel_glog_bridge` | [README](../src/adapters/glog/README.md) |
| `src/adapters/log4cxx/` | `microtel_log4cxx_bridge` | [README](../src/adapters/log4cxx/README.md) |
| `src/adapters/otelcpp/` | `microtel_otelcpp_shim` (opentelemetry-cpp API shim) | [README](../src/adapters/otelcpp/README.md) |

Consumers link one target, `microtel::microtel`; the rest are the static
components behind it. Which track owns which directory is in
[`development.md`](development.md).

### `leaf/` — the C library for microcontrollers

A C11 span builder and OTLP encoder for constrained devices: one public header
(`leaf/include/microtel/leaf.h`), sources in `leaf/src/`, and the nanopb
options file in `leaf/nanopb/`. It builds standalone with only a C compiler
(`cmake -S leaf …`) or in-tree with `MICROTEL_BUILD_LEAF=ON`, and is exported
as `microtel::leaf`. Experimental in v1.2. See
[`leaf/README.md`](../leaf/README.md).

### `examples/` — runnable programs

One directory per example, each with a README; the list is in
[`examples/README.md`](../examples/README.md). `examples/stack/` is a
Compose stack (OpenTelemetry Collector, Tempo, Grafana) the others can export to;
`examples/leaf/` and `examples/leaf_mqtt/` pair a leaf with a concentrator.
Built with `MICROTEL_BUILD_EXAMPLES=ON`.

### `tools/`

- `tools/preflight/` — the `microtel-preflight` operator CLI, which is
  installed alongside the libraries.
- `tools/diagrams/` — Python scripts that generate the SVGs in
  `docs/images/`; see [`tools/diagrams/README.md`](../tools/diagrams/README.md).
  Edit the scripts, not the SVGs.

### `docs/`

All design and user documentation, plus `docs/sequences/` (sequence diagrams),
`docs/icps/` (Interface Change Proposals, append-only), `docs/bench-results/`
(the committed benchmark snapshot and leaf footprint) and `docs/images/`
(generated SVGs). [`docs/README.md`](README.md) indexes all of it.

### `tests/`

All tests; the taxonomy and conventions are in
[`tests/README.md`](../tests/README.md).

| Directory | What | README |
|---|---|---|
| `tests/unit/` | gtest unit tests, mirroring `src/` (plus `leaf/`, `preflight/`, `sugar/`, `nanopb/`) | [README](../tests/unit/README.md) |
| `tests/integration/` | Real components wired together | [README](../tests/integration/README.md) |
| `tests/conformance/` | End-to-end against a real OpenTelemetry Collector | [README](../tests/conformance/README.md) |
| `tests/wire/`, `tests/grpc-wire/` | Indexes of byte-level coverage; the tests live under `tests/unit/wire/` | [wire](../tests/wire/README.md), [grpc-wire](../tests/grpc-wire/README.md) |
| `tests/fuzz/` | libFuzzer harnesses, `corpus/` and `crashes/` | [README](../tests/fuzz/README.md) |
| `tests/leaf/` | Leaf test vectors (`vectors/`), the dual-backend shim (`dual/`), the differential program (`diff/`) and the cross-target project (`target/`, driven by `ci/scripts/leaf-target.sh`) | — |
| `tests/consumer/` | External `find_package(microtel)` project driven by `ci/scripts/consumer-smoke.sh`; not part of the main build | — |
| `tests/mocks/` | Dumb mocks, one per interface | [README](../tests/mocks/README.md) |
| `tests/fakes/` | Test doubles with logic | [README](../tests/fakes/README.md) |
| `tests/helpers/` | Shared test-only utilities | — |

### `bench/`

The benchmark harness comparing microtel with opentelemetry-cpp and other
SDKs: emit app, systems under test (`sut/`), sinks, workload profiles, driver
and baselines. Runs under Docker. See [`bench/README.md`](../bench/README.md)
and [`bench-spec.md`](bench-spec.md); committed results are in
`docs/bench-results/`.

### `ci/`

`ci/scripts/` holds the scripts the workflows run (format, tidy, coverage,
symbol scan, test presence, regen check, citation check, leaf footprint and
target, consumer smoke, version drift, …).
`ci/header_check.cpp` is the standalone header compile check. Pipeline
structure is in [`ci-architecture.md`](ci-architecture.md).

### `cmake/`

`cmake/microtelConfig.cmake.in` (the `find_package(microtel)` config
template) and `cmake/toolchains/` (aarch64, i686 and arm-none-eabi
cross toolchains, used by the leaf target tests).

### `proto/`, `gen/`, `third_party/` — vendored and generated code

| Directory | What | README |
|---|---|---|
| `proto/` | OpenTelemetry `.proto` files, pinned | [README](../proto/README.md) |
| `gen/opentelemetry/` | upb C accessors generated from `proto/` (`microtel_upb_gen`) | [README](../gen/README.md) |
| `gen/nanopb/` | nanopb C code for the leaf (`microtel_nanopb_gen`) | [README](../gen/README.md) |
| `third_party/upb/` | upb runtime (`microtel_upb_runtime`), symbols renamed `microtel_upb_*` | [README](../third_party/upb/README.md) |
| `third_party/utf8_range/` | UTF-8 validator used by upb (`microtel_utf8_range`) | [README](../third_party/utf8_range/README.md) |
| `third_party/nanopb/` | nanopb runtime for the leaf (`microtel_nanopb`) | [README](../third_party/nanopb/README.md) |
| `third_party/tl-expected/` | `tl::expected`, behind `microtel::Expected` on C++20 | [README](../third_party/tl-expected/README.md) |

`gen/` is committed so a clone builds without a code-generation step.
Regenerate with `ci/scripts/regen-protos.sh`; the `regen-check` CI job fails
if the committed output differs.

---

## 3. Build options

Declared in the top-level `CMakeLists.txt` unless noted.

| Option | Default | What it does |
|---|---|---|
| `MICROTEL_BUILD_TESTS` | `ON` | Build `tests/` (fetches GoogleTest). |
| `MICROTEL_BUILD_EXAMPLES` | `OFF` | Build `examples/`. |
| `MICROTEL_BUILD_BENCH` | `OFF` | Build `bench/` (needs Docker). |
| `MICROTEL_BUILD_FUZZ` | `OFF` | Build the libFuzzer harnesses (clang only). |
| `MICROTEL_BUILD_HEADER_CHECK` | `ON` | Build `ci/header_check.cpp`. |
| `MICROTEL_USE_SPDLOG` | `ON` | Use spdlog for internal diagnostics; `OFF` uses a stderr fallback. |
| `MICROTEL_BUILD_OTELCPP_SHIM` | `OFF` | Build the opentelemetry-cpp API shim. |
| `MICROTEL_BUILD_GLOG_BRIDGE` | `OFF` | Build the glog log bridge (needs glog installed). |
| `MICROTEL_BUILD_LOG4CXX_BRIDGE` | `OFF` | Build the log4cxx log bridge (needs log4cxx installed). |
| `MICROTEL_FORBID_INSECURE_TLS` | `OFF` | Fail at init if `insecure = true` is configured. |
| `MICROTEL_BUILD_LEAF` | `OFF` | Build the leaf C library. |
| `MICROTEL_LEAF_ENCODER` | `nanopb` | Leaf encoder backend: `upb` or `nanopb` (also honoured by a standalone `leaf/` build). |
| `MICROTEL_WITH_CONCENTRATOR` | `OFF` | Compile the concentrator's leaf receiver. |
| `MICROTEL_SANITIZER` | empty | `asan`, `tsan` or `ubsan`. |
| `MICROTEL_COVERAGE` | `OFF` | Coverage instrumentation. |

`tests/leaf/target/CMakeLists.txt` adds `MICROTEL_LEAF_TARGET_BOARD` and
`MICROTEL_LEAF_TARGET_GTEST`; `cmake/toolchains/arm-none-eabi.cmake` adds
`MICROTEL_LEAF_CPU`; `bench/emit-app/` has its own `BENCH_*` options.

---

## 4. Committed vs generated

Committed: all source, headers and docs; `gen/`; everything under `proto/`
and `third_party/`; the generated SVGs in `docs/images/`; the benchmark
snapshot in `docs/bench-results/`.

Not committed (see `.gitignore`): `build/`, `build-*/`, `build_*/`, `_deps/`,
`install-tree/`, `bench/results/`, `graphify-out/`, and
`examples/tls/certs/`.

---

## 5. Where a new file goes

| File | Goes in |
|---|---|
| Public C++ header | `include/microtel/` |
| Internal interface | `include/microtel/internal/` (locked; changes need an [ICP](icps/README.md)) |
| Implementation | `src/<area>/`, per that directory's README |
| RAII wrapper for a C resource | `src/common/raii/` (`UpbArena` is the exception: `src/wire/encoder/`) |
| Unit test | `tests/unit/<area>/` |
| Mock / fake | `tests/mocks/` (no logic) / `tests/fakes/` |
| Leaf code | `leaf/`; its tests under `tests/unit/leaf/` and `tests/leaf/` |
| Example | `examples/<name>/`, with a README, and an `add_subdirectory` in `examples/CMakeLists.txt` |
| CI script | `ci/scripts/` |
| Design doc | `docs/`, and a line in [`docs/README.md`](README.md) |
| Sequence diagram | `docs/sequences/` |
| Interface change | `docs/icps/NNNN-<slug>.md` |
| Vendored code | `third_party/<name>/` with README and LICENSE |

If a new top-level directory or build option lands, update this file in the
same PR.
