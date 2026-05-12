# syntax=docker/dockerfile:1
# microtel SUT: builds the microtel library from source and the emit-app
# against it. Build context must be the repo root.
#
#   docker build -f bench/sut/microtel/Dockerfile -t bench-sut-microtel .
#
# Layer caching:
#   deps        OS + apt packages          — cached until ubuntu_image changes
#   lib-build   microtel library + deps    — rebuilds on any source change
#   emit-build  emit-app binary            — rebuilds on emit-app source change
#   runtime     minimal image with binary  — seconds
#
# microtel is always built from the working tree — no version ARG is needed.
# The environment fingerprint records `git describe --always --dirty` so
# every result set is annotated with the exact source state that was benchmarked.

# ---------------------------------------------------------------------------
# Stage 1: deps — OS base and C dependencies
# Cached as long as the ubuntu image and package list are unchanged.
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
# Stage 2: lib-build — build microtel and its vendored deps (upb, tl-expected)
# Rebuilds when any source file in the repo changes.
# ---------------------------------------------------------------------------
FROM deps AS lib-build

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS}" \
        -DCMAKE_INSTALL_PREFIX=/opt/microtel \
        -DMICROTEL_BUILD_TESTS=OFF \
        -DMICROTEL_BUILD_BENCH=OFF \
    && cmake --build build \
    && cmake --install build

# ---------------------------------------------------------------------------
# Stage 3: emit-build — compile the workload binary against the installed lib
# Only this stage rebuilds when emit-app source changes.
# ---------------------------------------------------------------------------
FROM lib-build AS emit-build

RUN cmake -S /src/bench/emit-app -B /build/emit-app \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS}" \
        -DBENCH_BACKEND=MICROTEL \
        -DCMAKE_PREFIX_PATH=/opt/microtel \
    && cmake --build /build/emit-app

# ---------------------------------------------------------------------------
# Stage 4: runtime — minimal image containing only the binary
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libnghttp2-14 \
        libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=emit-build /build/emit-app/emit_app /emit_app

EXPOSE 9090
ENTRYPOINT ["/emit_app"]
