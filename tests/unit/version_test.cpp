// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// include/microtel/version.hpp spells the release twice: as kVersionString and
// as kVersionMajor/Minor/Patch. version-drift-check.sh compares both against
// PROJECT_VERSION, but nothing compiled checked the two against each other, so
// a hand edit could leave the public API reporting two different versions.

#include "microtel/version.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{

TEST(VersionTest, ComponentsSpellTheVersionString)
{
    const std::string composed = std::to_string(microtel::kVersionMajor) + "." +
                                 std::to_string(microtel::kVersionMinor) + "." +
                                 std::to_string(microtel::kVersionPatch);
    EXPECT_EQ(composed, microtel::kVersionString);
}

}  // namespace
