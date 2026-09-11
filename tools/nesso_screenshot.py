"""Capture the Nesso N1's 135x240 screen over USB serial and save it as a PNG.

Sends the `screen` console command, reads back the base64 RGB565 framebuffer and
writes a PNG. Useful for checking the generated dashboard, or the setup QR code,
without pointing a camera at the panel.

    python3 tools/nesso_screenshot.py                 # -> nesso-screen.png
    python3 tools/nesso_screenshot.py shot.png
    python3 tools/nesso_screenshot.py --reset shot.png   # reboot first

To capture a screen that needs the board to get somewhere first, drive it with
console commands and give it time to settle:

    python3 tools/nesso_screenshot.py --reset --pre scan,connect --settle 8 dash.png

Set NESSO_PORT_DEV to pin a port; otherwise the first /dev/ttyACM* is used.
"""

import base64
import os
import struct
import sys
import time
import zlib

from nesso_serial import open_port, read_until, reset, resolve_port, send


def write_png(path: str, width: int, height: int, rgb: bytes) -> None:
    """Minimal PNG writer, so this needs nothing but the standard library."""

    def chunk(tag: bytes, payload: bytes) -> bytes:
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))

    # Each scanline is prefixed with filter type 0.
    raw = b"".join(
        b"\x00" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height)
    )
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as handle:
        handle.write(png)


def rgb565_to_rgb888(data: bytes, count: int) -> bytes:
    out = bytearray(count * 3)
    for i in range(count):
        # LovyanGFX stores 16bpp sprites in the panel's own byte order, which
        # is big-endian (swap565).
        value = (data[i * 2] << 8) | data[i * 2 + 1]
        r = (value >> 11) & 0x1F
        g = (value >> 5) & 0x3F
        b = value & 0x1F
        out[i * 3] = (r << 3) | (r >> 2)
        out[i * 3 + 1] = (g << 2) | (g >> 4)
        out[i * 3 + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out_path = args[0] if args else "nesso-screen.png"

    pre = []
    settle = 4.0
    for i, arg in enumerate(sys.argv):
        if arg == "--pre" and i + 1 < len(sys.argv):
            pre = [c for c in sys.argv[i + 1].split(",") if c]
        if arg == "--settle" and i + 1 < len(sys.argv):
            settle = float(sys.argv[i + 1])
    # --pre/--settle take a value, so drop those values from the positional args.
    for taken in ("--pre", "--settle"):
        if taken in sys.argv:
            value = sys.argv[sys.argv.index(taken) + 1]
            if value in args:
                args.remove(value)
    out_path = args[0] if args else out_path

    port = resolve_port()
    fd = open_port(port)
    try:
        if "--reset" in sys.argv:
            reset(fd)
            read_until(fd, "[nesso] companion", 8)
            time.sleep(2)
        for command in pre:
            send(fd, command)
            read_for_settle = read_until(fd, "\n", 6)
            time.sleep(2)
        time.sleep(settle)  # let the board settle on a screen
        text = ""
        for attempt in range(3):
            send(fd, "screen")
            text = read_until(fd, "[screen] end", 45)
            if "[screen] begin" in text and "[screen] end" in text:
                break
            print(f"(no dump on attempt {attempt + 1}, retrying)")
            time.sleep(1)
    finally:
        os.close(fd)

    if "[screen] begin" not in text or "[screen] end" not in text:
        print("no framebuffer dump in the reply:\n" + text[-500:])
        sys.exit(1)

    header = text.split("[screen] begin", 1)[1].split("\n", 1)[0].split()
    width, height = int(header[0]), int(header[1])
    payload = text.split("\n", 1)[0]  # placeholder, replaced below

    body = text.split("[screen] begin", 1)[1].split("\n", 1)[1]
    body = body.split("[screen] end", 1)[0]
    payload = "".join(line.strip() for line in body.splitlines() if line.strip())

    data = base64.b64decode(payload)
    expected = width * height * 2
    if len(data) < expected:
        print(f"short dump: got {len(data)} bytes, expected {expected}")
        sys.exit(1)

    write_png(out_path, width, height, rgb565_to_rgb888(data, width * height))
    print(f"wrote {out_path} ({width}x{height})")


main()
