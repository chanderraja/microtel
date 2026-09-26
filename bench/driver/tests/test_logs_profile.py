# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""The hot-loop-logs profile: signal=logs counts log records at the sink,
so delivery, records/sec and wire bytes/record are all defined."""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from driver.__main__ import (
    _delivery_rate_pct,
    _received_count,
    _sink_mode_error,
    _spans_per_iteration,
)
from driver.profile import load
from driver.report import _delivery_rate_from_sink, _render_md, _summarize
from driver.sink_client import SinkClient
from driver.tests.test_sink_client import _FakeHTTPServer

PROFILES_DIR = Path(__file__).parent.parent.parent / "profiles"


# ---------------------------------------------------------------------------
# Profile
# ---------------------------------------------------------------------------

def test_load_hot_loop_logs():
    p = load(PROFILES_DIR, "hot-loop-logs")
    assert p.name == "hot-loop-logs"
    assert p.signal == "logs"
    assert p.env["EMIT_WORKLOAD"] == "hot_loop_logs"
    assert p.sink_mode == "blackhole"
    # Both protocols for both libraries, as hot-loop-traces runs.
    for sut in ("microtel", "microtel-grpc", "otelcpp-grpc", "otelcpp-http"):
        assert sut in p.suts


# ---------------------------------------------------------------------------
# Per-sample counting
# ---------------------------------------------------------------------------

def test_one_log_record_per_iteration():
    assert _spans_per_iteration({"EMIT_WORKLOAD": "hot_loop_logs"}) == 1


def test_received_count_logs_reads_log_records():
    snap = {"spans_received": 0, "log_records_received": 9_000}
    assert _received_count(snap, "logs") == 9_000


def test_received_count_traces_reads_spans():
    snap = {"spans_received": 7_000, "log_records_received": 0}
    assert _received_count(snap, "traces") == 7_000


def test_received_count_logs_missing_field_is_zero():
    # An older sink image without log counting reports no records, not a crash.
    assert _received_count({"spans_received": 0}, "logs") == 0


def test_delivery_rate_defined_for_logs():
    assert _delivery_rate_pct(9_500, 10_000, "logs") == 95.0


def test_delivery_rate_still_none_for_metrics():
    assert _delivery_rate_pct(0, 10_000, "metrics") is None


# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

def _log_sample(expected: int, records_rx: int, bytes_rx: int):
    return {
        "spans_emitted": expected,
        "spans_expected": expected,
        "items_received": records_rx,
        "spans_dropped": {"total": 0},
        "delivery_rate_pct": round(records_rx / expected * 100, 2),
        "latency_p50_ns": 100, "latency_p95_ns": 200, "latency_p99_ns": 300,
        "latency_min_ns": 50, "latency_max_ns": 900,
        "flush_ns": 1_000,
        "sink": {"spans_received": 0, "log_records_received": records_rx,
                 "bytes_received": bytes_rx},
    }


def test_summary_delivery_counts_log_records():
    samples = [_log_sample(10_000, 8_000, 400_000) for _ in range(2)]
    assert _delivery_rate_from_sink(samples) == 80.0


def test_summary_wire_bytes_per_log_record():
    samples = [_log_sample(10_000, 10_000, 450_000)]
    assert _summarize(samples)["wire_bytes_per_span"]["median"] == 45.0


def test_summary_legacy_trace_samples_still_use_spans_received():
    legacy = [{
        "spans_emitted": 1_000,
        "delivery_rate_pct": 100.0,
        "sink": {"spans_received": 1_000},
    }]
    assert _delivery_rate_from_sink(legacy) == 100.0


def test_markdown_labels_log_rows_per_record():
    doc = {
        "profile": {"name": "hot-loop-logs", "signal": "logs"},
        "suts": [{"name": "microtel",
                  "summary": _summarize([_log_sample(100, 100, 4_000)])}],
    }
    md = _render_md(doc)
    assert "Emit p50 (ns)" in md
    assert "Records/sec" in md
    assert "Wire bytes/record" in md
    assert "Records dropped (total)" in md
    assert "StartSpan p50" not in md


def test_markdown_trace_labels_unchanged():
    doc = {"profile": {"name": "hot-loop-traces"}, "suts": [{"name": "x", "summary": {}}]}
    md = _render_md(doc)
    assert "StartSpan p50 (ns)" in md
    assert "Spans/sec" in md
    assert "Wire bytes/span" in md


def test_collector_sink_rejected_for_logs():
    # The collector sink has no logs pipeline and scrapes only span counters,
    # so a logs run against it would report 0% delivery.
    assert _sink_mode_error("logs", "collector") is not None


def test_blackhole_sink_accepted_for_logs():
    assert _sink_mode_error("logs", "blackhole") is None


def test_collector_sink_accepted_for_traces():
    assert _sink_mode_error("traces", "collector") is None


# ---------------------------------------------------------------------------
# Sink client
# ---------------------------------------------------------------------------

def _stats_server(payload: dict):
    body = json.dumps(payload)
    return _FakeHTTPServer(lambda path: (200, body) if path == "/stats" else (404, ""))


def test_sink_client_reports_log_records():
    with _stats_server({"spans_received": 0, "bytes_received": 10,
                        "log_records_received": 42}) as srv:
        snap = SinkClient("127.0.0.1", srv.port).stats()
    assert snap["log_records_received"] == 42


def test_sink_client_log_records_default_zero():
    with _stats_server({"spans_received": 0, "bytes_received": 10}) as srv:
        snap = SinkClient("127.0.0.1", srv.port).stats()
    assert snap["log_records_received"] == 0
