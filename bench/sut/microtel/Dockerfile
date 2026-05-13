# syntax=docker/dockerfile:1
# microtel SUT: builds microtel + emit-app together from source.
# Build context must be the repo root.
#
#   docker build -f bench/sut/microtel/Dockerfile -t bench-sut-microtel .
#
# Layer caching:
#   deps    OS + apt packages              — cached until ubuntu_image changes
#   build   microtel + emit-app (in-tree)  — rebuilds on any source change
#   runtime minimal image with binary      — seconds
#
# emit-app is built as an in-tree cmake subdirectory (MICROTEL_BUILD_BENCH=ON)
# so that microtel_sdk is available directly without an install() step.
# Tracked in issue #19: add install() targets so an out-of-tree build is
# possible in a future milestone.

# ---------------------------------------------------------------------------
# Stage 1: deps — OS base and C dependencies
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS deps

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
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
# Stage 2: build — compile microtel + emit-app together
# emit-app links against in-tree microtel_sdk; no install() step needed.
# ---------------------------------------------------------------------------
FROM deps AS build

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS}" \
        -DMICROTEL_BUILD_TESTS=OFF \
        -DMICROTEL_BUILD_BENCH=ON \
        -DBENCH_BACKEND=MICROTEL \
    && cmake --build build --target emit_app

# ---------------------------------------------------------------------------
# Stage 3: runtime — minimal image containing only the binary
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libnghttp2-14 \
        libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/bench/emit-app/emit_app /emit_app

EXPOSE 9090
ENTRYPOINT ["/emit_app"]
