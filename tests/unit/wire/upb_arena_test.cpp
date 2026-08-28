// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// UpbArena: the RAII wrapper that makes memory-model.md §3.1's LOCKED
// guarantee ("the arena is destroyed before Encode() returns") hold on the
// exception path as well as the return path.
//
// Before this type, the encoder created the arena with upb_Arena_New() and
// freed it with a manual upb_Arena_Free() at the bottom of the function. The
// output-buffer allocation sits between the two and is documented as able to
// throw std::bad_alloc, so a throw skipped the free and leaked the arena.
//
// The leak is only observable under a leak checker, so the LeaksNothing test
// below is the one that matters and is meaningful only under LSan/ASan
// (-DMICROTEL_SANITIZER=asan). The rest pin the ownership semantics.

#include "wire/encoder/upb_arena.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <utility>
#include <vector>

namespace
{

using microtel::wire::UpbArena;

TEST(UpbArenaTest, DefaultConstructedOwnsAnArena)
{
    const UpbArena arena;
    EXPECT_TRUE(arena.IsValid());
    EXPECT_NE(arena.Get(), nullptr);
}

TEST(UpbArenaTest, MoveConstructionTransfersOwnership)
{
    UpbArena source;
    auto* const raw = source.Get();
    ASSERT_NE(raw, nullptr);

    const UpbArena moved{std::move(source)};

    EXPECT_EQ(moved.Get(), raw);
    // Inspecting the moved-from object is the point of this test: it must not
    // free on destruction, and a double free is the failure mode a
    // hand-written wrapper gets wrong. Both check names are listed because
    // hicpp-invalid-access-moved aliases bugprone-use-after-move and fires
    // under its own name.
    // NOLINTBEGIN(bugprone-use-after-move,hicpp-invalid-access-moved)
    EXPECT_FALSE(source.IsValid());
    EXPECT_EQ(source.Get(), nullptr);
    // NOLINTEND(bugprone-use-after-move,hicpp-invalid-access-moved)
}

TEST(UpbArenaTest, MoveAssignmentFreesTheTargetAndTransfers)
{
    UpbArena source;
    auto* const raw = source.Get();
    UpbArena target;
    ASSERT_NE(target.Get(), raw);

    target = std::move(source);

    EXPECT_EQ(target.Get(), raw);
    // NOLINTBEGIN(bugprone-use-after-move,hicpp-invalid-access-moved)
    EXPECT_FALSE(source.IsValid());
    // NOLINTEND(bugprone-use-after-move,hicpp-invalid-access-moved)
}

TEST(UpbArenaTest, SelfMoveAssignmentDoesNotFreeTheHeldArena)
{
    UpbArena arena;
    auto* const raw = arena.Get();

    auto& alias = arena;
    arena = std::move(alias);  // NOLINT(clang-diagnostic-self-move)

    EXPECT_TRUE(arena.IsValid());
    EXPECT_EQ(arena.Get(), raw);
}

TEST(UpbArenaTest, ReleaseHandsOffOwnership)
{
    UpbArena arena;
    auto* const raw = arena.Get();

    auto* const released = arena.Release();

    EXPECT_EQ(released, raw);
    EXPECT_FALSE(arena.IsValid());
    // We took ownership, so we free it — if the wrapper also freed it, ASan
    // would report a double free here.
    upb_Arena_Free(released);
}

TEST(UpbArenaTest, DestructorRunsWhenScopeExitsViaException)
{
    // The regression this type exists for: the arena must be freed when the
    // scope is left by a throw, not only by a return. Meaningful under a leak
    // checker; harmless otherwise.
    const auto allocate_then_throw = []
    {
        const UpbArena arena;
        EXPECT_TRUE(arena.IsValid());
        throw std::runtime_error("stand-in for the bad_alloc Encode documents");
    };

    EXPECT_THROW(allocate_then_throw(), std::runtime_error);
}

TEST(UpbArenaTest, ManyArenasLeakNothing)
{
    // Bulk allocate/destroy so a leak is large enough for LSan to report
    // rather than lose in noise.
    constexpr int kIterations = 256;
    for (int i = 0; i < kIterations; ++i)
    {
        const UpbArena arena;
        ASSERT_TRUE(arena.IsValid());
    }
}

}  // namespace
