// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string_view>

namespace microtel::adapters
{

/// @name OTel semantic-convention keys for source-code location.
///
/// The log bridges attach these to each `LogRecord` when the native logging
/// library reports where the call was made. They are the current (stable)
/// `code.*` names; the older `code.filepath`, `code.lineno` and
/// `code.function` are deprecated in the semantic conventions and are not
/// emitted.
///
/// @see https://opentelemetry.io/docs/specs/semconv/registry/attributes/code/
/// @{

/// Source file path of the logging call site (string).
inline constexpr std::string_view kCodeFilePath = "code.file.path";

/// Line number of the logging call site (int).
inline constexpr std::string_view kCodeLineNumber = "code.line.number";

/// Function containing the logging call site, as qualified as the native
/// library can report it (string).
inline constexpr std::string_view kCodeFunctionName = "code.function.name";

/// @}

}  // namespace microtel::adapters
