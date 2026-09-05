# Milestone 0 contract

Active target: Linux/POSIX. Milestone 1 is intentionally rejected on non-Linux platforms.

## Supported values

`null`, booleans, signed 64-bit integers, IEEE-754 binary64, UTF-8 text, and byte strings. Containers, Python object references, buffer handles, and raw addresses are unsupported.

## Handler file

A handler file exports `HANDLERS`, a dictionary of nonempty string names to callables. A callable receives positional decoded values and must return a supported value. Exceptions are delivered as type name, message, and formatted traceback.

## Wire protocol

Each message is `[u32 big-endian body length][u16 version=1][u8 type][u64 request id][body]`; maximum body size is 16 MiB. Message types are HELLO, INVOKE, RESULT, ERROR, SHUTDOWN, and SHUTDOWN_ACK. Invalid framing, tags, UTF-8, or trailing data fail the worker; no stream resynchronization is attempted.

`HELLO` follows handler loading and reports CPython identity, version, ABI flags, `Py_GIL_DISABLED`, actual `sys._is_gil_enabled()`, and sorted handler IDs. The coordinator accepts only CPython 3.14: regular is non-`t`, build flag false, GIL enabled; free-threaded is `t`, build flag true, GIL disabled.

## Observed prerequisite probe

```json
{"regular":{"executable":"/usr/bin/python3.14","version":"3.14.6","Py_GIL_DISABLED":0,"gil_enabled":true},"free":{"executable":"/home/dev/.local/share/uv/python/cpython-3.14.3+freethreaded-linux-x86_64-gnu/bin/python3.14t","version":"3.14.3","Py_GIL_DISABLED":1,"gil_enabled":false}}
```

Patch versions differ and are accepted; major/minor must be 3.14. The free-threaded executable is environment-specific and configuration fails rather than falling back if it disappears.
