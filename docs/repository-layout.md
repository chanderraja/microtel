# microtel Repository Layout

**Status:** Source of truth for where every file lives.
**Companion to:** `microtel-spec.md` §11 (which shows the same tree at a high level).

This document is the operational map. It shows:

- Where each file goes.
- Which files are committed vs generated.
- What's empty in M0 vs filled in by M2.
- What's optional vs required.

If you're an agent or contributor wondering "where should this file go?" — start here.

---

## 1. Top-level layout

```
microtel/
├── .github/                       # GitHub-specific configuration
│   ├── workflows/                 # GitHub Actions CI (per ci-architecture.md)
│   ├── ISSUE_TEMPLATE/            # bug report, feature request templates
│   ├── PULL_REQUEST_TEMPLATE.md   # PR description template + review checklist
│   └── dependabot.yml             # dependency update automation
│
├── ci/                            # Shared CI scripts and configs (referenced by .github/workflows/)
│   ├── README.md                  # this directory's purpose
│   ├── scripts/                   # shell scripts called by both CI and pre-commit
│   ├── docker/                    # Dockerfiles for collector test containers
│   └── sonar-project.properties   # SonarQube configuration
│
├── cmake/                         # CMake helper modules
│   ├── microtelConfig.cmake.in    # for find_package consumers
│   ├── presets/                   # CI preset definitions
│   └── modules/                   # custom Find<Foo>.cmake when needed
│
├── docs/                          # Project documentation
│   ├── architecture.md            # M0 deliverable
│   ├── threading-model.md         # M0 deliverable
│   ├── memory-model.md            # M0 deliverable
│   ├── error-model.md             # M0 deliverable
│   ├── interfaces.md              # M0 deliverable; signed off before M1
│   ├── coding-standards.md        # M0 deliverable; this is the SonarQube ruleset
│   ├── grpc-wire-protocol.md      # M0 deliverable; gRPC-on-nghttp2 implementation notes
│   ├── repository-layout.md       # this document
│   ├── ci-architecture.md         # CI pipeline architecture (companion to .github/workflows/)
│   ├── branch-protection.md       # GitHub branch-protection and repo-settings policy
│   ├── bench-spec.md              # benchmark harness specification (companion to spec)
│   ├── configuration.md           # M0 deliverable; config precedence per setting
│   ├── migration-from-otel-cpp.md # M9 deliverable; ships with v1.0
│   ├── compatibility-matrix.md    # spec §15.1 in operational form
│   ├── interop-matrix.md          # spec §15.2 in operational form
│   ├── release-policy.md          # LTS / deprecation calendar
│   ├── development.md             # M0 deliverable; track-to-directory atlas; spec §13.3
│   ├── sequences/                 # M0 deliverable; sequence diagrams
│   │   ├── connection-establishment.md
│   │   ├── retry-after-failure.md
│   │   ├── goaway-handling.md
│   │   ├── shutdown-drain.md
│   │   ├── fork-survival.md
│   │   ├── backpressure-and-drop.md
│   │   ├── partial-success.md
│   │   └── grpc-trailer-only-and-multi-frame.md
│   └── icps/                      # Interface Change Proposals
│       ├── README.md              # ICP template and process
│       └── NNNN-<slug>.md         # numbered, append-only (0001 in place)
│
├── examples/                      # standalone example programs
│   ├── README.md
│   ├── hello-trace/               # minimal trace export
│   ├── http-server/               # tracing in an HTTP server
│   └── (more added over time)
│
├── gen/                           # Generated upb C accessors (committed)
│   ├── README.md                  # explains regeneration; references third_party/upb/
│   └── opentelemetry/             # mirrors proto/ directory tree
│
├── include/microtel/              # Public C++ headers (the API surface)
│   ├── tracer.hpp                 # public Tracer / Span API
│   ├── provider.hpp               # public Provider API
│   ├── sdk_builder.hpp            # SdkBuilder fluent API
│   ├── resource.hpp               # public Resource type
│   ├── status.hpp                 # ForceFlush/Shutdown status types
│   ├── error.hpp                  # microtel::Error, ConfigError
│   ├── version.hpp                # version constants (auto-generated)
│   ├── log_sink.hpp               # SetLogSink hook
│   └── internal/                  # M0 deliverable; interface stubs
│       ├── transport.hpp          # Transport interface
│       ├── otlp_encoder.hpp       # OtlpEncoder interface
│       ├── exporter.hpp           # Exporter interface
│       ├── sampler.hpp            # Sampler interface
│       ├── processor.hpp          # SpanProcessor interface
│       └── (more as needed)
│
├── proto/                         # Vendored OTel protos (pinned to a tag)
│   ├── README.md                  # which tag, how to update
│   └── opentelemetry/             # upstream tree, unchanged
│
├── python/                        # Python bindings (M8 deliverable; empty in M0)
│   ├── pyproject.toml
│   ├── README.md
│   ├── src/microtel/              # Python package
│   ├── bindings/                  # nanobind C++
│   └── tests/                     # Python-side tests
│
├── packaging/                     # OS package builds (M9 deliverable; empty in M0)
│   ├── deb/
│   ├── rpm/
│   └── README.md
│
├── shims/                         # Compatibility shims (experimental, M9; empty in M0)
│   ├── README.md                  # explicitly experimental
│   ├── otelcpp/                   # opentelemetry-cpp API shim
│   └── python/                    # opentelemetry-python API shim
│
├── src/                           # Implementation (begins M2; mostly empty in M0)
│   ├── api/                       # Track A — public API implementation
│   │   └── README.md              # what lives here, owned-by, depends-on, tests, style notes
│   ├── sdk/                       # Track A — SDK implementation
│   │   └── README.md
│   ├── exporter/                  # Batching, OTLP encoding (protocol-agnostic)
│   │   └── README.md
│   ├── wire/
│   │   ├── encoder/               # Track F — OtlpEncoder; only place upb is touched directly
│   │   │   └── README.md
│   │   ├── http/                  # Track B — OTLP/HTTP-protobuf codec
│   │   │   └── README.md
│   │   └── grpc/                  # Track C — OTLP/gRPC codec on nghttp2
│   │       └── README.md
│   ├── transport/                 # Track D — Transport interface; nghttp2 + OpenSSL
│   │   └── README.md
│   └── common/                    # logging, errors, limits, RAII wrappers, config
│       ├── README.md
│       ├── config/                # Track E — config parser and validation
│       └── raii/                  # custom RAII wrappers (Socket, SslCtx, etc.)
│
├── tests/                         # All tests (begins M2; mostly empty in M0)
│   ├── README.md                  # test organization and conventions
│   ├── unit/                      # gtest unit tests, mirrors src/ structure
│   ├── integration/               # multi-component flows; uses real components + collector
│   ├── conformance/               # against a real OTel Collector
│   ├── wire/                      # protocol byte-level tests
│   ├── grpc-wire/                 # specifically for gRPC-on-nghttp2 edge cases
│   ├── fuzz/                      # libFuzzer targets
│   ├── mocks/                     # interface mocks for cross-component testing
│   └── fakes/                     # interface fakes (smarter than mocks)
│
├── third_party/                   # Vendored third-party code
│   ├── upb/                       # OTLP wire encoder; pinned commit
│   │   ├── README.md              # SHA, license, update procedure
│   │   ├── LICENSE                # upstream license file
│   │   └── (upstream tree, pinned)
│   └── tl-expected/               # std::expected polyfill — see ICP 0002
│       ├── README.md              # SHA, license, update procedure
│       ├── LICENSE                # CC0-1.0 dedication
│       └── tl/expected.hpp        # single header, vendored at v1.3.1
│
├── .clang-format                  # M0 deliverable; layout rules
├── .clang-tidy                    # M0 deliverable; lint rules per coding-standards.md
├── .editorconfig                  # editor consistency
├── .gitignore                     # standard CMake/build/Python ignores
├── .gitattributes                 # line-ending rules
├── CMakeLists.txt                 # top-level build entry point
├── CMakePresets.json              # CMake configure presets used by CI
├── CLAUDE.md                      # agent instructions; durable rules
├── CODEOWNERS                     # review-request routing
├── CHANGELOG.md                   # added at M9; pre-1.0 versions appended as we go
├── CONTRIBUTING.md                # how to contribute; references CLAUDE.md and coding-standards.md
├── LICENSE                        # Apache 2.0
├── NOTICE                         # third-party attribution per Apache 2.0 §4
├── README.md                      # project overview; links to spec and roadmap
├── SECURITY.md                    # vulnerability disclosure policy
├── THIRD_PARTY_NOTICES.md         # auto-generated from third_party/*/README.md
├── microtel-spec.md               # v1 specification
└── microtel-roadmap.md            # multi-year roadmap
```

---

## 2. State by milestone

### What exists at the **start of M0**

Just the planning documents, nothing else:

```
microtel/
├── CLAUDE.md
├── README.md (stub: "see microtel-spec.md")
├── LICENSE
├── microtel-spec.md
└── microtel-roadmap.md
```

### What exists at the **end of M0**

Architecture docs and compilable interface header stubs. Still no source code:

```
microtel/
├── CLAUDE.md
├── CODEOWNERS
├── CONTRIBUTING.md
├── LICENSE
├── NOTICE                                  # placeholder for third-party attributions
├── README.md
├── SECURITY.md
├── microtel-spec.md
├── microtel-roadmap.md
├── .clang-format
├── .clang-tidy
├── .editorconfig
├── .gitattributes
├── .gitignore
├── .github/
│   ├── PULL_REQUEST_TEMPLATE.md
│   └── ISSUE_TEMPLATE/
│       ├── bug_report.md
│       ├── feature_request.md
│       ├── documentation.md
│       └── config.yml
├── ci/
│   └── sonar-project.properties
├── docs/
│   ├── architecture.md
│   ├── threading-model.md
│   ├── memory-model.md
│   ├── error-model.md
│   ├── interfaces.md                       # signed off
│   ├── coding-standards.md
│   ├── grpc-wire-protocol.md
│   ├── repository-layout.md
│   ├── ci-architecture.md
│   ├── branch-protection.md
│   ├── bench-spec.md                       # at v0.1; updated before M7
│   ├── configuration.md                    # per-setting precedence (spec §12.1)
│   ├── development.md                      # track-to-directory atlas (spec §13.3)
│   ├── sequences/
│   │   ├── connection-establishment.md
│   │   ├── retry-after-failure.md
│   │   ├── goaway-handling.md
│   │   ├── shutdown-drain.md
│   │   ├── fork-survival.md
│   │   ├── backpressure-and-drop.md
│   │   ├── partial-success.md
│   │   └── grpc-trailer-only-and-multi-frame.md
│   └── icps/
│       ├── README.md                       # ICP template and process
│       └── 0001-m0-deliverables-clarification.md
├── include/microtel/
│   ├── tracer.hpp                          # public API headers (full Doxygen, abstract)
│   ├── provider.hpp
│   ├── sdk_builder.hpp
│   ├── resource.hpp
│   ├── status.hpp
│   ├── error.hpp
│   ├── log_sink.hpp
│   └── internal/                           # interface stubs (compile, no impls)
│       ├── transport.hpp
│       ├── otlp_encoder.hpp
│       ├── exporter.hpp
│       ├── sampler.hpp
│       └── processor.hpp
└── CMakeLists.txt                          # minimal: declares header-only target
```

### What exists at the **end of M1 (Spike)**

The M0 tree, plus a `spike/` directory containing throwaway code that exercises nghttp2 + protobuf to a real collector. The spike code is **not** in `src/` — it's in its own directory, clearly marked as throwaway, and the issues it surfaces are recorded as M0 ICPs.

```
microtel/
├── (everything from M0)
└── spike/
    ├── README.md                           # "throwaway code; do not depend on this"
    ├── CMakeLists.txt
    └── main.cpp                            # nghttp2 + protobuf to a collector
```

### What exists at the **end of M2 (Skeleton & contracts)**

The full project skeleton: every directory in §1 exists, with `README.md` files and stub implementations sufficient to make tests-with-mocks pass. After M2, parallel implementation tracks unblock and source files start populating `src/`.

### What exists at the **end of M10 (v1.0 release)**

The full intended layout from §1, populated.

---

## 3. Ownership: which file goes WHERE

A reverse map. If you're creating a new file and unsure where to put it, find the row that fits.

| File type | Goes in | Notes |
|---|---|---|
| Public C++ header | `include/microtel/` | API consumers see these |
| Internal C++ header | `include/microtel/internal/` (interfaces) or `src/<area>/` (implementation-only) | interfaces are the locked contracts |
| C++ source file | `src/<area>/` matching the area | one `.cpp` per `.hpp` typically |
| Custom RAII wrapper | `src/common/raii/` | only place to put these |
| Test (unit) | `tests/unit/<area>/` | mirrors `src/` structure |
| Test (integration) | `tests/integration/` | spans multiple components |
| Test (conformance) | `tests/conformance/` | runs against real collector |
| Test (wire-level) | `tests/wire/` or `tests/grpc-wire/` | byte-level protocol tests |
| Test (fuzz) | `tests/fuzz/` | libFuzzer targets |
| Mock for an interface | `tests/mocks/` | dumb — no logic |
| Fake (logic-bearing) | `tests/fakes/` | when mocks aren't enough |
| Sequence diagram | `docs/sequences/` | one Markdown file per flow |
| Architecture decision | `docs/icps/NNNN-name.md` | numbered, append-only |
| New top-level doc | `docs/` | reference from README.md or spec |
| CI workflow | `.github/workflows/` | one YAML per workflow |
| CI helper script | `ci/scripts/` | reusable bash/python |
| CI config (SonarQube etc.) | `ci/` directly | top-level CI configs |
| Build config (CMake) | `cmake/` for modules; `CMakeLists.txt` for build entry | |
| Generated proto code | `gen/` | committed; CI verifies zero-diff |
| Vendored dependency | `third_party/<name>/` | with README.md, LICENSE, SHA |
| OS packaging | `packaging/{deb,rpm}/` | M9 deliverable |
| Python binding | `python/bindings/` | nanobind C++ |
| Python package | `python/src/microtel/` | the importable Python tree |
| Python tests | `python/tests/` | separate from C++ tests |
| Compat shim | `shims/{otelcpp,python}/` | separate experimental packages |
| Example program | `examples/<example-name>/` | each example is self-contained |

---

## 4. What's committed vs generated

**Committed** (lives in git):

- All `.cpp`, `.hpp`, `.md`, `.cmake`, YAML, JSON config files.
- `gen/` — generated upb code is committed and CI verifies zero-diff against regeneration.
- `third_party/upb/` — vendored at a pinned commit.
- `third_party/tl-expected/` — vendored single-header `tl::expected` polyfill (CC0-1.0). Wrapped as `microtel::Expected` per [ICP 0002](icps/0002-vendor-tl-expected.md). Header-only; zero runtime closure impact.
- `proto/` — vendored OTel protos.
- `THIRD_PARTY_NOTICES.md` — generated by CI but committed for releases.
- Status badges and badge URLs in README.md.

**Generated, not committed** (in `.gitignore`):

- `build/` — CMake out-of-source build output.
- `build-*/` — CI-specific build configurations.
- Coverage reports (`*.gcov`, `*.gcda`, `*.profraw`).
- Compile commands (`compile_commands.json` is symlinked from `build/`).
- Python eggs, dist artifacts (`*.egg-info`, `dist/`, `wheels/`).
- IDE files (`.vscode/settings.json` if user-specific; `.vscode/launch.json` shared OK).
- ASAN/TSAN/UBSAN output logs.

**Generated and committed** (the regeneration is verified in CI):

- `gen/opentelemetry/**/*.upb*.{c,h}` — upb generates these from `proto/`. CI runs `make regen-protos` and fails on diff.
- `include/microtel/version.hpp` — generated from `CMakeLists.txt` at configure time. *Either* committed with placeholder values *or* generated into `build/`; decision lands in M2.
- `THIRD_PARTY_NOTICES.md` — generated from `third_party/*/README.md` license entries during release builds.

---

## 5. Per-directory READMEs

Every directory under `src/` and several others have a `README.md`. The convention is one screen of plain text covering:

- **Purpose.** What lives here.
- **Owner.** Per CODEOWNERS.
- **Implements.** Which interfaces from `docs/interfaces.md`.
- **Depends on.** Other directories or external libraries.
- **Mocks live in.** Path to mocks for downstream consumers.
- **Test entry points.** Which test directories cover this code.
- **Style notes.** Anything specific to this area.

Example template (saved at `docs/templates/directory-readme.md` for reference):

```markdown
# src/wire/grpc/

## Purpose
OTLP/gRPC codec implementation on nghttp2. Handles the unary RPC wire protocol —
framing, headers, trailers, status mapping — without linking the gRPC library.

## Owner
@TBD-username (Track C — OTLP/gRPC wire)

## Implements
- `microtel::internal::WireCodec` (the gRPC variant)

## Depends on
- `src/wire/encoder/` for protobuf serialization
- `src/transport/` for nghttp2 session and HTTP/2 framing
- `src/common/` for logging, errors, RAII wrappers
- nghttp2 library (system or vendored)

## Mocks live in
- `tests/mocks/wire/grpc/` — for components that consume this codec

## Test entry points
- `tests/unit/wire/grpc/` — codec unit tests
- `tests/grpc-wire/` — protocol-level tests including misbehaving servers
- `tests/fuzz/grpc-trailer/` — fuzz targets for trailer parsing

## Style notes
- All upb usage stays in `src/wire/encoder/`. This directory uses the wrapper
  C++ API only — no `upb_*` symbols here.
- HTTP/2 frame parsing follows the conventions documented in
  `docs/grpc-wire-protocol.md`.
```

This is a contract for both human contributors and AI coding agents — agents can pick up a task in this directory after reading just the directory README plus the relevant interface header, without needing to read the entire codebase.

---

## 6. Maintenance

This document is updated whenever:

- A new top-level directory is added.
- A new file conventionally goes somewhere this doc doesn't cover.
- The "what's committed vs generated" rules change.

If you're adding something and the existing rules are ambiguous, propose an update to this doc as part of the same PR.
