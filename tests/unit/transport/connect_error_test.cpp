// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit test: how a failed TCP connect is diagnosed (issue #333).
//
// A peer that accepts and then resets leaves `ECONNRESET` in `SO_ERROR` only
// when the reset beats the client's `getsockopt`, which a loopback test can
// only race for. The errno-to-Error mapping is therefore tested here, where
// every errno can be handed in directly; the integration tests in
// `tests/integration/transport/http2_connect_test.cpp` cover the loop that
// feeds it.

#include "transport/connect_error.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <string>

namespace mt = microtel;
namespace mtt = microtel::transport;

TEST(ConnectErrorTest, Refused_KeepsTheExistingMessage)
{
    const mt::Error error = mtt::ConnectFailureError(ECONNREFUSED);
    EXPECT_EQ(error.kind, mt::Error::Kind::Network);
    EXPECT_EQ(error.message, "connection refused");
    EXPECT_EQ(error.os_errno, ECONNREFUSED);
}

TEST(ConnectErrorTest, NoErrnoRecorded_ReportsRefused)
{
    const mt::Error error = mtt::ConnectFailureError(0);
    EXPECT_EQ(error.kind, mt::Error::Kind::Network);
    EXPECT_EQ(error.message, "connection refused");
    EXPECT_EQ(error.os_errno, 0);
}

TEST(ConnectErrorTest, ResetAfterAccept_NamesThePeerAndTheReset)
{
    const mt::Error error = mtt::ConnectFailureError(ECONNRESET);
    EXPECT_EQ(error.kind, mt::Error::Kind::Network);
    EXPECT_NE(error.message.find("peer closed the connection"), std::string::npos) << error.message;
    EXPECT_NE(error.message.find("reset"), std::string::npos) << error.message;
    EXPECT_EQ(error.message.find("refused"), std::string::npos) << error.message;
    EXPECT_EQ(error.os_errno, ECONNRESET);
}

TEST(ConnectErrorTest, BrokenPipe_NamesThePeer)
{
    const mt::Error error = mtt::ConnectFailureError(EPIPE);
    EXPECT_EQ(error.kind, mt::Error::Kind::Network);
    EXPECT_NE(error.message.find("peer closed the connection"), std::string::npos) << error.message;
    EXPECT_EQ(error.message.find("refused"), std::string::npos) << error.message;
    EXPECT_EQ(error.os_errno, EPIPE);
}

TEST(ConnectErrorTest, OtherErrno_CarriesItsText)
{
    const mt::Error error = mtt::ConnectFailureError(ENETUNREACH);
    EXPECT_EQ(error.kind, mt::Error::Kind::Network);
    EXPECT_EQ(error.message.rfind("connect failed: ", 0), 0U) << error.message;
    EXPECT_GT(error.message.size(), std::string{"connect failed: "}.size());
    EXPECT_EQ(error.message.find("refused"), std::string::npos) << error.message;
    EXPECT_EQ(error.os_errno, ENETUNREACH);
}

TEST(ConnectErrorTest, Merge_FirstFailureIsKept)
{
    EXPECT_EQ(mtt::MergeConnectErrno(0, ECONNREFUSED), ECONNREFUSED);
    EXPECT_EQ(mtt::MergeConnectErrno(0, ECONNRESET), ECONNRESET);
}

TEST(ConnectErrorTest, Merge_RefusalNeverHidesAReset)
{
    // e.g. "localhost": 127.0.0.1 accepted and reset, then ::1 refused.
    EXPECT_EQ(mtt::MergeConnectErrno(ECONNRESET, ECONNREFUSED), ECONNRESET);
}

TEST(ConnectErrorTest, Merge_LaterNonRefusalWins)
{
    EXPECT_EQ(mtt::MergeConnectErrno(ECONNREFUSED, ECONNRESET), ECONNRESET);
    EXPECT_EQ(mtt::MergeConnectErrno(ENETUNREACH, ECONNRESET), ECONNRESET);
}
