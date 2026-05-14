# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from driver.profile import load, Profile

PROFILES_DIR = Path(__file__).parent.parent.parent / "profiles"


def test_load_hot_loop_traces():
    p = load(PROFILES_DIR, "hot-loop-traces")
    assert p.name == "hot-loop-traces"
    assert p.spans_per_sample == 10_000
    assert p.samples == 10
    assert p.warmup_spans == 1_000
    assert p.sink_mode == "blackhole"
    assert "microtel" in p.suts
    assert "otelcpp-grpc" in p.suts


def test_load_missing_raises():
    import pytest
    with pytest.raises(FileNotFoundError):
        load(PROFILES_DIR, "nonexistent-profile")


def test_load_invalid_sink_mode(tmp_path):
    import pytest
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        "profile: bad\n"
        "workload:\n"
        "  spans_per_sample: 100\n"
        "sink:\n"
        "  mode: invalid\n"
        "suts:\n"
        "  - microtel\n"
        "metrics:\n"
        "  - spans_emitted\n"
    )
    with pytest.raises(ValueError, match="sink.mode"):
        load(tmp_path, "bad")
