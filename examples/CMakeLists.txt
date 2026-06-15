# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# examples/ — standalone, runnable demonstrations of the public microtel API.
# Built only when MICROTEL_BUILD_EXAMPLES=ON (see top-level CMakeLists.txt and
# examples/README.md). Linking microtel_sdk transitively provides the public
# headers (via microtel_headers) and all runtime link dependencies.

add_executable(microtel_example_basic_trace basic_trace/main.cpp)

target_link_libraries(microtel_example_basic_trace PRIVATE microtel_sdk)

target_compile_features(microtel_example_basic_trace PRIVATE cxx_std_20)

target_compile_options(microtel_example_basic_trace PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
