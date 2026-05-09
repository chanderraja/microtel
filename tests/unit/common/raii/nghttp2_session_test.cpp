// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for Nghttp2Session. Verifies move-only semantics, Release(),
// Reset(), and that the destructor calls nghttp2_session_del — ASan catches
// double-free if it does not.

#include "common/raii/nghttp2_session.hpp"

#include <gtest/gtest.h>
#include <nghttp2/nghttp2.h>

namespace mcr = microtel::common::raii;

static mcr::Nghttp2Session MakeClientSession()
{
    nghttp2_session_callbacks* cbs = nullptr;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session* raw = nullptr;
    nghttp2_session_client_new(&raw, cbs, nullptr);
    nghttp2_session_callbacks_del(cbs);
    return mcr::Nghttp2Session{raw};
}

TEST(Nghttp2SessionTest, DefaultConstruct_NotValid)
{
    mcr::Nghttp2Session session;
    EXPECT_FALSE(session.IsValid());
    EXPECT_EQ(session.Get(), nullptr);
}

TEST(Nghttp2SessionTest, ExplicitConstruct_Valid)
{
    mcr::Nghttp2Session session = MakeClientSession();
    EXPECT_TRUE(session.IsValid());
    EXPECT_NE(session.Get(), nullptr);
}

TEST(Nghttp2SessionTest, MoveConstruct_TransfersOwnership)
{
    mcr::Nghttp2Session src = MakeClientSession();
    nghttp2_session* raw = src.Get();

    mcr::Nghttp2Session dst{std::move(src)};

    EXPECT_FALSE(src.IsValid());
    EXPECT_TRUE(dst.IsValid());
    EXPECT_EQ(dst.Get(), raw);
}

TEST(Nghttp2SessionTest, MoveAssign_TransfersOwnership)
{
    mcr::Nghttp2Session src = MakeClientSession();
    mcr::Nghttp2Session dst = MakeClientSession();
    nghttp2_session* raw = src.Get();

    dst = std::move(src);

    EXPECT_FALSE(src.IsValid());
    EXPECT_TRUE(dst.IsValid());
    EXPECT_EQ(dst.Get(), raw);
}

TEST(Nghttp2SessionTest, Release_RelinquishesOwnership)
{
    mcr::Nghttp2Session session = MakeClientSession();
    nghttp2_session* raw = session.Get();

    nghttp2_session* released = session.Release();

    EXPECT_EQ(released, raw);
    EXPECT_FALSE(session.IsValid());

    nghttp2_session_del(released);
}

TEST(Nghttp2SessionTest, Reset_FreesAndInvalidates)
{
    mcr::Nghttp2Session session = MakeClientSession();
    ASSERT_TRUE(session.IsValid());

    session.Reset();

    EXPECT_FALSE(session.IsValid());
    EXPECT_EQ(session.Get(), nullptr);
}

TEST(Nghttp2SessionTest, Reset_OnInvalid_IsNoOp)
{
    mcr::Nghttp2Session session;
    EXPECT_NO_FATAL_FAILURE(session.Reset());
    EXPECT_FALSE(session.IsValid());
}
