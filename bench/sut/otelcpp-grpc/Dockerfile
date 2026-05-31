# syntax=docker/dockerfile:1
# otelcpp-grpc SUT: builds opentelemetry-cpp (gRPC exporter) via CMake
# FetchContent superbuild, then builds the emit-app against it.
# Build context must be the repo root.
#
#   docker build \
#       --build-arg OTELCPP_VERSION=v1.19.0 \
#       -f bench/sut/otelcpp-grpc/Dockerfile \
#       -t bench-sut-otelcpp-grpc .
#
# Layer caching (critical — cold otelcpp build takes 20-30 min):
#   base          OS + apt packages                              ~2-3 min
#   otelcpp-build otel-cpp + all superbuild deps (abseil,        ~20-30 min cold
#                 re2, c-ares, protobuf, gRPC, otel-cpp)        seconds with cache
#   emit-build    emit-app binary against installed otel-cpp     ~1-2 min
#   runtime       minimal image with binary only                 seconds
#
# The otelcpp-build stage is cached as long as OTELCPP_VERSION and the base
# image are unchanged. Only emit-build rebuilds when emit-app source changes.
#
# Why CMake superbuild (not system packages)?
# System packages pin to distro versions that may differ from what otel-cpp
# was tested against. The superbuild ensures abseil/gRPC/protobuf are at
# exactly the versions otel-cpp v1.19.0 expects — the same dep graph a
# production otel-cpp user gets. This is the only fair baseline.

ARG OTELCPP_VERSION=v1.19.0

# Flamegraph variant (B3): pass --build-arg INSTALL_PERF=true to add linux-perf
# to the runtime image and tag as bench-sut-otelcpp-grpc:perf.
# Run with --cap-add=PERFMON (Linux >=5.8).  Normal images never install perf.
ARG INSTALL_PERF=false

# ---------------------------------------------------------------------------
# Stage 1: base — OS and toolchain
# Invalidated only when the ubuntu image or apt package list changes.
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS base

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        clang-18 \
        cmake \
        ninja-build \
        git \
        curl \
        ca-certificates \
        pkg-config \
        libssl-dev \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

ENV CC=clang-18
ENV CXX=clang++-18
ENV BENCH_CXXFLAGS="-O2 -g -fno-omit-frame-pointer"

# ---------------------------------------------------------------------------
# Stage 2: otelcpp-build — superbuild of otel-cpp and all transitive deps
# Cached on OTELCPP_VERSION. Change the version pin → this stage re-runs.
# abseil, re2, c-ares, protobuf, gRPC, and otel-cpp itself all build here.
# ---------------------------------------------------------------------------
FROM base AS otelcpp-build

ARG OTELCPP_VERSION

RUN git clone --depth 1 --branch ${OTELCPP_VERSION} \
        https://github.com/open-telemetry/opentelemetry-cpp.git \
        /src/opentelemetry-cpp

RUN cmake -S /src/opentelemetry-cpp -B /build/otelcpp \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS}" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/otelcpp \
        -DBUILD_TESTING=OFF \
        -DWITH_BENCHMARK=OFF \
        -DWITH_EXAMPLES=OFF \
        -DWITH_OTLP_GRPC=ON \
        -DWITH_OTLP_HTTP=OFF \
        -DWITH_ABSEIL=ON \
        -DOPENTELEMETRY_BUILD_SHARED_LIBS=OFF \
    && cmake --build /build/otelcpp \
    && cmake --install /build/otelcpp

# ---------------------------------------------------------------------------
# Stage 3: emit-build — workload binary against installed otel-cpp
# Only this stage rebuilds when emit-app source changes.
# ---------------------------------------------------------------------------
FROM otelcpp-build AS emit-build

COPY bench/emit-app /src/emit-app

RUN cmake -S /src/emit-app -B /build/emit-app \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS}" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DBENCH_BACKEND=OTELCPP_GRPC \
        -DCMAKE_PREFIX_PATH=/opt/otelcpp \
    && cmake --build /build/emit-app

# ---------------------------------------------------------------------------
# Stage 4: runtime — minimal image with binary only
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

# Re-declare so this stage can read the global ARG.
ARG INSTALL_PERF=false

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libssl3 \
    && if [ "$INSTALL_PERF" = "true" ]; then \
        apt-get install -y --no-install-recommends linux-perf; \
    fi \
    && rm -rf /var/lib/apt/lists/*

COPY --from=emit-build /build/emit-app/emit_app /emit_app

EXPOSE 9090
ENTRYPOINT ["/emit_app"]
