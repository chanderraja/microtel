# syntax=docker/dockerfile:1
# go-otel SUT: Go application using go.opentelemetry.io/otel, OTLP/HTTP exporter.
# Build context must be the repo root.
#
#   docker build -f bench/sut/go-otel/Dockerfile -t bench-sut-go-otel .
#
# Flamegraph variant (B3):
#   docker build --build-arg INSTALL_PERF=true \
#       -f bench/sut/go-otel/Dockerfile -t bench-sut-go-otel:perf .
#   Run with --cap-add=PERFMON (Linux >=5.8).
#
# Go 1.12+ emits frame pointers on amd64 by default — no extra flags needed
# for perf / flamegraph unwinding.
#
# Layer caching:
#   deps    module download                        — cached until go.mod changes
#   build   compile emit_app                      — rebuilds on source change
#   runtime minimal image with static binary      — seconds

ARG INSTALL_PERF=false

# ---------------------------------------------------------------------------
# Stage 1: deps — download Go modules
# ---------------------------------------------------------------------------
FROM golang:1.24-bookworm AS deps

WORKDIR /build

# GONOSUMDB=* allows go mod download without a committed go.sum.
# For reproducible builds: run `go mod tidy` locally and commit go.sum.
ENV GONOSUMDB='*'
ENV GOFLAGS='-mod=mod'

COPY bench/sut/go-otel/go.mod bench/sut/go-otel/go.sum* ./
RUN go mod download

# ---------------------------------------------------------------------------
# Stage 2: build
# ---------------------------------------------------------------------------
FROM deps AS build

COPY bench/sut/go-otel/main.go ./

# CGO_ENABLED=0 → fully static binary; no libc dependency in runtime image.
# -ldflags="-w -s" strips DWARF and symbol table to reduce binary size.
# Note: strip debug info ONLY from the final binary, not the intermediate stages;
#       the build itself retains symbols for the compiler.
RUN CGO_ENABLED=0 GOFLAGS='-mod=mod' \
    go build -o emit_app .

# ---------------------------------------------------------------------------
# Stage 3: runtime
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS runtime

ARG INSTALL_PERF=false

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates \
    && if [ "$INSTALL_PERF" = "true" ]; then \
        apt-get install -y --no-install-recommends linux-tools-generic \
        && PERF_BIN=$(find /usr/lib/linux-tools -name perf | head -1) \
        && ln -sf "$PERF_BIN" /usr/local/bin/perf; \
    fi \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /build/emit_app /emit_app

EXPOSE 9090
ENTRYPOINT ["/emit_app"]
