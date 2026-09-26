// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Byte identity between the upb and nanopb backends in one binary
// (docs/leaf-concentrator-design.md §7.2), through microtel_leaf_dual. The
// deterministic half of tests/fuzz/leaf_backend_diff_fuzz.cpp: every golden
// vector, and a fixed set of pseudo-random builder programs, so the
// differential check runs on every PR and not only in the periodic fuzz job.

#include "leaf/diff/leaf_diff_program.hpp"
#include "leaf/vectors/leaf_vectors.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace
{

namespace ld = microtel::leaf_diff;

constexpr std::uint32_t kSeed = 20260926U;
constexpr int kPrograms = 2000;
constexpr std::size_t kMaxProgramBytes = 1024;

TEST(LeafBackendDiffTest, EveryGoldenVector_IsByteIdentical)
{
    ASSERT_GT(microtel_leaf_test_vector_count(), 0U);
    for (std::size_t i = 0; i < microtel_leaf_test_vector_count(); ++i)
    {
        const ld::Outcome out = ld::CompareVector(i);
        EXPECT_TRUE(out.mismatch.empty())
            << microtel_leaf_test_vector_name(i) << ": " << out.mismatch;
    }
}

TEST(LeafBackendDiffTest, RandomPrograms_AreByteIdentical)
{
    // A fixed seed is deliberate: the same programs run on every PR, so a
    // mismatch reproduces; the fuzz target explores beyond them.
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    std::mt19937 rng(kSeed);
    std::uniform_int_distribution<std::size_t> length(0, kMaxProgramBytes);
    std::uniform_int_distribution<int> byte(0, UINT8_MAX);
    std::size_t encodes = 0;
    for (int p = 0; p < kPrograms; ++p)
    {
        std::vector<std::uint8_t> program(length(rng));
        for (auto& b : program)
        {
            b = static_cast<std::uint8_t>(byte(rng));
        }
        const ld::Outcome out = ld::RunProgram(program.data(), program.size());
        ASSERT_TRUE(out.mismatch.empty()) << "program " << p << ": " << out.mismatch;
        encodes += out.encodes;
    }
    // The programs must actually reach the encoder, or this proves nothing.
    EXPECT_GT(encodes, static_cast<std::size_t>(kPrograms));
}

}  // namespace
