import sys
import time


def echo(value):
    return value


def identity():
    return f"{sys.abiflags}|{sys._is_gil_enabled()}"


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
