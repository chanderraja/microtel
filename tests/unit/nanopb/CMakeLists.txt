# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# The vendored nanopb runtime and the generated OTLP trace descriptors
# (third_party/nanopb/, gen/nanopb/). A plain C11 program rather than a gtest
# binary: the leaf's nanopb backend is C, so the headers and the rename header
# are checked in the language that consumes them.

add_executable(nanopb_encode_test nanopb_encode_test.c)
target_link_libraries(nanopb_encode_test PRIVATE microtel_nanopb_gen)
set_target_properties(nanopb_encode_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
target_compile_options(nanopb_encode_test PRIVATE
    -pedantic-errors -Wall -Wextra -Werror)
add_test(NAME nanopb_encode_test COMMAND nanopb_encode_test)
set_tests_properties(nanopb_encode_test PROPERTIES LABELS "unit")
