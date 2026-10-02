# Build options

Every CMake option microtel declares, with its default. This is the one list:
[configuration.md](configuration.md) §4 and
[repository-layout.md](repository-layout.md) §3 point here.

Build options are compile-time properties of the binary. None of them appears
in `microtel.toml` or has an environment-variable equivalent; runtime settings
are in [configuration.md](configuration.md).

## Building and using microtel

Declared in the top-level `CMakeLists.txt`.

| Option | Default | Effect |
|---|---|---|
| `MICROTEL_BUILD_TESTS` | `ON` | Builds the test tree (fetches GoogleTest). Set `OFF` for install-only or cross builds. |
| `MICROTEL_BUILD_EXAMPLES` | `OFF` | Builds the programs under [`examples/`](../examples/). |
| `MICROTEL_USE_SPDLOG` | `ON` | spdlog for internal diagnostics. `OFF` uses a minimal stderr logger instead; sink injection still works. |
| `MICROTEL_FORBID_INSECURE_TLS` | `OFF` | `ON` makes a configuration with `tls.insecure = true` fail `Build()` with `ConfigError::Kind::InsecureDisallowed`; default builds warn instead ([configuration.md](configuration.md) §3.5). |
| `MICROTEL_BUILD_OTELCPP_SHIM` | `OFF` | Builds the experimental opentelemetry-cpp API shim, source-only ([ICP 0014](icps/0014-otelcpp-shim-and-rule-13.md), [migration guide](migration-from-otel-cpp.md)). |
| `MICROTEL_BUILD_GLOG_BRIDGE` | `OFF` | Builds the header-only glog log bridge and its tests ([`src/adapters/glog/`](../src/adapters/glog/README.md)). Needs glog 0.6 or 0.7 installed (`find_package(glog)`). |
| `MICROTEL_BUILD_LOG4CXX_BRIDGE` | `OFF` | Builds the header-only log4cxx log bridge and its tests ([`src/adapters/log4cxx/`](../src/adapters/log4cxx/README.md)). Needs log4cxx 1.1 or later installed (`find_package(log4cxx)`). |

## Leaf and concentrator

Declared in the top-level `CMakeLists.txt`; experimental in v1.2.

| Option | Default | Effect |
|---|---|---|
| `MICROTEL_BUILD_LEAF` | `OFF` | Builds the C11 leaf library, exported as `microtel::leaf` ([`leaf/`](../leaf/README.md)). It also builds on its own with `cmake -S leaf`, below. |
| `MICROTEL_LEAF_ENCODER` | `nanopb` | Leaf encoder backend: `nanopb` (microcontrollers, no heap) or `upb` (Linux-class boards). Same bytes either way. |
| `MICROTEL_WITH_CONCENTRATOR` | `OFF` | Compiles the concentrator's leaf receiver. When `OFF`, `Provider::GetLeafReceiver()` returns a receiver that answers `Disabled`, and a configuration that enables it fails `Build()` ([configuration.md](configuration.md) §3.14). Off by default because it parses untrusted bytes and links upb's decoder. |

## Packaging

Declared in the top-level `CMakeLists.txt`, except where noted.

| Option | Default | Effect |
|---|---|---|
| `MICROTEL_USE_SYSTEM_DEPS` | `OFF` | `ON` takes toml++, spdlog (if `MICROTEL_USE_SPDLOG`) and GoogleTest (if `MICROTEL_BUILD_TESTS`) from `find_package(… CONFIG REQUIRED)` instead of FetchContent, so configure needs no network. toml++ is compiled header-only either way; the installed package never needs a toml++ library. |
| `MICROTEL_WARNINGS_AS_ERRORS` | `ON` | Builds microtel's own targets, the leaf included, with `-Werror`. Packagers set `OFF` so that a new warning from a newer compiler does not fail the build; the warnings themselves stay on. Declared in [`cmake/MicrotelWarnings.cmake`](../cmake/MicrotelWarnings.cmake), which the standalone leaf and the leaf target runner include too. |

The [vcpkg overlay port](../packaging/vcpkg/ports/microtel/README.md) sets
`MICROTEL_USE_SYSTEM_DEPS=ON` and `MICROTEL_WARNINGS_AS_ERRORS=OFF`.

## Development and CI

Declared in the top-level `CMakeLists.txt`. See
[CONTRIBUTING.md](../CONTRIBUTING.md) and [development.md](development.md).

| Option | Default | Effect |
|---|---|---|
| `MICROTEL_BUILD_HEADER_CHECK` | `ON` | Builds `ci/header_check.cpp`, which includes every public and internal header. |
| `MICROTEL_BUILD_FUZZ` | `OFF` | Builds the libFuzzer harnesses (clang only, `-fsanitize=fuzzer`). |
| `MICROTEL_BUILD_BENCH` | `OFF` | Builds the benchmark harness under [`bench/`](../bench/) (needs Docker or Podman). |
| `MICROTEL_COVERAGE` | `OFF` | Builds instrumented for coverage: clang source-based (`-fprofile-instr-generate -fcoverage-mapping`), which is what `ci/scripts/coverage.sh` gates on, or gcov `--coverage` under gcc. |
| `MICROTEL_SANITIZER` | *(empty)* | One of `asan`, `tsan`, `ubsan`. A cache string, not a boolean option. |

## Other projects in the tree

These configure on their own (`cmake -S <dir>`), not through the top-level
build.

| Option | Declared in | Default | Effect |
|---|---|---|---|
| `MICROTEL_LEAF_ENCODER` | [`leaf/CMakeLists.txt`](../leaf/CMakeLists.txt) | `nanopb` | The same backend choice as above, for a standalone leaf build. In-tree, the top-level definition wins. |
| `MICROTEL_WARNINGS_AS_ERRORS` | `cmake/MicrotelWarnings.cmake` | `ON` | As above, for the standalone leaf and the leaf target runner. |
| `MICROTEL_LEAF_TARGET_BOARD` | [`tests/leaf/target/CMakeLists.txt`](../tests/leaf/target/CMakeLists.txt) | `hosted` | Where the leaf test runner runs: `hosted`, `microbit` (Cortex-M0) or `mps2-an386` (Cortex-M4). The runner builds both encoder backends and forces `MICROTEL_LEAF_ENCODER=upb` internally. |
| `MICROTEL_LEAF_TARGET_GTEST` | `tests/leaf/target/CMakeLists.txt` | `OFF` | Also builds the gtest leaf suite (hosted boards only). |
| `MICROTEL_LEAF_CPU` | [`cmake/toolchains/arm-none-eabi.cmake`](../cmake/toolchains/arm-none-eabi.cmake) | `cortex-m0plus` | The Cortex-M core passed as `-mcpu`; any value the compiler accepts. |

`bench/emit-app/` has its own `BENCH_*` options; see
[`bench/`](../bench/) and [bench-spec.md](bench-spec.md).

`MICROTEL_BUILD_PYTHON` and `MICROTEL_BUILD_COMPAT_SHIMS` do not exist (#196):
there is no Python extension in the tree, and the shim option is
`MICROTEL_BUILD_OTELCPP_SHIM`.
