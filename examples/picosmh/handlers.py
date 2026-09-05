"""PicoSMH workloads run inside each OptiRun worker."""

from __future__ import annotations

import os
import sys
import time
import uuid
from pathlib import Path

picosmh_python = os.environ.get("PICOSMH_PYTHON_DIR")
if not picosmh_python:
    raise RuntimeError("PICOSMH_PYTHON_DIR must name PicoSMH's python directory")
sys.path.insert(0, str(Path(picosmh_python).resolve()))

from smh_q import Ring, impl_name


def ring_roundtrip(iterations: int, payload_bytes: int) -> str:
    """Publish then consume scalar-sized messages through one PicoSMH ring."""
    if iterations <= 0 or payload_bytes < 0:
        raise ValueError("iterations must be positive and payload_bytes non-negative")
    name = f"optirun_picosmh_{os.getpid()}_{uuid.uuid4().hex}"
    Ring.unlink(name)
    ring = Ring(
        name,
        create=True,
        slot_count=max(256, min(iterations, 4096)),
        slot_size=max(64, payload_bytes + 8),
    )
    payload = bytes(payload_bytes)
    try:
        started = time.perf_counter_ns()
        for _ in range(iterations):
            while not ring.try_publish(payload):
                pass
            while ring.try_consume() is None:
                pass
        elapsed_ns = time.perf_counter_ns() - started
        return f"{impl_name()}:{elapsed_ns}"
    finally:
        ring.close(unlink=True)


def identity() -> str:
    """Report the PicoSMH backend and actual Python GIL state."""
    return f"{impl_name()}|gil={sys._is_gil_enabled()}|exe={sys.executable}"


HANDLERS = {"identity": identity, "ring_roundtrip": ring_roundtrip}
