# Nesso N1 Companion — Custom Brick

Pairs an **Arduino Nesso N1** with an Arduino App as a companion device: a
second screen, a remote control and an extra set of sensors, on the other side
of a WebSocket.

The brick is a [Custom Brick](https://docs.arduino.cc/software/app-lab/bricks/custom-bricks/):
it lives inside the App that uses it, under `bricks/nesso_companion/`, and is not
published to the Bricks index. It carries everything the Nesso side needs too:
the firmware ready to flash, and the sketch it was built from.

```
nesso_companion/
├── __init__.py          # public exports
├── companion.py         # the NessoCompanion class
├── protocol.py          # wire protocol + standard names, shared with the sketch
├── provision.py         # autoflash: version probe + esptool update
├── brick_config.yaml    # id, name, port, variables
├── requirements.txt     # websockets, esptool
├── firmware/            # prebuilt Nesso N1 images + version.txt
└── sketch/              # the Nesso N1 sketch sources (see sketch/README.md)
```

## Install

1. Copy this folder into your App as `bricks/nesso_companion/` (the folder name
   must match the brick id).
2. Add it to `app.yaml`, without listing its port (see the quick start below).
3. Put the firmware on the Nesso N1: [flash it by hand](#flash-the-nesso-by-hand)
   once, or turn on [autoflash](#updating-the-nesso-from-the-app).
4. Start the App. On first boot the Nesso shows a QR code that joins its
   `Nesso-N1-Setup` access point; pick the Wi-Fi network the board is on, then
   pick the board from the list.

## The idea

The App declares what it wants. The Nesso does only that.

Registering `companion.on("accelerometerX", cb)` is not a filter applied to a
firehose — it is what makes the Nesso turn the accelerometer on in the first
place. The list of registrations is sent to the Nesso as a single `config` frame
during the handshake, and the Nesso starts exactly those sensors, hooks exactly
those buttons, brings up exactly those Modulinos, and draws exactly those
widgets. An App that never asks for the gyroscope leaves the gyroscope asleep.

## Quick start

The brick exposes port 8077 on its own, so `app.yaml` must **not** list it under
`ports:` too: since App Lab 0.14 a port declared twice stops the App from
starting (`port 8077 is declared by multiple sources`).

`app.yaml`:

```yaml
bricks:
  - nesso_companion:
      variables:
        NESSO_PORT: "8077"
        NESSO_NAME: "My App"
```

`python/main.py`:

```python
from arduino.app_utils import App
from nesso_companion import NessoCompanion

companion = NessoCompanion()

companion.on("accelerometerX", lambda v: print("ax", v))
companion.on("button_1", lambda action: print("main button", action))
companion.on_modulino("knob/press", lambda v: print("knob pressed"))

companion.add_value("slide", "read-only", label="Slide")
companion.on_value("next", "button", lambda v: print("next!"), label="Next")

companion.send_value("slide", "3 / 10")

App.run()
```

## API

### Constructor

```python
NessoCompanion(port=None, name=None, host="0.0.0.0", default_rate_hz=None)
```

| Argument | Default | Meaning |
| --- | --- | --- |
| `port` | `NESSO_PORT` env, else `8077` | TCP port of the WebSocket server. The brick exposes 8077 itself; on any other port, add it to `app.yaml` under `ports:`. |
| `name` | `NESSO_NAME` env, else the App folder | Shown on the Nesso screen and in its board picker. |
| `host` | `0.0.0.0` | Bind address. |
| `default_rate_hz` | `NESSO_DEFAULT_RATE_HZ` env, else `10` | Rate for sensors registered without `rate_hz`. |
| `screen_idle_s` | `NESSO_SCREEN_IDLE_S` env, else `60` | Seconds before the Nesso blanks its screen; `0` keeps it lit. Never applies during Wi-Fi setup. |

The class is decorated with `@brick`, so instantiating it before `App.run()` is
enough — the framework calls `start()` and `stop()` for you.

### Asking the Nesso for things

```python
companion.on(name, callback=None, *, rate_hz=None)
```

Subscribes to a **standard value** or a **button**. Usable as a decorator when
`callback` is omitted.

Standard value names:

| Group | Names | Unit |
| --- | --- | --- |
| Accelerometer | `accelerometerX`, `accelerometerY`, `accelerometerZ` | g |
| Gyroscope | `gyroscopeX`, `gyroscopeY`, `gyroscopeZ` | °/s |
| Orientation | `pitch`, `roll` | degrees, derived from gravity |
| Battery | `batteryLevel`, `batteryVoltage` | %, V |

Sensor callbacks receive `(value)`, and optionally `(value, name, timestamp_ms)`
— you take as many arguments as you want and the rest are dropped.

Button names are `button_1` (KEY1) and `button_2` (KEY2). Button callbacks
receive `(action)`, optionally `(action, name, timestamp_ms)`, where action is
one of `press`, `release`, `click`, `double_click`, `long_press`.

```python
companion.on_modulino(spec, callback=None)
```

Subscribes to a Modulino plugged into the Nesso's Qwiic connector. `spec` is
`'<kind>/<event>'` (`'knob/press'`) or just `'<kind>'` for every event of that
Modulino. Callbacks receive `(value)`, optionally `(value, event, kind)`.

Known kinds on the Nesso side: `knob` (`rotate`, `press`, `release`),
`buttons` (`a`, `b`, `c`), `movement` (`x`, `y`, `z`), `distance` (`mm`, and
`None` when nothing is in range), `pixels` (output only).

`companion.modulinos` returns `{kind: present}` for everything the App
subscribed to, as reported by the Nesso, and updates when one is plugged in
mid-session. That is how you tell "this App never asked for a Knob" apart from
"it asked, and there is no Knob attached".

### Widgets on the Nesso screen

```python
companion.add_value(name, kind="read-only", label=None, **options)
companion.on_value(name, kind, callback=None, label=None, **options)
```

`add_value` declares something the Nesso only displays. `on_value` declares
something it displays **and** lets the user change, calling back with the new
value.

| kind | Drawn as | Options | Reports |
| --- | --- | --- | --- |
| `read-only` / `value` | label + value | `unit` | — |
| `button` | full-width tappable button | — | `True` on tap |
| `switch` | toggle pill | `value` | `True` / `False` |
| `slider` | track with a filled bar | `min`, `max`, `step`, `value` | the number |
| `stepper` | `-  value  +` | `min`, `max`, `step`, `value` | the number |

### Pushing values out

```python
companion.send_value(name, value)        # one widget
companion.send_values({name: value})     # several, one frame
companion.send_modulino(kind, value)     # e.g. ("pixels", [(255,0,0)] * 8)
```

All three return `True` when the frame reached a connected Nesso. Sending to an
unregistered widget logs a warning and returns `False`.

### More than one Nesso

Several remotes can connect at once — to the **same port**, since one WebSocket
server accepts many clients. Each gets its own `Device`:

```python
companion.devices                 # everything connected, oldest first
companion.device                  # the longest-connected one, or None
companion.device_by_id("48:F6:…") # by the id it sent in its hello

device.name, device.id, device.address, device.firmware
device.values                     # this remote's readings
device.modulinos                  # what is plugged into this remote
device.send_value("slide", 3)     # to this remote alone
device.send_modulino("pixels", bar)
```

**Every callback receives the sending device as its last argument**, and
handlers that do not declare it never see it:

```python
@companion.on("button_1")
def next_slide(action, name, ts, device):
    log(f"{device.name} pressed next")

@companion.on("button_1")
def also_fine(action):        # still works, untouched
    ...
```

Two brick-level properties merge across devices, which is right for one remote
and worth knowing about for several:

| | |
| --- | --- |
| `companion.values` | merged key by key, **last writer wins** — two remotes streaming `pitch` overwrite each other, so read `device.values` instead |
| `companion.modulinos` | "is one attached **anywhere**" — for "does *that* remote have one", read `device.modulinos` |

Screen widgets are deliberately **not** per-device: they are declared by the App
and every connected Nesso draws the same ones, so a tap on one is a change for
all. The device argument tells you who did it.

### Link lifecycle and state

```python
companion.on_connect(cb)      # cb(device_info)
companion.on_disconnect(cb)   # cb(device_info)

companion.connected           # bool
companion.device              # the longest-connected Device, or None
companion.devices             # every connected Device
companion.values              # {name: last value} for every subscribed sensor
companion.widget_values       # {widget id: current value}
companion.url                 # ws://<board ip>:<port>
companion.describe()          # the whole picture as plain data, for a UI
```

Callbacks run in a worker thread, not on the brick's asyncio loop, so a handler
that blocks — a `Bridge.call`, a Web UI emit, a slow HTTP request — cannot stall
the socket.

Registering more callbacks after a Nesso has already connected is fine: the
config is versioned, and a changed registration set is re-sent on the next frame
the Nesso delivers.

## Discovery and the wire protocol

The Nesso finds the board by browsing mDNS for `_arduino._tcp`, which every
Q-family board advertises. That service points at port 80, not at this brick, so
the Nesso then makes a plain HTTP request to `http://<board>:<port>/info` on the
companion port. The brick answers without upgrading to a WebSocket:

```json
{"service":"nesso-companion","proto":1,"app":"Nesso N1 Companion","port":8077,"connected":false}
```

That is what lets the Nesso's picker mark which of the boards on the network are
actually running a companion App.

The socket itself carries one JSON object per frame, each with a `t` type key.
`protocol.py` documents every frame; the short version:

| Direction | `t` | Carries |
| --- | --- | --- |
| Nesso → board | `hello` | protocol version, device id, capabilities |
| board → Nesso | `config` | the handshake: sensors + rates, buttons, Modulinos, widgets |
| Nesso → board | `data` | a batch of `{name: value}` for everything that came due |
| Nesso → board | `event` | a button action |
| Nesso → board | `ui` | the user changed a widget on the screen |
| Nesso → board | `modulino` | a Modulino event |
| Nesso → board | `status` | which subscribed Modulinos are actually attached |
| Nesso → board | `log` | a line for the App log |
| board → Nesso | `value` | new value(s) for widget(s) |
| board → Nesso | `modulino` | data for an output Modulino |

## Why not the RemoteSensor peripheral?

`arduino.app_peripherals.remote_sensor` also hosts a WebSocket server, and it
was the obvious candidate. It does not fit:

- **It is one-way.** Its public API is `on_datapoint` / `on_status_changed`;
  there is no public `send`. This brick's whole point is the handshake plus the
  `value` and `modulino` frames going back out to the device.
- **It accepts a single client** and closes any second one with "Server busy".
- **Its framing is BPP** with optional HMAC/ChaCha20, designed for the phone
  companion app, and its payload body is left entirely undefined — so the
  handshake, the standard names and the widget model would have had to be built
  on top anyway.

What was worth borrowing is its shape, and this brick follows it: the same
`websockets` server library, the same "callbacks run off the event loop" rule,
the same lifecycle-status idea. If RemoteSensor grows a public bidirectional
API, the transport here could be swapped for it without changing the App-facing
methods.

## Updating the Nesso from the App

The brick can check an attached Nesso at start-up and update it if its firmware
differs from the one shipped in this brick's `firmware/`. It is **off unless you
ask for it**:

```yaml
bricks:
  - nesso_companion:
      variables:
        NESSO_AUTOFLASH: "true"   # default is false
```

Why it defaults to off: probing **resets whatever is on the port**, and the USB
identifiers (`303a:1001`) say "an Espressif device", not "a Nesso N1". An
unrelated ESP32 plugged into the same board would be reset and, if the update
went ahead, overwritten.

There is no protocol for the version check - the firmware prints its version on
boot, so a reset and a short read is the whole probe. Measured on this board:

| | |
| --- | --- |
| Already current | **0.75 s**, no write |
| Out of date | **21 s**, four images, hashes verified |

It runs on a background thread, so App start-up never waits for it. The Nesso
must be plugged into the board's USB-C hub. Keep `firmware/version.txt` in step
with `NESSO_FIRMWARE_VERSION` in `sketch/NessoCompanion/config.h`, or the App
will either update forever or never.

## Flash the Nesso by hand

`firmware/` holds the four images of the current build. From a computer with
the Nesso on USB, with [esptool](https://docs.espressif.com/projects/esptool/)
(`pip install esptool`):

```bash
cd firmware
esptool --chip esp32c6 --port /dev/ttyACM0 --baud 921600 write-flash \
    --flash-mode dio --flash-freq 80m --flash-size 16MB \
    0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

Replace `/dev/ttyACM0` with the Nesso's port (`COM5` on Windows,
`/dev/cu.usbmodem…` on macOS). With esptool 4.x the command is spelled
`esptool.py … write_flash`.

To change the firmware instead, build it from `sketch/` with the Arduino IDE or
`arduino-cli`: [`sketch/README.md`](sketch/README.md) lists the four libraries
it needs and how to compile and upload it.
