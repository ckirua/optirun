import sys
import sysconfig
import time


def identity():
    return f"{sys.executable}|{sys.abiflags}|gil={sys._is_gil_enabled()}"


def echo(value):
    return value


def raise_value_error():
    raise ValueError("expected handler failure")


def sleep_then_echo(value, seconds):
    time.sleep(seconds)
    return value


HANDLERS = {
    "echo": echo,
    "identity": identity,
    "raise_value_error": raise_value_error,
    "sleep_then_echo": sleep_then_echo,
}
