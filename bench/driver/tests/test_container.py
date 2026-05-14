# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0

import socket
import sys
import threading
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from driver.container import detect_engine, wait_tcp, wait_http


def test_detect_engine_auto():
    engine = detect_engine("auto")
    assert engine in ("podman", "docker")


def test_detect_engine_bad_preference():
    with pytest.raises(RuntimeError):
        detect_engine("nonexistent-engine-xyz")


def test_wait_tcp_succeeds():
    # Start a real listening socket on a free port.
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    try:
        wait_tcp("127.0.0.1", port, timeout=5.0)
    finally:
        srv.close()


def test_wait_tcp_times_out():
    # Pick a port that has nothing listening.
    with pytest.raises(TimeoutError):
        wait_tcp("127.0.0.1", 19999, timeout=1.0, interval=0.2)


def test_wait_http_succeeds():
    import http.server
    import urllib.request

    class _Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"ok")
        def log_message(self, *args): pass

    srv = http.server.HTTPServer(("127.0.0.1", 0), _Handler)
    port = srv.server_address[1]
    t = threading.Thread(target=srv.handle_request, daemon=True)
    t.start()
    try:
        wait_http(f"http://127.0.0.1:{port}/health", timeout=5.0)
    finally:
        srv.server_close()


def test_wait_http_times_out():
    with pytest.raises(TimeoutError):
        wait_http("http://127.0.0.1:19998/health", timeout=1.0, interval=0.2)
