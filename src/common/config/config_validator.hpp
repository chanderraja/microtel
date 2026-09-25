// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"

#include "common/config/config.hpp"

#include <optional>
#include <string_view>

namespace microtel::config
{

/// @brief Why a `BatchOptions` was refused.
///
/// Both views point at string literals, so a fault costs no allocation and
/// outlives any call.
struct BatchOptionsFault
{
    std::string_view field;    ///< the `microtel.toml` key, e.g. `sdk.max_queue_size`
    std::string_view message;  ///< what is wrong with it
};

/// @brief The one `BatchOptions` rule set (issue #267).
///
/// Shared by `Validate` (hence `SdkBuilder::Build` and `microtel-preflight`)
/// and `Provider::SetBatchOptions`, so the builder and the setter accept and
/// reject exactly the same values. Rejects, checked in this order:
///   - `max_queue_size == 0` (every record is refused on arrival);
///   - `max_export_batch_size == 0` (the queue never drains);
///   - `max_export_batch_size > max_queue_size`;
///   - `schedule_delay <= 0ms` (the worker's wait turns into a spin).
///
/// @return The first rule @p opts breaks, or `std::nullopt` if it is coherent.
[[nodiscard]] std::optional<BatchOptionsFault> CheckBatchOptions(const BatchOptions& opts) noexcept;

/// @brief Resolve the remaining defaults in a Config and validate the result.
///
/// Performs eager validation without network access:
///   - Endpoint URL structure and scheme.
///   - Protocol against the endpoint scheme (spec §12.2): a `grpc://` or
///     `grpcs://` endpoint selects `Protocol::Grpc` unless the user named a
///     protocol, and is rejected with `ProtocolMismatch` when the protocol
///     they named is `http`. `http://` and `https://` say nothing about the
///     protocol and leave it alone.
///   - Path rejection for gRPC endpoints (spec §12.2).
///   - `insecure = true` rejection when the build sets
///     `MICROTEL_FORBID_INSECURE_TLS=ON` (spec §12.3).
///   - TLS file readability (ca_bundle, client_cert, client_key).
///   - mTLS key-cert pairing (both or neither).
///   - Batch: `CheckBatchOptions` — non-zero queue and batch sizes,
///     max_export_batch_size ≤ max_queue_size, positive schedule_delay.
///
/// On success `cfg.endpoint` holds the parsed URL components, `cfg.protocol`
/// holds the resolved protocol, and `cfg.service_name` holds `unknown_service`
/// if nothing else supplied one (OTel resource semantic conventions).
/// Returns the first validation failure encountered.
///
/// @param cfg Config to resolve and validate. Mutated in place as resolution
///        proceeds; a later failure does not roll the earlier steps back,
///        because a Config that failed validation is discarded by its caller.
[[nodiscard]] microtel::Expected<void, ConfigError> Validate(Config& cfg);

}  // namespace microtel::config
