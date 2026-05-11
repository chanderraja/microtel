# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# M6-A — Configuration layer: TOML parsing, env-var overlay, validation.
# Implements the IAuthProvider realisations (M6-B, added in a later commit).
#
# Owner: Track E (Configuration).

add_library(microtel_config STATIC
    toml_loader.cpp
    env_resolver.cpp
    config_validator.cpp)

target_link_libraries(microtel_config
    PUBLIC  microtel_headers
    PRIVATE tomlplusplus::tomlplusplus)

# src/ is on the private include path so #include "config/..." resolves.
target_include_directories(microtel_config
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_features(microtel_config PUBLIC cxx_std_20)

target_compile_options(microtel_config PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

if(MICROTEL_FORBID_INSECURE_TLS)
    target_compile_definitions(microtel_config
        PRIVATE MICROTEL_FORBID_INSECURE_TLS=1)
endif()
