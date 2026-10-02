// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// ICP 0038 (issue #408): `SdkBuilder::Build()` rejects a static request header
// that HTTP/2 forbids or that microtel sets itself. Every door a header comes
// through — `WithHeaders`, `OTEL_EXPORTER_OTLP_HEADERS`, `[exporter.headers]`
// — reaches the same check, and a static `authorization` conflicts with
// `WithAuthProvider`. The per-name rules are in
// tests/unit/common/config/config_test.cpp.

#include "microtel/error.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace mt = microtel;

namespace
{

constexpr std::string_view kEndpoint = "https://localhost:4318";
constexpr auto kShutdownTimeout = std::chrono::milliseconds(500);
constexpr const char* kHeadersEnv = "OTEL_EXPORTER_OTLP_HEADERS";

using BuildResult = mt::Expected<std::shared_ptr<mt::Provider>, mt::ConfigError>;

mt::Expected<std::string, mt::Error> FixedToken()
{
    return std::string{"Bearer from-callback"};
}

void ExpectAccepted(BuildResult& result)
{
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ((*result)->Shutdown(kShutdownTimeout), mt::Status::Completed);
}

void ExpectRejected(const BuildResult& result, std::string_view field)
{
    ASSERT_FALSE(result.has_value()) << "Build() accepted it";
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(result.error().field, field);
}

/// Sets `OTEL_EXPORTER_OTLP_HEADERS` for one test and unsets it afterwards.
class HeadersEnv
{
public:
    explicit HeadersEnv(const char* value)
    {
        ::setenv(kHeadersEnv, value, /*overwrite=*/1);
    }
    ~HeadersEnv()
    {
        ::unsetenv(kHeadersEnv);
    }

    HeadersEnv(const HeadersEnv&) = delete;
    HeadersEnv& operator=(const HeadersEnv&) = delete;
    HeadersEnv(HeadersEnv&&) = delete;
    HeadersEnv& operator=(HeadersEnv&&) = delete;
};

/// Writes a `microtel.toml` whose `[exporter.headers]` table is @p table_body,
/// and removes it on destruction.
class TomlFile
{
public:
    explicit TomlFile(std::string_view table_body)
        : m_path(std::filesystem::temp_directory_path() /
                 ("microtel_header_validation_" + std::to_string(::getpid()) + ".toml"))
    {
        std::ofstream f{m_path};
        f << "[exporter.headers]\n" << table_body << "\n";
    }

    ~TomlFile()
    {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    TomlFile(const TomlFile&) = delete;
    TomlFile& operator=(const TomlFile&) = delete;
    TomlFile(TomlFile&&) = delete;
    TomlFile& operator=(TomlFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

}  // namespace

TEST(HeaderValidation, WithHeaders_ReservedName_FailsBuild)
{
    auto result = mt::SdkBuilder()
                      .WithEndpoint(std::string{kEndpoint})
                      .WithHeaders({{.key = "host", .value = std::string{"evil.example"}}})
                      .Build();
    ExpectRejected(result, "exporter.headers.host");
}

TEST(HeaderValidation, EnvVar_ReservedName_FailsBuild)
{
    const HeadersEnv env{"x-tenant=a,te=trailers"};
    auto result = mt::SdkBuilder().WithEndpoint(std::string{kEndpoint}).Build();
    ExpectRejected(result, "exporter.headers.te");
}

TEST(HeaderValidation, TomlTable_ReservedName_FailsBuild)
{
    const TomlFile file{R"("connection" = "close")"};
    auto result =
        mt::SdkBuilder().FromFile(file.Path()).WithEndpoint(std::string{kEndpoint}).Build();
    ExpectRejected(result, "exporter.headers.connection");
}

TEST(HeaderValidation, ProtocolOwnedName_FollowsTheResolvedProtocol)
{
    // `user-agent` is set by the gRPC codec only.
    auto grpc = mt::SdkBuilder()
                    .WithEndpoint("https://localhost:4317")
                    .WithProtocol(mt::Protocol::Grpc)
                    .WithHeaders({{.key = "user-agent", .value = std::string{"custom"}}})
                    .Build();
    ExpectRejected(grpc, "exporter.headers.user-agent");

    auto http = mt::SdkBuilder()
                    .WithEndpoint(std::string{kEndpoint})
                    .WithHeaders({{.key = "user-agent", .value = std::string{"custom"}}})
                    .Build();
    ExpectAccepted(http);
}

TEST(HeaderValidation, OrdinaryHeader_Builds)
{
    auto result = mt::SdkBuilder()
                      .WithEndpoint(std::string{kEndpoint})
                      .WithHeaders({{.key = "x-tenant", .value = std::string{"a"}}})
                      .Build();
    ExpectAccepted(result);
}

TEST(HeaderValidation, StaticAuthorization_WithoutCallback_Builds)
{
    auto result = mt::SdkBuilder()
                      .WithEndpoint(std::string{kEndpoint})
                      .WithHeaders({{.key = "authorization", .value = std::string{"Bearer x"}}})
                      .Build();
    ExpectAccepted(result);
}

TEST(HeaderValidation, Callback_WithoutStaticAuthorization_Builds)
{
    auto result =
        mt::SdkBuilder().WithEndpoint(std::string{kEndpoint}).WithAuthProvider(FixedToken).Build();
    ExpectAccepted(result);
}

TEST(HeaderValidation, StaticAuthorization_WithCallback_FailsBuild)
{
    auto result = mt::SdkBuilder()
                      .WithEndpoint(std::string{kEndpoint})
                      .WithHeaders({{.key = "authorization", .value = std::string{"Bearer x"}}})
                      .WithAuthProvider(FixedToken)
                      .Build();
    ExpectRejected(result, "exporter.headers.authorization");
}

TEST(HeaderValidation, EnvAuthorization_WithCallback_FailsBuild)
{
    const HeadersEnv env{"Authorization=Bearer x"};
    auto result =
        mt::SdkBuilder().WithEndpoint(std::string{kEndpoint}).WithAuthProvider(FixedToken).Build();
    ExpectRejected(result, "exporter.headers.Authorization");
}
