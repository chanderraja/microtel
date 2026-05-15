# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Tests for the regression detection module."""

from __future__ import annotations

import pytest

from driver.regression import check, format_report


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_doc(suts: list[dict]) -> dict:
    return {"schema_version": "1.0", "suts": suts}


def _make_sut(name: str, p50: float, p95: float, p99: float, drop_rate: float) -> dict:
    def _stat(v: float) -> dict:
        return {"median": v, "p25": v * 0.9, "p75": v * 1.1, "min": v * 0.5, "max": v * 2.0}

    return {
        "name": name,
        "summary": {
            "latency_p50_ns": _stat(p50),
            "latency_p95_ns": _stat(p95),
            "latency_p99_ns": _stat(p99),
            "drop_rate_pct":  drop_rate,
        },
    }


_BASELINE = _make_doc([
    _make_sut("microtel-http", p50=1000, p95=2000, p99=3000, drop_rate=0.0),
    _make_sut("otelcpp-http",  p50=2000, p95=4000, p99=6000, drop_rate=0.5),
])


# ---------------------------------------------------------------------------
# No-regression cases
# ---------------------------------------------------------------------------

def test_no_regression_identical():
    regressions = check(_BASELINE, _BASELINE)
    assert regressions == []


def test_no_regression_within_threshold():
    current = _make_doc([
        _make_sut("microtel-http", p50=1040, p95=2040, p99=3040, drop_rate=0.0),
    ])
    regressions = check(_BASELINE, current)
    assert regressions == []


def test_no_regression_improvement():
    current = _make_doc([
        _make_sut("microtel-http", p50=800, p95=1600, p99=2400, drop_rate=0.0),
    ])
    regressions = check(_BASELINE, current)
    assert regressions == []


def test_sut_absent_from_baseline_skipped():
    current = _make_doc([
        _make_sut("new-sut", p50=9999, p95=9999, p99=9999, drop_rate=99.0),
    ])
    regressions = check(_BASELINE, current)
    assert regressions == []


# ---------------------------------------------------------------------------
# Latency regressions
# ---------------------------------------------------------------------------

def test_p50_regression_detected():
    current = _make_doc([
        _make_sut("microtel-http", p50=1060, p95=2000, p99=3000, drop_rate=0.0),
    ])
    regressions = check(_BASELINE, current, threshold=0.05)
    assert len(regressions) == 1
    assert regressions[0]["metric"] == "latency_p50_ns"
    assert regressions[0]["sut"] == "microtel-http"


def test_multiple_metrics_regressed():
    current = _make_doc([
        _make_sut("microtel-http", p50=1100, p95=2200, p99=3300, drop_rate=0.0),
    ])
    regressions = check(_BASELINE, current, threshold=0.05)
    metrics = {r["metric"] for r in regressions}
    assert "latency_p50_ns" in metrics
    assert "latency_p95_ns" in metrics
    assert "latency_p99_ns" in metrics


def test_custom_threshold_respected():
    # 10% increase — passes 5% threshold but should fail 5% if increased further
    current = _make_doc([
        _make_sut("microtel-http", p50=1060, p95=2000, p99=3000, drop_rate=0.0),
    ])
    # Should flag at default 5% threshold
    assert len(check(_BASELINE, current, threshold=0.05)) == 1
    # Should not flag at 10% threshold
    assert len(check(_BASELINE, current, threshold=0.10)) == 0


def test_zero_baseline_latency_skipped():
    """A zero-median baseline should not produce a division-by-zero regression."""
    baseline = _make_doc([{"name": "x", "summary": {
        "latency_p50_ns": {"median": 0},
        "latency_p95_ns": {"median": 0},
        "latency_p99_ns": {"median": 0},
        "drop_rate_pct":  0,
    }}])
    current = _make_doc([_make_sut("x", p50=1000, p95=2000, p99=3000, drop_rate=0.0)])
    regressions = check(baseline, current)
    latency_regs = [r for r in regressions if r["metric"] != "drop_rate_pct"]
    assert latency_regs == []


# ---------------------------------------------------------------------------
# Drop-rate regressions
# ---------------------------------------------------------------------------

def test_drop_rate_regression_detected():
    # otelcpp-http baseline drop is 0.5%; increase by 6 pp → 6.5% → above 5 pp threshold
    current = _make_doc([
        _make_sut("otelcpp-http", p50=2000, p95=4000, p99=6000, drop_rate=6.5),
    ])
    regressions = check(_BASELINE, current, threshold=0.05)
    drop_regs = [r for r in regressions if r["metric"] == "drop_rate_pct"]
    assert len(drop_regs) == 1
    assert drop_regs[0]["sut"] == "otelcpp-http"


def test_drop_rate_no_regression_below_threshold():
    # Increase of 4 pp — below default 5 pp threshold
    current = _make_doc([
        _make_sut("otelcpp-http", p50=2000, p95=4000, p99=6000, drop_rate=4.4),
    ])
    regressions = check(_BASELINE, current, threshold=0.05)
    drop_regs = [r for r in regressions if r["metric"] == "drop_rate_pct"]
    assert drop_regs == []


def test_drop_rate_improvement_not_flagged():
    current = _make_doc([
        _make_sut("otelcpp-http", p50=2000, p95=4000, p99=6000, drop_rate=0.0),
    ])
    assert check(_BASELINE, current) == []


# ---------------------------------------------------------------------------
# format_report
# ---------------------------------------------------------------------------

def test_format_report_no_regressions():
    assert "No regressions" in format_report([])


def test_format_report_contains_sut_and_metric():
    regressions = check(
        _BASELINE,
        _make_doc([_make_sut("microtel-http", p50=1100, p95=2000, p99=3000, drop_rate=0.0)]),
    )
    report = format_report(regressions)
    assert "microtel-http" in report
    assert "latency_p50_ns" in report
    assert "+" in report
