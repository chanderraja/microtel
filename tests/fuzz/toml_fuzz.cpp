// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

// Fuzz harness for the microtel.toml parser.
//
// Exercises: TOML syntax parsing (via toml++), unknown-key detection in all
// three modes (error / warn / ignore), field validation and enum coercion
// (protocol, compression, drop_policy, unknown_keys), all section/key paths
// ([exporter], [exporter.headers], [service], [resource], [tls], [sdk],
// [timeouts], [config]).

#include "common/config/toml_loader.hpp"

#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const std::string_view input{reinterpret_cast<const char*>(data), size};
    (void)microtel::config::ParseTomlString(input);
    return 0;
}
