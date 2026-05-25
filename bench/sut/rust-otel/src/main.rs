// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

//! emit_app for the Rust OTel SDK SUT.
//!
//! Implements the same ndjson control protocol as the C++ emit-app:
//!   {"cmd":"run","spans":N,"threads":T,"rate_hz":R}  →  RunResult JSON
//!   {"cmd":"quit"}                                    →  {"ok":true}
//!
//! Configuration via environment variables (same names as C++ emit-app):
//!   EMIT_ENDPOINT              — OTLP/HTTP base URL (default: http://sink:4318)
//!   EMIT_SERVICE_NAME          — service.name resource attribute (default: bench)
//!   EMIT_SERVICE_VER           — service.version attribute (default: 0.0.0)
//!   EMIT_ATTRIBUTES_PER_SPAN   — number of string attributes per span (default: 0)
//!   EMIT_ATTRIBUTE_VALUE_BYTES — byte length of each attribute value (default: 24)
//!   EMIT_WORKLOAD              — hot_loop | realistic_request (default: hot_loop)

use std::io::{BufRead, BufReader, Write as IoWrite};
use std::net::TcpListener;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use opentelemetry::trace::{Span, SpanContext, Tracer, TraceContextExt};
use opentelemetry::{global, Context, KeyValue};
use opentelemetry_sdk::{runtime, trace as sdk_trace, Resource};
use serde::{Deserialize, Serialize};

// ---------------------------------------------------------------------------
// 64-bucket power-of-2 histogram (matches C++ Histogram layout)
// Bucket i covers [2^i, 2^(i+1)) ns; bucket 0 covers [0, 2) ns.
// ---------------------------------------------------------------------------

struct Histogram {
    buckets: [AtomicU64; 64],
    count:   AtomicU64,
    min_ns:  AtomicU64, // initialised to u64::MAX; fetch_min on each record
    max_ns:  AtomicU64,
}

impl Histogram {
    fn new() -> Self {
        Self {
            buckets: std::array::from_fn(|_| AtomicU64::new(0)),
            count:   AtomicU64::new(0),
            min_ns:  AtomicU64::new(u64::MAX),
            max_ns:  AtomicU64::new(0),
        }
    }

    fn bucket_for(ns: u64) -> usize {
        if ns < 2 {
            return 0;
        }
        // floor(log2(ns)) = 63 - leading_zeros
        let b = 63usize.saturating_sub(ns.leading_zeros() as usize);
        b.min(63)
    }

    fn record(&self, ns: u64) {
        self.buckets[Self::bucket_for(ns)].fetch_add(1, Ordering::Relaxed);
        self.count.fetch_add(1, Ordering::Relaxed);
        self.min_ns.fetch_min(ns, Ordering::Relaxed);
        self.max_ns.fetch_max(ns, Ordering::Relaxed);
    }

    fn percentile(&self, p: f64) -> u64 {
        let total = self.count.load(Ordering::Relaxed);
        if total == 0 {
            return 0;
        }
        let target = ((total as f64) * p).ceil() as u64;
        let mut cum: u64 = 0;
        for (i, b) in self.buckets.iter().enumerate() {
            cum += b.load(Ordering::Relaxed);
            if cum >= target {
                let lo: u64 = if i == 0 { 0 } else { 1u64 << i };
                let hi: u64 = 1u64 << (i + 1);
                return (lo + hi) / 2;
            }
        }
        0
    }

    fn snapshot(&self) -> [u64; 64] {
        std::array::from_fn(|i| self.buckets[i].load(Ordering::Relaxed))
    }

    fn get_min(&self) -> u64 {
        let v = self.min_ns.load(Ordering::Relaxed);
        if v == u64::MAX { 0 } else { v }
    }
}

// ---------------------------------------------------------------------------
// Token-bucket rate limiter (per-thread, not Send)
// ---------------------------------------------------------------------------

struct TokenBucket {
    interval_ns: u64,
    next: Instant,
}

impl TokenBucket {
    fn new(rate_hz: u64) -> Self {
        Self {
            interval_ns: if rate_hz > 0 { 1_000_000_000 / rate_hz } else { 0 },
            next: Instant::now(),
        }
    }

    fn consume(&mut self) {
        if self.interval_ns == 0 {
            return;
        }
        let now = Instant::now();
        if now < self.next {
            std::thread::sleep(self.next - now);
        }
        self.next = Instant::now() + Duration::from_nanos(self.interval_ns);
    }
}

// ---------------------------------------------------------------------------
// Workload
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq)]
enum WorkloadMode {
    HotLoop,
    RealisticRequest,
}

fn emit_span(attr_keys: &[String], attr_value: &str) {
    let tracer = global::tracer("bench");
    let mut span = tracer.start("bench.span");
    for k in attr_keys {
        span.set_attribute(KeyValue::new(k.clone(), attr_value.to_owned()));
    }
    span.end();
}

fn emit_request() {
    let tracer = global::tracer("bench");
    let mut parent = tracer.start("bench.request");
    // Use the parent's SpanContext to propagate trace+span IDs to children.
    let parent_sc: SpanContext = parent.span_context().clone();
    let cx = Context::default().with_remote_span_context(parent_sc);
    let mut child1 = tracer.start_with_context("bench.request.op1", &cx);
    child1.end();
    let mut child2 = tracer.start_with_context("bench.request.op2", &cx);
    child2.end();
    parent.end();
}

fn run_workload(
    span_count: u64,
    threads: u64,
    rate_hz: u64,
    mode: WorkloadMode,
    attr_keys: Arc<Vec<String>>,
    attr_value: Arc<String>,
) -> (Arc<Histogram>, Duration) {
    let hist = Arc::new(Histogram::new());
    let per_thread = span_count / threads;
    let remainder = span_count % threads;
    let rate_per_thread = if rate_hz > 0 { rate_hz / threads } else { 0 };

    let t0 = Instant::now();

    let handles: Vec<_> = (0..threads)
        .map(|t| {
            let count = per_thread + if t == 0 { remainder } else { 0 };
            let hist = Arc::clone(&hist);
            let attr_keys = Arc::clone(&attr_keys);
            let attr_value = Arc::clone(&attr_value);
            std::thread::spawn(move || {
                let mut tb = TokenBucket::new(rate_per_thread);
                for _ in 0..count {
                    tb.consume();
                    let t0 = Instant::now();
                    match mode {
                        WorkloadMode::HotLoop => {
                            emit_span(&attr_keys, &attr_value);
                        }
                        WorkloadMode::RealisticRequest => {
                            emit_request();
                        }
                    }
                    hist.record(t0.elapsed().as_nanos() as u64);
                }
            })
        })
        .collect();

    for h in handles {
        h.join().expect("worker thread panicked");
    }

    (hist, t0.elapsed())
}

// ---------------------------------------------------------------------------
// Control protocol — JSON types
// ---------------------------------------------------------------------------

#[derive(Deserialize)]
struct RunCmd {
    spans:    u64,
    threads:  Option<u64>,
    rate_hz:  Option<u64>,
}

#[derive(Serialize, Default)]
struct DroppedCounts {
    queue_full:               u64,
    record_too_large:         u64,
    span_attribute_limit:     u64,
    attribute_value_truncated: u64,
    other:                    u64,
    total:                    u64,
}

#[derive(Serialize)]
struct RunResult {
    spans_emitted:      u64,
    spans_dropped:      DroppedCounts,
    bytes_sent:         u64,
    duration_ns:        u64,
    latency_p50_ns:     u64,
    latency_p95_ns:     u64,
    latency_p99_ns:     u64,
    latency_min_ns:     u64,
    latency_max_ns:     u64,
    latency_histogram:  [u64; 64],
}

// ---------------------------------------------------------------------------
// Control loop
// ---------------------------------------------------------------------------

fn handle_run(
    line: &str,
    mode: WorkloadMode,
    attr_keys: &Arc<Vec<String>>,
    attr_value: &Arc<String>,
) -> RunResult {
    let cmd: RunCmd = serde_json::from_str(line).expect("malformed run command");
    let threads = cmd.threads.unwrap_or(1).max(1);
    let rate_hz = cmd.rate_hz.unwrap_or(0);

    let (hist, dur) = run_workload(
        cmd.spans,
        threads,
        rate_hz,
        mode,
        Arc::clone(attr_keys),
        Arc::clone(attr_value),
    );

    RunResult {
        spans_emitted:     hist.count.load(Ordering::Relaxed),
        spans_dropped:     DroppedCounts::default(),
        bytes_sent:        0,
        duration_ns:       dur.as_nanos() as u64,
        latency_p50_ns:    hist.percentile(0.50),
        latency_p95_ns:    hist.percentile(0.95),
        latency_p99_ns:    hist.percentile(0.99),
        latency_min_ns:    hist.get_min(),
        latency_max_ns:    hist.max_ns.load(Ordering::Relaxed),
        latency_histogram: hist.snapshot(),
    }
}

fn control_loop(
    mode: WorkloadMode,
    attr_keys: Arc<Vec<String>>,
    attr_value: Arc<String>,
) {
    let listener = TcpListener::bind(("0.0.0.0", 9090u16))
        .expect("failed to bind port 9090");

    let (stream, _) = listener.accept().expect("accept failed");
    drop(listener);

    let mut writer = stream.try_clone().expect("clone stream");
    let reader = BufReader::new(stream);

    for line in reader.lines() {
        let line = line.expect("read error");

        // Extract cmd field with a simple JSON envelope parse.
        let cmd: serde_json::Value =
            serde_json::from_str(&line).unwrap_or(serde_json::Value::Null);
        let cmd_str = cmd.get("cmd").and_then(|v| v.as_str()).unwrap_or("");

        match cmd_str {
            "quit" => {
                let _ = writeln!(writer, r#"{{"ok":true}}"#);
                return;
            }
            "run" => {
                let result = handle_run(&line, mode, &attr_keys, &attr_value);
                let json = serde_json::to_string(&result).expect("serialize");
                let _ = writeln!(writer, "{json}");
            }
            _ => {}
        }
    }
}

// ---------------------------------------------------------------------------
// OTel initialisation
// ---------------------------------------------------------------------------

fn init_tracer(
    endpoint: &str,
    service_name: &str,
    service_version: &str,
) -> Result<sdk_trace::SdkTracerProvider, Box<dyn std::error::Error + Send + Sync>> {
    let exporter = opentelemetry_otlp::SpanExporter::builder()
        .with_http()
        .with_endpoint(endpoint)
        .build()?;

    let processor = sdk_trace::BatchSpanProcessor::builder(exporter, runtime::Tokio).build();

    let resource = Resource::new(vec![
        KeyValue::new("service.name",    service_name.to_owned()),
        KeyValue::new("service.version", service_version.to_owned()),
    ]);

    let provider = sdk_trace::SdkTracerProvider::builder()
        .with_span_processor(processor)
        .with_resource(resource)
        .build();

    global::set_tracer_provider(provider.clone());
    Ok(provider)
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn env_or<'a>(key: &str, fallback: &'a str) -> String {
    std::env::var(key).unwrap_or_else(|_| fallback.to_owned())
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

#[tokio::main]
async fn main() {
    // The bench driver sets OTEL_EXPORTER_OTLP_ENDPOINT for all SUTs.
    // EMIT_ENDPOINT is a developer override; OTEL_EXPORTER_OTLP_ENDPOINT takes priority.
    let endpoint = std::env::var("OTEL_EXPORTER_OTLP_ENDPOINT")
        .unwrap_or_else(|_| env_or("EMIT_ENDPOINT", "http://sink:4318"));
    let service_name    = env_or("EMIT_SERVICE_NAME",          "bench");
    let service_version = env_or("EMIT_SERVICE_VER",           "0.0.0");
    let attrs_per_span: usize =
        env_or("EMIT_ATTRIBUTES_PER_SPAN", "0").parse().unwrap_or(0);
    let attr_value_bytes: usize =
        env_or("EMIT_ATTRIBUTE_VALUE_BYTES", "24").parse().unwrap_or(24);
    let workload_env = env_or("EMIT_WORKLOAD", "hot_loop");

    let mode = if workload_env == "realistic_request" {
        WorkloadMode::RealisticRequest
    } else {
        WorkloadMode::HotLoop
    };

    let attr_keys: Arc<Vec<String>> = Arc::new(
        (0..attrs_per_span)
            .map(|i| format!("bench.attr.{i}"))
            .collect(),
    );
    let attr_value: Arc<String> = Arc::new("x".repeat(attr_value_bytes));

    let provider = init_tracer(&endpoint, &service_name, &service_version)
        .expect("failed to initialise OTel tracer");

    eprintln!("emit_app: ready on control port 9090");

    // Run the control socket on a blocking thread so the tokio runtime
    // remains available for the OTel batch processor in the background.
    let ak = Arc::clone(&attr_keys);
    let av = Arc::clone(&attr_value);
    tokio::task::spawn_blocking(move || {
        control_loop(mode, ak, av);
    })
    .await
    .expect("control loop panicked");

    provider.shutdown().expect("OTel shutdown failed");
}
