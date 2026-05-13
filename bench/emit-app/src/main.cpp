// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "backend.hpp"
#include "control_socket.hpp"
#include "histogram.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{

constexpr int kDefaultControlPort = 9090;

std::string EnvOr(const char* name, const char* fallback)
{
    const char* val = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
    return (val != nullptr && val[0] != '\0') ? std::string(val) : std::string(fallback);
}

}  // namespace

int main()
{
    // Configuration via environment variables — injected by the SUT Dockerfile
    // or overridden on the developer machine.
    const std::string endpoint    = EnvOr("EMIT_ENDPOINT",     "http://sink:4318");
    const std::string svc_name    = EnvOr("EMIT_SERVICE_NAME", "bench");
    const std::string svc_version = EnvOr("EMIT_SERVICE_VER",  "0.0.0");
    const int control_port        = kDefaultControlPort;

    const bench::BackendOptions opts{
        .endpoint        = endpoint,
        .service_name    = svc_name,
        .service_version = svc_version,
        .compression_gzip = false,
    };

    std::unique_ptr<bench::IBackend> backend{bench::CreateBackend()};
    bench::Histogram hist;

    try
    {
        backend->Init(opts);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "emit_app: backend init failed: " << ex.what() << '\n';
        return 1;
    }

    std::cerr << "emit_app: ready on control port " << control_port << '\n';

    try
    {
        bench::RunControlLoop(control_port, *backend, hist);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "emit_app: control loop error: " << ex.what() << '\n';
        backend->Shutdown();
        return 1;
    }

    backend->Shutdown();
    return 0;
}
