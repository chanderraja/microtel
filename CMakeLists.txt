# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# M0 minimal build: declares a header-only INTERFACE target plus an optional
# header-check executable that includes every public and internal header.
# Implementation begins in M3; see microtel-roadmap.md.

cmake_minimum_required(VERSION 3.20)

project(microtel
        VERSION 1.0.0
        LANGUAGES C CXX
        DESCRIPTION "Lightweight OpenTelemetry-compatible trace runtime built on nghttp2")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# bindir / libdir / includedir for the install rules at the foot of this file.
# Included up here because the $<INSTALL_INTERFACE:> expressions below need
# CMAKE_INSTALL_INCLUDEDIR.
include(GNUInstallDirs)

# Header-only target — exposes include/ and the vendored tl::expected to
# consumers and to the M0 header check. Per ICP 0002 the public-API
# `microtel::Expected<T, E>` aliases to `tl::expected` on C++20 and to
# `std::expected` on C++23.
#
# In an installed tree the vendored tl::expected moves to
# <includedir>/microtel/vendor/tl/expected.hpp, so `microtel/vendor` — not
# `microtel` — goes on the include path: expected.hpp says
# `#include "tl/expected.hpp"`, and the extra `vendor` level keeps that from
# resolving against a consumer's own tl-expected (or ours from theirs).
add_library(microtel_headers INTERFACE)
target_include_directories(microtel_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/tl-expected>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/microtel/vendor>)
target_compile_features(microtel_headers INTERFACE cxx_std_20)

# M0 header compile check.
#
# Verifies every public and internal header compiles standalone with -Werror.
# Per ICP 0001 this is the operational form of the rule "Both header trees
# compile under an INTERFACE CMake target with -Werror. No implementations."
option(MICROTEL_BUILD_HEADER_CHECK "Build the M0 header compile check" ON)

if(MICROTEL_BUILD_HEADER_CHECK)
    add_executable(microtel_header_check ci/header_check.cpp)
    target_link_libraries(microtel_header_check PRIVATE microtel_headers)
    target_compile_options(microtel_header_check PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()

# Sanitizer build helper. MICROTEL_SANITIZER ∈ { '', asan, tsan, ubsan }.
# Empty (default) means no sanitizer. Per spec §14.2 / docs/coding-standards.md
# §14.2, asan + tsan + ubsan must be clean on the full test suite for v1.0.
set(MICROTEL_SANITIZER "" CACHE STRING
    "Sanitizer to enable: '', asan, tsan, ubsan")
set_property(CACHE MICROTEL_SANITIZER PROPERTY STRINGS "" asan tsan ubsan)

if(NOT MICROTEL_SANITIZER STREQUAL "")
    if(MICROTEL_SANITIZER STREQUAL "asan")
        set(_microtel_san_flags -fsanitize=address -fno-omit-frame-pointer)
    elseif(MICROTEL_SANITIZER STREQUAL "tsan")
        set(_microtel_san_flags -fsanitize=thread -fno-omit-frame-pointer)
    elseif(MICROTEL_SANITIZER STREQUAL "ubsan")
        set(_microtel_san_flags -fsanitize=undefined -fno-omit-frame-pointer)
    else()
        message(FATAL_ERROR
            "MICROTEL_SANITIZER='${MICROTEL_SANITIZER}' invalid; "
            "expected one of: '', asan, tsan, ubsan")
    endif()
    add_compile_options(${_microtel_san_flags})
    add_link_options(${_microtel_san_flags})
    message(STATUS "microtel: ${MICROTEL_SANITIZER} enabled")
endif()

# Coverage build helper. Per spec §14.2 the v1.0 thresholds are 90% line /
# 85% branch on SDK + encoder code and 80% line on transport + exporter;
# `ci/scripts/coverage.sh` enforces all three.
#
# Clang gets **source-based** coverage (`-fprofile-instr-generate
# -fcoverage-mapping`), which is what the gate measures. Its counters are
# attached to source *regions* the front end knows about, so a branch record
# corresponds to a conditional someone wrote. gcov, by contrast, emits an edge
# for the unwind path out of every potentially-throwing call, which put whole-
# tree branch coverage at 57.8% against 91.0% line and made the §14.2 branch
# floor unmeasurable (issue #198).
#
# The gcc branch below is a fallback for a local `--coverage` run; it is not
# what CI gates on. `-fprofile-update=atomic` makes gcov's counter increments
# atomic — without it, multi-threaded tests (log_sink_test, the
# BatchSpanProcessor tests) race on shared counters and produce corrupted
# .gcda files, which lcov surfaces as a negative delta.
option(MICROTEL_COVERAGE "Build with coverage instrumentation" OFF)
if(MICROTEL_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # -fprofile-update=atomic covers the same multi-threaded counter race
        # for llvm's instrprof counters that it does for gcov's.
        add_compile_options(
            -fprofile-instr-generate -fcoverage-mapping
            -fprofile-update=atomic -O0 -g)
        add_link_options(-fprofile-instr-generate)
        message(STATUS "microtel: clang source-based coverage enabled")
    else()
        add_compile_options(--coverage -fprofile-update=atomic -O0 -g)
        add_link_options(--coverage)
        message(STATUS "microtel: gcov coverage enabled")
    endif()
endif()

# spdlog for internal diagnostic logging. Default ON per spec §9.2 + §9.4;
# OFF uses the minimal stderr fallback. Either way, the public LogSink
# hook in include/microtel/log_sink.hpp lets applications redirect into
# their own logger (per docs/error-model.md §9.3).
option(MICROTEL_USE_SPDLOG "Use spdlog for internal diagnostic logs" ON)

if(MICROTEL_USE_SPDLOG)
    include(FetchContent)
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.15.3
        GIT_SHALLOW    TRUE
    )
    # std::format mode — avoids the fmt transitive dependency that
    # microtel-spec.md §9.4 explicitly excludes.
    set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(spdlog)
endif()

# toml++ — TOML config file parser (M6-A, ICP 0004).
# Header-only; linked PRIVATE into microtel_config only.
# Runtime closure is unchanged: zero shared-library symbols added.
include(FetchContent)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
)
set(TOMLPLUSPLUS_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(tomlplusplus)

# opentelemetry-cpp API-adapter shim (M17, ICP 0014) — opt-in, source-only.
#
# Only the header-only *API* is consumed. otel-cpp's SDK and exporters — and
# their protobuf / libcurl dependencies — are never configured or built:
# SOURCE_SUBDIR points at a directory containing no CMakeLists.txt, so
# FetchContent populates the sources without calling add_subdirectory() on
# otel-cpp's own project.
#
# OPENTELEMETRY_STL_VERSION=2020 is load-bearing, not a style preference. Under
# the upstream default (WITH_STL=OFF) the API resolves nostd:: to a vendored
# abseil snapshot shipped inside its own headers — WITH_ABSEIL=OFF does not
# prevent that; only the STL mode does. Pinned by static_assert in
# tests/unit/adapters/otelcpp_config_test.cpp so a change cannot pass silently.
option(MICROTEL_BUILD_OTELCPP_SHIM
       "Build the opentelemetry-cpp API-adapter shim (source-only, ICP 0014)" OFF)

if(MICROTEL_BUILD_OTELCPP_SHIM)
    include(FetchContent)
    FetchContent_Declare(
        opentelemetry_cpp_api
        GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp.git
        GIT_TAG        v1.28.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  api/include
    )
    FetchContent_MakeAvailable(opentelemetry_cpp_api)

    if(TARGET opentelemetry_api)
        message(FATAL_ERROR
                "opentelemetry-cpp's CMake project was added to the build. Only "
                "its header-only API may be consumed. See ICP 0014.")
    endif()

    # SYSTEM: third-party headers must not trip this project's warning settings.
    add_library(microtel_otelcpp_api INTERFACE)
    target_include_directories(microtel_otelcpp_api SYSTEM INTERFACE
        ${opentelemetry_cpp_api_SOURCE_DIR}/api/include)
    target_compile_definitions(microtel_otelcpp_api INTERFACE
        OPENTELEMETRY_STL_VERSION=2020
        OPENTELEMETRY_ABI_VERSION_NO=1)
    target_compile_features(microtel_otelcpp_api INTERFACE cxx_std_20)
endif()

# Compile-time gate: when ON, insecure = true in config causes init failure.
# Default OFF (emit runtime warning only). See microtel-spec.md §12.3.
option(MICROTEL_FORBID_INSECURE_TLS
    "Fail at init if insecure = true is configured" OFF)

# Every vendored upb/utf8_range global is renamed to microtel_* so that a
# consumer who also links a real upb cannot get two definitions and a silent
# static-link selection between them (ICP 0020 Decision 4). The header is
# force-included into every target that compiles a TU reaching a upb header;
# its own comment block carries the rationale and the regeneration recipe.
# `ci/scripts/symbol-scan.sh` fails the build if any global escapes it.
set(MICROTEL_UPB_RENAME_HEADER
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/upb/microtel_upb_rename.h)

# Third-party libraries: upb runtime + utf8_range validator.
# CMake targets: microtel_upb_runtime, microtel_utf8_range.
# These are compiled as C11 with -w (no warnings from vendored code).
add_subdirectory(third_party/utf8_range)
add_subdirectory(third_party/upb)

# Generated upb accessors (protoc-gen-upb output committed under gen/).
# CMake target: microtel_upb_gen. Regen: ci/scripts/regen-protos.sh.
add_subdirectory(gen)

# System dependencies: OpenSSL (TLS) and nghttp2 (HTTP/2 transport).
# Required by src/common/raii/ and src/transport/; also linked by their tests.
find_package(OpenSSL REQUIRED)
# zlib: request compression (`compression = "gzip"`). Already a member of the
# v1 runtime dependency closure per CLAUDE.md rule 12; this wires it up.
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)

# src/ — production code per the track ownership in
# `docs/development.md`. Subdirectories grow as M3+ implementation
# chunks land. Track A (src/sdk/) leads M3.
add_subdirectory(src/common)
add_subdirectory(src/api)
add_subdirectory(src/sdk)
add_subdirectory(src/wire)
add_subdirectory(src/transport)
add_subdirectory(src/exporter)
if(MICROTEL_USE_SPDLOG)
    add_subdirectory(src/adapters/spdlog)
endif()
if(MICROTEL_BUILD_OTELCPP_SHIM)
    add_subdirectory(src/adapters/otelcpp)
endif()
add_subdirectory(tools)

# ---------------------------------------------------------------------------
# Install rules and the exported target set — issue #19, ICP 0020.
#
# Centralised here rather than spread across src/*/CMakeLists.txt because the
# exported names are a compatibility surface (ICP 0020 Decision 2): "what
# microtel ships" should be one readable list, not something reconstructed by
# grepping fifteen directories.
# ---------------------------------------------------------------------------

# The aggregate. `microtel::microtel` is the ONLY supported exported name
# (ICP 0020 Decision 2); every other exported target exists so the static link
# closure resolves and may merge, split or disappear without an ICP.
#
# Linking microtel_sdk alone is deliberate: CMake derives the rest of the
# closure — and its link order — from the real dependency graph, which is the
# whole point of Decision 1. A static closure this wide is exactly what a
# consumer should not have to order by hand.
add_library(microtel INTERFACE)
add_library(microtel::microtel ALIAS microtel)
target_link_libraries(microtel INTERFACE microtel_sdk)

# The shipped set: the aggregate, the header carrier, and the fourteen static
# components. `microtel_preflight_lib` is tool-only and is not exported
# (ICP 0020 "What exists to export"); neither adapter is exported (Decision 3).
set(MICROTEL_EXPORTED_TARGETS
    microtel
    microtel_headers
    microtel_api
    microtel_common
    microtel_config
    microtel_sdk
    microtel_exporter
    microtel_transport
    microtel_otlp_response
    microtel_gzip
    microtel_http_wire
    microtel_grpc_wire
    microtel_encoder
    microtel_upb_gen
    microtel_upb_runtime
    microtel_utf8_range)

# `microtel_sdk` exports as `microtel::sdk`, not `microtel::microtel_sdk`.
# The aggregate is already bare `microtel`, so the regex leaves it alone.
foreach(microtel_target IN LISTS MICROTEL_EXPORTED_TARGETS)
    string(REGEX REPLACE "^microtel_" "" microtel_export_name "${microtel_target}")
    set_target_properties(${microtel_target} PROPERTIES
        EXPORT_NAME "${microtel_export_name}")
endforeach()

install(TARGETS ${MICROTEL_EXPORTED_TARGETS}
    EXPORT MicrotelTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

# The operator CLI from spec §6.4. The binary ships; `microtel_preflight_lib`
# does not, and no target for either is exported — installing an executable
# needs no export entry.
install(TARGETS microtel-preflight
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Public headers, plus `internal/`.
#
# `include/microtel/internal/` is not public API — but public headers include
# it (provider.hpp reaches internal/processor.hpp, sdk_builder.hpp reaches
# internal/sampler.hpp, and so on), so an install that omitted it would ship a
# tree that does not compile. Shipping the files is not a promise about them:
# ICP 0020 Decision 2 draws the supported line at `include/microtel/*.hpp`.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/microtel
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp")

# The vendored tl::expected, under a `vendor/` level of its own so that
# `#include "tl/expected.hpp"` from expected.hpp keeps resolving without the
# installed tree putting a bare `tl/` on a consumer's include path — where it
# would collide with their own copy. See the microtel_headers include
# directories at the top of this file.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tl-expected/tl/expected.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/microtel/vendor/tl)

# License texts (issue #191). Until this rule, `cmake --install` produced
# archives, headers and a binary with no license of any kind — which Apache-2.0
# §4(a)/§4(d) asks for on microtel's own account, and which the vendored upb's
# BSD-3 binary-redistribution clause requires outright, since upb is compiled
# into the archives above.
#
# CMAKE_INSTALL_DATADIR, not DATAROOTDIR: GNUInstallDirs defines DATADIR as the
# read-only architecture-independent data root and defaults it to DATAROOTDIR,
# so the default path is the same `share/` either way — but DATADIR is the
# variable a distro packager retargets, and license text is data, not a
# packaging root. `licenses/microtel/` matches what Fedora, Debian and vcpkg
# already look for.
#
# THIRD_PARTY_NOTICES.md carries the full text of every vendored license, so
# these three files are the whole attribution set; the per-component
# third_party/*/LICENSE files need no separate install rule.
install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
        ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE
        ${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/microtel)

# opentelemetry-cpp shim headers — ICP 0020 Decision 3: headers install, no
# binary and no exported target, permanently. Installed unconditionally
# (i.e. not gated on MICROTEL_BUILD_OTELCPP_SHIM) because the consumer opts in
# from *their* build, which means the headers have to be in the package they
# installed.
#
# They live under src/adapters/otelcpp/ and include each other as
# "adapters/otelcpp/…", so they need a root of their own that is not
# <includedir>: a consumer adds <includedir>/microtel-shim to their include
# path and the self-includes resolve unchanged.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/otelcpp/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/microtel-shim/adapters/otelcpp
    FILES_MATCHING PATTERN "*.hpp")

install(EXPORT MicrotelTargets
    FILE MicrotelTargets.cmake
    NAMESPACE microtel::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/microtel)

# The package config pair that makes `find_package(microtel)` work.
#
# The installed filenames are `microtelConfig.cmake` /
# `microtelConfigVersion.cmake` — lowercase `microtel`, because find_package
# builds the filename from the package name it is given and the documented
# usage is `find_package(microtel …)`. `MicrotelConfig.cmake` would only be
# found by `find_package(Microtel)`.
#
# The version is the project version, 1.0.0, and include/microtel/version.hpp
# agrees with it. Both are hand-set — generating the header from PROJECT_VERSION
# would make a public header a build artifact, which RELEASING.md records as a
# rejected trade. A release bump edits this file, version.hpp, the grpc
# user-agent literal and tools/preflight/preflight.cpp; ci/scripts/version-drift-check.sh
# (CI job version-drift-check) fails any PR in which they stop agreeing.
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/microtelConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/microtelConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/microtel)

# Now that the project is 1.0.0, SameMajorVersion is the right mode: semver
# says a 1.x consumer requirement is satisfied by any later 1.y. ExactVersion
# was correct only below 1.0, where minor bumps are breaking by convention;
# keeping it past the 1.0 cut would fail `find_package(microtel 1.0)` against a
# 1.1 install for no reason.
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/microtelConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/microtelConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/microtelConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/microtel)

# Test tree — gtest-based unit/integration/conformance/wire/grpc-wire/fuzz
# per spec §14.2 / docs/coding-standards.md §11. Default ON; OFF skips the
# whole tree for cross-compilation or constrained builds (spec §14.2).
option(MICROTEL_BUILD_BENCH "Build the benchmark harness (requires Docker)" OFF)

if(MICROTEL_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# Standalone, runnable demonstrations of the public API. Default OFF; the
# examples link microtel_sdk, so they require the libraries to be configured.
option(MICROTEL_BUILD_EXAMPLES "Build the standalone API examples" OFF)

if(MICROTEL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

option(MICROTEL_BUILD_TESTS "Build the test tree" ON)

if(MICROTEL_BUILD_TESTS)
    enable_testing()

    # GoogleTest via FetchContent — pinned to a stable tag. No system
    # dependency required. Test-only; not part of the runtime closure
    # in microtel-spec.md §9.1.
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.17.0
        GIT_SHALLOW    TRUE
    )
    # Don't install gtest as part of microtel's install target.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests)
endif()

# Fuzz harnesses — libFuzzer, clang-only, independent of the gtest tree.
# cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#       -DMICROTEL_BUILD_FUZZ=ON -DMICROTEL_BUILD_TESTS=OFF
option(MICROTEL_BUILD_FUZZ "Build libFuzzer harnesses (requires clang -fsanitize=fuzzer)" OFF)

if(MICROTEL_BUILD_FUZZ)
    add_subdirectory(tests/fuzz)
endif()
