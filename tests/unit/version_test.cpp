// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// include/microtel/version.hpp spells the release twice: as kVersionString and
// as kVersionMajor/Minor/Patch. version-drift-check.sh compares both against
// PROJECT_VERSION, but nothing compiled checked the two against each other, so
// a hand edit could leave the public API reporting two different versions.
// It also holds the README and CONTRIBUTING.md to the same version.

#include "microtel/version.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
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

/// The whole of a file in the source tree, or empty if it cannot be read.
std::string ReadSourceFile(const char* path)
{
    const std::ifstream in{path};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// RELEASING.md §1: the README and CONTRIBUTING.md name the release in prose
// that version-drift-check.sh does not read, so a release could ship telling
// readers to clone the previous tag.
TEST(VersionTest, ReadmeNamesTheCurrentRelease)
{
    const std::string readme = ReadSourceFile(MICROTEL_README_PATH);
    ASSERT_FALSE(readme.empty()) << "cannot read " << MICROTEL_README_PATH;
    const std::string version{microtel::kVersionString};
    EXPECT_NE(readme.find("The current release is **v" + version + "**"), std::string::npos)
        << "README Status section does not name v" << version;
    EXPECT_NE(readme.find("git clone --branch v" + version + " "), std::string::npos)
        << "README Getting started does not clone v" << version;
}

TEST(VersionTest, ContributingNamesTheCurrentRelease)
{
    const std::string contributing = ReadSourceFile(MICROTEL_CONTRIBUTING_PATH);
    ASSERT_FALSE(contributing.empty()) << "cannot read " << MICROTEL_CONTRIBUTING_PATH;
    EXPECT_NE(
        contributing.find("The current release is v" + std::string{microtel::kVersionString} + "."),
        std::string::npos)
        << "CONTRIBUTING.md does not name v" << microtel::kVersionString;
}

}  // namespace
