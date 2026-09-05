# PicoSMH dual-runtime comparison

`benchmark.py` runs the same PicoSMH shared-memory-ring round-trip workload inside the regular CPython 3.14 and free-threaded CPython 3.14t workers from one Python caller. It reports each worker's identity, PicoSMH backend, worker-only p50/p95, caller-observed dispatch p50/p95, messages/s, and the free-threaded/regular throughput ratio.

## Prerequisites

1. Install this repository into the caller Python (`pip install -e .`).
2. PicoSMH is expected at `../picosmh/python` relative to this repository by default. Supply `--picosmh-python-dir` if it is elsewhere.
3. For the native PicoSMH path, build/install a compatible `smh_q` extension for **each** worker interpreter. The extension ABI must match its worker (`cp314` or `cp314t`).

Use the pure-Python PicoSMH implementation when validating orchestration without building both PicoSMH extension ABIs:

```bash
python examples/picosmh/benchmark.py \
  --gil-python /usr/bin/python3.14 \
  --free-python /path/to/python3.14t \
  --backend pure
```

Use `--backend pybind11` only after installing PicoSMH's matching native package into both worker environments. The script intentionally does not claim that one runtime is universally faster; it prints measurements for the selected host, worker packages, payload size, and workload.

The comparison measures a sequential publish/consume cycle in each worker. It validates that both worker processes import and execute the same PicoSMH handler, but it is not a cross-worker shared-memory transport benchmark; hybrid-python's shared-buffer transport is a future milestone.
