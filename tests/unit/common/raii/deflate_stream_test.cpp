// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the DeflateStream RAII wrapper — the compression sibling of
// `inflate_stream_test.cpp`, deliberately the same shape. The zlib state it
// owns is invisible to the test, so these pin the observable contract instead:
// initialise once, hand out a borrowed pointer only while initialised, and
// survive a move without losing the state zlib is holding (issue #186).

#include "common/raii/deflate_stream.hpp"

#include <gtest/gtest.h>

#include <utility>

#include <zlib.h>

namespace mtr = microtel::common::raii;

namespace
{

// Window bits 15 plus 16 selects the gzip wrapper, matching what `GzipCompress`
// asks for; level -1 and mem level 8 are zlib's defaults.
constexpr int kGzipWindowBits = 15 + 16;
constexpr int kDefaultLevel = -1;
constexpr int kDefaultMemLevel = 8;

[[nodiscard]] bool InitDefault(mtr::DeflateStream& stream)
{
    return stream.Init(kDefaultLevel, kGzipWindowBits, kDefaultMemLevel);
}

}  // namespace

TEST(DeflateStreamTest, DefaultConstructedHoldsNothing)
{
    mtr::DeflateStream stream;
    EXPECT_EQ(stream.Get(), nullptr);
}

TEST(DeflateStreamTest, InitSucceedsAndExposesTheStream)
{
    mtr::DeflateStream stream;
    ASSERT_TRUE(InitDefault(stream));
    EXPECT_NE(stream.Get(), nullptr);
}

TEST(DeflateStreamTest, SecondInitFailsRatherThanLeakingTheFirst)
{
    mtr::DeflateStream stream;
    ASSERT_TRUE(InitDefault(stream));
    EXPECT_FALSE(InitDefault(stream));
    EXPECT_NE(stream.Get(), nullptr);
}

TEST(DeflateStreamTest, ResetReleasesAndIsIdempotent)
{
    mtr::DeflateStream stream;
    ASSERT_TRUE(InitDefault(stream));
    stream.Reset();
    EXPECT_EQ(stream.Get(), nullptr);
    stream.Reset();
    EXPECT_EQ(stream.Get(), nullptr);
    // Reset is what makes the object reusable: a second Init must now work.
    EXPECT_TRUE(InitDefault(stream));
}

/// zlib's internal `deflate_state` holds a back-pointer to the `z_stream` it
/// was initialised against, and `deflateStateCheck` compares it on every call.
/// If a move changed the stream's address, this returns `Z_STREAM_ERROR` — and
/// so does the `deflateEnd` in the destructor, which then frees nothing (issue
/// #186; LeakSanitizer measured the inflate sibling at 42 KiB per moved
/// stream). Asserting it here states the guarantee in the test rather than
/// leaving it to whoever next runs LeakSanitizer.
static void ExpectStillUsable(mtr::DeflateStream& stream)
{
    ASSERT_NE(stream.Get(), nullptr);
    EXPECT_EQ(deflateReset(stream.Get()), Z_OK)
        << "zlib no longer recognises this stream — its address moved";
}

TEST(DeflateStreamTest, MoveConstructionTransfersOwnership)
{
    mtr::DeflateStream source;
    ASSERT_TRUE(InitDefault(source));

    mtr::DeflateStream moved{std::move(source)};
    EXPECT_NE(moved.Get(), nullptr);
    // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(source.Get(), nullptr);
    ExpectStillUsable(moved);
}

TEST(DeflateStreamTest, MoveAssignmentReleasesTheTargetFirst)
{
    mtr::DeflateStream source;
    ASSERT_TRUE(InitDefault(source));
    mtr::DeflateStream target;
    ASSERT_TRUE(InitDefault(target));

    target = std::move(source);
    EXPECT_NE(target.Get(), nullptr);
    // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(source.Get(), nullptr);
    ExpectStillUsable(target);
}

TEST(DeflateStreamTest, SelfMoveAssignmentKeepsTheStream)
{
    mtr::DeflateStream stream;
    ASSERT_TRUE(InitDefault(stream));

    // Guards the `this != &other` branch: without it a self-move would end the
    // stream and then hand out a dangling pointer. Routed through a pointer so
    // the compiler's -Wself-move heuristic does not reject the line outright.
    mtr::DeflateStream* const self = &stream;
    stream = std::move(*self);
    EXPECT_NE(stream.Get(), nullptr);
}

/// The leak probe from issue #186, as a test: a moved-from-then-destroyed
/// stream must actually release zlib's allocation. `deflateEnd` on the moved
/// stream is the same call the destructor makes, and its return code is the
/// direct evidence — `Z_STREAM_ERROR` means nothing was freed. Under ASAN/LSAN
/// this test also fails by leak report rather than by assertion.
TEST(DeflateStreamTest, MovedStreamStillEndsCleanly)
{
    mtr::DeflateStream source;
    ASSERT_TRUE(InitDefault(source));

    mtr::DeflateStream moved{std::move(source)};
    ASSERT_NE(moved.Get(), nullptr);
    EXPECT_EQ(deflateEnd(moved.Get()), Z_OK)
        << "deflateEnd freed nothing — the moved z_stream is at a new address";
    // The wrapper still believes it owns a stream; its own Reset will call
    // deflateEnd a second time, which zlib reports rather than punishes.
    moved.Reset();
}

TEST(DeflateStreamTest, RejectsInvalidWindowBits)
{
    mtr::DeflateStream stream;
    constexpr int kNonsenseWindowBits = 3;
    EXPECT_FALSE(stream.Init(kDefaultLevel, kNonsenseWindowBits, kDefaultMemLevel));
    EXPECT_EQ(stream.Get(), nullptr);
}
