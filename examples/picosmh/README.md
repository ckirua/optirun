# PicoSMH dual-runtime comparison

`benchmark.py` runs the same PicoSMH shared-memory-ring round-trip workload inside the regular CPython 3.14 and free-threaded CPython 3.14t workers from one Python caller. It reports each worker's identity, PicoSMH backend, worker-only p50/p95, caller-observed dispatch p50/p95, messages/s, and the free-threaded/regular throughput ratio.

## Prerequisites

1. Install this repository into the caller Python (`pip install -e .`).
2. PicoSMH is expected at `../picosmh/python` relative to this repository by default. Supply `--picosmh-python-dir` if it is elsewhere.
3. For the native PicoSMH path, build/install a compatible `smh_q` extension for **each** worker interpreter. The extension ABI must match its worker (`cp314` or `cp314t`).

Run the comparison with no interpreter arguments:

```bash
python examples/picosmh/benchmark.py
```

The script validates and launches **both** local CPython 3.14 and CPython 3.14t interpreters in one invocation. It checks the caller interpreter first, then `python3.14` / `python3.14t` on `PATH`, `~/.local/bin`, and `/usr/bin`. Set `OPTIRUN_GIL_EXECUTABLE` and `OPTIRUN_FREE_THREADED_EXECUTABLE`, or pass `--gil-python` and `--free-python`, only to override selection.

The default uses PicoSMH's `pure` backend so the comparison runs from the sibling source checkout. Use the pure-Python implementation when validating orchestration without building both PicoSMH extension ABIs:

```bash
python examples/picosmh/benchmark.py --backend pure
```

Use `--backend pybind11` only after installing PicoSMH's matching native package into both worker environments. The script intentionally does not claim that one runtime is universally faster; it prints measurements for the selected host, worker packages, payload size, and workload.

The comparison measures a sequential publish/consume cycle in each worker. It validates that both worker processes import and execute the same PicoSMH handler, but it is not a cross-worker shared-memory transport benchmark; OptiRun's shared-buffer transport is a future milestone.
