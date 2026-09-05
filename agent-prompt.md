Agent handoff prompt

You are implementing a C++ library called `optirun` that lets C++ and Python callers dispatch handlers to either persistent CPython 3.14 workers or persistent free-threaded CPython 3.14t workers. Read the accompanying `plan.md` before making changes. The plan describes a proposed project; do not assume any of its code already exists.

## User intent

Some workloads perform better on ordinary Python and others on free-threaded Python. The user wants one library to manage both, with runtime selection and eventually build-time/generated integration. The C++ API should be callable from Python through pybind11, with Cython an optional alternative. A shared-memory manager (the working interpretation of “SMH”) and allocation arenas should support in-place processing of compatible payloads. Background dispatch threads, persistent handler executors, and message queues should keep per-request overhead low.

## Non-negotiable design boundaries

1. Use separate processes for the two CPython builds. Do not mix their Python heaps, native ABIs, or `PyObject*` values.
2. Route complete invocations, not running frames. Keep stateful objects on their owning worker.
3. Keep the core C++ transport/allocator independent of Python objects. Bindings must obey interpreter thread-state/GIL rules and be built for each ABI.
4. Verify both requested interpreter identities and actual GIL status; unsupported extensions can enable the GIL in a free-threaded worker.
5. Shared payloads use segment IDs, offsets, lengths, and generations. Never exchange raw addresses between processes.
6. Use explicit ownership and leases. Arena closure, timeout, cancellation, and worker failure must not cause memory reuse while a worker or exported view can still access it.
7. Zero-copy is a supported data-path contract, not a promise for arbitrary Python objects or algorithms. An explicit no-copy request must reject conversion rather than silently copy.
8. Bounded queues and memory quotas provide backpressure. Do not start with a complex lock-free allocator or queue without measurement.
9. Never automatically duplicate or retry side-effecting handlers for profiling or failure recovery.

## Initial implementation assignment

Inspect the repository, applicable instructions, toolchain, and installed Python builds. Use Windows as the initial platform unless the repository or user specifies otherwise. Verify current official documentation for chosen dependencies. Record missing prerequisites and continue independent scaffolding where possible; do not claim tests against unavailable runtimes passed.

Implement milestones 0 and 1 from `plan.md` first: a minimal buildable C++ runtime manager that launches two persistent Python executable workers, handshakes with them, registers handlers, sends framed requests over pipes, routes explicitly, returns futures/results, propagates remote errors, bounds pending requests, and shuts down cleanly.

Provide a small example showing a handler called on each actual Python build and meaningful tests for routing, failure propagation, and lifecycle behavior. Begin with scalars and bytes. Keep the transport interface replaceable so shared-memory buffers and later queues fit without rewriting routing.

After the baseline works, follow the plan in order: pybind11 binding, shared buffers and arenas, measured transport optimization, then automatic routing and generated wrappers. Prefer a correct vertical slice over broad placeholder APIs. Do not implement every future feature before validating the baseline.

## Design expectations for later milestones

- One Python execution thread per ordinary worker initially; a configurable pool for explicitly thread-safe 3.14t handlers.
- Native transport threads handle descriptors; Python handlers/callbacks execute with the correct interpreter state.
- Coordinator-owned allocation ledger, per-task bump arenas, persistent reusable buffers, and tracked view lifetimes.
- Contiguous fixed-width numeric arrays first; no object-dtype arrays or arbitrary shared Python graphs.
- Inline tiny messages, shared-memory handles for large payloads, batching, and sleeping consumers by default.
- No assumption that standard C++ synchronization objects are automatically portable across processes.
- On worker failure, distinguish unknown handler outcome from a clean failure; partially written outputs must not appear successful.

## Validation and reporting

Measure steady-state dispatch separately from startup. Compare direct execution, serialized worker calls, and shared-buffer calls using representative payload sizes. Report latency distributions, throughput, CPU usage, and memory, including the crossover where runtime selection helps. Do not promise universal speedups.

At each meaningful milestone, explain what works, how it was verified, actual prerequisites/limitations, and the next step. Keep the plan current when evidence changes a design choice. Treat API samples as proposals, not fixed compatibility commitments. Ask only for decisions that block correct progress; otherwise use documented defaults.

