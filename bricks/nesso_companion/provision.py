"""Check what firmware an attached Nesso N1 is running, and update it if needed.

This is **off by default** and has to be asked for, because probing resets
whatever is on the port. The USB identifiers say "an Espressif device", not "a
Nesso N1", so an unrelated ESP32 plugged into the same board would be reset and,
if the update went ahead, overwritten.

The version check needs no protocol of its own: the firmware prints its version
on boot, so a reset plus a short read is the whole probe.
"""

from __future__ import annotations

import glob
import os
import re
import subprocess
import sys
import time

from arduino.app_utils import Logger

logger = Logger("NessoProvision")

# Espressif's USB serial/JTAG identifiers. Necessary but not sufficient to call
# something a Nesso, which is why this feature is opt-in.
ESPRESSIF_VID = "303a"
ESPRESSIF_PID = "1001"

BANNER_RE = re.compile(rb"\[nesso\] companion ([0-9][0-9A-Za-z._-]*)")

# Where each image goes, matching the Arduino build for this board.
FLASH_LAYOUT = (
    ("0x0", "bootloader.bin"),
    ("0x8000", "partitions.bin"),
    ("0xe000", "boot_app0.bin"),
    ("0x10000", "firmware.bin"),
)


def _usb_ids(port: str) -> tuple[str, str] | None:
    """Reads idVendor/idProduct for a tty by walking up the sysfs device tree."""
    node = os.path.realpath(f"/sys/class/tty/{os.path.basename(port)}/device")
    for _ in range(6):
        vid_path = os.path.join(node, "idVendor")
        pid_path = os.path.join(node, "idProduct")
        if os.path.isfile(vid_path) and os.path.isfile(pid_path):
            with open(vid_path) as vid, open(pid_path) as pid:
                return vid.read().strip().lower(), pid.read().strip().lower()
        parent = os.path.dirname(node)
        if parent == node:
            break
        node = parent
    return None


def find_nesso_port() -> str | None:
    """First serial port that looks like an Espressif USB device."""
    for port in sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*")):
        if _usb_ids(port) == (ESPRESSIF_VID, ESPRESSIF_PID):
            return port
    return None


def read_version(port: str, timeout: float = 8.0) -> str | None:
    """Resets the board and reads the version out of its boot banner."""
    try:
        import serial
    except ImportError:
        logger.error("pyserial is missing; add esptool to the App requirements")
        return None

    try:
        with serial.Serial(port, 115200, timeout=0.3) as ser:
            # The USB-Serial/JTAG reset sequence: DTR low, then pulse RTS.
            ser.dtr = False
            ser.rts = True
            time.sleep(0.1)
            ser.rts = False

            buffer = b""
            deadline = time.time() + timeout
            while time.time() < deadline:
                buffer += ser.read(256)
                found = BANNER_RE.search(buffer)
                if found:
                    return found.group(1).decode()
    except Exception as err:
        logger.warning(f"could not read a version from {port}: {err}")
    return None


def _image_arguments(firmware_dir: str) -> list[str] | None:
    arguments: list[str] = []
    for offset, name in FLASH_LAYOUT:
        path = os.path.join(firmware_dir, name)
        if not os.path.isfile(path):
            logger.error(f"firmware image missing: {path}")
            return None
        arguments += [offset, path]
    return arguments


def write_firmware(port: str, firmware_dir: str) -> bool:
    """Writes the four images to the board with esptool."""
    images = _image_arguments(firmware_dir)
    if images is None:
        return False

    command = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32c6",
        "--port", port,
        "--baud", "921600",
        "write-flash",
        "--flash-mode", "dio",
        "--flash-freq", "80m",
        "--flash-size", "16MB",
        *images,
    ]
    logger.info(f"updating {port} from {firmware_dir}")
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=300)
    except FileNotFoundError:
        logger.error("esptool is not installed; add it to the App requirements")
        return False
    except subprocess.TimeoutExpired:
        logger.error("esptool timed out")
        return False

    if result.returncode != 0:
        logger.error(f"esptool failed: {result.stderr.strip()[-400:]}")
        return False

    logger.info(f"written, {result.stdout.count('Hash of data verified')} image hash(es) verified")
    return True


def ensure_firmware(firmware_dir: str, expected_version: str, port: str | None = None) -> str:
    """Brings an attached Nesso up to `expected_version`.

    Returns one of: absent, current, updated, failed.
    """
    port = port or find_nesso_port()
    if port is None:
        logger.info("no Espressif USB device attached, nothing to provision")
        return "absent"

    found = read_version(port)
    if found == expected_version:
        logger.info(f"{port} already runs {found}")
        return "current"

    logger.info(
        f"{port} runs {found or 'no recognisable companion firmware'}, "
        f"expected {expected_version}"
    )
    if not write_firmware(port, firmware_dir):
        return "failed"

    # Confirm it came back up on the version just written.
    time.sleep(2.0)
    port = find_nesso_port() or port
    after = read_version(port)
    if after == expected_version:
        logger.info(f"{port} now runs {after}")
        return "updated"
    logger.warning(f"updated, but {port} reports {after or 'nothing'}")
    return "failed"
