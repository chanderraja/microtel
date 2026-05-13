# syntax=docker/dockerfile:1
# Shared base layer for all SUT images.
#
# Both microtel and otelcpp-grpc SUTs inherit from this stage (directly or
# by copying the apt install list). Compiler version is identical across all
# SUTs by construction — any benchmark difference reflects the library, not
# the toolchain.
#
# BENCH_CXXFLAGS is exported so child Dockerfiles and the emit-app CMake build
# pick it up without further configuration. All SUTs must use these flags
# unchanged — do not add per-SUT overrides.

FROM ubuntu:24.04

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

# Compiler flags applied to every SUT emit-app build.
# -O2                      release-optimised
# -g                       debug info retained for flamegraph (B3)
# -fno-omit-frame-pointer  frame-pointer based stack unwinding for perf
ENV BENCH_CXXFLAGS="-O2 -g -fno-omit-frame-pointer"
