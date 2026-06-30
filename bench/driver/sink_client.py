# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Sink client implementations for blackhole and otel-collector sinks."""

from __future__ import annotations

import json
import urllib.request
from typing import Any, Optional


class SinkClient:
    """Client for the blackhole-sink /stats and /reset endpoints."""

    def __init__(self, host: str, port: int = 19080):
        self._base = f"http://{host}:{port}"

    def stats(self) -> dict[str, Any]:
        """GET /stats — returns snapshot with mode='blackhole'."""
        with urllib.request.urlopen(f"{self._base}/stats", timeout=5) as resp:
            data = json.loads(resp.read().decode())
        return {
            "mode": "blackhole",
            "spans_received": data["spans_received"],
            "bytes_received": data["bytes_received"],
            "http_requests_received": data.get("http_requests_received", 0),
            "grpc_requests_received": data.get("grpc_requests_received", 0),
            "errors": data.get("errors", 0),
            "last_error": data.get("last_error", ""),
        }

    def reset(self) -> None:
        """POST /reset — zero all counters."""
        req = urllib.request.Request(
            f"{self._base}/reset",
            data=b"",
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=5):
            pass

    def health(self) -> bool:
        """GET /health — returns True if sink responds 200."""
        try:
            with urllib.request.urlopen(f"{self._base}/health", timeout=5) as resp:
                return resp.status == 200
        except Exception:
            return False


class CollectorSinkClient:
    """Prometheus-scrape client for the otel-collector sink.

    Uses a two-scrape bracket: reset() records the baseline cumulative
    counter values; stats() scrapes again and subtracts.  Bytes fields
    are None because the collector does not expose received-bytes via
    Prometheus.
    """

    _PROM_PORT = 8888
    _HEALTH_PORT = 13133

    # Prometheus metric names for accepted and refused spans.
    _ACCEPTED = "otelcol_receiver_accepted_spans"
    _REFUSED = "otelcol_receiver_refused_spans"

    def __init__(self, host: str):
        self._host = host
        self._prom_base = f"http://{host}:{self._PROM_PORT}"
        self._health_base = f"http://{host}:{self._HEALTH_PORT}"
        self._baseline_accepted: float = 0.0
        self._baseline_refused: float = 0.0

    def reset(self) -> None:
        """Record the current Prometheus counter values as the baseline."""
        accepted, refused = self._scrape()
        self._baseline_accepted = accepted
        self._baseline_refused = refused

    def stats(self) -> dict[str, Any]:
        """Scrape Prometheus and return delta since last reset().

        Returns a dict with mode='collector'. bytes_received is None.
        """
        accepted, refused = self._scrape()
        spans_received = max(0, int(accepted - self._baseline_accepted))
        spans_refused = max(0, int(refused - self._baseline_refused))
        return {
            "mode": "collector",
            "spans_received": spans_received + spans_refused,
            "bytes_received": None,
        }

    def health(self) -> bool:
        """GET /health — returns True if the collector health endpoint responds 200."""
        try:
            url = f"{self._health_base}/health/status"
            with urllib.request.urlopen(url, timeout=5) as resp:
                return resp.status == 200
        except Exception:
            return False

    def _scrape(self) -> tuple[float, float]:
        """Scrape Prometheus metrics endpoint and parse accepted/refused totals."""
        url = f"{self._prom_base}/metrics"
        with urllib.request.urlopen(url, timeout=10) as resp:
            body = resp.read().decode()
        accepted = _parse_counter(body, self._ACCEPTED)
        refused = _parse_counter(body, self._REFUSED)
        return accepted, refused


def _parse_counter(text: str, metric_name: str) -> float:
    """Sum all samples of a Prometheus counter across all label sets."""
    total = 0.0
    for line in text.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        name_part = line.split("{")[0].split(" ")[0]
        if name_part == metric_name or name_part == metric_name + "_total":
            try:
                total += float(line.rsplit(" ", 1)[-1])
            except ValueError:
                pass
    return total
