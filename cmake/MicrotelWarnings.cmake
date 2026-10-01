# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# MICROTEL_WARNINGS_AS_ERRORS: whether the project's own targets promote
# warnings to errors. Default ON, so developer and CI builds are unchanged. A
# package manager (vcpkg, Conan, a distro) turns it OFF so that a newer
# compiler's new warning cannot break the package build.
#
# Defined once here and included by every project that compiles microtel
# sources: the top-level CMakeLists.txt, the standalone leaf (cmake -S leaf)
# and the leaf target tests (cmake -S tests/leaf/target). In-tree, option()
# finds the cached value and leaves it alone.
#
# Targets put ${MICROTEL_WERROR} after their -W flags; it expands to -Werror
# or to nothing. The warning flags themselves stay on either way.
# CMAKE_COMPILE_WARNING_AS_ERROR would do this natively, but needs CMake 3.24
# and the project's minimum is 3.20.

include_guard(GLOBAL)

option(MICROTEL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if(MICROTEL_WARNINGS_AS_ERRORS)
    set(MICROTEL_WERROR -Werror)
else()
    set(MICROTEL_WERROR "")
endif()
