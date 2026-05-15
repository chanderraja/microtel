# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Tests for sink_client module."""

from __future__ import annotations

import json
from http.server import BaseHTTPRequestHandler, HTTPServer
from threading import Thread
from typing import Callable

import pytest

from driver.sink_client import CollectorSinkClient, SinkClient, _parse_counter


# ---------------------------------------------------------------------------
# _parse_counter unit tests
# ---------------------------------------------------------------------------

_PROM_BODY = """\
# HELP otelcol_receiver_accepted_spans Number of spans accepted by the receiver.
# TYPE otelcol_receiver_accepted_spans counter
otelcol_receiver_accepted_spans{receiver="otlp",transport="grpc"} 1000
otelcol_receiver_accepted_spans{receiver="otlp",transport="http"} 500
# HELP otelcol_receiver_refused_spans Number of spans refused by the receiver.
# TYPE otelcol_receiver_refused_spans counter
otelcol_receiver_refused_spans{receiver="otlp",transport="grpc"} 10
"""


def test_parse_counter_sums_all_label_sets():
    assert _parse_counter(_PROM_BODY, "otelcol_receiver_accepted_spans") == pytest.approx(1500)


def test_parse_counter_single_label_set():
    assert _parse_counter(_PROM_BODY, "otelcol_receiver_refused_spans") == pytest.approx(10)


def test_parse_counter_missing_metric_returns_zero():
    assert _parse_counter(_PROM_BODY, "otelcol_nonexistent_metric") == pytest.approx(0)


def test_parse_counter_empty_body():
    assert _parse_counter("", "otelcol_receiver_accepted_spans") == pytest.approx(0)


def test_parse_counter_handles_total_suffix():
    body = "otelcol_receiver_accepted_spans_total{transport=\"grpc\"} 42\n"
    assert _parse_counter(body, "otelcol_receiver_accepted_spans") == pytest.approx(42)


# ---------------------------------------------------------------------------
# Minimal HTTP server helper
# ---------------------------------------------------------------------------

class _FakeHTTPServer:
    """Minimal HTTP server for unit testing sink clients."""

    def __init__(self, handler: Callable[[str], tuple[int, str]]):
        self._handler = handler
        self._server: HTTPServer | None = None
        self._thread: Thread | None = None

    @property
    def port(self) -> int:
        assert self._server is not None
        return self._server.server_address[1]

    def __enter__(self) -> "_FakeHTTPServer":
        handler = self._handler

        class H(BaseHTTPRequestHandler):
            def _respond(self):
                status, body = handler(self.path)
                encoded = body.encode()
                self.send_response(status)
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

            def do_GET(self):
                self._respond()

            def do_POST(self):
                self._respond()

            def log_message(self, *args):
                pass  # suppress test noise

        self._server = HTTPServer(("127.0.0.1", 0), H)
        self._thread = Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *_):
        if self._server:
            self._server.shutdown()


# ---------------------------------------------------------------------------
# SinkClient tests
# ---------------------------------------------------------------------------

def test_sink_client_stats_returns_mode():
    payload = json.dumps({"spans_received": 100, "bytes_received": 2048})

    def handler(path):
        if path == "/stats":
            return 200, payload
        return 404, ""

    with _FakeHTTPServer(handler) as srv:
        client = SinkClient("127.0.0.1", srv.port)
        snap = client.stats()
        assert snap["mode"] == "blackhole"
        assert snap["spans_received"] == 100
        assert snap["bytes_received"] == 2048


def test_sink_client_reset_posts():
    called = []

    def handler(path):
        called.append(path)
        return 200, ""

    with _FakeHTTPServer(handler) as srv:
        client = SinkClient("127.0.0.1", srv.port)
        client.reset()
    assert "/reset" in called


# ---------------------------------------------------------------------------
# CollectorSinkClient tests
# ---------------------------------------------------------------------------

def _make_prom_handler(body_seq: list[str]):
    """Returns a handler that serves successive Prometheus bodies."""
    idx = [0]

    def handler(path):
        if path == "/metrics":
            body = body_seq[min(idx[0], len(body_seq) - 1)]
            idx[0] += 1
            return 200, body
        if path == "/health/status":
            return 200, "ok"
        return 404, ""

    return handler


def test_collector_client_baseline_subtracted():
    prom_after_warmup = (
        'otelcol_receiver_accepted_spans{transport="grpc"} 500\n'
        'otelcol_receiver_refused_spans{transport="grpc"} 5\n'
    )
    prom_after_sample = (
        'otelcol_receiver_accepted_spans{transport="grpc"} 1100\n'
        'otelcol_receiver_refused_spans{transport="grpc"} 8\n'
    )
    handler = _make_prom_handler([prom_after_warmup, prom_after_sample])
    with _FakeHTTPServer(handler) as srv:
        client = CollectorSinkClient.__new__(CollectorSinkClient)
        client._host = "127.0.0.1"
        client._prom_base = f"http://127.0.0.1:{srv.port}"
        client._health_base = f"http://127.0.0.1:{srv.port}"
        client._baseline_accepted = 0.0
        client._baseline_refused = 0.0

        client.reset()  # scrape 1: sets baseline to 500 + 5
        snap = client.stats()  # scrape 2: 1100 + 8, delta = 600 + 3

    assert snap["mode"] == "collector"
    assert snap["spans_received"] == 603
    assert snap["bytes_received"] is None


def test_collector_client_bytes_always_none():
    prom = 'otelcol_receiver_accepted_spans{transport="grpc"} 0\n'
    handler = _make_prom_handler([prom, prom])
    with _FakeHTTPServer(handler) as srv:
        client = CollectorSinkClient.__new__(CollectorSinkClient)
        client._host = "127.0.0.1"
        client._prom_base = f"http://127.0.0.1:{srv.port}"
        client._health_base = f"http://127.0.0.1:{srv.port}"
        client._baseline_accepted = 0.0
        client._baseline_refused = 0.0

        client.reset()
        snap = client.stats()

    assert snap["bytes_received"] is None


def test_collector_client_negative_delta_clamped_to_zero():
    # If counters reset mid-run (e.g. collector restart), delta must not go negative.
    prom_high = 'otelcol_receiver_accepted_spans{transport="grpc"} 9999\n'
    prom_low = 'otelcol_receiver_accepted_spans{transport="grpc"} 0\n'
    handler = _make_prom_handler([prom_high, prom_low])
    with _FakeHTTPServer(handler) as srv:
        client = CollectorSinkClient.__new__(CollectorSinkClient)
        client._host = "127.0.0.1"
        client._prom_base = f"http://127.0.0.1:{srv.port}"
        client._health_base = f"http://127.0.0.1:{srv.port}"
        client._baseline_accepted = 0.0
        client._baseline_refused = 0.0

        client.reset()
        snap = client.stats()

    assert snap["spans_received"] == 0
