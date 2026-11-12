# Nesso N1 Companion

An Arduino App for the **UNO Q** that pairs an **Arduino Nesso N1** with the
board as a companion device, and mirrors everything it streams on a live Web UI:
IMU, battery, buttons, Modulino events and the Nesso's own screen widgets.

![The Web UI, with a real Nesso N1 streaming](assets/img/webui.png)

The Nesso side is driven by the `nesso_companion` Custom Brick in
[`bricks/nesso_companion`](bricks/nesso_companion): its README documents the
API, how to flash the firmware it carries, and the opt-in autoflash.

## Run it

1. Import this App in App Lab.
2. Flash the Nesso N1 once, by hand or with autoflash: see the brick README.
3. Start the App. The Web UI opens on port 7000; the Nesso connects on 8077.
4. On first boot the Nesso shows a QR code that joins its `Nesso-N1-Setup`
   access point. Pick the Wi-Fi network the UNO Q is on, then pick the board
   from the list: boards running a companion App carry a green dot.

The Nesso discovers the board over mDNS, so both must be on the same network
segment.

## What it asks of the Nesso

| | |
| --- | --- |
| Sensors | accelerometer and gyroscope at 20 Hz, pitch and roll at 10 Hz, battery at 1 Hz |
| Buttons | `button_1` and `button_2`, every action |
| Modulinos | Knob, Buttons, Movement and Distance events, when plugged into the Qwiic connector |
| Modulino output | a Modulino Pixels, as a proximity bar from Distance or set by hand |
| Screen widgets | `board`, `uptime`, `browsers`, `ping`, `stream`, `brightness`, `slide`, and three LED controls |

`GET /api/state` returns the whole link state, and `GET /api/pixels/test` sweeps
the Modulino Pixels to check the output path end to end.
