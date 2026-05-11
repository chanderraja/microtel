// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <iosfwd>

namespace tools
{

/// @brief Process exit codes returned by `RunPreflight`.
inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 1;   ///< bad argument / missing flag
inline constexpr int kExitConfig = 2;  ///< configuration validation failure
inline constexpr int kExitRuntime = 3; ///< connect or export network failure

/// @brief Run the preflight check. Returns a process exit code.
///
/// Parses `--preflight={connect|export}` from argv, builds a `Provider` from
/// the optional config-file argument plus OTel env-var overlay, then:
///
/// - **connect**: calls `Provider::Connect()` and prints the outcome.
/// - **export**: calls `Provider::Connect()`, sends one synthetic span, calls
///   `Provider::ForceFlush()`, and prints the outcome.
///
/// @param argc  argument count (same as `main`'s argc).
/// @param argv  argument vector (same as `main`'s argv).
[[nodiscard]] int RunPreflight(int argc, char** argv);

/// @brief Injectable-stream overload for unit testing.
[[nodiscard]] int RunPreflight(int argc, char** argv, std::ostream& out, std::ostream& err);

}  // namespace tools
