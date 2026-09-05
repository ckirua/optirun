# Milestone 0 contract

Active target: Linux/POSIX. Milestone 1 is intentionally rejected on non-Linux platforms.

## Supported values

`null`, booleans, signed 64-bit integers, IEEE-754 binary64, UTF-8 text, and byte strings. Containers, Python object references, buffer handles, and raw addresses are unsupported.

## Handler file

A handler file exports `HANDLERS`, a dictionary of nonempty string names to callables. A callable receives positional decoded values and must return a supported value. Exceptions are delivered as type name, message, and formatted traceback.

## Wire protocol

Each message is `[u32 big-endian body length][u16 version=1][u8 type][u64 request id][body]`; maximum body size is 16 MiB. Message types are HELLO, INVOKE, RESULT, ERROR, SHUTDOWN, and SHUTDOWN_ACK. Invalid framing, tags, UTF-8, or trailing data fail the worker; no stream resynchronization is attempted.

`HELLO` follows handler loading and reports CPython identity, version, ABI flags, `Py_GIL_DISABLED`, actual `sys._is_gil_enabled()`, and sorted handler IDs. The coordinator accepts only CPython 3.14: regular is non-`t`, build flag false, GIL enabled; free-threaded is `t`, build flag true, GIL disabled.

## Python environments and extensions

Each worker is launched with the executable supplied by its `WorkerConfig`; the C++ manager does not embed Python, discover a virtual environment, or share an interpreter between workers. Configure the virtual environment's interpreter explicitly:

```cpp
RuntimeConfig config;
config.gil = {"/path/to/regular-venv/bin/python", "handlers.py"};
config.free_threaded = {"/path/to/free-threaded-venv/bin/python", "handlers.py"};
```

The worker uses its own `sys.path`, so handler code imports dependencies normally. Install packages independently into every worker environment that may execute the handler:

```bash
/path/to/regular-venv/bin/python -m pip install your-package
/path/to/free-threaded-venv/bin/python -m pip install your-package
```

Pure-Python packages can be installed in both environments. Compiled extensions must be built or installed separately for the regular CPython 3.14 ABI and free-threaded CPython 3.14t ABI; never share an extension binary between them. Imported module state and extension state are persistent within one worker, but never shared across the two processes.

The free-threaded worker verifies its build identity and actual GIL state *after* loading the handler module. An extension that enables the GIL causes free-threaded startup to fail rather than silently accepting a fallback. Extension import failures also fail startup before the worker becomes available.

### Current virtual-environment limitation

The worker command is currently:

```text
PYTHON_EXECUTABLE -I -B -S workers/hybrid_python_worker.py --handler-file HANDLER_FILE
```

`-I` intentionally ignores ambient `PYTHONPATH` and user-site packages, and `-B` avoids writing bytecode. `-S` also suppresses `site` initialization, which means a normal virtual environment's `site-packages` is not automatically available. Therefore venv-installed packages and extensions are **not reliably importable in the baseline** despite configuring the venv executable.

The intended correction is to remove `-S` while retaining `-I -B`; then the configured venv's standard `site-packages` is used without accepting ambient import paths. This has not yet been implemented.

### Current transport limit

Imports are independent of transport support. The baseline only transports `null`, booleans, signed 64-bit integers, binary64 floats, UTF-8 strings, and bytes. NumPy arrays, extension-owned objects, and arbitrary Python objects cannot cross the process boundary yet. Encode supported values yourself or wait for the shared-buffer milestone.

## Observed prerequisite probe

```json
{"regular":{"executable":"/usr/bin/python3.14","version":"3.14.6","Py_GIL_DISABLED":0,"gil_enabled":true},"free":{"executable":"/home/dev/.local/share/uv/python/cpython-3.14.3+freethreaded-linux-x86_64-gnu/bin/python3.14t","version":"3.14.3","Py_GIL_DISABLED":1,"gil_enabled":false}}
```

Patch versions differ and are accepted; major/minor must be 3.14. The free-threaded executable is environment-specific and configuration fails rather than falling back if it disappears.
