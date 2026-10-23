# syntax=docker/dockerfile:1
# The emulated microcontroller: the demo firmware cross-compiled for Cortex-M4
# and run bare-metal on QEMU's Arm MPS2 AN386 board. UART0 is exposed as a TCP
# server on :5555, which the gateway connects to as its "serial line".
# Build context: the repo root.

FROM ubuntu:24.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        cmake ninja-build gcc-arm-none-eabi libnewlib-arm-none-eabi \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S examples/demo-edge-trace/firmware -B /build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchains/arm-none-eabi.cmake \
        -DMICROTEL_LEAF_CPU=cortex-m4 -DCMAKE_BUILD_TYPE=MinSizeRel \
    && cmake --build /build \
    && arm-none-eabi-size /build/edge_sensor.elf

FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends qemu-system-arm \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /build/edge_sensor.elf /firmware/edge_sensor.elf
EXPOSE 5555
# -serial tcp:...,server=on,wait=off: boot immediately, accept the gateway
# whenever it connects. No semihosting, no monitor: the UART is the only link.
ENTRYPOINT ["qemu-system-arm", "-M", "mps2-an386", "-nographic", "-monitor", "none", \
            "-kernel", "/firmware/edge_sensor.elf", \
            "-serial", "tcp:0.0.0.0:5555,server=on,wait=off"]
