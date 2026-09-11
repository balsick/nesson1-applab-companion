"""A fake Nesso N1, for testing the companion brick without the hardware.

It speaks the same protocol as the sketch: connects, says hello, prints the
handshake the App sent back, then streams a little motion and fires one of each
event so every callback path gets exercised.

Run it from inside the App container, which already has `websockets`:

    docker exec nesso-companion-main-1 \\
        /app/.cache/.venv/bin/python /app/tools/fake_nesso.py

Or from any machine on the network:

    python3 tools/fake_nesso.py ws://<board-ip>:8077/nesso
"""

import asyncio
import json
import math
import sys
import time

import websockets

URL = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8077/nesso"
DURATION_S = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

HELLO = {
    "t": "hello",
    "proto": 1,
    "device": "nesso-n1",
    "id": "48:F6:EE:C4:97:F4",
    "name": "Fake Nesso",
    "fw": "sim-1.0.0",
    "caps": {
        "sensors": [
            "accelerometerX", "accelerometerY", "accelerometerZ",
            "gyroscopeX", "gyroscopeY", "gyroscopeZ",
            "pitch", "roll", "batteryLevel", "batteryVoltage",
        ],
        "buttons": ["button_1", "button_2"],
        "screen": [135, 240],
        "modulinos": ["knob", "buttons", "movement", "pixels"],
    },
}


def synth(name: str, t: float) -> float:
    """A recognisable waveform per value, so the Web UI visibly moves."""
    if name == "accelerometerX":
        return round(math.sin(t) * 0.8, 3)
    if name == "accelerometerY":
        return round(math.cos(t * 0.7) * 0.8, 3)
    if name == "accelerometerZ":
        return round(0.95 + math.sin(t * 2) * 0.05, 3)
    if name.startswith("gyroscope"):
        phase = {"X": 0.0, "Y": 2.1, "Z": 4.2}.get(name[-1], 0.0)
        return round(math.sin(t * 1.3 + phase) * 90, 2)
    if name == "pitch":
        return round(math.sin(t * 0.5) * 35, 2)
    if name == "roll":
        return round(math.cos(t * 0.4) * 35, 2)
    if name == "batteryLevel":
        return 76.0
    if name == "batteryVoltage":
        return 3.92
    return 0.0


async def reader(ws: websockets.ClientConnection, state: dict) -> None:
    """Prints everything the board pushes, and records the handshake."""
    async for raw in ws:
        frame = json.loads(raw)
        kind = frame.get("t")
        if kind == "config":
            state["config"] = frame
            print("\n--- handshake ------------------------------------------")
            print(f"app       : {frame.get('app')}  (revision {frame.get('rev')})")
            print(f"sensors   : {[s['name'] + '@' + str(s['rate_hz']) + 'Hz' for s in frame.get('sensors', [])]}")
            print(f"buttons   : {[b['name'] for b in frame.get('buttons', [])]}")
            print(f"modulinos : {[(m['kind'], m['dir']) for m in frame.get('modulinos', [])]}")
            print("widgets   :")
            for w in frame.get("ui", []):
                print(f"              {w['kind']:8s} {w['id']:12s} {w.get('label')!r} = {w.get('value')!r}")
            print("--------------------------------------------------------\n")
        else:
            print(f"<- {raw}")


async def main() -> None:
    print(f"connecting to {URL}")
    async with websockets.connect(URL) as ws:
        state: dict = {}
        await ws.send(json.dumps(HELLO))
        task = asyncio.create_task(reader(ws, state))

        # Give the board a moment to answer the hello.
        for _ in range(50):
            if "config" in state:
                break
            await asyncio.sleep(0.1)
        if "config" not in state:
            print("!! no config frame received - the handshake failed")
            return

        wanted = [s["name"] for s in state["config"].get("sensors", [])]
        # Only the interactive widgets can report a value back.
        widgets = [w for w in state["config"].get("ui", []) if w["kind"] != "value"]

        started = time.monotonic()
        next_event = 1.0
        step = 0
        while time.monotonic() - started < DURATION_S:
            t = time.monotonic() - started
            await ws.send(json.dumps({
                "t": "data",
                "ts": int(t * 1000),
                "v": {name: synth(name, t) for name in wanted},
            }))

            if t >= next_event:
                next_event += 1.5
                step += 1
                if step == 1:
                    print("-> button_1 click")
                    await ws.send(json.dumps({"t": "event", "name": "button_1", "action": "click"}))
                elif step == 2:
                    print("-> button_2 long_press")
                    await ws.send(json.dumps({"t": "event", "name": "button_2", "action": "long_press"}))
                elif step == 3:
                    print("-> modulino knob/rotate")
                    await ws.send(json.dumps({"t": "modulino", "kind": "knob", "event": "rotate", "value": 7}))
                elif step == 4:
                    print("-> modulino knob/press")
                    await ws.send(json.dumps({"t": "modulino", "kind": "knob", "event": "press", "value": None}))
                elif step <= 4 + len(widgets):
                    widget = widgets[step - 5]
                    value = {"button": True, "switch": False, "slider": 45, "stepper": 3}[widget["kind"]]
                    print(f"-> widget {widget['id']} = {value}")
                    await ws.send(json.dumps({"t": "ui", "id": widget["id"], "value": value}))

            await asyncio.sleep(0.05)

        task.cancel()
    print("\ndone")


asyncio.run(main())
