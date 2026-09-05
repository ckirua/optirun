#!/usr/bin/env python3
"""Compare PicoSMH ring work on CPython 3.14 and CPython 3.14t workers."""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import time
from pathlib import Path

from hybrid_python import Runtime

ROOT = Path(__file__).resolve().parent
DEFAULT_PICOSMH = ROOT.parents[1].parent / "picosmh" / "python"


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    return ordered[round((len(ordered) - 1) * fraction)]


def run_backend(
    runtime: Runtime, backend: str, samples: int, iterations: int, payload_bytes: int
) -> dict[str, object]:
    identity = runtime.submit("identity", backend=backend).result()
    worker_seconds: list[float] = []
    caller_seconds: list[float] = []
    implementation = ""
    for _ in range(samples):
        started = time.perf_counter()
        result = runtime.submit(
            "ring_roundtrip", [iterations, payload_bytes], backend=backend
        ).result()
        caller_seconds.append(time.perf_counter() - started)
        implementation, elapsed_ns = result.rsplit(":", 1)
        worker_seconds.append(int(elapsed_ns) / 1_000_000_000)
    return {
        "identity": identity,
        "picosmh_backend": implementation,
        "worker_p50_ms": statistics.median(worker_seconds) * 1_000,
        "worker_p95_ms": percentile(worker_seconds, 0.95) * 1_000,
        "worker_messages_per_second": iterations / statistics.median(worker_seconds),
        "dispatch_p50_ms": statistics.median(caller_seconds) * 1_000,
        "dispatch_p95_ms": percentile(caller_seconds, 0.95) * 1_000,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gil-python", required=True, type=Path)
    parser.add_argument("--free-python", required=True, type=Path)
    parser.add_argument("--picosmh-python-dir", type=Path, default=DEFAULT_PICOSMH)
    parser.add_argument("--backend", choices=("pybind11", "pure"), default="pybind11")
    parser.add_argument("--iterations", type=int, default=10_000)
    parser.add_argument("--payload-bytes", type=int, default=64)
    parser.add_argument("--samples", type=int, default=7)
    args = parser.parse_args()
    if args.samples < 1 or args.iterations < 1 or args.payload_bytes < 0:
        parser.error(
            "samples and iterations must be positive; payload bytes cannot be negative"
        )
    if not args.picosmh_python_dir.is_dir():
        parser.error(
            f"PicoSMH Python directory does not exist: {args.picosmh_python_dir}"
        )

    os.environ["PICOSMH_PYTHON_DIR"] = str(args.picosmh_python_dir.resolve())
    os.environ["SMH_Q_BACKEND"] = args.backend
    with Runtime(
        str(args.gil_python), str(args.free_python), str(ROOT / "handlers.py")
    ) as runtime:
        runtime.register_handler("identity", ("gil", "free_threaded"))
        runtime.register_handler("ring_roundtrip", ("gil", "free_threaded"))
        report = {
            "iterations": args.iterations,
            "payload_bytes": args.payload_bytes,
            "samples": args.samples,
            "regular": run_backend(
                runtime, "gil", args.samples, args.iterations, args.payload_bytes
            ),
            "free_threaded": run_backend(
                runtime,
                "free_threaded",
                args.samples,
                args.iterations,
                args.payload_bytes,
            ),
        }
    regular = report["regular"]["worker_messages_per_second"]
    free_threaded = report["free_threaded"]["worker_messages_per_second"]
    expected_impl = "pure_python" if args.backend == "pure" else args.backend
    for runtime_name in ("regular", "free_threaded"):
        actual_impl = report[runtime_name]["picosmh_backend"]
        if actual_impl != expected_impl:
            raise RuntimeError(
                f"{runtime_name} worker loaded PicoSMH {actual_impl!r}, "
                f"expected {expected_impl!r}; install matching PicoSMH extensions "
                "into both worker environments"
            )
    report["free_threaded_speedup"] = free_threaded / regular
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
