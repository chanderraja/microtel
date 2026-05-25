# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Container engine abstraction — build images, start/stop containers."""

from __future__ import annotations

import shutil
import socket
import subprocess
import time
from pathlib import Path
from typing import Optional


# ---------------------------------------------------------------------------
# Engine detection
# ---------------------------------------------------------------------------

def detect_engine(preference: str = "auto") -> str:
    """Return 'podman' or 'docker' based on preference and availability."""
    if preference == "podman":
        if shutil.which("podman"):
            return "podman"
        raise RuntimeError("podman requested but not found on PATH")
    if preference == "docker":
        if shutil.which("docker"):
            return "docker"
        raise RuntimeError("docker requested but not found on PATH")
    if preference not in ("auto", "podman", "docker"):
        raise RuntimeError(f"unknown container engine: {preference!r}")
    # auto: prefer podman
    if shutil.which("podman"):
        return "podman"
    if shutil.which("docker"):
        return "docker"
    raise RuntimeError("neither podman nor docker found on PATH")


# ---------------------------------------------------------------------------
# Image building
# ---------------------------------------------------------------------------

def build_image(
    engine: str,
    dockerfile: str,
    image_name: str,
    build_context: Path,
    build_args: Optional[dict] = None,
    verbose: bool = False,
) -> str:
    """Build a container image; return the image ID (sha256:...)."""
    cmd = [engine, "build", "-f", dockerfile, "-t", image_name]
    for key, val in (build_args or {}).items():
        cmd += ["--build-arg", f"{key}={val}"]
    cmd.append(str(build_context))

    _run(cmd, verbose=verbose, desc=f"build {image_name}")
    return _image_id(engine, image_name)


def _image_id(engine: str, image_name: str) -> str:
    out = subprocess.check_output(
        [engine, "inspect", "--format", "{{.Id}}", image_name],
        text=True,
        stderr=subprocess.DEVNULL,
    ).strip()
    return out


# ---------------------------------------------------------------------------
# Container lifecycle
# ---------------------------------------------------------------------------

class Container:
    """A running container.  Use as a context manager for automatic cleanup."""

    def __init__(self, engine: str, name: str, verbose: bool = False):
        self._engine = engine
        self._name = name
        self._verbose = verbose
        self._id: Optional[str] = None

    def start(
        self,
        image: str,
        ports: Optional[dict] = None,
        env: Optional[dict] = None,
        network: Optional[str] = None,
        network_alias: Optional[str] = None,
        cap_add: Optional[list] = None,
    ) -> None:
        cmd = [self._engine, "run", "--rm", "-d", "--name", self._name]
        for host_port, container_port in (ports or {}).items():
            cmd += ["-p", f"{host_port}:{container_port}"]
        for key, val in (env or {}).items():
            cmd += ["-e", f"{key}={val}"]
        if network:
            cmd += ["--network", network]
        if network_alias:
            cmd += ["--network-alias", network_alias]
        for cap in (cap_add or []):
            cmd += ["--cap-add", cap]
        cmd.append(image)

        out = subprocess.check_output(cmd, text=True, stderr=subprocess.PIPE)
        self._id = out.strip()
        if self._verbose:
            print(f"[container] started {self._name} ({self._id[:12]})")

    def stop(self) -> None:
        if not self._id:
            return
        subprocess.run(
            [self._engine, "stop", self._name],
            check=False,
            capture_output=not self._verbose,
        )
        self._id = None

    def logs(self) -> str:
        if not self._id:
            return ""
        try:
            return subprocess.check_output(
                [self._engine, "logs", self._name],
                text=True,
                stderr=subprocess.STDOUT,
            )
        except subprocess.CalledProcessError:
            return ""

    def __enter__(self) -> "Container":
        return self

    def __exit__(self, *_) -> None:
        self.stop()


# ---------------------------------------------------------------------------
# Readiness polling
# ---------------------------------------------------------------------------

def wait_tcp(host: str, port: int, timeout: float = 30.0, interval: float = 0.5) -> None:
    """Poll host:port until a TCP connection succeeds or timeout expires."""
    deadline = time.monotonic() + timeout
    last_err = ""
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1.0):
                return
        except OSError as exc:
            last_err = str(exc)
            time.sleep(interval)
    raise TimeoutError(
        f"TCP {host}:{port} not reachable after {timeout}s: {last_err}"
    )


def wait_http(url: str, timeout: float = 30.0, interval: float = 0.5) -> None:
    """Poll an HTTP endpoint until it returns 200 or timeout expires."""
    import urllib.request
    import urllib.error

    deadline = time.monotonic() + timeout
    last_err = ""
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=2) as resp:
                if resp.status == 200:
                    return
        except Exception as exc:
            last_err = str(exc)
            time.sleep(interval)
    raise TimeoutError(f"HTTP {url} not ready after {timeout}s: {last_err}")


# ---------------------------------------------------------------------------
# Network helpers
# ---------------------------------------------------------------------------

def create_network(engine: str, name: str) -> None:
    subprocess.run(
        [engine, "network", "create", name],
        check=False,
        capture_output=True,
    )


def remove_network(engine: str, name: str) -> None:
    subprocess.run(
        [engine, "network", "rm", name],
        check=False,
        capture_output=True,
    )


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _run(cmd: list, verbose: bool, desc: str) -> None:
    if verbose:
        print(f"[container] {desc}: {' '.join(cmd)}")
        subprocess.run(cmd, check=True)
    else:
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise subprocess.CalledProcessError(
                result.returncode, cmd,
                output=result.stdout, stderr=result.stderr,
            )
