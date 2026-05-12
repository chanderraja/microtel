# syntax=docker/dockerfile:1
# Build stage
FROM golang:1.25-alpine AS builder
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" \
    -o /out/blackhole-sink ./cmd/blackhole-sink

# Runtime stage — distroless for minimal attack surface
FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=builder /out/blackhole-sink /blackhole-sink
EXPOSE 4317 4318 8080
ENTRYPOINT ["/blackhole-sink"]
