// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The ON half of the `MICROTEL_FORBID_INSECURE_TLS` gate (issue #200).
//
// The macro is a property of how `config_validator.cpp` was compiled, not of
// the test that links it: the default build compiles `microtel_config` with
// the option OFF, so a test linking that archive can only ever observe the OFF
// behaviour. This target therefore compiles the one translation unit that
// carries the check with `MICROTEL_FORBID_INSECURE_TLS=1` (see the
// CMakeLists.txt entry) and asserts the rejection.
//
// The OFF half lives in `config_test.cpp`
// (`ValidateTest.InsecureTls_WithoutForbidOption_Succeeds`), which links the
// default-compiled archive. Both binaries run in the default `ctest` set, so
// both sides of the gate are exercised by an ordinary CI run.

#include "microtel/error.hpp"
#include "microtel/protocol.hpp"

#include "common/config/config.hpp"
#include "common/config/config_validator.hpp"

#include <gtest/gtest.h>

namespace mc = microtel::config;
namespace mt = microtel;

namespace
{

mc::Config MinimalValidConfig()
{
    mc::Config cfg;
    cfg.endpoint_url = "https://collector.internal:4317";
    cfg.protocol = mt::Protocol::Grpc;
    return cfg;
}

}  // namespace

TEST(ForbidInsecureTlsTest, InsecureTrue_ReturnsInsecureDisallowed)
{
    mc::Config cfg = MinimalValidConfig();
    cfg.tls.insecure = true;
    const auto result = mc::Validate(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::InsecureDisallowed);
    EXPECT_EQ(result.error().field, "tls.insecure");
}

TEST(ForbidInsecureTlsTest, InsecureFalse_Succeeds)
{
    mc::Config cfg = MinimalValidConfig();
    cfg.tls.insecure = false;
    const auto result = mc::Validate(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
}
