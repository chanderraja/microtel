# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Flamegraph integration — perf record inside SUT container → SVG.

Flamegraph mode is opt-in (--flamegraph flag).  When active the driver:

  1. Builds a :perf image variant (linux-perf installed, debug symbols kept).
  2. Runs measurement samples in that image with --cap-add=PERFMON.
  3. After measurements, runs one dedicated profiling sample while perf records.
  4. Generates an SVG per SUT via stackcollapse-perf.pl | flamegraph.pl on host.

Normal benchmark mode (no --flamegraph) never builds or touches :perf images
and requires no kernel capability or perf_event_paranoid setting on the host.

Host requirements for flamegraph mode:
  - kernel.perf_event_paranoid <= 1  (or CAP_PERFMON on the container)
  - perl on PATH
  - FlameGraph scripts (stackcollapse-perf.pl + flamegraph.pl):
      export FLAMEGRAPH_DIR=~/FlameGraph  or clone to ~/FlameGraph
"""

from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

_STACKCOLLAPSE = "stackcollapse-perf.pl"
_FLAMEGRAPH_PL = "flamegraph.pl"

_DEFAULT_SEARCH_PATHS = [
    Path(os.environ.get("FLAMEGRAPH_DIR", "") or "__nonexistent__"),
    Path.home() / "FlameGraph",
    Path("/usr/share/flamegraph"),
    Path("/opt/flamegraph"),
]


def find_flamegraph_dir() -> Optional[Path]:
    """Return the first directory that contains both FlameGraph Perl scripts."""
    for p in _DEFAULT_SEARCH_PATHS:
        try:
            if (p / _STACKCOLLAPSE).exists() and (p / _FLAMEGRAPH_PL).exists():
                return p
        except (TypeError, OSError):
            continue
    return None


def check_prerequisites(flamegraph_dir: Optional[Path]) -> list[str]:
    """Return a list of unmet prerequisite descriptions; empty means all good."""
    missing: list[str] = []
    if not shutil.which("perl"):
        missing.append(
            "perl not found on PATH — required to run stackcollapse-perf.pl "
            "and flamegraph.pl"
        )
    if flamegraph_dir is None:
        missing.append(
            "FlameGraph scripts not found. "
            "Run: git clone https://github.com/brendangregg/FlameGraph ~/FlameGraph "
            "or set FLAMEGRAPH_DIR to the clone directory."
        )
    return missing


def perf_image_name(base_image_name: str) -> str:
    """Return the :perf tag for a given base image name.

    Examples::

        perf_image_name("bench-sut-microtel")         # "bench-sut-microtel:perf"
        perf_image_name("bench-sut-microtel:latest")  # "bench-sut-microtel:perf"
    """
    repo = base_image_name.rsplit(":", 1)[0] if ":" in base_image_name else base_image_name
    return f"{repo}:perf"


class FlameGraphRecorder:
    """Manages one perf record session inside a running SUT container.

    The SUT process is PID 1 inside the container.  perf attaches to it
    (and all its threads) using the PERFMON capability granted at container
    start time.

    Typical usage::

        recorder = FlameGraphRecorder(engine, container_name)
        recorder.start()
        ctrl.run(spans_per_sample, ...)   # workload runs while perf records
        svg_path = recorder.finish(sut_name, out_dir, flamegraph_dir)
    """

    def __init__(self, engine: str, container_name: str) -> None:
        self._engine = engine
        self._container_name = container_name
        self._proc: Optional[subprocess.Popen] = None  # type: ignore[type-arg]

    def start(self) -> None:
        """Launch perf record inside the container targeting PID 1 (the SUT)."""
        cmd = [
            self._engine, "exec", self._container_name,
            "perf", "record",
            "-F", "99",
            "--call-graph", "fp",  # frame-pointer unwinding; SUT built with -fno-omit-frame-pointer
            "-p", "1",             # SUT process (all threads)
            "-o", "/tmp/perf.data",
        ]
        self._proc = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def stop(self) -> bool:
        """Send SIGINT to perf so it flushes perf.data and exits cleanly.

        Returns True if perf exited with a success or signal-interrupted code.
        """
        if self._proc is None:
            return False
        try:
            self._proc.send_signal(signal.SIGINT)
            self._proc.wait(timeout=15)
            # perf exits 0 on clean flush, or with code 130 / -SIGINT on interrupt
            return self._proc.returncode in (0, 130, -int(signal.SIGINT))
        except subprocess.TimeoutExpired:
            self._proc.kill()
            self._proc.wait()
            return False
        except OSError:
            return False
        finally:
            self._proc = None

    def finish(
        self,
        sut_name: str,
        out_dir: Path,
        flamegraph_dir: Path,
    ) -> Optional[Path]:
        """Stop recording and generate an SVG flamegraph.

        Returns the path to the SVG, or None if any step fails.
        """
        ok = self.stop()
        if not ok:
            _warn(f"perf exited uncleanly for {sut_name!r} — SVG may be incomplete")

        # Brief pause: perf may still be flushing the final mmap'd pages.
        time.sleep(1)

        return _generate_svg(
            engine=self._engine,
            container_name=self._container_name,
            sut_name=sut_name,
            out_dir=out_dir,
            flamegraph_dir=flamegraph_dir,
        )


# ---------------------------------------------------------------------------
# SVG generation
# ---------------------------------------------------------------------------

def _generate_svg(
    engine: str,
    container_name: str,
    sut_name: str,
    out_dir: Path,
    flamegraph_dir: Path,
) -> Optional[Path]:
    """Run perf script inside container, fold stacks, render SVG on host."""
    # perf script runs inside the container where the binary + debug symbols live,
    # ensuring all application frames are resolved correctly.
    try:
        perf_out = subprocess.run(
            [engine, "exec", container_name,
             "perf", "script", "-i", "/tmp/perf.data"],
            capture_output=True,
            check=True,
        )
    except subprocess.CalledProcessError as exc:
        _warn(f"perf script failed for {sut_name!r}: "
              f"{exc.stderr.decode(errors='replace').strip()}")
        return None

    collapse = subprocess.run(
        ["perl", str(flamegraph_dir / _STACKCOLLAPSE)],
        input=perf_out.stdout,
        capture_output=True,
    )
    if collapse.returncode != 0:
        _warn(f"stackcollapse-perf.pl failed for {sut_name!r}")
        return None

    fg = subprocess.run(
        [
            "perl", str(flamegraph_dir / _FLAMEGRAPH_PL),
            "--title", f"{sut_name} — microtel bench",
            "--subtitle", "perf record -F 99 --call-graph fp",
            "--colors", "hot",
        ],
        input=collapse.stdout,
        capture_output=True,
    )
    if fg.returncode != 0:
        _warn(f"flamegraph.pl failed for {sut_name!r}")
        return None

    out_dir.mkdir(parents=True, exist_ok=True)
    svg_path = out_dir / f"flamegraph-{sut_name}.svg"
    svg_path.write_bytes(fg.stdout)
    return svg_path


def _warn(msg: str) -> None:
    print(f"[flamegraph] {msg}", file=sys.stderr, flush=True)
