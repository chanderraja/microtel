# syntax=docker/dockerfile:1
# rust-otel SUT: Rust application using opentelemetry-rust, OTLP/HTTP exporter.
# Build context must be the repo root.
#
#   docker build -f bench/sut/rust-otel/Dockerfile -t bench-sut-rust-otel .
#
# Flamegraph variant (B3):
#   docker build --build-arg INSTALL_PERF=true \
#       -f bench/sut/rust-otel/Dockerfile -t bench-sut-rust-otel:perf .
#   Run with --cap-add=PERFMON (Linux >=5.8).
#
# Layer caching:
#   deps    dependency compilation with dummy src/main.rs  — cached on Cargo.toml change
#   build   compile actual emit_app binary                 — rebuilds on source change
#   runtime minimal image with binary                      — seconds
#
# Cold dep build takes 5-10 min (tokio + opentelemetry ecosystem).
# With cache, rebuilds on source change take ~30 s.

ARG INSTALL_PERF=false

# ---------------------------------------------------------------------------
# Stage 1: deps — build dependency crate graph with a stub binary
# Cached as long as Cargo.toml (and Cargo.lock if present) are unchanged.
# ---------------------------------------------------------------------------
FROM rust:1.87-bookworm AS deps

WORKDIR /build

RUN apt-get update \
    && apt-get install -y --no-install-recommends pkg-config libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Frame pointers for flamegraph unwinding (release profile retains them).
ENV RUSTFLAGS="-C force-frame-pointers=yes"

COPY bench/sut/rust-otel/Cargo.toml bench/sut/rust-otel/Cargo.lock* ./
# Stub source: lets cargo resolve and build all dependencies without the real code.
RUN mkdir src && printf 'fn main() {}' > src/main.rs
RUN cargo build --release
# Remove stub fingerprint so the real source triggers a rebuild of emit_app only.
RUN rm -rf src \
    && find target/release/.fingerprint -name 'emit_app-*' -exec rm -rf {} + 2>/dev/null || true

# ---------------------------------------------------------------------------
# Stage 2: build — compile the actual emit_app binary
# ---------------------------------------------------------------------------
FROM deps AS build

COPY bench/sut/rust-otel/src ./src
RUN touch src/main.rs
RUN cargo build --release

# ---------------------------------------------------------------------------
# Stage 3: runtime
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS runtime

ARG INSTALL_PERF=false

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates libssl3 \
    && if [ "$INSTALL_PERF" = "true" ]; then \
        apt-get install -y --no-install-recommends linux-tools-generic \
        && PERF_BIN=$(find /usr/lib/linux-tools -name perf | head -1) \
        && ln -sf "$PERF_BIN" /usr/local/bin/perf; \
    fi \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /build/target/release/emit_app /emit_app

EXPOSE 9090
ENTRYPOINT ["/emit_app"]
