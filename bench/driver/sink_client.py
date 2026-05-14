# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""HTTP client for the blackhole-sink control API (port 8080)."""

from __future__ import annotations

import json
import urllib.request
from typing import Any


class SinkClient:
    """Client for the blackhole-sink /stats and /reset endpoints."""

    def __init__(self, host: str, port: int = 8080):
        self._base = f"http://{host}:{port}"

    def stats(self) -> dict[str, Any]:
        """GET /stats — returns the Snapshot JSON."""
        with urllib.request.urlopen(f"{self._base}/stats", timeout=5) as resp:
            return json.loads(resp.read().decode())

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
