// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the M6-A config layer:
//   - config.hpp         (Config struct, ParsedEndpoint)
//   - toml_loader.hpp    (LoadToml / ParseTomlString)
//   - env_resolver.hpp   (OverlayEnv)
//   - config_validator.hpp (Validate)

#include "common/config/config.hpp"
#include "common/config/config_validator.hpp"
#include "common/config/env_resolver.hpp"
#include "common/config/toml_loader.hpp"

#include "microtel/error.hpp"
#include "microtel/protocol.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace mc = microtel::config;
namespace mt = microtel;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void SetEnv(const char* key, const char* value)
{
    ::setenv(key, value, /*overwrite=*/1);
}

static void UnsetEnv(const char* key)
{
    ::unsetenv(key);
}

/// RAII guard that unsets a list of env vars on destruction.
struct EnvGuard
{
    std::vector<std::string> keys;
    ~EnvGuard()
    {
        for (const auto& k : keys)
        {
            ::unsetenv(k.c_str());
        }
    }
};

static mc::Config MinimalValidConfig()
{
    mc::Config cfg;
    cfg.endpoint_url = "https://collector.internal:4317";
    cfg.protocol = mt::Protocol::Grpc;
    return cfg;
}

// ---------------------------------------------------------------------------
// Config struct defaults
// ---------------------------------------------------------------------------

TEST(ConfigTest, DefaultConfig_HasSensibleDefaults)
{
    const mc::Config cfg;
    EXPECT_EQ(cfg.unknown_key_mode, mc::UnknownKeyMode::Error);
    EXPECT_EQ(cfg.protocol, mt::Protocol::Http);
    EXPECT_FALSE(cfg.compression_gzip);
    EXPECT_TRUE(cfg.endpoint_url.empty());
    EXPECT_FALSE(cfg.tls.insecure);
}

// ---------------------------------------------------------------------------
// ParseTomlString — happy paths
// ---------------------------------------------------------------------------

TEST(ParseTomlStringTest, EmptyDocument_ReturnsDefaultConfig)
{
    const auto result = mc::ParseTomlString("");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->endpoint_url.empty());
}

TEST(ParseTomlStringTest, ExporterEndpointAndProtocol)
{
    const std::string toml = R"toml(
[exporter]
endpoint = "https://otel.example.com:4317"
protocol = "grpc"
)toml";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->endpoint_url, "https://otel.example.com:4317");
    EXPECT_EQ(result->protocol, mt::Protocol::Grpc);
}

TEST(ParseTomlStringTest, ExporterProtocolHttp)
{
    const std::string toml = R"toml(
[exporter]
endpoint = "https://otel.example.com:4318"
protocol = "http"
)toml";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->protocol, mt::Protocol::Http);
}

TEST(ParseTomlStringTest, CompressionGzip)
{
    const std::string toml = R"toml(
[exporter]
compression = "gzip"
)toml";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->compression_gzip);
}

TEST(ParseTomlStringTest, ExporterHeaders)
{
    const std::string toml = R"toml(
[exporter.headers]
"Authorization" = "Bearer tok123"
"X-Custom" = "val"
)toml";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->headers.size(), 2U);
    // Keys are not order-guaranteed by toml++; find by key.
    bool found_auth = false;
    for (const auto& h : result->headers)
    {
        if (h.key == "Authorization")
        {
            EXPECT_EQ(std::get<std::string>(h.value), "Bearer tok123");
            found_auth = true;
        }
    }
    EXPECT_TRUE(found_auth);
}

TEST(ParseTomlStringTest, ServiceNameAndVersion)
{
    const std::string toml = R"toml(
[service]
name    = "my-svc"
version = "2.0.0"
)toml";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->service_name, "my-svc");
    EXPECT_EQ(result->service_version, "2.0.0");
}

TEST(ParseTomlStringTest, ResourceAttributes)
{
    const std::string toml = R"toml(
[resource]
"deployment.environment" = "prod"
"host.id"                = "srv-1"
)toml";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->resource_attrs.size(), 2U);
}

TEST(ParseTomlStringTest, TlsOptions)
{
    const std::string toml = R"toml(
[tls]
insecure    = true
ca_bundle   = "/etc/ssl/custom-ca.pem"
sni_override = "alt.example.com"
)toml";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->tls.insecure);
    EXPECT_EQ(result->tls.ca_bundle, "/etc/ssl/custom-ca.pem");
    EXPECT_EQ(result->tls.sni_override, "alt.example.com");
}

TEST(ParseTomlStringTest, SdkBatchOptions)
{
    const std::string toml = R"toml(
[sdk]
max_queue_size        = 4096
max_export_batch_size = 256
schedule_delay_ms     = 2000
drop_policy           = "oldest"
)toml";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->batch.max_queue_size, 4096U);
    EXPECT_EQ(result->batch.max_export_batch_size, 256U);
    EXPECT_EQ(result->batch.schedule_delay.count(), 2000);
    EXPECT_EQ(result->batch.drop_policy, mt::DropPolicy::DropOldest);
}

TEST(ParseTomlStringTest, TimeoutOptions)
{
    const std::string toml = R"toml(
[timeouts]
connect_ms      = 5000
tls_ms          = 3000
per_export_ms   = 8000
retry_budget_ms = 30000
flush_ms        = 2000
shutdown_ms     = 4000
)toml";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->timeouts.connect.count(), 5000);
    EXPECT_EQ(result->timeouts.tls_handshake.count(), 3000);
    EXPECT_EQ(result->timeouts.per_export.count(), 8000);
    EXPECT_EQ(result->timeouts.retry_budget.count(), 30000);
    EXPECT_EQ(result->timeouts.flush.count(), 2000);
    EXPECT_EQ(result->timeouts.shutdown.count(), 4000);
}

TEST(ParseTomlStringTest, UnknownKeysModeWarn)
{
    const std::string toml = R"toml(
[config]
unknown_keys = "warn"
)toml";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->unknown_key_mode, mc::UnknownKeyMode::Warn);
}

TEST(ParseTomlStringTest, UnknownKeysModeIgnore)
{
    const std::string toml = R"toml(
[config]
unknown_keys = "ignore"
)toml";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->unknown_key_mode, mc::UnknownKeyMode::Ignore);
}

// ---------------------------------------------------------------------------
// ParseTomlString — error paths
// ---------------------------------------------------------------------------

TEST(ParseTomlStringTest, SyntaxError_ReturnsFileParseFailure)
{
    const std::string toml = "not valid toml !!!@@@";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::FileParseFailure);
}

TEST(ParseTomlStringTest, UnknownKey_InErrorMode_ReturnsUnknownKey)
{
    const std::string toml = R"toml(
[exporter]
endpoint      = "https://host:4317"
unknown_field = "oops"
)toml";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::UnknownKey);
    EXPECT_FALSE(result.error().field.empty());
}

TEST(ParseTomlStringTest, UnknownKey_InWarnMode_Succeeds)
{
    const std::string toml = R"toml(
[config]
unknown_keys = "warn"

[exporter]
endpoint      = "https://host:4317"
unknown_field = "oops"
)toml";
    const auto result = mc::ParseTomlString(toml);
    EXPECT_TRUE(result.has_value());
}

TEST(ParseTomlStringTest, UnknownKey_InIgnoreMode_Succeeds)
{
    const std::string toml = R"toml(
[config]
unknown_keys = "ignore"

[exporter]
endpoint      = "https://host:4317"
unknown_field = "oops"
)toml";
    const auto result = mc::ParseTomlString(toml);
    EXPECT_TRUE(result.has_value());
}

TEST(ParseTomlStringTest, InvalidProtocolValue_ReturnsInvalidValue)
{
    const std::string toml = R"toml(
[exporter]
protocol = "websocket"
)toml";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(result.error().field, "exporter.protocol");
}

TEST(ParseTomlStringTest, InvalidDropPolicy_ReturnsInvalidValue)
{
    const std::string toml = R"toml(
[sdk]
drop_policy = "random"
)toml";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(result.error().field, "sdk.drop_policy");
}

TEST(ParseTomlStringTest, InvalidUnknownKeysValue_ReturnsInvalidValue)
{
    const std::string toml = R"toml(
[config]
unknown_keys = "explode"
)toml";
    const auto result = mc::ParseTomlString(toml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(result.error().field, "config.unknown_keys");
}

// ---------------------------------------------------------------------------
// LoadToml — file I/O paths
// ---------------------------------------------------------------------------

TEST(LoadTomlTest, FileNotFound_ReturnsFileNotFound)
{
    const auto result = mc::LoadToml("/nonexistent/path/config.toml");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::FileNotFound);
}

TEST(LoadTomlTest, ValidFile_ParsesCorrectly)
{
    const auto tmp = std::filesystem::temp_directory_path() / "microtel_test_config.toml";
    {
        std::ofstream f{tmp};
        f << "[service]\nname = \"svc-from-file\"\n";
    }
    const auto result = mc::LoadToml(tmp);
    std::filesystem::remove(tmp);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->service_name, "svc-from-file");
}

// ---------------------------------------------------------------------------
// OverlayEnv — happy paths
// ---------------------------------------------------------------------------

TEST(OverlayEnvTest, OtelEndpoint_Overlays)
{
    EnvGuard guard{{"OTEL_EXPORTER_OTLP_ENDPOINT"}};
    SetEnv("OTEL_EXPORTER_OTLP_ENDPOINT", "https://env-host:4317");
    mc::Config cfg;
    const auto result = mc::OverlayEnv(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(cfg.endpoint_url, "https://env-host:4317");
}

TEST(OverlayEnvTest, OtelEndpoint_OverridesFileValue)
{
    EnvGuard guard{{"OTEL_EXPORTER_OTLP_ENDPOINT"}};
    SetEnv("OTEL_EXPORTER_OTLP_ENDPOINT", "https://env-host:4317");
    mc::Config cfg;
    cfg.endpoint_url = "https://file-host:4317";
    const auto result = mc::OverlayEnv(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(cfg.endpoint_url, "https://env-host:4317");
}

TEST(OverlayEnvTest, OtelProtocolGrpc_SetsGrpc)
{
    EnvGuard guard{{"OTEL_EXPORTER_OTLP_PROTOCOL"}};
    SetEnv("OTEL_EXPORTER_OTLP_PROTOCOL", "grpc");
    mc::Config cfg;
    const auto result = mc::OverlayEnv(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(cfg.protocol, mt::Protocol::Grpc);
}

TEST(OverlayEnvTest, OtelProtocolHttpProtobuf_SetsHttp)
{
    EnvGuard guard{{"OTEL_EXPORTER_OTLP_PROTOCOL"}};
    SetEnv("OTEL_EXPORTER_OTLP_PROTOCOL", "http/protobuf");
    mc::Config cfg;
    const auto result = mc::OverlayEnv(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(cfg.protocol, mt::Protocol::Http);
}

TEST(OverlayEnvTest, OtelServiceName_Overlays)
{
    EnvGuard guard{{"OTEL_SERVICE_NAME"}};
    SetEnv("OTEL_SERVICE_NAME", "env-service");
    mc::Config cfg;
    const auto result = mc::OverlayEnv(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(cfg.service_name, "env-service");
}

TEST(OverlayEnvTest, OtelResourceAttributes_ParsesKeyValuePairs)
{
    EnvGuard guard{{"OTEL_RESOURCE_ATTRIBUTES"}};
    SetEnv("OTEL_RESOURCE_ATTRIBUTES", "deployment.env=prod,host.id=srv-1");
    mc::Config cfg;
    const auto result = mc::OverlayEnv(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(cfg.resource_attrs.size(), 2U);
    EXPECT_EQ(cfg.resource_attrs[0].key, "deployment.env");
    EXPECT_EQ(std::get<std::string>(cfg.resource_attrs[0].value), "prod");
    EXPECT_EQ(cfg.resource_attrs[1].key, "host.id");
    EXPECT_EQ(std::get<std::string>(cfg.resource_attrs[1].value), "srv-1");
}

TEST(OverlayEnvTest, OtelHeaders_ParsesKeyValuePairs)
{
    EnvGuard guard{{"OTEL_EXPORTER_OTLP_HEADERS"}};
    SetEnv("OTEL_EXPORTER_OTLP_HEADERS", "Authorization=Bearer tok,X-Tenant=acme");
    mc::Config cfg;
    const auto result = mc::OverlayEnv(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(cfg.headers.size(), 2U);
    EXPECT_EQ(cfg.headers[0].key, "Authorization");
    EXPECT_EQ(std::get<std::string>(cfg.headers[0].value), "Bearer tok");
}

TEST(OverlayEnvTest, OtelTimeout_SetsPeerExport)
{
    EnvGuard guard{{"OTEL_EXPORTER_OTLP_TIMEOUT"}};
    SetEnv("OTEL_EXPORTER_OTLP_TIMEOUT", "15000");
    mc::Config cfg;
    const auto result = mc::OverlayEnv(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(cfg.timeouts.per_export.count(), 15000);
}

TEST(OverlayEnvTest, OtelCompression_Gzip_SetsTrue)
{
    EnvGuard guard{{"OTEL_EXPORTER_OTLP_COMPRESSION"}};
    SetEnv("OTEL_EXPORTER_OTLP_COMPRESSION", "gzip");
    mc::Config cfg;
    const auto result = mc::OverlayEnv(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(cfg.compression_gzip);
}

TEST(OverlayEnvTest, OtelCertificate_SetsCaBundle)
{
    EnvGuard guard{{"OTEL_EXPORTER_OTLP_CERTIFICATE"}};
    SetEnv("OTEL_EXPORTER_OTLP_CERTIFICATE", "/etc/ssl/ca.pem");
    mc::Config cfg;
    const auto result = mc::OverlayEnv(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(cfg.tls.ca_bundle, "/etc/ssl/ca.pem");
}

TEST(OverlayEnvTest, UnsetVars_LeaveConfigUnchanged)
{
    UnsetEnv("OTEL_EXPORTER_OTLP_ENDPOINT");
    UnsetEnv("OTEL_SERVICE_NAME");
    mc::Config cfg;
    cfg.endpoint_url = "https://original:4317";
    cfg.service_name = "original-svc";
    const auto result = mc::OverlayEnv(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(cfg.endpoint_url, "https://original:4317");
    EXPECT_EQ(cfg.service_name, "original-svc");
}

// ---------------------------------------------------------------------------
// OverlayEnv — error paths
// ---------------------------------------------------------------------------

TEST(OverlayEnvTest, InvalidProtocol_ReturnsEnvParseFailure)
{
    EnvGuard guard{{"OTEL_EXPORTER_OTLP_PROTOCOL"}};
    SetEnv("OTEL_EXPORTER_OTLP_PROTOCOL", "quic");
    mc::Config cfg;
    const auto result = mc::OverlayEnv(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::EnvParseFailure);
}

TEST(OverlayEnvTest, InvalidTimeout_NonInteger_ReturnsEnvParseFailure)
{
    EnvGuard guard{{"OTEL_EXPORTER_OTLP_TIMEOUT"}};
    SetEnv("OTEL_EXPORTER_OTLP_TIMEOUT", "ten-seconds");
    mc::Config cfg;
    const auto result = mc::OverlayEnv(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::EnvParseFailure);
}

TEST(OverlayEnvTest, MalformedHeader_MissingEquals_ReturnsEnvParseFailure)
{
    EnvGuard guard{{"OTEL_EXPORTER_OTLP_HEADERS"}};
    SetEnv("OTEL_EXPORTER_OTLP_HEADERS", "AuthorizationBearerNoEquals");
    mc::Config cfg;
    const auto result = mc::OverlayEnv(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::EnvParseFailure);
}

// ---------------------------------------------------------------------------
// Validate — endpoint URL parsing
// ---------------------------------------------------------------------------

TEST(ValidateTest, EmptyEndpoint_ReturnsEndpointMalformed)
{
    mc::Config cfg;
    EXPECT_FALSE(mc::Validate(cfg).has_value());
    EXPECT_EQ(mc::Validate(cfg).error().kind, mt::ConfigError::Kind::EndpointMalformed);
}

TEST(ValidateTest, ValidHttpsUrl_ParsesComponents)
{
    mc::Config cfg = MinimalValidConfig();
    cfg.endpoint_url = "https://collector.internal:4317";
    const auto result = mc::Validate(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(cfg.endpoint.scheme, "https");
    EXPECT_EQ(cfg.endpoint.host, "collector.internal");
    EXPECT_EQ(cfg.endpoint.port, 4317U);
    EXPECT_TRUE(cfg.endpoint.path.empty());
}

TEST(ValidateTest, ValidHttpsUrl_WithPath_ParsesPath)
{
    mc::Config cfg;
    cfg.endpoint_url = "https://collector.internal:4318/prefix";
    cfg.protocol = mt::Protocol::Http;
    const auto result = mc::Validate(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(cfg.endpoint.path, "/prefix");
}

TEST(ValidateTest, HttpUrl_NoPort_UsesDefaultHttp)
{
    mc::Config cfg;
    cfg.endpoint_url = "http://collector.internal";
    cfg.protocol = mt::Protocol::Http;
    const auto result = mc::Validate(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(cfg.endpoint.scheme, "http");
    EXPECT_EQ(cfg.endpoint.port, 4318U);
}

TEST(ValidateTest, GrpcScheme_SetsGrpcProtocolAndHttpsScheme)
{
    mc::Config cfg;
    cfg.endpoint_url = "grpcs://collector.internal:4317";
    cfg.protocol = mt::Protocol::Grpc;
    const auto result = mc::Validate(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(cfg.endpoint.scheme, "https");
}

TEST(ValidateTest, GrpcEndpoint_WithPath_ReturnsProtocolMismatch)
{
    mc::Config cfg;
    cfg.endpoint_url = "https://collector.internal:4317/mypath";
    cfg.protocol = mt::Protocol::Grpc;
    const auto result = mc::Validate(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::ProtocolMismatch);
}

TEST(ValidateTest, MissingScheme_ReturnsEndpointMalformed)
{
    mc::Config cfg;
    cfg.endpoint_url = "collector.internal:4317";
    const auto result = mc::Validate(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::EndpointMalformed);
}

TEST(ValidateTest, EmptyHost_ReturnsEndpointMalformed)
{
    mc::Config cfg;
    cfg.endpoint_url = "https://:4317";
    const auto result = mc::Validate(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::EndpointMalformed);
}

// ---------------------------------------------------------------------------
// Validate — TLS checks
// ---------------------------------------------------------------------------

TEST(ValidateTest, CaBundleNotReadable_ReturnsTlsMaterialUnreadable)
{
    mc::Config cfg = MinimalValidConfig();
    cfg.tls.ca_bundle = "/nonexistent/ca.pem";
    const auto result = mc::Validate(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::TlsMaterialUnreadable);
}

TEST(ValidateTest, ClientCertWithoutKey_ReturnsInvalidValue)
{
    mc::Config cfg = MinimalValidConfig();
    cfg.tls.client_cert = "/path/to/cert.pem";
    // client_key left empty
    const auto result = mc::Validate(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(result.error().field, "tls.client_key");
}

TEST(ValidateTest, ClientKeyWithoutCert_ReturnsInvalidValue)
{
    mc::Config cfg = MinimalValidConfig();
    cfg.tls.client_key = "/path/to/key.pem";
    // client_cert left empty
    const auto result = mc::Validate(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(result.error().field, "tls.client_cert");
}

TEST(ValidateTest, BatchSizeExceedsQueueSize_ReturnsInvalidValue)
{
    mc::Config cfg = MinimalValidConfig();
    cfg.batch.max_queue_size = 100;
    cfg.batch.max_export_batch_size = 200;
    const auto result = mc::Validate(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(result.error().field, "sdk.max_export_batch_size");
}

TEST(ValidateTest, ValidMinimalConfig_Succeeds)
{
    mc::Config cfg = MinimalValidConfig();
    const auto result = mc::Validate(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message;
}
