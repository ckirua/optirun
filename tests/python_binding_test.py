import sys
from optirun import RemoteException, Runtime

if len(sys.argv) != 4:
    raise SystemExit(2)

with Runtime(sys.argv[1], sys.argv[2], sys.argv[3], max_pending=1) as runtime:
    for name in ("echo", "raise_value_error"):
        runtime.register_handler(name, ("gil", "free_threaded"))
    assert runtime.submit("echo", [b"bytes"], backend="gil").result() == b"bytes"
    assert runtime.submit("echo", ["free"], backend="free_threaded").result() == "free"
    try:
        runtime.submit("raise_value_error", backend="gil").result()
    except RemoteException as error:
        assert error.type_name == "ValueError"
        assert error.remote_message == "expected handler failure"
        assert "raise_value_error" in error.traceback
    else:
        raise AssertionError("remote failure did not propagate")
