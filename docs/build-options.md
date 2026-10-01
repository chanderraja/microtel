# Build options

The CMake options you are likely to set when building or packaging microtel.
They are compile-time properties of the binary: none of them appears in
`microtel.toml` or has an environment-variable equivalent (runtime settings
are in [configuration.md](configuration.md)).

| Option | Default | Effect |
|---|---|---|
| `MICROTEL_BUILD_TESTS` | `ON` | Test tree (fetches GoogleTest). Set `OFF` for install-only or cross builds. |
| `MICROTEL_BUILD_EXAMPLES` | `OFF` | The programs under [`examples/`](../examples/). |
| `MICROTEL_BUILD_LEAF` | `OFF` | Experimental C11 leaf library, exported as `microtel::leaf` ([`leaf/`](../leaf/README.md)). It also builds on its own with `cmake -S leaf`. |
| `MICROTEL_LEAF_ENCODER` | `nanopb` | Leaf encoder backend: `nanopb` (microcontrollers, no heap) or `upb` (Linux-class boards). Same bytes either way. |
| `MICROTEL_WITH_CONCENTRATOR` | `OFF` | Experimental `LeafReceiver`, which makes a Provider a concentrator for leaves. Off by default because it parses untrusted bytes and links upb's decoder. |
| `MICROTEL_BUILD_OTELCPP_SHIM` | `OFF` | Experimental opentelemetry-cpp API shim, source-only ([ICP 0014](icps/0014-otelcpp-shim-and-rule-13.md)). |
| `MICROTEL_USE_SPDLOG` | `ON` | spdlog for internal diagnostics. `OFF` uses a minimal stderr logger. |
| `MICROTEL_USE_SYSTEM_DEPS` | `OFF` | Find toml++, spdlog and GoogleTest with `find_package(… CONFIG)` instead of downloading them, for offline and package-manager builds. toml++ is still compiled header-only, so the installed package does not depend on it. |
| `MICROTEL_BUILD_GLOG_BRIDGE` | `OFF` | Header-only glog log bridge ([`src/adapters/glog/`](../src/adapters/glog/README.md)); needs glog 0.6+ installed. |
| `MICROTEL_BUILD_LOG4CXX_BRIDGE` | `OFF` | Header-only log4cxx log bridge ([`src/adapters/log4cxx/`](../src/adapters/log4cxx/README.md)); needs log4cxx 1.1+ installed. |
| `MICROTEL_FORBID_INSECURE_TLS` | `OFF` | Makes `tls.insecure = true` a `Build()` error instead of a warning. |
| `MICROTEL_SANITIZER` | empty | `asan`, `tsan` or `ubsan`. |
| `MICROTEL_WARNINGS_AS_ERRORS` | `ON` | Builds microtel's own targets, the leaf included, with `-Werror`. Packagers set `OFF` so that a new warning from a newer compiler does not fail the build; the warnings themselves stay on. |

`MICROTEL_BUILD_HEADER_CHECK`, `MICROTEL_BUILD_FUZZ`, `MICROTEL_BUILD_BENCH`
and `MICROTEL_COVERAGE` are for development and CI; see
[CONTRIBUTING.md](../CONTRIBUTING.md).
