# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# M6-B unit tests: StaticHeadersAuthProvider and CallbackAuthProvider.

microtel_add_test(
    NAME    auth_providers_test
    SOURCES auth_providers_test.cpp
    LIBS    microtel_config)

target_include_directories(auth_providers_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
