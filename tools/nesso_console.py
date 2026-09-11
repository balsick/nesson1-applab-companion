"""Talk to the Nesso N1 firmware over USB serial.

Sends console commands and shows the replies. Handy for provisioning over the
cable instead of the captive portal:

    python3 tools/nesso_console.py "wifi MyNetwork my-password"
    python3 tools/nesso_console.py scan connect
    python3 tools/nesso_console.py status

With no arguments it tails the log. Pass --reset to reboot the board first.
Set NESSO_PORT_DEV to pin a port; otherwise the first /dev/ttyACM* is used.
"""

import os
import sys

from nesso_serial import open_port, read_for, reset, resolve_port, send


def main() -> None:
    commands = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = resolve_port()
    print(f"[{port}]")

    fd = open_port(port)
    try:
        if "--reset" in sys.argv:
            reset(fd)
            read_for(fd, 5)
        else:
            read_for(fd, 1)

        for command in commands:
            print(f"\n>>> {command}")
            send(fd, command)
            read_for(fd, 4)

        if not commands:
            print("(tailing - Ctrl+C to stop)")
            while True:
                read_for(fd, 5)
    except KeyboardInterrupt:
        pass
    finally:
        os.close(fd)


main()
