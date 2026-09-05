# hybrid-python

A small Python and C++ runtime that explicitly dispatches handler calls to persistent **CPython 3.14** and **free-threaded CPython 3.14t** worker processes.

The two interpreters never share a process, CPython ABI, Python heap, or `PyObject*`. A caller chooses the backend for every invocation; the manager verifies the requested build and the worker's actual GIL state after handler imports.

> Status: Linux/POSIX alpha. The baseline supports scalar values, strings, and bytes. Arrays, shared-memory buffers, automatic routing, and retries are intentionally not implemented yet.

## Quick start

Install the package into the Python that will call it:

```bash
python -m pip install -e .
```

Create a handler file:

```python
# handlers.py
def echo(value):
    return value

HANDLERS = {"echo": echo}
```

Dispatch through either worker from one Python process:

```python
from hybrid_python import Runtime

with Runtime(
    gil_python="/path/to/regular-venv/bin/python",
    free_python="/path/to/free-threaded-venv/bin/python",
    handler_file="handlers.py",
) as runtime:
    runtime.register_handler("echo", ["gil", "free_threaded"])

    regular = runtime.submit("echo", ["regular"], backend="gil")
    free = runtime.submit("echo", ["free"], backend="free_threaded")
    print(regular.result(), free.result())
```

`submit()` starts workers on first use. Register every handler before `start()` or the first submission. `DispatchFuture.result()` waits without holding the caller's GIL.

## Requirements

- Linux/POSIX
- CPython 3.14 caller or free-threaded CPython 3.14t caller
- Explicit paths to both worker interpreters
- CMake 4+, a C++20 compiler, and Python development headers to build from source

Build one wheel per caller ABI. A `cp314` wheel is for regular callers; a `cp314t` wheel is for free-threaded callers. Worker executables and their dependencies are configured at runtime and are not bundled by the wheel.

For an editable install without build isolation:

```bash
python -m pip install -r requirements-build.txt
python -m pip install -e . --no-build-isolation
```

## Handlers and extensions

Handlers export a nonempty mapping named `HANDLERS`. Their arguments and return values may be `None`, `bool`, signed 64-bit `int`, `float`, `str`, or `bytes`.

Each worker imports packages from its own interpreter environment. Install pure-Python packages into every worker environment that needs them. Compiled extensions must be built separately for `cp314` and `cp314t`; a free-threaded extension that enables the GIL causes startup rejection.

## Development

```bash
cmake --preset linux-gcc14
cmake --build --preset linux-gcc14
ctest --preset linux-gcc14 --output-on-failure
```

Run the native and Python examples in [`examples/`](examples/). The detailed protocol, lifecycle, packaging, and environment contract is in [`docs/milestone-0-contract.md`](docs/milestone-0-contract.md).

## License

[MIT](LICENSE)
