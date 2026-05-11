// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "common/config/config.hpp"

#include "microtel/error.hpp"
#include "microtel/expected.hpp"

#include <filesystem>
#include <string_view>

namespace microtel::config
{

/// @brief Parse a microtel.toml document from a string.
///
/// Processes strict-mode unknown-key detection, field validation (protocol,
/// drop_policy, unknown_keys mode), and populates a `Config`. All sections
/// are optional — absent keys keep their `Config` defaults.
///
/// Used directly by tests and internally by `LoadToml`.
///
/// @return Config on success.
/// @return ConfigError on parse failure, unknown-key violation, or
///         unrecognised field value.
[[nodiscard]] microtel::Expected<Config, ConfigError>
ParseTomlString(std::string_view content);

/// @brief Load and parse a microtel.toml file.
///
/// @param path Path to the TOML config file.
/// @return Config on success.
/// @return ConfigError::Kind::FileNotFound if the file does not exist.
/// @return ConfigError::Kind::FileParseFailure on TOML syntax error.
/// @return Any other ConfigError from `ParseTomlString`.
[[nodiscard]] microtel::Expected<Config, ConfigError>
LoadToml(const std::filesystem::path& path);

}  // namespace microtel::config
