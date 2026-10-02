// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/provider.hpp"
#include "microtel/status.hpp"

#include <iosfwd>
#include <string>
#include <string_view>

namespace tools
{

/// @brief What the synthetic `microtel.preflight` span reports about itself.
///
/// `Provider` exposes no accessor for its resolved runtime configuration, so
/// a built provider's `Resource` cannot be read back and the span's identity
/// cannot be observed after the fact (issue #209; a `Provider` accessor is
/// v1.1 work). This struct is the seam instead: `RunPreflight` resolves it
/// once and then uses it for *both* the provider's service name and the
/// span's `microtel.protocol` attribute, so a test that asserts on it is
/// asserting on the values the span actually carries rather than on a
/// parallel copy of the same logic.
struct SpanIdentity
{
    /// @brief `service.name` placed on the span's resource.
    ///
    /// Always `"microtel-preflight"` — the value
    /// exists so a collector rule can drop preflight traffic, which it cannot
    /// do if the config file under test is allowed to rename it.
    std::string service_name;

    /// @brief `microtel.protocol` attribute: `"grpc"` or `"http"`.
    ///
    /// The protocol the configuration actually resolves to, not a fixed
    /// string: a preflight run against a gRPC endpoint has to say `grpc`.
    std::string protocol;

    /// @brief The version preflight reports: its `microtel.version` attribute
    ///        and tracer version.
    ///
    /// Taken from the hand-maintained literal in `preflight.cpp`, which has no
    /// compile-time guard against `microtel::kVersionString` and once went
    /// unbumped for several releases (`RELEASING.md` §1). Exposed here so a
    /// test can pin the two together.
    std::string version;
};

/// @brief Resolve what the synthetic span reports.
///
/// Runs the same file → environment → validate chain `SdkBuilder::Build()`
/// runs, because there is no way to ask the built `Provider` what it resolved.
/// preflight applies no exporter-level code override, so the protocol this
/// computes and the protocol the provider uses agree by construction.
///
/// Resolution failures are swallowed: they are `Build()`'s to report, with a
/// message naming the offending field, and this function is called before it.
/// A failed resolution yields the defaults, and no span is ever sent in that
/// case because `Build()` fails first.
///
/// @param config_path `microtel.toml` to resolve against; empty for none.
[[nodiscard]] SpanIdentity ResolveSpanIdentity(std::string_view config_path);

/// @brief Process exit codes returned by `RunPreflight`.
inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 1;   ///< bad argument / missing flag
inline constexpr int kExitConfig = 2;  ///< configuration validation failure
/// connect failed, or export failed or was rejected (a failed batch, or a
/// span the collector rejected via OTLP partial success)
inline constexpr int kExitRuntime = 3;

/// @brief Turn an export run's flush status and exporter health into the
///        `--preflight=export` verdict, printed to @p out or @p err.
///
/// The seam between the run and its verdict, so a unit test can hand it a
/// rejected batch without a collector that rejects one.
///
/// `export OK` requires all three: the flush returned `Completed`,
/// `batches_failed` is 0, and the `PartialSuccessRejection` drop counter is 0.
/// A collector answering OTLP partial success accepts the request but rejects
/// some or all of its items; the exporter counts that batch as sent, so only
/// the drop counter shows the rejection.
///
/// @param flush   what `Provider::ForceFlush()` returned.
/// @param health  `Provider::GetExporterHealth()`, read after the flush.
/// @param out     receives `export OK` on success.
/// @param err     receives the `error: ...` line on failure.
/// @return `kExitOk` or `kExitRuntime`.
[[nodiscard]] int ReportExport(microtel::Status flush,
                               const microtel::HealthSnapshot& health,
                               std::ostream& out,
                               std::ostream& err);

/// @brief Run the preflight check. Returns a process exit code.
///
/// Parses `--preflight={connect|export}` from argv, builds a `Provider` from
/// the optional config-file argument plus OTel env-var overlay, then:
///
/// - **connect**: calls `Provider::Connect()` and prints the outcome.
/// - **export**: calls `Provider::Connect()`, sends one synthetic span, calls
///   `Provider::ForceFlush()`, and prints the outcome (see `ReportExport`):
///   OK only if the flush completed, no batch failed, and the collector
///   rejected no span via OTLP partial success.
///
/// @param argc  argument count (same as `main`'s argc).
/// @param argv  argument vector (same as `main`'s argv).
[[nodiscard]] int RunPreflight(int argc, char** argv);

/// @brief Injectable-stream overload for unit testing.
[[nodiscard]] int RunPreflight(int argc, char** argv, std::ostream& out, std::ostream& err);

}  // namespace tools
