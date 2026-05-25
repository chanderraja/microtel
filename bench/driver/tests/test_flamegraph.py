# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from driver.flamegraph import (
    check_prerequisites,
    find_flamegraph_dir,
    perf_image_name,
    _generate_svg,
)


# ---------------------------------------------------------------------------
# perf_image_name
# ---------------------------------------------------------------------------

def test_perf_image_name_plain():
    assert perf_image_name("bench-sut-microtel") == "bench-sut-microtel:perf"


def test_perf_image_name_with_tag():
    assert perf_image_name("bench-sut-microtel:latest") == "bench-sut-microtel:perf"


def test_perf_image_name_with_perf_tag():
    # Idempotent: already tagged :perf → still :perf.
    assert perf_image_name("bench-sut-microtel:perf") == "bench-sut-microtel:perf"


# ---------------------------------------------------------------------------
# find_flamegraph_dir
# ---------------------------------------------------------------------------

def test_find_flamegraph_dir_none_when_missing(tmp_path, monkeypatch):
    monkeypatch.setenv("FLAMEGRAPH_DIR", str(tmp_path / "nonexistent"))
    monkeypatch.setattr(
        "driver.flamegraph._DEFAULT_SEARCH_PATHS",
        [tmp_path / "nonexistent"],
    )
    assert find_flamegraph_dir() is None


def test_find_flamegraph_dir_finds_scripts(tmp_path, monkeypatch):
    fg_dir = tmp_path / "FlameGraph"
    fg_dir.mkdir()
    (fg_dir / "stackcollapse-perf.pl").write_text("# stub\n")
    (fg_dir / "flamegraph.pl").write_text("# stub\n")

    monkeypatch.setattr(
        "driver.flamegraph._DEFAULT_SEARCH_PATHS",
        [fg_dir],
    )
    result = find_flamegraph_dir()
    assert result == fg_dir


def test_find_flamegraph_dir_requires_both_scripts(tmp_path, monkeypatch):
    fg_dir = tmp_path / "FlameGraph"
    fg_dir.mkdir()
    # Only one script present
    (fg_dir / "stackcollapse-perf.pl").write_text("# stub\n")

    monkeypatch.setattr(
        "driver.flamegraph._DEFAULT_SEARCH_PATHS",
        [fg_dir],
    )
    assert find_flamegraph_dir() is None


# ---------------------------------------------------------------------------
# check_prerequisites
# ---------------------------------------------------------------------------

def test_check_prerequisites_missing_dir():
    problems = check_prerequisites(None)
    assert any("FlameGraph" in p for p in problems)


def test_check_prerequisites_ok(tmp_path):
    fg_dir = tmp_path / "FlameGraph"
    fg_dir.mkdir()
    (fg_dir / "stackcollapse-perf.pl").write_text("# stub\n")
    (fg_dir / "flamegraph.pl").write_text("# stub\n")

    problems = check_prerequisites(fg_dir)
    # perl may or may not be installed; the only guaranteed check is the dir.
    assert not any("FlameGraph" in p for p in problems)


# ---------------------------------------------------------------------------
# _generate_svg — tested with minimal stub Perl scripts and fake perf output
# ---------------------------------------------------------------------------

_FAKE_PERF_SCRIPT_OUTPUT = b"""\
emit_app 123 1000.000: cycles:
        ffffffff81234567 do_work ([kernel.kallsyms])
        0000000000400abc StartSpan (emit_app)
"""

_STACKCOLLAPSE_STUB = """\
#!/usr/bin/env perl
# Minimal stub: emit one folded stack line so flamegraph.pl has input.
print "emit_app;StartSpan;do_work 1\\n";
"""

_FLAMEGRAPH_STUB = """\
#!/usr/bin/env perl
# Minimal stub: emit a bare-minimum SVG.
print "<?xml version=\\"1.0\\"?>\\n<svg/>\\n";
"""


def _write_stub_scripts(directory: Path) -> None:
    (directory / "stackcollapse-perf.pl").write_text(_STACKCOLLAPSE_STUB)
    (directory / "flamegraph.pl").write_text(_FLAMEGRAPH_STUB)


class _FakeEngine:
    """Substitute for docker/podman: returns canned perf script output."""

    def __init__(self, perf_output: bytes) -> None:
        self._perf_output = perf_output

    def run_perf_script(self, container_name: str) -> bytes:
        return self._perf_output


def test_generate_svg_produces_svg_file(tmp_path, monkeypatch):
    import subprocess as _sp

    fg_dir = tmp_path / "FlameGraph"
    fg_dir.mkdir()
    _write_stub_scripts(fg_dir)

    out_dir = tmp_path / "results"

    # Patch subprocess.run so the first call (perf script) returns fake output,
    # and subsequent calls (stackcollapse, flamegraph.pl) run the real stubs.
    original_run = _sp.run
    call_count = {"n": 0}

    def _fake_run(cmd, **kwargs):
        call_count["n"] += 1
        if call_count["n"] == 1:
            # First call is `docker exec <container> perf script`.
            class _R:
                stdout = _FAKE_PERF_SCRIPT_OUTPUT
                returncode = 0
            return _R()
        return original_run(cmd, **kwargs)

    monkeypatch.setattr("driver.flamegraph.subprocess.run", _fake_run)

    svg = _generate_svg(
        engine="docker",
        container_name="bench-sut-test",
        sut_name="test-sut",
        out_dir=out_dir,
        flamegraph_dir=fg_dir,
    )

    assert svg is not None
    assert svg.exists()
    assert svg.suffix == ".svg"
    content = svg.read_text()
    assert "<svg" in content


def test_generate_svg_returns_none_on_perf_script_failure(tmp_path, monkeypatch):
    import subprocess as _sp

    fg_dir = tmp_path / "FlameGraph"
    fg_dir.mkdir()
    _write_stub_scripts(fg_dir)

    def _fail_run(cmd, **kwargs):
        raise _sp.CalledProcessError(1, cmd, stderr=b"no perf.data")

    monkeypatch.setattr("driver.flamegraph.subprocess.run", _fail_run)

    svg = _generate_svg(
        engine="docker",
        container_name="bench-sut-test",
        sut_name="test-sut",
        out_dir=tmp_path / "results",
        flamegraph_dir=fg_dir,
    )
    assert svg is None
