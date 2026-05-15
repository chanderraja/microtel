# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Regression detection: compare a results.json against a stored baseline."""

from __future__ import annotations

from typing import Any


# Latency metrics evaluated with a relative threshold.
_LATENCY_METRICS = ("latency_p50_ns", "latency_p95_ns", "latency_p99_ns")


def check(
    baseline: dict[str, Any],
    current: dict[str, Any],
    threshold: float = 0.05,
) -> list[dict[str, Any]]:
    """Compare current results against a baseline document.

    For each SUT present in both documents, checks:
    - latency_p50/p95/p99 median: regression if relative increase > threshold
    - drop_rate_pct: regression if absolute increase > threshold * 100 pp

    Returns a list of regression dicts (empty = no regression).  Each dict has:
    ``sut``, ``metric``, ``baseline``, ``current``, ``delta``.
    """
    baseline_suts = {s["name"]: s for s in baseline.get("suts", [])}
    regressions: list[dict[str, Any]] = []

    for sut in current.get("suts", []):
        name = sut["name"]
        if name not in baseline_suts:
            continue
        b_summary = baseline_suts[name].get("summary", {})
        c_summary = sut.get("summary", {})
        _check_latency(name, b_summary, c_summary, threshold, regressions)
        _check_drop_rate(name, b_summary, c_summary, threshold, regressions)

    return regressions


def _check_latency(
    sut_name: str,
    b_summary: dict,
    c_summary: dict,
    threshold: float,
    regressions: list,
) -> None:
    for metric in _LATENCY_METRICS:
        b_val = b_summary.get(metric, {}).get("median", 0.0)
        c_val = c_summary.get(metric, {}).get("median", 0.0)
        if b_val <= 0:
            continue
        relative = (c_val - b_val) / b_val
        if relative > threshold:
            regressions.append({
                "sut":      sut_name,
                "metric":   metric,
                "baseline": b_val,
                "current":  c_val,
                "delta":    relative * 100,
            })


def _check_drop_rate(
    sut_name: str,
    b_summary: dict,
    c_summary: dict,
    threshold: float,
    regressions: list,
) -> None:
    b_drop = b_summary.get("drop_rate_pct", 0.0)
    c_drop = c_summary.get("drop_rate_pct", 0.0)
    # threshold is a fraction (0.05 = 5%); convert to percentage-point delta.
    max_pp_delta = threshold * 100
    if (c_drop - b_drop) > max_pp_delta:
        regressions.append({
            "sut":      sut_name,
            "metric":   "drop_rate_pct",
            "baseline": b_drop,
            "current":  c_drop,
            "delta":    c_drop - b_drop,
        })


def format_report(regressions: list[dict[str, Any]]) -> str:
    """Return a human-readable regression report string."""
    if not regressions:
        return "No regressions detected."
    lines = [f"{'SUT':<30} {'Metric':<22} {'Baseline':>12} {'Current':>12} {'Delta':>10}"]
    lines.append("-" * 90)
    for r in regressions:
        metric = r["metric"]
        if metric == "drop_rate_pct":
            b_str = f"{r['baseline']:.3f}%"
            c_str = f"{r['current']:.3f}%"
            d_str = f"+{r['delta']:.3f} pp"
        else:
            b_str = f"{r['baseline']:.0f} ns"
            c_str = f"{r['current']:.0f} ns"
            d_str = f"+{r['delta']:.1f}%"
        lines.append(f"{r['sut']:<30} {metric:<22} {b_str:>12} {c_str:>12} {d_str:>10}")
    return "\n".join(lines)
