"""Shared USB-serial helpers for the Nesso N1 tools.

Deliberately uses nothing but the standard library, so the tools run on the
board itself without installing pyserial.
"""

import fcntl
import glob
import os
import select
import struct
import sys
import termios
import time

TIOCMBIS, TIOCMBIC = 0x5416, 0x5417


def resolve_port() -> str:
    """Find the Nesso's serial port.

    The ESP32-C6 talks over native USB, so the device re-enumerates on every
    reset and can move between /dev/ttyACM0 and /dev/ttyACM1. NESSO_PORT_DEV
    pins it explicitly; otherwise take the first one that exists.
    """
    pinned = os.environ.get("NESSO_PORT_DEV")
    if pinned:
        return pinned
    ports = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    if not ports:
        print("no /dev/ttyACM* or /dev/ttyUSB* found - is the Nesso plugged in?")
        sys.exit(1)
    return ports[0]


def configure(fd: int) -> None:
    """Raw 115200, no echo.

    Worth calling again before each command: ModemManager probes new ttyACM
    devices and can leave ECHO on, which makes the kernel bounce the board's own
    log back at its console.
    """
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = termios.B115200
    attrs[0] = attrs[1] = attrs[3] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def open_port(path: str) -> int:
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    configure(fd)
    return fd


def reset(fd: int) -> None:
    """The USB-Serial/JTAG reset sequence: DTR low, then pulse RTS."""
    fcntl.ioctl(fd, TIOCMBIC, struct.pack("I", termios.TIOCM_DTR))
    fcntl.ioctl(fd, TIOCMBIS, struct.pack("I", termios.TIOCM_RTS))
    time.sleep(0.1)
    fcntl.ioctl(fd, TIOCMBIC, struct.pack("I", termios.TIOCM_RTS))


def read_for(fd: int, seconds: float, echo: bool = True) -> str:
    """Reads for a fixed window, optionally printing as it goes."""
    return _read(fd, seconds, marker=None, echo=echo)


def read_until(fd: int, marker: str, timeout: float, echo: bool = False) -> str:
    """Reads until `marker` shows up, or the timeout expires."""
    return _read(fd, timeout, marker=marker, echo=echo)


def _read(fd: int, timeout: float, marker: str | None, echo: bool) -> str:
    text = ""
    deadline = time.time() + timeout
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.2)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 8192)
        except BlockingIOError:
            continue
        except OSError:
            # The port disappears mid-read when the board resets itself.
            break
        if not chunk:
            continue
        decoded = chunk.decode("utf-8", "replace")
        text += decoded
        if echo:
            sys.stdout.write(decoded)
            sys.stdout.flush()
        if marker and marker in text:
            break
    return text


def send(fd: int, command: str) -> None:
    configure(fd)
    try:
        termios.tcflush(fd, termios.TCIFLUSH)
    except OSError:
        pass
    os.write(fd, (command + "\n").encode())
