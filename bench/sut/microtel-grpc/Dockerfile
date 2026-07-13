# syntax=docker/dockerfile:1
# microtel-grpc SUT: builds microtel + emit-app together from source,
# configured to use microtel's gRPC-over-nghttp2 transport.
# Build context must be the repo root.
#
#   docker build -f bench/sut/microtel-grpc/Dockerfile -t bench-sut-microtel-grpc .
#
# Layer caching:
#   deps    OS + apt packages              — cached until ubuntu_image changes
#   build   microtel + emit-app (in-tree)  — rebuilds on any source change
#   runtime minimal image with binary      — seconds

# Global build arg — available in every stage.  Default: false (normal image).
ARG INSTALL_PERF=false

# ---------------------------------------------------------------------------
# Stage 1: deps — OS base and C dependencies
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS deps

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        clang-18 \
        cmake \
        ninja-build \
        git \
        pkg-config \
        libssl-dev \
        libnghttp2-dev \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

ENV CC=clang-18
ENV CXX=clang++-18
ENV BENCH_CXXFLAGS="-O2 -g -fno-omit-frame-pointer"

# ---------------------------------------------------------------------------
# Stage 2: build — compile microtel + emit-app together with gRPC transport
# ---------------------------------------------------------------------------
FROM deps AS build

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS}" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DMICROTEL_BUILD_TESTS=OFF \
        -DMICROTEL_BUILD_BENCH=ON \
        -DBENCH_BACKEND=MICROTEL \
        -DBENCH_MICROTEL_GRPC=ON \
    && cmake --build build --target emit_app

# ---------------------------------------------------------------------------
# Stage 3: runtime — minimal image containing only the binary
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

# Re-declare so this stage can read the global ARG.
ARG INSTALL_PERF=false

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libnghttp2-14 \
        libssl3 \
    && if [ "$INSTALL_PERF" = "true" ]; then \
        apt-get install -y --no-install-recommends linux-tools-generic \
        && PERF_BIN=$(find /usr/lib/linux-tools -name perf | head -1) \
        && ln -sf "$PERF_BIN" /usr/local/bin/perf; \
    fi \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/bench/emit-app/emit_app /emit_app

EXPOSE 9090
ENTRYPOINT ["/emit_app"]
