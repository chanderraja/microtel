// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the thread-local current-context slot — issue #221,
// ICP 0025 §3 ("Current context and StartAsCurrentSpan").
//
// Contract under test:
//  - `CurrentContext()` is never null; a thread that installed nothing sees a
//    default-constructed `Context`.
//  - `ScopedContext` installs on construction and restores on destruction.
//  - Nesting is the C++ stack: each scope holds the value it displaced, so
//    reverse-order destruction unwinds the chain exactly.
//  - Restore is *positional*: destroying scopes out of order restores a stale
//    context. That is a documented programming error, pinned here so the
//    behaviour is defined rather than accidental.
//  - The slot is per-thread: a newly created thread starts from the root
//    context regardless of what the spawning thread has installed
//    (ICP 0025 §3 contract 4 — no cross-thread inheritance).

#include "microtel/context.hpp"

#include "microtel/trace.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace mt = microtel;

namespace
{

mt::SpanContext MakeSpanContext(std::uint8_t seed)
{
    mt::TraceId::Bytes trace_bytes{};
    trace_bytes[0] = seed;
    mt::SpanId::Bytes span_bytes{};
    span_bytes[0] = seed;
    return mt::SpanContext{
        .trace_id = mt::TraceId{trace_bytes},
        .span_id = mt::SpanId{span_bytes},
        .trace_flags = mt::TraceFlags{mt::TraceFlags::kSampled},
        .trace_state = {},
        .remote = false,
    };
}

std::uint8_t CurrentSeed()
{
    return mt::CurrentContext().active_span_context.trace_id.AsBytes()[0];
}

/// Seed unique to a (thread, depth) pair. `base` is 1..15 and `depth` 1..15,
/// so the low nibble carries the depth and the high nibble the thread.
constexpr int kNibble = 16;
std::uint8_t SeedFor(std::uint8_t base, int depth)
{
    return static_cast<std::uint8_t>((base * kNibble) + depth);
}

/// Builds a scope stack @p depth deep on the calling thread, checking on the
/// way down and again on the way back up that the slot holds this thread's own
/// seed for this level.
///
/// Deliberately recursive: a loop cannot create nested block scopes, and "each
/// level's context lives in a real stack frame" is precisely the shape
/// ICP 0025 §3 chose over a thread-local container. Depth is a bounded
/// compile-time constant.
// NOLINTNEXTLINE(misc-no-recursion)
void PushVerifyPop(std::uint8_t base, int depth, std::atomic<int>& mismatches)
{
    if (depth == 0)
    {
        return;
    }
    const std::uint8_t seed = SeedFor(base, depth);
    const mt::ScopedContext scope{mt::Context{MakeSpanContext(seed)}};
    if (CurrentSeed() != seed)
    {
        mismatches.fetch_add(1, std::memory_order_relaxed);
    }
    PushVerifyPop(base, depth - 1, mismatches);
    if (CurrentSeed() != seed)
    {
        mismatches.fetch_add(1, std::memory_order_relaxed);
    }
}

constexpr int kStackDepth = 12;
constexpr int kStackRounds = 40;

/// The body of each worker thread in the concurrency test below.
void RunScopeStackRounds(std::uint8_t base, std::atomic<int>& mismatches)
{
    for (int round = 0; round < kStackRounds; ++round)
    {
        PushVerifyPop(base, kStackDepth, mismatches);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// noexcept / special-member guards (ICP 0025 §2, §3)
// ---------------------------------------------------------------------------

static_assert(std::is_nothrow_default_constructible_v<mt::Context>);
static_assert(std::is_nothrow_copy_constructible_v<mt::Context>,
              "Context is copied into every ScopedContext — see ICP 0025 §2");
static_assert(std::is_nothrow_move_constructible_v<mt::Context>);
static_assert(std::is_nothrow_copy_assignable_v<mt::Context>);
static_assert(std::is_nothrow_move_assignable_v<mt::Context>);

static_assert(std::is_nothrow_default_constructible_v<mt::ScopedContext>);
static_assert(std::is_nothrow_move_constructible_v<mt::ScopedContext>,
              "a factory must be able to return a ScopedContext by value");
static_assert(std::is_nothrow_destructible_v<mt::ScopedContext>);
static_assert(!std::is_copy_constructible_v<mt::ScopedContext>);
static_assert(!std::is_copy_assignable_v<mt::ScopedContext>);
static_assert(!std::is_move_assignable_v<mt::ScopedContext>,
              "move-assignment would restore out of order — deleted by ICP 0025 §3");

// ---------------------------------------------------------------------------
// The root context
// ---------------------------------------------------------------------------

TEST(ContextTest, CurrentContext_DefaultsToAnInvalidSpanContext)
{
    EXPECT_FALSE(mt::CurrentContext().active_span_context.IsValid());
}

// ---------------------------------------------------------------------------
// Install / restore
// ---------------------------------------------------------------------------

TEST(ContextTest, ScopedContext_InstallsForItsLifetime)
{
    const mt::ScopedContext scope{mt::Context{MakeSpanContext(0x11)}};
    EXPECT_TRUE(mt::CurrentContext().active_span_context.IsValid());
    EXPECT_EQ(CurrentSeed(), 0x11);
}

TEST(ContextTest, ScopedContext_RestoresOnDestruction)
{
    {
        const mt::ScopedContext scope{mt::Context{MakeSpanContext(0x22)}};
        ASSERT_EQ(CurrentSeed(), 0x22);
    }
    EXPECT_FALSE(mt::CurrentContext().active_span_context.IsValid());
}

TEST(ContextTest, ScopedContext_NestsThroughTheCallStack)
{
    const mt::ScopedContext outer{mt::Context{MakeSpanContext(0x01)}};
    ASSERT_EQ(CurrentSeed(), 0x01);
    {
        const mt::ScopedContext middle{mt::Context{MakeSpanContext(0x02)}};
        ASSERT_EQ(CurrentSeed(), 0x02);
        {
            const mt::ScopedContext inner{mt::Context{MakeSpanContext(0x03)}};
            EXPECT_EQ(CurrentSeed(), 0x03);
        }
        EXPECT_EQ(CurrentSeed(), 0x02);
    }
    EXPECT_EQ(CurrentSeed(), 0x01);
}

TEST(ContextTest, ScopedContext_DefaultConstructedIsInert)
{
    const mt::ScopedContext installed{mt::Context{MakeSpanContext(0x44)}};
    {
        const mt::ScopedContext inert;
        EXPECT_EQ(CurrentSeed(), 0x44);
    }
    EXPECT_EQ(CurrentSeed(), 0x44);
}

TEST(ContextTest, ScopedContext_MovedFromScopeRestoresNothing)
{
    {
        mt::ScopedContext original{mt::Context{MakeSpanContext(0x55)}};
        ASSERT_EQ(CurrentSeed(), 0x55);
        {
            const mt::ScopedContext moved{std::move(original)};
            EXPECT_EQ(CurrentSeed(), 0x55);
        }
        // `moved` restored the root; `original` is disarmed and must not
        // restore a second time when it leaves scope below.
        EXPECT_FALSE(mt::CurrentContext().active_span_context.IsValid());
    }
    EXPECT_FALSE(mt::CurrentContext().active_span_context.IsValid());
}

// ---------------------------------------------------------------------------
// Out-of-order destruction — a documented programming error (ICP 0025 §3)
// ---------------------------------------------------------------------------

// Run on a worker thread precisely because it ends with the slot corrupted:
// the misuse leaves a context installed that no live scope will ever restore,
// and the only way back to a clean slot is for the thread to end. That is
// itself part of the contract worth stating out loud.
TEST(ContextTest, ScopedContext_OutOfOrderDestruction_RestoresAStaleContext)
{
    bool root_after_first_reset = false;
    std::uint8_t seed_after_second_reset = 0;

    std::thread worker(
        [&root_after_first_reset, &seed_after_second_reset]
        {
            std::optional<mt::ScopedContext> first;
            std::optional<mt::ScopedContext> second;
            first.emplace(mt::Context{MakeSpanContext(0x0A)});
            second.emplace(mt::Context{MakeSpanContext(0x0B)});
            ASSERT_EQ(CurrentSeed(), 0x0B);

            // Destroying the *outer* scope first writes back what it displaced
            // (the root) even though `second` is still live.
            first.reset();
            root_after_first_reset = !mt::CurrentContext().active_span_context.IsValid();

            // And `second` then writes back what *it* displaced — 0x0A, a
            // context whose scope is already gone. Restore is positional; this
            // is the defined consequence, not a crash and not a silent no-op.
            second.reset();
            seed_after_second_reset = CurrentSeed();
        });
    worker.join();

    EXPECT_TRUE(root_after_first_reset);
    EXPECT_EQ(seed_after_second_reset, 0x0A);
    // The spawning thread's own slot was never touched.
    EXPECT_FALSE(mt::CurrentContext().active_span_context.IsValid());
}

// ---------------------------------------------------------------------------
// Thread confinement (ICP 0025 §3 contract 4)
// ---------------------------------------------------------------------------

TEST(ContextTest, CurrentContext_NewThreadStartsFromTheRootContext)
{
    const mt::ScopedContext scope{mt::Context{MakeSpanContext(0x77)}};
    ASSERT_EQ(CurrentSeed(), 0x77);

    bool worker_saw_a_span = true;
    std::thread worker([&worker_saw_a_span]
                       { worker_saw_a_span = mt::CurrentContext().active_span_context.IsValid(); });
    worker.join();

    EXPECT_FALSE(worker_saw_a_span);
    EXPECT_EQ(CurrentSeed(), 0x77);
}

TEST(ContextTest, ScopedContext_OnAWorkerThreadDoesNotDisturbTheSpawner)
{
    const mt::ScopedContext scope{mt::Context{MakeSpanContext(0x66)}};
    ASSERT_EQ(CurrentSeed(), 0x66);

    std::uint8_t worker_seed = 0;
    std::thread worker(
        [&worker_seed]
        {
            const mt::ScopedContext worker_scope{mt::Context{MakeSpanContext(0x99)}};
            worker_seed = CurrentSeed();
        });
    worker.join();

    EXPECT_EQ(worker_seed, 0x99);
    EXPECT_EQ(CurrentSeed(), 0x66);
}

// Every thread drives its own deep scope stack at the same time. Run under
// TSAN (-DMICROTEL_SANITIZER=tsan) this is the guard that the slot really is
// thread_local: a shared slot would show up both as a TSAN data race and as a
// non-zero mismatch count here.
TEST(ContextTest, ConcurrentScopeStacks_StayIndependentPerThread)
{
    constexpr int kThreads = 8;

    std::atomic<int> mismatches{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 1; t <= kThreads; ++t)
    {
        workers.emplace_back([t, &mismatches]
                             { RunScopeStackRounds(static_cast<std::uint8_t>(t), mismatches); });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }

    EXPECT_EQ(mismatches.load(), 0);
    EXPECT_FALSE(mt::CurrentContext().active_span_context.IsValid());
}

TEST(ContextTest, CurrentContext_ExplicitHandOffIsHowAWorkerJoinsTheTrace)
{
    const mt::ScopedContext scope{mt::Context{MakeSpanContext(0x31)}};

    std::uint8_t worker_seed = 0;
    // The hand-off: the context is copied here, on the spawning thread, and
    // installed over there. microtel grows no thread-creation hook of its own.
    std::thread worker(
        [carried = mt::CurrentContext(), &worker_seed]
        {
            const mt::ScopedContext worker_scope{carried};
            worker_seed = CurrentSeed();
        });
    worker.join();

    EXPECT_EQ(worker_seed, 0x31);
}
