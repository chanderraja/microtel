// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the InflateStream RAII wrapper. The zlib state it owns is
// invisible to the test — only ASAN/LSAN can see a leaked `inflateEnd` — so
// these tests pin the observable contract instead: initialise once, hand out a
// borrowed pointer only while initialised, and survive a move without
// double-ending the stream.

#include "common/raii/inflate_stream.hpp"

#include <gtest/gtest.h>

#include <utility>

namespace mtr = microtel::common::raii;

namespace
{

// Window bits 15 plus 16 selects the gzip wrapper, matching what the codecs
// ask for.
constexpr int kGzipWindowBits = 15 + 16;

}  // namespace

TEST(InflateStreamTest, DefaultConstructedHoldsNothing)
{
    mtr::InflateStream stream;
    EXPECT_EQ(stream.Get(), nullptr);
}

TEST(InflateStreamTest, InitSucceedsAndExposesTheStream)
{
    mtr::InflateStream stream;
    ASSERT_TRUE(stream.Init(kGzipWindowBits));
    EXPECT_NE(stream.Get(), nullptr);
}

TEST(InflateStreamTest, SecondInitFailsRatherThanLeakingTheFirst)
{
    mtr::InflateStream stream;
    ASSERT_TRUE(stream.Init(kGzipWindowBits));
    EXPECT_FALSE(stream.Init(kGzipWindowBits));
    EXPECT_NE(stream.Get(), nullptr);
}

TEST(InflateStreamTest, ResetReleasesAndIsIdempotent)
{
    mtr::InflateStream stream;
    ASSERT_TRUE(stream.Init(kGzipWindowBits));
    stream.Reset();
    EXPECT_EQ(stream.Get(), nullptr);
    stream.Reset();
    EXPECT_EQ(stream.Get(), nullptr);
    // Reset is what makes the object reusable: a second Init must now work.
    EXPECT_TRUE(stream.Init(kGzipWindowBits));
}

TEST(InflateStreamTest, MoveConstructionTransfersOwnership)
{
    mtr::InflateStream source;
    ASSERT_TRUE(source.Init(kGzipWindowBits));

    mtr::InflateStream moved{std::move(source)};
    EXPECT_NE(moved.Get(), nullptr);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(source.Get(), nullptr);
}

TEST(InflateStreamTest, MoveAssignmentReleasesTheTargetFirst)
{
    mtr::InflateStream source;
    ASSERT_TRUE(source.Init(kGzipWindowBits));
    mtr::InflateStream target;
    ASSERT_TRUE(target.Init(kGzipWindowBits));

    target = std::move(source);
    EXPECT_NE(target.Get(), nullptr);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(source.Get(), nullptr);
}

TEST(InflateStreamTest, SelfMoveAssignmentKeepsTheStream)
{
    mtr::InflateStream stream;
    ASSERT_TRUE(stream.Init(kGzipWindowBits));

    // Guards the `this != &other` branch: without it a self-move would end the
    // stream and then hand out a dangling pointer. Routed through a pointer so
    // the compiler's -Wself-move heuristic does not reject the line outright.
    mtr::InflateStream* const self = &stream;
    stream = std::move(*self);
    EXPECT_NE(stream.Get(), nullptr);
}

TEST(InflateStreamTest, RejectsInvalidWindowBits)
{
    mtr::InflateStream stream;
    constexpr int kNonsenseWindowBits = 3;
    EXPECT_FALSE(stream.Init(kNonsenseWindowBits));
    EXPECT_EQ(stream.Get(), nullptr);
}
