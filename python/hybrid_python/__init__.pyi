"""Typed API for dispatching handlers to regular and free-threaded CPython workers."""

from typing import Literal, Sequence, TypeAlias, TypedDict

BackendName: TypeAlias = Literal["gil", "free_threaded"]
Value: TypeAlias = None | bool | int | float | str | bytes

class WorkerInfo(TypedDict):
    backend: BackendName
    executable: str
    version: tuple[int, int, int]
    abiflags: str
    soabi: str
    build_supports_free_threading: bool
    gil_enabled: bool

class Backend:
    """Backend identifiers accepted by :meth:`Runtime.submit`."""

    GIL: Backend
    FREE_THREADED: Backend

class RemoteException(RuntimeError):
    """A handler failure, with the remote Python exception details."""

    type_name: str
    remote_message: str
    traceback: str

class DispatchFuture:
    """Result handle for one dispatched invocation."""
    def done(self) -> bool: ...
    def result(self, timeout: float | None = None) -> Value:
        """Wait for and return the result; raises TimeoutError or RemoteException."""

class Runtime:
    """Owns persistent CPython 3.14 and CPython 3.14t worker processes.

    Register handlers before calling :meth:`start` or :meth:`submit`. The first
    submit starts both workers automatically. Use as a context manager and call
    :meth:`shutdown` to reap worker processes deterministically.
    """
    def __init__(
        self,
        gil_python: str,
        free_python: str,
        handler_file: str,
        *,
        max_pending: int = 64,
        handshake_timeout: float = 5.0,
        shutdown_timeout: float = 5.0,
    ) -> None: ...
    def __enter__(self) -> Runtime: ...
    def __exit__(
        self, exc_type: object, exc_value: object, traceback: object
    ) -> None: ...
    def register_handler(
        self, name: str, backends: Sequence[BackendName | Backend]
    ) -> None:
        """Declare a handler and every backend it may be explicitly routed to."""
    def start(self) -> None:
        """Launch both workers and validate their interpreter/GIL identities."""
    def submit(
        self,
        handler: str,
        arguments: Sequence[Value] = (),
        *,
        backend: BackendName | Backend,
    ) -> DispatchFuture:
        """Route one supported-value invocation to the selected backend."""
    def worker_info(self, backend: BackendName | Backend) -> WorkerInfo:
        """Return validated identity and GIL state for one started worker."""
    def shutdown(self) -> None:
        """Stop accepting work, drain pending calls, and reap both workers."""
