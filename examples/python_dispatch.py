"""Run one handler on CPython 3.14 and CPython 3.14t from Python."""
import sys
from hybrid_python import Runtime

if len(sys.argv) != 4:
    raise SystemExit("usage: python_dispatch.py GIL_PYTHON FREE_PYTHON HANDLERS")

with Runtime(sys.argv[1], sys.argv[2], sys.argv[3]) as runtime:
    runtime.register_handler("echo", ["gil", "free_threaded"])
    runtime.register_handler("identity", ["gil", "free_threaded"])
    for backend in ("gil", "free_threaded"):
        print(runtime.worker_info(backend))
        print(runtime.submit("identity", backend=backend).result())
        print(runtime.submit("echo", ["ok"], backend=backend).result())
