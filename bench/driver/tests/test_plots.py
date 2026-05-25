# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

plotly = pytest.importorskip("plotly", reason="plotly not installed")

from driver.plots import (
    write_plots,
    _build_cdf_points,
    _has_byte_data,
    _latency_bar,
    _latency_cdf,
    _stability,
    _wire_bytes,
)


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

def _hist(dominant_bucket: int = 10) -> list[int]:
    """64-bucket histogram with all counts in one bucket (simple but valid)."""
    h = [0] * 64
    h[dominant_bucket] = 1000
    return h


def _make_sample(p50=100, p95=200, p99=300, pmin=50, pmax=400,
                 emitted=1000, dropped=0, bytes_recv=None,
                 with_histogram: bool = True):
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
        "latency_histogram": _hist() if with_histogram else [],
        "sink": {
            "mode": "blackhole",
            "spans_received": emitted,
            "bytes_received": bytes_recv,
        },
    }


def _sut(name, library, p50_offset=0, bytes_val=None, with_histogram=True):
    samples = [
        _make_sample(
            p50=100 + p50_offset + i,
            bytes_recv=bytes_val,
            with_histogram=with_histogram,
        )
        for i in range(5)
    ]
    wb = (
        {"median": float(bytes_val), "p25": float(bytes_val) * 0.9,
         "p75": float(bytes_val) * 1.1, "min": float(bytes_val) * 0.8,
         "max": float(bytes_val) * 1.2}
        if bytes_val else None
    )
    return {
        "name": name,
        "library": library,
        "transport": "http",
        "library_version": "",
        "library_build_flags": "",
        "image_tag": f"bench-sut-{name}",
        "image_id": "sha256:abc",
        "samples": samples,
        "summary": {
            "reps": 5,
            "latency_p50_ns": {"median": 102.0 + p50_offset, "p25": 100.0 + p50_offset,
                                "p75": 104.0 + p50_offset, "min": 100.0, "max": 104.0},
            "latency_p95_ns": {"median": 202.0, "p25": 200.0, "p75": 204.0,
                                "min": 200.0, "max": 204.0},
            "latency_p99_ns": {"median": 302.0, "p25": 300.0, "p75": 304.0,
                                "min": 300.0, "max": 304.0},
            "latency_min_ns": {"median": 50.0, "p25": 50.0, "p75": 50.0,
                                "min": 50.0, "max": 50.0},
            "latency_max_ns": {"median": 400.0, "p25": 400.0, "p75": 400.0,
                                "min": 400.0, "max": 400.0},
            "wire_bytes_per_span": wb,
            "drop_rate_pct": 0.0,
            "spans_emitted_total": 5000,
            "spans_dropped": {"total": 0},
        },
        "flamegraph_svg": None,
    }


def _make_doc(with_bytes=False, with_histogram=True):
    bytes_val = 128 if with_bytes else None
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
            _sut("microtel", "microtel",
                 p50_offset=0, bytes_val=bytes_val, with_histogram=with_histogram),
            _sut("otelcpp-grpc", "otelcpp",
                 p50_offset=50, bytes_val=bytes_val, with_histogram=with_histogram),
        ],
    }


# ---------------------------------------------------------------------------
# _build_cdf_points
# ---------------------------------------------------------------------------

def test_build_cdf_empty():
    xs, ys = _build_cdf_points([0] * 64)
    assert xs == []
    assert ys == []


def test_build_cdf_single_bucket():
    buckets = [0] * 64
    buckets[10] = 100  # all 100 observations in bucket 10
    xs, ys = _build_cdf_points(buckets)
    assert len(xs) == 1
    assert ys[-1] == pytest.approx(100.0)
    # Bucket 10 midpoint: lo=1024, hi=2048 → midpoint=1536
    assert xs[0] == (1024 + 2048) // 2


def test_build_cdf_monotone_increasing():
    buckets = [10] * 10 + [0] * 54
    xs, ys = _build_cdf_points(buckets)
    assert all(ys[i] <= ys[i + 1] for i in range(len(ys) - 1))
    assert ys[-1] == pytest.approx(100.0)


def test_build_cdf_xs_increasing():
    buckets = [5, 10, 15] + [0] * 61
    xs, _ = _build_cdf_points(buckets)
    assert all(xs[i] < xs[i + 1] for i in range(len(xs) - 1))


# ---------------------------------------------------------------------------
# _has_byte_data
# ---------------------------------------------------------------------------

def test_has_byte_data_true():
    assert _has_byte_data(_make_doc(with_bytes=True)["suts"]) is True


def test_has_byte_data_false():
    assert _has_byte_data(_make_doc(with_bytes=False)["suts"]) is False


# ---------------------------------------------------------------------------
# Chart 1: _latency_bar
# ---------------------------------------------------------------------------

def test_latency_bar_has_three_traces():
    fig = _latency_bar(_make_doc()["suts"])
    assert len(fig.data) == 3


def test_latency_bar_trace_names():
    fig = _latency_bar(_make_doc()["suts"])
    names = {t.name for t in fig.data}
    assert names == {"p50", "p95", "p99"}


# ---------------------------------------------------------------------------
# Chart 2: _latency_cdf
# ---------------------------------------------------------------------------

def test_latency_cdf_returns_figure_when_histogram_present():
    fig = _latency_cdf(_make_doc(with_histogram=True)["suts"])
    assert fig is not None
    assert len(fig.data) == 2  # one line per SUT


def test_latency_cdf_returns_none_when_no_histogram():
    fig = _latency_cdf(_make_doc(with_histogram=False)["suts"])
    assert fig is None


def test_latency_cdf_one_line_per_sut():
    doc = _make_doc(with_histogram=True)
    doc["suts"].append(_sut("extra-sut", "microtel", p50_offset=25, with_histogram=True))
    fig = _latency_cdf(doc["suts"])
    assert len(fig.data) == 3


# ---------------------------------------------------------------------------
# Chart 3: _stability
# ---------------------------------------------------------------------------

def test_stability_one_line_per_sut():
    fig = _stability(_make_doc()["suts"])
    assert len(fig.data) == 2


def test_stability_x_values_are_sample_indices():
    fig = _stability(_make_doc()["suts"])
    assert list(fig.data[0].x) == [1, 2, 3, 4, 5]


# ---------------------------------------------------------------------------
# Chart 4: _wire_bytes
# ---------------------------------------------------------------------------

def test_wire_bytes_returns_none_without_byte_data():
    assert _wire_bytes(_make_doc(with_bytes=False)["suts"]) is None


def test_wire_bytes_returns_figure_with_byte_data():
    fig = _wire_bytes(_make_doc(with_bytes=True)["suts"])
    assert fig is not None


def test_wire_bytes_correctness_pass():
    # Two microtel SUTs with identical bytes/span — should show green annotation.
    suts = [
        _sut("microtel-http",  "microtel", bytes_val=128),
        _sut("microtel-grpc",  "microtel", bytes_val=128),
    ]
    fig = _wire_bytes(suts)
    annotations = [a.text for a in fig.layout.annotations]
    assert any("consistent" in t for t in annotations)


def test_wire_bytes_correctness_fail():
    # Two microtel SUTs with >5% divergence — should flag in red.
    suts = [
        _sut("microtel-http", "microtel", bytes_val=128),
        _sut("microtel-grpc", "microtel", bytes_val=200),  # ~56% divergence
    ]
    fig = _wire_bytes(suts)
    annotations = [a.text for a in fig.layout.annotations]
    assert any("FAILED" in t or "divergence" in t for t in annotations)


def test_wire_bytes_no_annotation_for_single_microtel():
    # Only one microtel SUT — no comparison possible, no annotation.
    suts = [_sut("microtel", "microtel", bytes_val=128)]
    fig = _wire_bytes(suts)
    assert not fig.layout.annotations


# ---------------------------------------------------------------------------
# write_plots integration
# ---------------------------------------------------------------------------

def test_write_plots_produces_html(tmp_path):
    path = write_plots(_make_doc(with_bytes=True, with_histogram=True), tmp_path)
    assert path is not None and path.exists()
    content = path.read_text(encoding="utf-8")
    assert "plotly" in content.lower()
    assert "microtel" in content


def test_write_plots_contains_all_chart_titles(tmp_path):
    content = write_plots(
        _make_doc(with_bytes=True, with_histogram=True), tmp_path
    ).read_text(encoding="utf-8")
    for phrase in ["Latency comparison", "Latency CDF", "Run stability", "Wire bytes"]:
        assert phrase in content, f"missing chart: {phrase}"


def test_write_plots_without_cdf_data(tmp_path):
    # Should still produce charts 1, 3 (no bytes = no chart 4, no histogram = no CDF).
    path = write_plots(_make_doc(with_bytes=False, with_histogram=False), tmp_path)
    assert path is not None
    content = path.read_text(encoding="utf-8")
    assert "Latency comparison" in content
    assert "Run stability" in content
    assert "Latency CDF" not in content
    assert "Wire bytes" not in content


def test_write_plots_returns_none_for_empty_suts(tmp_path):
    doc = _make_doc()
    doc["suts"] = []
    assert write_plots(doc, tmp_path) is None


def test_write_plots_creates_out_dir(tmp_path):
    out = tmp_path / "nested" / "results"
    path = write_plots(_make_doc(), out)
    assert path is not None
    assert out.exists()
