# syntax=docker/dockerfile:1
# The Linux side: edge_gateway, built against the microtel C++ SDK with the
# concentrator (leaf receiver) compiled in. Build context: the repo root.
# git: the configure step fetches toml++ and spdlog with FetchContent.

FROM ubuntu:24.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates clang cmake git ninja-build pkg-config \
        libssl-dev libnghttp2-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
ENV CC=clang CXX=clang++
WORKDIR /src
COPY . .
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DMICROTEL_BUILD_TESTS=OFF -DMICROTEL_BUILD_EXAMPLES=ON \
        -DMICROTEL_WITH_CONCENTRATOR=ON \
    && cmake --build /build --target microtel_example_edge_gateway

FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends libnghttp2-14 libssl3 zlib1g \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /build/examples/microtel_example_edge_gateway /usr/local/bin/edge_gateway
ENTRYPOINT ["/usr/local/bin/edge_gateway"]
