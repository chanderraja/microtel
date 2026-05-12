// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for SslSession. Verifies move-only semantics, Release(), Reset(),
// and that the destructor calls SSL_free — ASan catches double-free if it does not.

#include "common/raii/ssl_session.hpp"

#include <gtest/gtest.h>
#include <openssl/ssl.h>

namespace mcr = microtel::common::raii;

static SSL_CTX* TestCtx()
{
    static SSL_CTX* const kCtx = ::SSL_CTX_new(::TLS_method());  // NOLINT(misc-const-correctness)
    return kCtx;
}

static mcr::SslSession MakeSession()
{
    return mcr::SslSession{::SSL_new(TestCtx())};
}

TEST(SslSessionTest, DefaultConstruct_NotValid)
{
    const mcr::SslSession session;
    EXPECT_FALSE(session.IsValid());
    EXPECT_EQ(session.Get(), nullptr);
}

TEST(SslSessionTest, ExplicitConstruct_Valid)
{
    const mcr::SslSession session = MakeSession();
    EXPECT_TRUE(session.IsValid());
    EXPECT_NE(session.Get(), nullptr);
}

TEST(SslSessionTest, MoveConstruct_TransfersOwnership)
{
    mcr::SslSession src = MakeSession();
    const SSL* const raw = src.Get();

    const mcr::SslSession dst{std::move(src)};

    EXPECT_FALSE(src.IsValid());  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
    EXPECT_TRUE(dst.IsValid());
    EXPECT_EQ(dst.Get(), raw);
}

TEST(SslSessionTest, MoveAssign_TransfersOwnership)
{
    mcr::SslSession src = MakeSession();
    mcr::SslSession dst = MakeSession();
    const SSL* const raw = src.Get();

    dst = std::move(src);

    EXPECT_FALSE(src.IsValid());  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
    EXPECT_TRUE(dst.IsValid());
    EXPECT_EQ(dst.Get(), raw);
}

TEST(SslSessionTest, Release_RelinquishesOwnership)
{
    mcr::SslSession session = MakeSession();
    const SSL* const raw = session.Get();

    SSL* released = session.Release();

    EXPECT_EQ(released, raw);
    EXPECT_FALSE(session.IsValid());

    ::SSL_free(released);
}

TEST(SslSessionTest, Reset_FreesAndInvalidates)
{
    mcr::SslSession session = MakeSession();
    ASSERT_TRUE(session.IsValid());

    session.Reset();

    EXPECT_FALSE(session.IsValid());
    EXPECT_EQ(session.Get(), nullptr);
}

TEST(SslSessionTest, Reset_OnInvalid_IsNoOp)
{
    mcr::SslSession session;
    EXPECT_NO_FATAL_FAILURE(session.Reset());
    EXPECT_FALSE(session.IsValid());
}
