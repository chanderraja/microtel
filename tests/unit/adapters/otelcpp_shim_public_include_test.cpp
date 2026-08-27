// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Regression test for the shim's public include path: this TU deliberately
// adds no include directory of its own beyond what linking
// microtel_otelcpp_shim provides (see this file's CMakeLists.txt entry —
// no target_include_directories call). Every other shim test manually adds
// ${CMAKE_SOURCE_DIR}/src; if microtel_otelcpp_shim ever stops exposing
// src/ publicly, this is the file that fails to compile.

#include "adapters/otelcpp/global_registration.hpp"
#include "fakes/fake_provider.hpp"

#include <gtest/gtest.h>

#include <memory>

TEST(OtelCppShimPublicIncludePath, ShimHeadersResolveWithNoExtraIncludeDirs)
{
    auto provider = std::make_shared<microtel::testing::FakeProvider>();

    microtel::adapters::otelcpp::RegisterGlobally(provider);
    microtel::adapters::otelcpp::UnregisterGlobally();

    SUCCEED();
}
