// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "common/config/config.hpp"

#include "microtel/error.hpp"
#include "microtel/expected.hpp"

namespace microtel::config
{

/// @brief Validate a resolved Config and parse the endpoint URL.
///
/// Performs eager validation without network access:
///   - Endpoint URL structure and scheme.
///   - Path rejection for gRPC endpoints (spec §12.2).
///   - TLS file readability (ca_bundle, client_cert, client_key).
///   - mTLS key-cert pairing (both or neither).
///   - Batch: max_export_batch_size ≤ max_queue_size.
///
/// On success, `cfg.endpoint` is populated with the parsed URL components.
/// Returns the first validation failure encountered.
///
/// @param cfg Config to validate (cfg.endpoint mutated on success).
[[nodiscard]] microtel::Expected<void, ConfigError> Validate(Config& cfg);

}  // namespace microtel::config
