# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Generate interactive Plotly charts from a results.json document.

Plotly is an optional dependency.  If it is not installed this module
gracefully returns None from write_plots() rather than raising.

Install:  pip install plotly

Output: a single self-contained plots.html (CDN-hosted Plotly JS) written
to out_dir alongside results.json and results.md.

Charts produced:
  1. Latency distribution — grouped box plot.
     X = SUT name, box series per metric (p50 / p95 / p99 across samples).
     Primary comparison view.

  2. Run stability — line chart.
     X = sample number, Y = p50 ns, one line per SUT.
     Shows whether measurements drift over the run.

  3. Wire bytes / span — grouped bar chart.
     Only rendered when blackhole-sink byte data is available.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any, Optional


def write_plots(doc: dict[str, Any], out_dir: Path) -> Optional[Path]:
    """Build plots.html from a results document; return its path or None.

    Returns None (without raising) if plotly is not installed or if the
    document contains no usable SUT data.
    """
    try:
        import plotly.graph_objects as go
        from plotly.subplots import make_subplots
    except ImportError:
        print("[plots] plotly not installed — skipping HTML charts "
              "(pip install plotly to enable)", file=sys.stderr, flush=True)
        return None

    suts = doc.get("suts", [])
    if not suts or not any(s.get("samples") for s in suts):
        return None

    has_bytes = _has_byte_data(suts)
    n_rows = 3 if has_bytes else 2
    subplot_titles = ["Latency distribution", "Run stability (p50 per sample)"]
    if has_bytes:
        subplot_titles.append("Wire bytes / span")

    fig = make_subplots(
        rows=n_rows,
        cols=1,
        subplot_titles=subplot_titles,
        vertical_spacing=0.10,
    )

    _add_latency_boxes(fig, suts, row=1)
    _add_stability_lines(fig, suts, row=2)
    if has_bytes:
        _add_bytes_bars(fig, suts, row=3)

    profile = doc.get("profile", {})
    env = doc.get("environment", {})
    cpu = env.get("cpu_model", "")
    title = (
        f"microtel bench — {profile.get('name', '')}  "
        f"({profile.get('spans_per_sample', '?')} spans/sample × "
        f"{profile.get('samples', '?')} samples)"
    )
    if cpu:
        title += f"<br><sup>{cpu}</sup>"

    fig.update_layout(
        title_text=title,
        height=280 * n_rows + 120,
        boxmode="group",
        legend={"orientation": "h", "y": -0.05},
        template="plotly_white",
    )
    fig.update_yaxes(title_text="nanoseconds", row=1, col=1)
    fig.update_yaxes(title_text="p50 (ns)", row=2, col=1)
    if has_bytes:
        fig.update_yaxes(title_text="bytes", row=3, col=1)

    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / "plots.html"
    fig.write_html(str(path), include_plotlyjs="cdn")
    return path


# ---------------------------------------------------------------------------
# Chart builders
# ---------------------------------------------------------------------------

_METRICS = [
    ("latency_p50_ns", "p50"),
    ("latency_p95_ns", "p95"),
    ("latency_p99_ns", "p99"),
]

# Consistent colour palette — one colour per metric, shared across SUTs.
_METRIC_COLORS = {
    "p50": "#1f77b4",
    "p95": "#ff7f0e",
    "p99": "#d62728",
}


def _add_latency_boxes(fig: Any, suts: list[dict], row: int) -> None:
    """Grouped box plot: X = SUT name, one series per latency metric."""
    shown: set[str] = set()
    for key, label in _METRICS:
        for sut in suts:
            values = [float(s[key]) for s in sut.get("samples", []) if key in s]
            if not values:
                continue
            fig.add_trace(
                _go().Box(
                    x=[sut["name"]] * len(values),
                    y=values,
                    name=label,
                    legendgroup=label,
                    showlegend=label not in shown,
                    marker_color=_METRIC_COLORS[label],
                    boxpoints="all",
                    jitter=0.3,
                    pointpos=0,
                ),
                row=row,
                col=1,
            )
            shown.add(label)


def _add_stability_lines(fig: Any, suts: list[dict], row: int) -> None:
    """Line chart: p50 per sample index, one line per SUT."""
    for sut in suts:
        samples = sut.get("samples", [])
        if not samples:
            continue
        x = list(range(1, len(samples) + 1))
        y = [float(s["latency_p50_ns"]) for s in samples]
        fig.add_trace(
            _go().Scatter(
                x=x,
                y=y,
                mode="lines+markers",
                name=sut["name"],
                legendgroup=f"sut-{sut['name']}",
            ),
            row=row,
            col=1,
        )
    fig.update_xaxes(title_text="sample #", row=row, col=1, dtick=1)


def _add_bytes_bars(fig: Any, suts: list[dict], row: int) -> None:
    """Grouped bar: median wire bytes per span per SUT."""
    names, medians = [], []
    for sut in suts:
        summary = sut.get("summary", {})
        wb = summary.get("wire_bytes_per_span")
        if wb and isinstance(wb, dict):
            names.append(sut["name"])
            medians.append(wb.get("median", 0))
    if not names:
        return
    fig.add_trace(
        _go().Bar(x=names, y=medians, name="wire bytes/span", showlegend=False),
        row=row,
        col=1,
    )


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _has_byte_data(suts: list[dict]) -> bool:
    for sut in suts:
        if sut.get("summary", {}).get("wire_bytes_per_span"):
            return True
    return False


def _go() -> Any:
    import plotly.graph_objects as go  # noqa: PLC0415
    return go
