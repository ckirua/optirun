#!/usr/bin/env python3
import argparse, importlib.util, json, os, platform, struct, sys, sysconfig, traceback

MAX_BODY = 16 * 1024 * 1024
HELLO, INVOKE, RESULT, ERROR, SHUTDOWN, SHUTDOWN_ACK = range(1, 7)

def fail(message):
    print(f"hybrid_python worker: {message}", file=sys.stderr, flush=True)
    raise SystemExit(2)
def read_exact(n):
    data = sys.stdin.buffer.read(n)
    if len(data) != n: fail("unexpected end of input")
    return data
def read_frame():
    raw = sys.stdin.buffer.read(4)
    if not raw: return None
    if len(raw) != 4: fail("truncated frame length")
    n, = struct.unpack(">I", raw)
    if n < 11 or n > MAX_BODY: fail("invalid frame length")
    data = read_exact(n)
    version, typ, request_id = struct.unpack(">HBQ", data[:11])
    if version != 1 or typ not in range(1, 7): fail("invalid frame header")
    return typ, request_id, memoryview(data)[11:]
def write_frame(typ, request_id, body=b""):
    data = struct.pack(">HBQ", 1, typ, request_id) + body
    if len(data) > MAX_BODY: fail("frame too large")
    sys.stdout.buffer.write(struct.pack(">I", len(data)) + data); sys.stdout.buffer.flush()
def string_encode(s):
    raw = s.encode("utf-8"); return struct.pack(">I", len(raw)) + raw
def take_string(data, offset):
    if offset + 4 > len(data): raise ValueError("truncated string")
    n, = struct.unpack(">I", data[offset:offset+4]); offset += 4
    if offset + n > len(data): raise ValueError("truncated string")
    return bytes(data[offset:offset+n]).decode("utf-8"), offset+n
def encode_value(value):
    if value is None: return b"\0"
    if value is False: return b"\1"
    if value is True: return b"\2"
    if isinstance(value, int) and -(1<<63) <= value < (1<<63): return b"\3" + struct.pack(">q", value)
    if isinstance(value, float): return b"\4" + struct.pack(">d", value)
    if isinstance(value, str): return b"\5" + string_encode(value)
    if isinstance(value, bytes): return b"\6" + struct.pack(">I", len(value)) + value
    raise TypeError("handler returned unsupported value type")
def decode_value(data, offset):
    if offset >= len(data): raise ValueError("truncated value")
    tag = data[offset]; offset += 1
    if tag == 0: return None, offset
    if tag == 1: return False, offset
    if tag == 2: return True, offset
    if tag == 3:
        if offset+8 > len(data): raise ValueError("truncated integer")
        return struct.unpack(">q", data[offset:offset+8])[0], offset+8
    if tag == 4:
        if offset+8 > len(data): raise ValueError("truncated double")
        return struct.unpack(">d", data[offset:offset+8])[0], offset+8
    if tag in (5, 6):
        if offset+4 > len(data): raise ValueError("truncated payload")
        n, = struct.unpack(">I", data[offset:offset+4]); offset += 4
        if offset+n > len(data): raise ValueError("truncated payload")
        raw = bytes(data[offset:offset+n]); return (raw.decode("utf-8") if tag == 5 else raw), offset+n
    raise ValueError("unknown value tag")
def load_handlers(path):
    spec = importlib.util.spec_from_file_location("hybrid_python_handlers", path)
    if spec is None or spec.loader is None: fail("cannot load handler module")
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    handlers = getattr(module, "HANDLERS", None)
    if not isinstance(handlers, dict) or not handlers: fail("HANDLERS must be a nonempty dictionary")
    if any(not isinstance(k, str) or not k or not callable(v) for k,v in handlers.items()): fail("invalid HANDLERS entry")
    return dict(sorted(handlers.items()))
def main():
    parser=argparse.ArgumentParser(); parser.add_argument("--handler-file", required=True); args=parser.parse_args()
    handlers=load_handlers(args.handler_file)
    impl=sys.implementation
    gil_enabled = getattr(sys, "_is_gil_enabled", lambda: True)()
    hello = json.dumps({"implementation":impl.name,"version":list(sys.version_info[:3]),"abiflags":getattr(sys,"abiflags", ""),"soabi":sysconfig.get_config_var("SOABI") or "","build_supports_free_threading":sysconfig.get_config_var("Py_GIL_DISABLED") == 1,"gil_enabled":gil_enabled,"handlers":list(handlers)}, separators=(",", ":")).encode()
    write_frame(HELLO, 0, hello)
    while True:
        frame=read_frame()
        if frame is None: return
        typ, request_id, body=frame
        if typ == SHUTDOWN:
            if body: fail("shutdown payload")
            write_frame(SHUTDOWN_ACK, request_id); return
        if typ != INVOKE: fail("unexpected message type")
        try:
            if len(body)<6: raise ValueError("truncated invocation")
            handler_id, argc=struct.unpack(">IH", body[:6]); offset=6
            names=list(handlers)
            if handler_id == 0 or handler_id > len(names): raise ValueError("unknown handler id")
            values=[]
            for _ in range(argc): value,offset=decode_value(body,offset); values.append(value)
            if offset != len(body): raise ValueError("trailing invocation data")
            write_frame(RESULT, request_id, encode_value(handlers[names[handler_id-1]](*values)))
        except BaseException as exc:
            write_frame(ERROR, request_id, string_encode(type(exc).__name__) + string_encode(str(exc)) + string_encode(traceback.format_exc()))
if __name__ == "__main__": main()
