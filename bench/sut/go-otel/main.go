// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

// emit_app for the Go OTel SDK SUT.
//
// Implements the same ndjson control protocol as the C++ emit-app:
//   {"cmd":"run","spans":N,"threads":T,"rate_hz":R}  →  RunResult JSON
//   {"cmd":"flush"}                                   →  {"flush_ns":N}
//   {"cmd":"quit"}                                    →  {"ok":true}
//
// Configuration via environment variables (same names as C++ emit-app):
//   EMIT_ENDPOINT              — OTLP/HTTP base URL (default: http://sink:4318)
//   EMIT_SERVICE_NAME          — service.name resource attribute (default: bench)
//   EMIT_SERVICE_VER           — service.version attribute (default: 0.0.0)
//   EMIT_ATTRIBUTES_PER_SPAN   — number of string attributes per span (default: 0)
//   EMIT_ATTRIBUTE_VALUE_BYTES — byte length of each attribute value (default: 24)
//   EMIT_WORKLOAD              — hot_loop | realistic_request | hot_loop_metrics (default: hot_loop)

package main

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"log"
	"math/bits"
	"net"
	"net/url"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	metrichttp "go.opentelemetry.io/otel/exporters/otlp/otlpmetric/otlpmetrichttp"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracehttp"
	"go.opentelemetry.io/otel/metric"
	"go.opentelemetry.io/otel/sdk/resource"
	sdkmetric "go.opentelemetry.io/otel/sdk/metric"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/trace"
)

const controlPort = 19090

// ---------------------------------------------------------------------------
// 64-bucket power-of-2 histogram (matches C++ Histogram layout)
// Bucket i covers [2^i, 2^(i+1)) ns; bucket 0 covers [0, 2) ns.
// ---------------------------------------------------------------------------

type histogram struct {
	buckets [64]atomic.Uint64
	count   atomic.Uint64
	minNs   atomic.Uint64 // initialised to ^uint64(0); compare-and-swap min
	maxNs   atomic.Uint64
}

func newHistogram() *histogram {
	h := &histogram{}
	h.minNs.Store(^uint64(0))
	return h
}

func bucketFor(ns uint64) int {
	if ns < 2 {
		return 0
	}
	b := 63 - bits.LeadingZeros64(ns) // floor(log2(ns))
	if b > 63 {
		b = 63
	}
	return b
}

func (h *histogram) record(ns uint64) {
	h.buckets[bucketFor(ns)].Add(1)
	h.count.Add(1)
	for {
		cur := h.minNs.Load()
		if ns >= cur || h.minNs.CompareAndSwap(cur, ns) {
			break
		}
	}
	for {
		cur := h.maxNs.Load()
		if ns <= cur || h.maxNs.CompareAndSwap(cur, ns) {
			break
		}
	}
}

func (h *histogram) percentile(p float64) uint64 {
	total := h.count.Load()
	if total == 0 {
		return 0
	}
	target := uint64(float64(total)*p) + 1
	var cum uint64
	for i := 0; i < 64; i++ {
		cum += h.buckets[i].Load()
		if cum >= target {
			lo := uint64(0)
			if i > 0 {
				lo = uint64(1) << i
			}
			hi := uint64(1) << (i + 1)
			return (lo + hi) / 2
		}
	}
	return 0
}

func (h *histogram) snapshot() [64]uint64 {
	var snap [64]uint64
	for i := range snap {
		snap[i] = h.buckets[i].Load()
	}
	return snap
}

func (h *histogram) getMin() uint64 {
	v := h.minNs.Load()
	if v == ^uint64(0) {
		return 0
	}
	return v
}

// ---------------------------------------------------------------------------
// Token-bucket rate limiter (per-goroutine, not thread-safe)
// ---------------------------------------------------------------------------

type tokenBucket struct {
	intervalNs int64
	next       time.Time
}

func newTokenBucket(rateHz uint64) tokenBucket {
	if rateHz == 0 {
		return tokenBucket{}
	}
	return tokenBucket{intervalNs: int64(1_000_000_000 / rateHz)}
}

func (tb *tokenBucket) consume() {
	if tb.intervalNs == 0 {
		return
	}
	now := time.Now()
	if tb.next.After(now) {
		time.Sleep(tb.next.Sub(now))
	}
	tb.next = time.Now().Add(time.Duration(tb.intervalNs))
}

// ---------------------------------------------------------------------------
// Workload
// ---------------------------------------------------------------------------

type workloadMode int

const (
	modeHotLoop workloadMode = iota
	modeRealisticRequest
	modeHotLoopMetrics
)

type workloadOpts struct {
	mode      workloadMode
	attrKeys  []string
	attrValue string
	// metric instruments — only valid when mode == modeHotLoopMetrics
	counter   metric.Int64Counter
	histogram metric.Float64Histogram
}

func emitSpan(tr trace.Tracer, opts *workloadOpts) {
	_, span := tr.Start(context.Background(), "bench.span")
	for _, k := range opts.attrKeys {
		span.SetAttributes(attribute.String(k, opts.attrValue))
	}
	span.End()
}

func emitRequest(tr trace.Tracer) {
	ctx, parent := tr.Start(context.Background(), "bench.request")
	_, child1 := tr.Start(ctx, "bench.request.op1")
	child1.End()
	_, child2 := tr.Start(ctx, "bench.request.op2")
	child2.End()
	parent.End()
}

func emitRecord(opts *workloadOpts) {
	ctx := context.Background()
	opts.counter.Add(ctx, 1)
	opts.histogram.Record(ctx, 1.0)
}

func runWorkload(spanCount, threads, rateHz uint64, opts *workloadOpts, tr trace.Tracer) (*histogram, uint64) {
	hist := newHistogram()
	perThread := spanCount / threads
	remainder := spanCount % threads
	ratePerThread := uint64(0)
	if rateHz > 0 {
		ratePerThread = rateHz / threads
	}

	var wg sync.WaitGroup
	t0 := time.Now()

	for t := uint64(0); t < threads; t++ {
		count := perThread
		if t == 0 {
			count += remainder
		}
		wg.Add(1)
		go func(n uint64) {
			defer wg.Done()
			tb := newTokenBucket(ratePerThread)
			for i := uint64(0); i < n; i++ {
				tb.consume()
				s := time.Now()
				switch opts.mode {
				case modeRealisticRequest:
					emitRequest(tr)
				case modeHotLoopMetrics:
					emitRecord(opts)
				default:
					emitSpan(tr, opts)
				}
				hist.record(uint64(time.Since(s).Nanoseconds()))
			}
		}(count)
	}

	wg.Wait()
	return hist, uint64(time.Since(t0).Nanoseconds())
}

// ---------------------------------------------------------------------------
// Control protocol — JSON types
// ---------------------------------------------------------------------------

type runCmd struct {
	Spans   uint64 `json:"spans"`
	Threads uint64 `json:"threads"`
	RateHz  uint64 `json:"rate_hz"`
}

type droppedCounts struct {
	QueueFull               uint64 `json:"queue_full"`
	RecordTooLarge          uint64 `json:"record_too_large"`
	SpanAttributeLimit      uint64 `json:"span_attribute_limit"`
	AttributeValueTruncated uint64 `json:"attribute_value_truncated"`
	Other                   uint64 `json:"other"`
	Total                   uint64 `json:"total"`
}

type runResult struct {
	SpansEmitted     uint64        `json:"spans_emitted"`
	SpansDropped     droppedCounts `json:"spans_dropped"`
	BytesSent        uint64        `json:"bytes_sent"`
	DurationNs       uint64        `json:"duration_ns"`
	LatencyP50Ns     uint64        `json:"latency_p50_ns"`
	LatencyP95Ns     uint64        `json:"latency_p95_ns"`
	LatencyP99Ns     uint64        `json:"latency_p99_ns"`
	LatencyMinNs     uint64        `json:"latency_min_ns"`
	LatencyMaxNs     uint64        `json:"latency_max_ns"`
	LatencyHistogram [64]uint64    `json:"latency_histogram"`
}

// ---------------------------------------------------------------------------
// Control loop
// ---------------------------------------------------------------------------

func handleRun(line string, opts *workloadOpts, tr trace.Tracer) (runResult, error) {
	var cmd runCmd
	if err := json.Unmarshal([]byte(line), &cmd); err != nil {
		return runResult{}, err
	}
	if cmd.Threads == 0 {
		cmd.Threads = 1
	}

	hist, durNs := runWorkload(cmd.Spans, cmd.Threads, cmd.RateHz, opts, tr)

	return runResult{
		SpansEmitted:     hist.count.Load(),
		SpansDropped:     droppedCounts{},
		BytesSent:        0,
		DurationNs:       durNs,
		LatencyP50Ns:     hist.percentile(0.50),
		LatencyP95Ns:     hist.percentile(0.95),
		LatencyP99Ns:     hist.percentile(0.99),
		LatencyMinNs:     hist.getMin(),
		LatencyMaxNs:     hist.maxNs.Load(),
		LatencyHistogram: hist.snapshot(),
	}, nil
}

func runControlLoop(opts *workloadOpts, tr trace.Tracer,
	tp *sdktrace.TracerProvider, mp *sdkmetric.MeterProvider) {

	ln, err := net.Listen("tcp", fmt.Sprintf(":%d", controlPort))
	if err != nil {
		log.Fatalf("listen: %v", err)
	}
	defer ln.Close()

	// The bench driver's wait_tcp probe connects and immediately disconnects to
	// verify the port is open. We loop on Accept, skipping connections that send
	// no data (probes), until we get the real control connection.
	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Fatalf("accept: %v", err)
		}

		gotCommand := false
		scanner := bufio.NewScanner(conn)
		for scanner.Scan() {
			gotCommand = true
			line := scanner.Text()

			var envelope struct {
				Cmd string `json:"cmd"`
			}
			if err := json.Unmarshal([]byte(line), &envelope); err != nil {
				continue
			}

			switch envelope.Cmd {
			case "quit":
				fmt.Fprintln(conn, `{"ok":true}`)
				conn.Close()
				return

			case "run":
				result, runErr := handleRun(line, opts, tr)
				if runErr != nil {
					log.Printf("run error: %v", runErr)
					continue
				}
				b, _ := json.Marshal(result)
				fmt.Fprintln(conn, string(b))

			case "flush":
				t0 := time.Now()
				flushCtx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
				_ = tp.ForceFlush(flushCtx)
				if mp != nil {
					_ = mp.ForceFlush(flushCtx)
				}
				cancel()
				ns := time.Since(t0).Nanoseconds()
				fmt.Fprintf(conn, "{\"flush_ns\":%d}\n", ns)
			}
		}
		conn.Close()
		if gotCommand {
			// Driver closed without quit — bench run ended.
			return
		}
		// No commands received: this was a probe connection. Loop back and
		// accept the real control connection.
	}
}

// ---------------------------------------------------------------------------
// OTel initialisation
// ---------------------------------------------------------------------------

func initTracer(ctx context.Context, endpoint, serviceName, serviceVersion string) (*sdktrace.TracerProvider, error) {
	u, err := url.Parse(endpoint)
	if err != nil {
		return nil, fmt.Errorf("parse endpoint: %w", err)
	}

	httpOpts := []otlptracehttp.Option{
		otlptracehttp.WithEndpoint(u.Host),
		otlptracehttp.WithURLPath("/v1/traces"),
	}
	if u.Scheme == "http" {
		httpOpts = append(httpOpts, otlptracehttp.WithInsecure())
	}

	exp, err := otlptracehttp.New(ctx, httpOpts...)
	if err != nil {
		return nil, fmt.Errorf("new exporter: %w", err)
	}

	res, err := resource.New(ctx,
		resource.WithAttributes(
			attribute.String("service.name", serviceName),
			attribute.String("service.version", serviceVersion),
		),
	)
	if err != nil {
		res = resource.Default()
	}

	tp := sdktrace.NewTracerProvider(
		sdktrace.WithBatcher(exp),
		sdktrace.WithResource(res),
	)
	otel.SetTracerProvider(tp)
	return tp, nil
}

// initMeter sets up an OTLP/HTTP MeterProvider with a 100 ms export interval.
// The interval matches the microtel bench configuration so flush timing is comparable.
func initMeter(ctx context.Context, endpoint, serviceName, serviceVersion string) (*sdkmetric.MeterProvider, error) {
	u, err := url.Parse(endpoint)
	if err != nil {
		return nil, fmt.Errorf("parse endpoint: %w", err)
	}

	mOpts := []metrichttp.Option{
		metrichttp.WithEndpoint(u.Host),
		metrichttp.WithURLPath("/v1/metrics"),
	}
	if u.Scheme == "http" {
		mOpts = append(mOpts, metrichttp.WithInsecure())
	}

	exp, err := metrichttp.New(ctx, mOpts...)
	if err != nil {
		return nil, fmt.Errorf("new metric exporter: %w", err)
	}

	res, err := resource.New(ctx,
		resource.WithAttributes(
			attribute.String("service.name", serviceName),
			attribute.String("service.version", serviceVersion),
		),
	)
	if err != nil {
		res = resource.Default()
	}

	mp := sdkmetric.NewMeterProvider(
		sdkmetric.WithReader(sdkmetric.NewPeriodicReader(exp,
			sdkmetric.WithInterval(100*time.Millisecond))),
		sdkmetric.WithResource(res),
	)
	otel.SetMeterProvider(mp)
	return mp, nil
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

func main() {
	endpoint       := envOr("OTEL_EXPORTER_OTLP_ENDPOINT", envOr("EMIT_ENDPOINT", "http://sink:4318"))
	serviceName    := envOr("EMIT_SERVICE_NAME", "bench")
	serviceVersion := envOr("EMIT_SERVICE_VER", "0.0.0")
	attrsPerSpan, _ := strconv.Atoi(envOr("EMIT_ATTRIBUTES_PER_SPAN", "0"))
	attrValueBytes, _ := strconv.Atoi(envOr("EMIT_ATTRIBUTE_VALUE_BYTES", "24"))
	workloadEnv    := envOr("EMIT_WORKLOAD", "hot_loop")

	mode := modeHotLoop
	switch workloadEnv {
	case "realistic_request":
		mode = modeRealisticRequest
	case "hot_loop_metrics":
		mode = modeHotLoopMetrics
	}

	attrKeys := make([]string, attrsPerSpan)
	for i := range attrKeys {
		attrKeys[i] = fmt.Sprintf("bench.attr.%d", i)
	}
	attrValue := make([]byte, attrValueBytes)
	for i := range attrValue {
		attrValue[i] = 'x'
	}

	opts := &workloadOpts{
		mode:      mode,
		attrKeys:  attrKeys,
		attrValue: string(attrValue),
	}

	ctx := context.Background()

	tp, err := initTracer(ctx, endpoint, serviceName, serviceVersion)
	if err != nil {
		log.Fatalf("init tracer: %v", err)
	}

	tr := tp.Tracer("bench")

	var mp *sdkmetric.MeterProvider
	if mode == modeHotLoopMetrics {
		mp, err = initMeter(ctx, endpoint, serviceName, serviceVersion)
		if err != nil {
			log.Fatalf("init meter: %v", err)
		}
		meter := mp.Meter("bench")
		opts.counter, err = meter.Int64Counter("bench.records",
			metric.WithDescription("Records emitted"),
			metric.WithUnit("{record}"))
		if err != nil {
			log.Fatalf("create counter: %v", err)
		}
		opts.histogram, err = meter.Float64Histogram("bench.record_latency_ns",
			metric.WithDescription("Record hot-path latency"),
			metric.WithUnit("ns"))
		if err != nil {
			log.Fatalf("create histogram: %v", err)
		}
	}

	fmt.Fprintf(os.Stderr, "emit_app: ready on control port %d\n", controlPort)

	runControlLoop(opts, tr, tp, mp)

	shutCtx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if err := tp.Shutdown(shutCtx); err != nil {
		log.Printf("tracer shutdown: %v", err)
	}
	if mp != nil {
		if err := mp.Shutdown(shutCtx); err != nil {
			log.Printf("meter shutdown: %v", err)
		}
	}
}
