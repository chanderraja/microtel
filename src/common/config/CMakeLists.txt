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
    table_merge.cpp
    concentrator_config.cpp
    config_validator.cpp
    auth_providers.cpp)

# toml++ is wrapped in $<BUILD_INTERFACE:> so it does not reach the install
# export set. That is not a workaround — it is the accurate statement:
# microtel_tomlplusplus (root CMakeLists.txt) is toml++'s headers with
# TOML_HEADER_ONLY=1, whichever way toml++ was found, so the code used is
# absorbed into this archive at compile time. libmicrotel_config.a carries no
# undefined toml symbol, and a consumer of the installed package needs nothing
# from toml++. Without the wrap, a PRIVATE dependency of a STATIC library
# still lands in the exported link interface as $<LINK_ONLY:…>, and
# install(EXPORT) rejects it because microtel does not install that target.
target_link_libraries(microtel_config
    PUBLIC  microtel_headers
    PRIVATE $<BUILD_INTERFACE:microtel_tomlplusplus>)

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
