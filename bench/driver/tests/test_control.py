# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0

import json
import socket
import sys
import threading
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from driver.control import ControlClient


def _make_server(responses: list[dict]):
    """Minimal server that reads one JSON line per response and writes back."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]

    def _serve():
        conn, _ = srv.accept()
        buf = b""
        for resp in responses:
            while b"\n" not in buf:
                buf += conn.recv(4096)
            _, _, buf = buf.partition(b"\n")
            conn.sendall((json.dumps(resp) + "\n").encode())
        conn.close()
        srv.close()

    t = threading.Thread(target=_serve, daemon=True)
    t.start()
    return port, t


def test_run_returns_parsed_result():
    run_result = {
        "spans_emitted": 1000,
        "spans_dropped": {"queue_full": 0, "record_too_large": 0,
                          "span_attribute_limit": 0, "attribute_value_truncated": 0,
                          "other": 0, "total": 0},
        "bytes_sent": 0,
        "duration_ns": 500_000_000,
        "latency_p50_ns": 82,
        "latency_p95_ns": 120,
        "latency_p99_ns": 185,
        "latency_min_ns": 40,
        "latency_max_ns": 9000,
    }
    quit_ack = {"ok": True}

    port, _ = _make_server([run_result, quit_ack])

    with ControlClient("127.0.0.1", port) as c:
        result = c.run(1000)
        c.quit()

    assert result["spans_emitted"] == 1000
    assert result["spans_dropped"]["total"] == 0
    assert result["latency_p50_ns"] == 82


def test_connection_error_on_bad_port():
    c = ControlClient("127.0.0.1", 19997)
    with pytest.raises(OSError):
        c.connect()
