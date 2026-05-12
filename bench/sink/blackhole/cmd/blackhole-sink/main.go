// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

// blackhole-sink is a zero-logic OTLP receiver for benchmarking microtel.
// It accepts OTLP/gRPC on :4317 and OTLP/HTTP on :4318, counts spans and
// bytes with atomic counters, and exposes a control API on :8080.
package main

import (
	"context"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"

	tracepb "go.opentelemetry.io/proto/otlp/collector/trace/v1"
	"golang.org/x/net/http2"
	"golang.org/x/net/http2/h2c"
	"google.golang.org/grpc"
	"google.golang.org/grpc/reflection"

	"github.com/chanderraja/microtel/bench/sink/blackhole/internal/control"
	"github.com/chanderraja/microtel/bench/sink/blackhole/internal/counters"
	"github.com/chanderraja/microtel/bench/sink/blackhole/internal/otlpgrpc"
	"github.com/chanderraja/microtel/bench/sink/blackhole/internal/otlphttp"
)

const (
	grpcAddr    = ":4317"
	httpAddr    = ":4318"
	controlAddr = ":8080"
)

func main() {
	c := counters.New()

	grpcSrv := buildGRPC(c)
	httpSrv := buildHTTP(c)
	ctrlSrv := buildControl(c)

	grpcLis, err := net.Listen("tcp", grpcAddr)
	if err != nil {
		log.Fatalf("listen gRPC %s: %v", grpcAddr, err)
	}
	httpLis, err := net.Listen("tcp", httpAddr)
	if err != nil {
		log.Fatalf("listen HTTP %s: %v", httpAddr, err)
	}
	ctrlLis, err := net.Listen("tcp", controlAddr)
	if err != nil {
		log.Fatalf("listen control %s: %v", controlAddr, err)
	}

	go func() {
		log.Printf("gRPC listening on %s", grpcAddr)
		if err := grpcSrv.Serve(grpcLis); err != nil {
			log.Printf("gRPC server stopped: %v", err)
		}
	}()
	go func() {
		log.Printf("HTTP/2 listening on %s", httpAddr)
		if err := httpSrv.Serve(httpLis); err != nil && err != http.ErrServerClosed {
			log.Printf("HTTP server stopped: %v", err)
		}
	}()
	go func() {
		log.Printf("control listening on %s", controlAddr)
		if err := ctrlSrv.Serve(ctrlLis); err != nil && err != http.ErrServerClosed {
			log.Printf("control server stopped: %v", err)
		}
	}()

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	<-quit
	log.Println("shutting down")

	grpcSrv.GracefulStop()
	ctx, cancel := context.WithTimeout(context.Background(), 0)
	defer cancel()
	_ = httpSrv.Shutdown(ctx)
	_ = ctrlSrv.Shutdown(ctx)
}

func buildGRPC(c *counters.Counters) *grpc.Server {
	srv := grpc.NewServer()
	tracepb.RegisterTraceServiceServer(srv, otlpgrpc.New(c))
	reflection.Register(srv)
	return srv
}

func buildHTTP(c *counters.Counters) *http.Server {
	h2s := &http2.Server{}
	handler := otlphttp.NewHandler(c)
	return &http.Server{
		Handler: h2c.NewHandler(handler, h2s),
	}
}

func buildControl(c *counters.Counters) *http.Server {
	return &http.Server{
		Handler: control.NewHandler(c),
	}
}
