# OptiRun: implementation plan

## Objective

Build a C++ library with a Python API that routes work to persistent CPython 3.14 and free-threaded CPython 3.14t worker processes. Select the runtime best suited to a handler while avoiding bulk data copies where the data format and algorithm permit it.

The library manages workers, registered handlers, asynchronous messages, shared-memory buffers, allocation arenas, and result lifetimes. The working interpretation of “SMH” is a shared-memory heap/manager; the name is provisional.

This is a design and implementation roadmap, not an assertion that the library or proposed APIs already exist.

## Scope and boundaries

- Use separate OS processes for the two Python builds. Do not attempt to load both CPython ABIs into one address space.
- Support a native C++ caller and, through a binding, a Python caller running either build.
- Route whole handler invocations. Do not migrate running Python frames or arbitrary Python objects between runtimes.
- Keep Python heaps and garbage collectors private to their interpreters. Shared arenas hold payload storage, not Python object headers or pointers.
- Support runtime selection first. Add build-time wrapper generation and profiled routing later.
- Target local, trusted workers. Distributed execution and execution of untrusted Python are outside the initial scope.
- Target Linux/POSIX for milestone 1, matching the observed development target and installed CPython 3.14 interpreters. Keep the platform boundary explicit for a future Windows implementation.

## Architecture

```text
C++ application or Python binding
                  |
            Runtime manager
       /          |           \
 Routing      Worker pool     Shared-memory manager
                 |               |
       +---------+---------+     Arenas / buffer leases
       |                   |         |
 CPython 3.14 process  CPython 3.14t process
       |                   |
 Dispatcher           Dispatcher
       |                   |
 Handler executor     Handler thread pool
       +---------+---------+
                 |
       Completions / futures
```

The transport and execution layers must be separable so pipes can establish a correct baseline before adding shared-memory queues.

### Runtime manager

- Launch explicitly configured interpreter executables and keep them alive.
- Perform a startup handshake: protocol version, Python version/build, actual GIL status, available handlers, and capabilities.
- Reject a worker intended for free-threaded execution if its GIL is unexpectedly enabled, unless the caller explicitly accepts that fallback.
- Route requests, enforce queue limits, maintain futures, and coordinate shutdown.
- Keep separate package environments for each Python build.

### Workers and handlers

- Begin with Python executable workers loading a worker bootstrap package. Embedded C++ worker executables are an optional later deployment mode.
- Register named handlers once and map them to numeric IDs after handshake.
- Initially execute one Python handler at a time per regular 3.14 worker. Use multiple processes for additional regular-Python parallelism.
- Allow multiple execution threads in 3.14t only for handlers explicitly declared safe for concurrent execution.
- Keep the C++ transport thread independent of Python execution. All calls into Python must use the correct interpreter thread state and GIL rules.
- Deliver callbacks through the caller's interpreter or event loop; never invoke Python callbacks from an unattached transport thread.
- Bound total worker and handler concurrency to avoid oversubscribing CPUs, including native library thread pools.

### Protocol and message processing

Requests contain a protocol version, request ID, handler ID, backend policy, deadline metadata, inline arguments, and buffer descriptors. Completions contain the request ID and either results or structured error information.

- Small scalar/byte payloads may be inline. Large compatible payloads use shared-memory descriptors.
- Define fixed-width fields, framing, validation, supported types, and size limits. Do not transmit raw C++ struct layouts as the wire format.
- Begin with bounded pipe-based request/completion transport and batches.
- Add bounded shared-memory rings only if benchmarks show meaningful benefit. Prefer simple per-worker queue ownership over a general lock-free MPMC queue.
- Use documented process-shared synchronization for each OS. Do not assume an arbitrary `std::atomic` or `std::mutex` placed in a mapping is portable interprocess synchronization.
- Sleep when idle and wake on work. Optional bounded spinning requires benchmark evidence and an explicit CPU/latency tradeoff.
- Define ordering: request IDs correlate results; concurrent completion order is not guaranteed.

### Shared-memory manager and arenas

The coordinator owns the allocation ledger. Workers request output allocations rather than concurrently mutating a shared allocator in the first version.

```cpp
// Logical descriptor; encode explicitly for transport.
struct BufferHandle {
    uint64_t segment_id;
    uint64_t offset;
    uint64_t byte_length;
    uint64_t generation;
};
```

- Resolve offsets against each process's local mapping; never transmit raw addresses.
- Validate segment identity, generation, alignment, overflow, and bounds before creating views.
- Add explicit dtype, shape, and supported layout metadata for arrays. Initially require contiguous, fixed-width, non-object arrays.
- Use aligned bump allocation for per-task arenas; reclaim whole arenas when all leases finish.
- Provide persistent buffers separately for reuse across requests.
- Track outstanding message, worker, result, and exported-view leases. Closing an arena prevents new allocations but does not invalidate live views.
- Transfer exclusive write ownership or share read-only access. Disjoint writable regions require explicit partitioning rules.
- Reclaim after worker failure only once process exit is confirmed and other leases have ended. A timeout alone does not prove the writer stopped.
- A stale generation must fail validation. Generation checks do not replace leases protecting views already exported.

### In-place and zero-copy contract

- Allocate in shared memory at the source whenever possible.
- Build local array/buffer wrappers over mapped storage. Wrapper allocation and metadata transfers are acceptable.
- Offer an explicit no-copy option that rejects incompatible inputs instead of silently converting them.
- Ordinary lists/dicts require encoding; existing private arrays may require an initial copy.
- Handler algorithms may allocate internally. Arena-backed input does not guarantee an algorithm is in-place.
- Reuse output buffers where supported and expose input/output ownership in the handler contract.
- Never promise zero dispatch cost: queueing, wakeups, cache coherence, wrappers, and synchronization remain.

### Bindings and packaging

- Prefer pybind11 for the first C++ binding; Cython is an optional alternative or computational-kernel tool.
- Keep the core C++ dispatcher independent of Python objects and headers where practical.
- Build and test Python bindings separately for 3.14 and 3.14t. Enabling the GIL in 3.14t does not change its ABI into regular 3.14.
- Release the caller's GIL during blocking waits. Audit native shared state before declaring free-threading support.
- Ensure exported views keep their mappings and allocation leases alive, including during runtime shutdown.
- Provide CMake integration for native consumers and separately packaged Python bindings/worker bootstrap.

## Proposed user-facing API

Names below are illustrative and should be refined during implementation.

```python
from optirun import Runtime

with Runtime(gil_python="...", free_python="...") as runtime:
    with runtime.arena(capacity=256 * 1024 * 1024) as arena:
        data = arena.empty((100_000, 64), dtype="float32")
        future = runtime.submit(
            "transform", data,
            backend="free_threaded",
            copy=False,
        )
        result = future.result()
        local_result = result.copy()  # Independent storage when desired.
```

Handler registration should declare entry point, accepted types, backend support, concurrency safety, mutation policy, and whether retries/profiling are safe. Stateful handles remain pinned to their owning worker. Define exact borrow/ownership behavior before implementing this sample API.

## Failure and lifecycle semantics

- Represent remote failures with exception type name, message, and formatted traceback; do not attempt to recreate arbitrary exception objects.
- Distinguish pending cancellation from cooperative cancellation of running work.
- A caller timeout does not release a buffer still in use by a worker.
- Do not automatically retry handlers with side effects. A crashed worker can leave execution outcome unknown and output buffers partially written.
- Fail or mark affected outputs unusable after interrupted writes; do not publish them as successful results.
- Shutdown stops new submissions, drains or cancels pending work according to policy, joins execution, and retains mappings for surviving exported views.
- Add quotas for queued work, arena capacity, and retained results so backpressure also bounds memory.

## Implementation milestones

### 0. Validate the environment and freeze the first contracts

Inspect the repository and its instructions. Locate both interpreters, validate build/GIL status, check pybind11 and array-library support, and establish a minimal build. Record missing prerequisites without silently substituting a different Python build.

Deliver: architecture decision notes, supported types, handler contract, protocol sketch, and startup probe.

### 1. End-to-end dual-runtime dispatch

Implement a Python-independent C++ manager, two persistent Python workers, framed pipe transport, explicit routing, bounded requests, futures, and structured exceptions. Use serialized scalar/byte payloads first.

Acceptance: a native C++ example calls a handler on both actual builds, reports their identities, returns correct results, and handles exceptions and shutdown.

### 2. Python binding

Expose the same manager through pybind11, build for each ABI, and release the GIL while waiting. Add handler registration and configurable 3.14t execution concurrency.

Acceptance: both caller builds can route calls to both worker builds; a waiting call does not block unrelated caller Python threads; declared concurrent handlers execute correctly.

### 3. Shared buffers and arenas

Implement OS mappings, coordinator-owned arenas, descriptor validation, leases, supported array views, and explicit mutation ownership.

Acceptance: one worker modifies a shared buffer and another observes the update; supported large payloads use descriptors without serialization of their contents; arena close, live views, worker crashes, stale handles, and timeout lifetimes behave correctly.

### 4. Message throughput and resilience

Measure pipe transport first. Implement shared-memory queues and batching only when justified. Add backpressure, failure recovery policies, metrics, and sustained-load verification.

Acceptance: bounded memory under overload, no lost/duplicate completion delivery in normal operation, defined outcomes under worker failure, and reproducible latency/throughput comparisons.

### 5. Routing and build-time tooling

Add offline profiles, routing by handler/input-size bucket, optional generated typed wrappers, and build manifests for both ABIs.

Acceptance: deterministic explicit routing remains available. Profiling is opt-in and never duplicates side effects implicitly. Stateful handlers stay pinned; unavailable backends produce an explicit policy decision.

## Benchmark and verification plan

Compare direct in-process execution on each build against persistent-worker dispatch using serialized transport and shared buffers. Measure startup separately from steady state.

- Workloads: tiny scalar messages, batched messages, large numeric buffers, Python CPU-bound handlers, and native kernels that release the GIL.
- Report p50/p95/p99 latency, throughput, CPU consumption while idle/loaded, memory use, allocation behavior, and payload sizes.
- Measure one producer and multiple producers, regular versus threaded workers, and contention on shared data.
- Track transport bytes and inspect the buffer path; observing the same numerical result alone does not establish zero-copy.
- Run lifecycle/concurrency tests for leases, writable ownership, queue saturation, cancellation, failures, and shutdown.
- Do not invent a speedup target. Identify the measured crossover where execution savings exceed dispatch and synchronization costs.

## Open decisions

Confirm deployment OSes, representative message sizes, throughput/latency goals, Python dependencies, typical handler duration, stateful-handler needs, and whether GPU buffers are relevant. These do not block the basic prototype; GPU memory support is deferred.

## Reference documentation

- CPython free threading: https://docs.python.org/3.14/howto/free-threading-python.html
- CPython extension ABI and free threading: https://docs.python.org/3.14/howto/free-threading-extensions.html
- CPython thread state: https://docs.python.org/3.14/c-api/threads.html
- Python shared memory: https://docs.python.org/3.14/library/multiprocessing.shared_memory.html
- pybind11 embedding: https://pybind11.readthedocs.io/en/stable/advanced/embedding.html
- pybind11 concurrency: https://pybind11.readthedocs.io/en/stable/advanced/misc.html
- Cython GIL behavior: https://docs.cython.org/en/latest/src/userguide/nogil.html

Recheck version-specific documentation before implementation. These references support the underlying capabilities; the architecture and APIs above are proposed project design.
