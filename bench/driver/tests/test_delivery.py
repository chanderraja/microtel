# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Span-count denominators: delivery rate and drop rate both divide by the
number of spans actually sent, not by the number of workload iterations."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from driver.__main__ import _delivery_rate_pct, _spans_per_iteration
from driver.report import _delivery_rate_from_sink, _drop_rate


# ---------------------------------------------------------------------------
# Spans per workload iteration
# ---------------------------------------------------------------------------

def test_spans_per_iteration_defaults_to_one():
    assert _spans_per_iteration({}) == 1


def test_spans_per_iteration_hot_loop():
    assert _spans_per_iteration({"EMIT_WORKLOAD": "hot_loop"}) == 1


def test_spans_per_iteration_realistic_request():
    # EmitRequest() emits one parent plus two children per iteration.
    assert _spans_per_iteration({"EMIT_WORKLOAD": "realistic_request"}) == 3


def test_spans_per_iteration_unknown_workload_defaults_to_one():
    assert _spans_per_iteration({"EMIT_WORKLOAD": "something_new"}) == 1


# ---------------------------------------------------------------------------
# Per-sample delivery rate
# ---------------------------------------------------------------------------

def test_delivery_rate_hot_loop_full_delivery():
    assert _delivery_rate_pct(10_000, 10_000, "traces") == 100.0


def test_delivery_rate_realistic_request_full_delivery():
    # 5 000 iterations x 3 spans = 15 000 spans sent, all delivered.
    # Dividing by the iteration count instead reported 300%.
    emitted_iterations = 5_000
    expected = emitted_iterations * _spans_per_iteration(
        {"EMIT_WORKLOAD": "realistic_request"})
    assert _delivery_rate_pct(15_000, expected, "traces") == 100.0


def test_delivery_rate_partial():
    assert _delivery_rate_pct(7_500, 15_000, "traces") == 50.0


def test_delivery_rate_none_for_metrics_signal():
    assert _delivery_rate_pct(0, 10_000, "metrics") is None


def test_delivery_rate_none_when_nothing_sent():
    assert _delivery_rate_pct(0, 0, "traces") is None


# ---------------------------------------------------------------------------
# Aggregate delivery rate across samples
# ---------------------------------------------------------------------------

def _sample(emitted: int, expected: int, received: int, rate):
    return {
        "spans_emitted": emitted,
        "spans_expected": expected,
        "delivery_rate_pct": rate,
        "sink": {"spans_received": received},
    }


def test_summary_delivery_uses_spans_expected():
    samples = [_sample(5_000, 15_000, 15_000, 100.0) for _ in range(3)]
    assert _delivery_rate_from_sink(samples) == 100.0


def test_summary_delivery_reports_real_loss():
    samples = [_sample(5_000, 15_000, 12_000, 80.0) for _ in range(2)]
    assert _delivery_rate_from_sink(samples) == 80.0


def test_summary_delivery_falls_back_to_spans_emitted():
    # Results documents written before spans_expected existed (and every
    # single-span profile) still aggregate against the emitted count.
    legacy = [{
        "spans_emitted": 10_000,
        "delivery_rate_pct": 100.0,
        "sink": {"spans_received": 10_000},
    }]
    assert _delivery_rate_from_sink(legacy) == 100.0


def test_summary_delivery_none_when_all_samples_none():
    samples = [_sample(5_000, 0, 0, None)]
    assert _delivery_rate_from_sink(samples) is None


# ---------------------------------------------------------------------------
# Aggregate drop rate across samples
#
# Same denominator as delivery: the SDK counts dropped *spans*, so dividing by
# the iteration count over-reports by the spans-per-iteration factor.
# ---------------------------------------------------------------------------

def _drop_sample(emitted: int, expected: int, dropped: int):
    return {
        "spans_emitted": emitted,
        "spans_expected": expected,
        "spans_dropped": {"total": dropped},
    }


def test_summary_drop_rate_uses_spans_expected():
    # realistic-request: 5 000 iterations x 3 spans = 15 000 spans sent.
    # 150 dropped spans is 1%; the iteration denominator reported 3%.
    samples = [_drop_sample(5_000, 15_000, 150)]
    assert _drop_rate(samples) == 1.0


def test_summary_drop_rate_aggregates_across_samples():
    samples = [_drop_sample(5_000, 15_000, 150) for _ in range(3)]
    assert _drop_rate(samples) == 1.0


def test_summary_drop_rate_falls_back_to_spans_emitted():
    # One span per iteration, and every results document written before
    # spans_expected existed.
    legacy = [{"spans_emitted": 10_000, "spans_dropped": {"total": 100}}]
    assert _drop_rate(legacy) == 1.0


def test_summary_drop_rate_zero_when_nothing_sent():
    assert _drop_rate([_drop_sample(0, 0, 0)]) == 0.0
