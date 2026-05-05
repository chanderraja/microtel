// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>

namespace microtel
{

/// @brief Runtime error description for non-init paths.
///
/// Used as the `E` in `microtel::Expected<T, Error>` returned from non-init
/// APIs, and as the carried payload in internal `WireResult::error` for
/// non-success cases. The message is short, pre-redacted, and safe to log at
/// any level.
///
/// @see docs/error-model.md §4.1
class Error
{
public:
    /// @brief Coarse classification of the error.
    enum class Kind : std::uint8_t
    {
        Unspecified = 0,
        Network = 1,            ///< socket / TLS / nghttp2 transport error
        Protocol = 2,           ///< OTLP wire failure (status interpretation)
        ResourceExhausted = 3,  ///< peer signalled overload; retryable per RetryInfo
        Cancelled = 4,          ///< local cancel (timeout, shutdown)
        Malformed = 5,          ///< unparseable response or trailer
        InternalFailure = 6,    ///< microtel internal bug; should not occur
    };

    Kind kind = Kind::Unspecified;
    std::string message;  ///< short, redacted, safe to log
    int os_errno = 0;     ///< optional OS errno or library code; 0 if unset
};

/// @brief Initialisation-time error description.
///
/// Used only as the `E` in `microtel::Expected<Provider, ConfigError>`
/// returned from `SdkBuilder::Build()` and analogous init paths. The optional
/// `field` carries a dotted path identifying the offending setting.
///
/// @see docs/error-model.md §4.2
/// @see docs/error-model.md §8 (init-failure taxonomy)
class ConfigError
{
public:
    /// @brief Coarse classification of the configuration failure.
    enum class Kind : std::uint8_t
    {
        Unspecified = 0,
        InvalidValue = 1,     ///< setting parsed but failed validation
        UnknownKey = 2,       ///< strict-mode unknown TOML key
        EnvParseFailure = 3,  ///< `OTEL_*` / `MICROTEL_*` env var malformed
        FileNotFound = 4,
        FileParseFailure = 5,       ///< TOML syntax error
        TlsMaterialUnreadable = 6,  ///< CA bundle / cert / key not openable or invalid
        EndpointMalformed = 7,
        ProtocolMismatch = 8,       ///< explicit protocol disagrees with URL scheme
        InsecureDisallowed = 9,     ///< `MICROTEL_FORBID_INSECURE_TLS=ON` and `insecure=true`
        BuildAlreadyConsumed = 10,  ///< `SdkBuilder::Build()` called twice
    };

    Kind kind = Kind::Unspecified;
    std::string field;    ///< dotted path; empty if not field-bound
    std::string message;  ///< human-readable, safe to log
};

}  // namespace microtel
