#!/usr/bin/env python3
"""Compare PicoSMH ring work on CPython 3.14 and CPython 3.14t workers."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

from optirun import Runtime

ROOT = Path(__file__).resolve().parent
DEFAULT_PICOSMH = ROOT.parents[1].parent / "picosmh" / "python"
PROBE = (
    "import json,sys,sysconfig; print(json.dumps({"
    "'implementation':sys.implementation.name,'version':list(sys.version_info[:2]),"
    "'free_threaded':sysconfig.get_config_var('Py_GIL_DISABLED') == 1,"
    "'gil_enabled':sys._is_gil_enabled()}))"
)


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    return ordered[round((len(ordered) - 1) * fraction)]


def interpreter_candidates(
    environment_variable: str, executable_name: str
) -> list[Path]:
    """Return configured, caller, PATH, and conventional CPython candidates."""
    raw_candidates = [os.environ.get(environment_variable), sys.executable]
    raw_candidates.extend(
        [
            shutil.which(executable_name),
            str(Path.home() / ".local" / "bin" / executable_name),
            f"/usr/bin/{executable_name}",
        ]
    )
    candidates: list[Path] = []
    for raw_candidate in raw_candidates:
        if not raw_candidate:
            continue
        candidate = Path(raw_candidate)
        if candidate.is_file() and os.access(candidate, os.X_OK):
            resolved = candidate.resolve()
            if resolved not in candidates:
                candidates.append(resolved)
    return candidates


def matches_backend(candidate: Path, free_threaded: bool) -> bool:
    """Verify CPython 3.14 build identity and actual GIL state."""
    try:
        probe = subprocess.run(
            [str(candidate), "-I", "-c", PROBE],
            capture_output=True,
            check=True,
            text=True,
            timeout=5,
        )
        identity = json.loads(probe.stdout)
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError):
        return False
    return (
        identity["implementation"] == "cpython"
        and identity["version"] == [3, 14]
        and identity["free_threaded"] is free_threaded
        and identity["gil_enabled"] is not free_threaded
    )


def resolve_interpreter(
    supplied: Path | None,
    environment_variable: str,
    executable_name: str,
    free_threaded: bool,
) -> Path:
    """Use an explicit interpreter or select the first verified local candidate."""
    if supplied is not None:
        candidate = supplied.expanduser().resolve()
        if not matches_backend(candidate, free_threaded):
            expected = (
                "free-threaded CPython 3.14t with GIL disabled"
                if free_threaded
                else "regular CPython 3.14 with GIL enabled"
            )
            raise ValueError(f"{candidate} is not {expected}")
        return candidate
    for candidate in interpreter_candidates(environment_variable, executable_name):
        if matches_backend(candidate, free_threaded):
            return candidate
    override = f"--{'free' if free_threaded else 'gil'}-python"
    raise ValueError(
        f"could not locate a {'free-threaded' if free_threaded else 'regular'} CPython 3.14; "
        f"pass {override} or set {environment_variable}"
    )


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
    parser.add_argument("--gil-python", type=Path)
    parser.add_argument("--free-python", type=Path)
    parser.add_argument("--picosmh-python-dir", type=Path, default=DEFAULT_PICOSMH)
    parser.add_argument("--backend", choices=("pybind11", "pure"), default="pure")
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
    try:
        gil_python = resolve_interpreter(
            args.gil_python, "OPTIRUN_GIL_EXECUTABLE", "python3.14", False
        )
        free_python = resolve_interpreter(
            args.free_python,
            "OPTIRUN_FREE_THREADED_EXECUTABLE",
            "python3.14t",
            True,
        )
    except ValueError as error:
        parser.error(str(error))

    os.environ["PICOSMH_PYTHON_DIR"] = str(args.picosmh_python_dir.resolve())
    os.environ["SMH_Q_BACKEND"] = args.backend
    with Runtime(
        str(gil_python), str(free_python), str(ROOT / "handlers.py")
    ) as runtime:
        runtime.register_handler("identity", ("gil", "free_threaded"))
        runtime.register_handler("ring_roundtrip", ("gil", "free_threaded"))
        report = {
            "gil_python": str(gil_python),
            "free_python": str(free_python),
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
    expected_impl = "pure_python" if args.backend == "pure" else args.backend
    for runtime_name in ("regular", "free_threaded"):
        actual_impl = report[runtime_name]["picosmh_backend"]
        if actual_impl != expected_impl:
            raise RuntimeError(
                f"{runtime_name} worker loaded PicoSMH {actual_impl!r}, "
                f"expected {expected_impl!r}; install matching PicoSMH extensions "
                "into both worker environments"
            )
    regular = report["regular"]["worker_messages_per_second"]
    free_threaded = report["free_threaded"]["worker_messages_per_second"]
    report["free_threaded_speedup"] = free_threaded / regular
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
