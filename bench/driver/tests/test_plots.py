# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

plotly = pytest.importorskip("plotly", reason="plotly not installed")

from driver.plots import write_plots, _has_byte_data


# ---------------------------------------------------------------------------
# Synthetic document fixture
# ---------------------------------------------------------------------------

def _make_sample(p50=100, p95=200, p99=300, pmin=50, pmax=400,
                 emitted=1000, dropped=0, bytes_recv=None):
    return {
        "latency_p50_ns": p50,
        "latency_p95_ns": p95,
        "latency_p99_ns": p99,
        "latency_min_ns": pmin,
        "latency_max_ns": pmax,
        "spans_emitted": emitted,
        "spans_dropped": {"total": dropped, "queue_full": 0, "other": 0,
                          "record_too_large": 0, "span_attribute_limit": 0,
                          "attribute_value_truncated": 0},
        "bytes_sent": 0,
        "duration_ns": 1_000_000,
        "sink": {
            "mode": "blackhole",
            "spans_received": emitted,
            "bytes_received": bytes_recv,
        },
    }


def _make_doc(with_bytes=False):
    bytes_val = 128 if with_bytes else None
    samples_a = [_make_sample(p50=100 + i, bytes_recv=bytes_val) for i in range(5)]
    samples_b = [_make_sample(p50=150 + i, bytes_recv=bytes_val) for i in range(5)]

    wire_bytes_summary = (
        {"median": 128.0, "p25": 120.0, "p75": 135.0, "min": 110.0, "max": 140.0}
        if with_bytes else None
    )

    return {
        "schema_version": "1.0",
        "generated_at": "2026-05-25T00:00:00Z",
        "warnings": [],
        "profile": {
            "name": "hot-loop-traces",
            "spans_per_sample": 1000,
            "samples": 5,
            "warmup_spans": 100,
            "sink_mode": "blackhole" if with_bytes else "collector",
        },
        "environment": {
            "cpu_model": "Test CPU",
            "cpu_physical_cores": 4,
            "cpu_governor": "performance",
            "hyperthreading": False,
            "kernel": "6.8.0",
            "container_engine": "docker",
            "container_engine_version": "27.0",
            "host_load_avg_1m": 0.5,
            "bench_started_at": "2026-05-25T00:00:00Z",
        },
        "suts": [
            {
                "name": "microtel",
                "library": "microtel",
                "transport": "http",
                "library_version": "",
                "library_build_flags": "",
                "image_tag": "bench-sut-microtel",
                "image_id": "sha256:abc",
                "samples": samples_a,
                "summary": {
                    "reps": 5,
                    "latency_p50_ns": {"median": 102.0, "p25": 100.0, "p75": 104.0,
                                       "min": 100.0, "max": 104.0},
                    "latency_p95_ns": {"median": 202.0, "p25": 200.0, "p75": 204.0,
                                       "min": 200.0, "max": 204.0},
                    "latency_p99_ns": {"median": 302.0, "p25": 300.0, "p75": 304.0,
                                       "min": 300.0, "max": 304.0},
                    "latency_min_ns": {"median": 50.0, "p25": 50.0, "p75": 50.0,
                                       "min": 50.0, "max": 50.0},
                    "latency_max_ns": {"median": 400.0, "p25": 400.0, "p75": 400.0,
                                       "min": 400.0, "max": 400.0},
                    "wire_bytes_per_span": wire_bytes_summary,
                    "drop_rate_pct": 0.0,
                    "spans_emitted_total": 5000,
                    "spans_dropped": {"total": 0},
                },
                "flamegraph_svg": None,
            },
            {
                "name": "otelcpp-grpc",
                "library": "otelcpp",
                "transport": "grpc",
                "library_version": "",
                "library_build_flags": "",
                "image_tag": "bench-sut-otelcpp-grpc",
                "image_id": "sha256:def",
                "samples": samples_b,
                "summary": {
                    "reps": 5,
                    "latency_p50_ns": {"median": 152.0, "p25": 150.0, "p75": 154.0,
                                       "min": 150.0, "max": 154.0},
                    "latency_p95_ns": {"median": 252.0, "p25": 250.0, "p75": 254.0,
                                       "min": 250.0, "max": 254.0},
                    "latency_p99_ns": {"median": 352.0, "p25": 350.0, "p75": 354.0,
                                       "min": 350.0, "max": 354.0},
                    "latency_min_ns": {"median": 50.0, "p25": 50.0, "p75": 50.0,
                                       "min": 50.0, "max": 50.0},
                    "latency_max_ns": {"median": 400.0, "p25": 400.0, "p75": 400.0,
                                       "min": 400.0, "max": 400.0},
                    "wire_bytes_per_span": wire_bytes_summary,
                    "drop_rate_pct": 0.0,
                    "spans_emitted_total": 5000,
                    "spans_dropped": {"total": 0},
                },
                "flamegraph_svg": None,
            },
        ],
    }


# ---------------------------------------------------------------------------
# _has_byte_data
# ---------------------------------------------------------------------------

def test_has_byte_data_true():
    doc = _make_doc(with_bytes=True)
    assert _has_byte_data(doc["suts"]) is True


def test_has_byte_data_false():
    doc = _make_doc(with_bytes=False)
    assert _has_byte_data(doc["suts"]) is False


# ---------------------------------------------------------------------------
# write_plots — without byte data
# ---------------------------------------------------------------------------

def test_write_plots_produces_html(tmp_path):
    doc = _make_doc(with_bytes=False)
    path = write_plots(doc, tmp_path)

    assert path is not None
    assert path.exists()
    assert path.suffix == ".html"
    content = path.read_text(encoding="utf-8")
    assert "plotly" in content.lower()


def test_write_plots_contains_sut_names(tmp_path):
    doc = _make_doc(with_bytes=False)
    path = write_plots(doc, tmp_path)

    content = path.read_text(encoding="utf-8")
    assert "microtel" in content
    assert "otelcpp-grpc" in content


def test_write_plots_creates_out_dir(tmp_path):
    out = tmp_path / "nested" / "results"
    doc = _make_doc(with_bytes=False)
    path = write_plots(doc, out)

    assert path is not None
    assert out.exists()


# ---------------------------------------------------------------------------
# write_plots — with byte data (3-row layout)
# ---------------------------------------------------------------------------

def test_write_plots_with_bytes(tmp_path):
    doc = _make_doc(with_bytes=True)
    path = write_plots(doc, tmp_path)

    assert path is not None
    content = path.read_text(encoding="utf-8")
    # Wire bytes subplot title should appear in the output
    assert "Wire bytes" in content


# ---------------------------------------------------------------------------
# write_plots — edge cases
# ---------------------------------------------------------------------------

def test_write_plots_returns_none_for_empty_suts(tmp_path):
    doc = _make_doc()
    doc["suts"] = []
    assert write_plots(doc, tmp_path) is None


def test_write_plots_returns_none_for_suts_with_no_samples(tmp_path):
    doc = _make_doc()
    for sut in doc["suts"]:
        sut["samples"] = []
    assert write_plots(doc, tmp_path) is None


def test_write_plots_single_sut(tmp_path):
    doc = _make_doc(with_bytes=False)
    doc["suts"] = doc["suts"][:1]
    path = write_plots(doc, tmp_path)
    assert path is not None
    assert path.exists()


def test_write_plots_single_sample(tmp_path):
    doc = _make_doc(with_bytes=False)
    for sut in doc["suts"]:
        sut["samples"] = sut["samples"][:1]
    path = write_plots(doc, tmp_path)
    assert path is not None
