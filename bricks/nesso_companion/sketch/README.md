# Nesso N1 companion firmware

The sketch that runs on the **Arduino Nesso N1** and talks to the
`nesso_companion` brick. It is a separate Arduino sketch for the Nesso's
ESP32-C6, not an App's MCU sketch, so it travels inside the brick rather than in
an App's own `sketch/` folder, where App Lab would try to build it for the UNO
Q's Zephyr MCU.

You only need these sources to change the firmware. To put the shipped one on a
Nesso, flash the prebuilt images in [`../firmware`](../firmware), by hand or
with the brick's autoflash: see the brick README.

```
sketch/NessoCompanion/
├── NessoCompanion.ino   # state machine: provision -> connect -> discover -> pick -> link
├── config.h             # ports, timings, colours, touch orientation flags
├── state.h/.cpp         # the handshake result: subscriptions and widgets
├── hw.h/.cpp            # screen, touch, KEY1/KEY2, BMI270, battery, Modulinos
├── net.h/.cpp           # Wi-Fi provisioning (captive portal + NVS), mDNS discovery
├── link.h/.cpp          # WebSocket client and the wire protocol
└── ui.h/.cpp            # 135x240 screens: picker and generated dashboard
```

## Libraries

Four, none of them bundled. Install them before the first compile:

| Library | Author | Why |
| --- | --- | --- |
| `Arduino_Nesso_N1` | Arduino | board support: screen, touch, keys, IMU, battery. Pulls in `M5GFX` and `Arduino_BMI270_BMM150` on its own. |
| `WebSockets` | **Markus Sattler** | the link to the App — this is the one that provides `WebSocketsClient.h` |
| `ArduinoJson` | Benoit Blanchon | the wire protocol |
| `Arduino_Modulino` | Arduino | Qwiic modules |

```bash
arduino-cli lib install Arduino_Nesso_N1 WebSockets ArduinoJson Arduino_Modulino
```

> ### `WebSocketsClient.h: No such file or directory`
>
> You have the wrong WebSockets library. Searching "websockets" in the Library
> Manager returns several, and two of them look identical at a glance:
>
> - **`WebSockets` by Markus Sattler** — <https://github.com/Links2004/arduinoWebSockets> — provides `WebSocketsClient.h`. **This is the one.**
> - `ArduinoWebsockets` by Gil Maimon — provides `ArduinoWebsockets.h` instead. Different API, will not build this sketch.
>
> In the IDE, read the **author** line before you click Install. From the command
> line `arduino-cli lib install WebSockets` resolves to the right one, because
> that is the exact index name.
>
> Tested against **WebSockets 2.7.2**. Use a 2.x release: 1.x is the AVR branch.

## What it does

1. **Wi-Fi.** Credentials come from NVS. With none stored, the Nesso opens a
   `Nesso-N1-Setup` access point with a captive portal and shows a QR code on
   screen; pointing a phone camera at it joins that access point directly, and
   the captive portal then pops up on its own. The portal also sets the
   companion port, for Apps that move the brick off 8077.

   The QR encodes a standard `WIFI:` URI (`WIFI:T:nopass;S:Nesso-N1-Setup;;`).
   At 32 bytes that is a 25x25 version-2 code, drawn 4px per module with a
   4-module quiet zone inside a 133x133 white card — comfortable on a 135x240
   panel, with the SSID and the portal URL still legible underneath.
2. **Discovery.** It browses mDNS for `_arduino._tcp` — advertised by every
   Q-family board — then makes an HTTP `GET /info` on the companion port of each
   one. Boards running a companion App get a green dot and their App name in the
   picker.
3. **Handshake.** It sends `hello` with its capabilities; the board replies with
   `config`. Only then does it start the IMU, hook the keys, bring up Modulinos
   and build the dashboard.
4. **Run.** Each subscribed value has its own schedule; everything that comes
   due in a tick goes out in a single `data` frame.

## Serial console

With the Nesso on a USB cable, provisioning does not need a phone. At 115200
baud:

| Command | Effect |
| --- | --- |
| `wifi <ssid> <password>` | Store credentials and reconnect. Everything after the SSID is the password, so passwords may contain spaces. |
| `port <n>` | Change the companion port, leaving the credentials alone. |
| `scan` | Re-run mDNS discovery. |
| `connect [n]` | Link to board `n` from the last scan, or the highlighted one. The same step as tapping it on screen, for scripting. |
| `status` | State, SSID, IP, port, boards found, link and handshake state. |
| `portal` | Re-open Wi-Fi setup **keeping** the stored network, to move to another one. |
| `forget` | Wipe the stored network and reopen setup. |
| `i2c` | Scan the shared I2C bus and name the devices it recognises. |
| `screen` | Dump the framebuffer as base64 RGB565 between two markers. |
| `tap <x> <y>` | Test aid: inject a tap, through the handlers the touch controller feeds. |
| `scroll <px>` | Test aid: scroll the current list by a pixel amount. |
| `idle <s>` | Blank the screen after this many seconds of no input; `0` never. |
| `help` | The list above. |

Any serial monitor works, for example
`arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200`.

The ESP32-C6 talks over native USB, so the port re-enumerates on every reset and
can move between `/dev/ttyACM0` and `/dev/ttyACM1`. If the console goes quiet,
reset the Nesso: the USB CDC endpoint stops emitting after a host detaches
without re-asserting DTR.

## Modulinos

A Modulino goes into the Nesso's Qwiic connector, and only the kinds the App
subscribed to are ever brought up. The firmware knows four:

| Kind | 7-bit address | Reports |
| --- | --- | --- |
| `knob` | 0x3A / 0x3B | `rotate` (absolute position), `press`, `release` |
| `buttons` | 0x3E | `a`, `b`, `c`, each 1 on press and 0 on release |
| `movement` | 0x6A | `x`, `y`, `z` acceleration |
| `distance` | 0x29 | `mm`, and `null` when nothing is in range |
| `pixels` | 0x36 | output only - driven with `send_modulino("pixels", ...)` |

> **Those addresses are halved on purpose.** The Modulino library's `match[]`
> tables hold **8-bit** write addresses and it divides them by two before
> touching the bus (`scan()` calls `beginTransmission(addr / 2)`), so a Pixels
> listed there as `0x6C` actually answers at **0x36**. Probing the `match[]`
> value directly finds nothing. Movement and Distance are ordinary sensors
> (LSM6DSOX, VL53L4CD) addressed directly, with no halving.

**Hot-plug works.** Subscribed modules that were missing at handshake time are
re-probed every `MODULINO_RESCAN_MS` (3 s), so plugging one in mid-session picks
it up without re-linking:

```
[modulino] knob: absent (will keep looking)
[modulino] knob: plugged in
```

`status` prints what is currently detected, and `i2c` shows the raw bus.

The Nesso reports what it found back to the board in a `status` frame, so the
App can tell "not subscribed" from "subscribed but not plugged in":

```json
{"t":"status","modulinos":[{"kind":"distance","present":true}]}
```

Events are filtered rather than streamed raw. A knob reports only when the
position or the button changes; buttons report only the one that changed, not
all three; and a Movement module — whose accelerometer always has a fresh
reading — reports an axis only when it moved by `MOVEMENT_DELTA_G` (0.02 g) and
at most every `MOVEMENT_MIN_INTERVAL_MS` (100 ms). Without that last filter a
single Movement module would put 75 frames a second on the link on its own.
Distance is filtered the same way (`DISTANCE_DELTA_MM`, 8 mm) and sends one
`null` when the target leaves range, rather than simply going quiet.

## The shared I2C bus

The Qwiic connector, the BMI270 IMU (0x68), the BQ27220 fuel gauge (0x55), the
FT6336U touch controller (0x38) and the two PI4IOE5V6408 expanders (0x43, 0x44)
all sit on the **same** I2C bus.

That matters when a Modulino is subscribed but not plugged in. The Modulino
library's own discovery leaves the bus wedged when a module is absent, and the
next reads of the IMU and the fuel gauge then come back as `0xFFFF` — a flat
accelerometer and a 65.535 V battery. So `hw::beginModulinos()` probes the known
Modulino addresses itself first and only lets the Modulino library touch the bus
for modules that actually answered, then restores the bus (clocking out any
slave still holding SDA low, issuing a STOP and re-initialising the controller).

Use the `i2c` console command when something on the bus misbehaves:

```
[i2c] scanning 0x08..0x77
[i2c]   0x38 FT6336U touch
[i2c]   0x43 PI4IOE5V6408 expander 1
[i2c]   0x44 PI4IOE5V6408 expander 2
[i2c]   0x49 AW32001 charger
[i2c]   0x55 BQ27220 gauge
[i2c]   0x68 BMI270 IMU (primary)
```

## The IMU macro collision

`Arduino_Nesso_N1.h` defines `IMU` as the on-board BMI270, but
`Arduino_Modulino` pulls in `Arduino_LSM6DSOX`, which redefines `IMU` as *its*
instance. Whoever is included last wins. `hw.cpp` therefore `#undef`s the macro
and calls the on-board sensor by its real name, `myIMU`. If you add a library
that touches an IMU, check this still holds — the symptom is an IMU that reports
"not found" while `i2c` clearly shows 0x68 answering.

## Controls

| Gesture | Effect |
| --- | --- |
| Tap | Picks a board; presses buttons, toggles switches, sets sliders and steppers |
| Drag up/down | Scrolls the board list and the dashboard, following the finger |
| Drag sideways on a slider | Changes its value continuously |
| KEY1 / KEY2 in the picker | Next board / select |
| **Hold KEY2 in the picker** | **Re-open Wi-Fi setup, keeping the stored network** |
| Either key while "Linking" | Cancels a wrong pick and goes back to the list |
| KEY1 / KEY2 on the dashboard | Sent to the App when it subscribed to them; otherwise move the selection / activate it |
| Hold both keys 1.5 s | Drop the link, back to the board picker |
| Hold both keys while booting | Forget the Wi-Fi credentials entirely |

When the App registers `button_1` / `button_2`, those keys belong to the App —
that is what makes the Nesso usable as a remote control — and the dashboard is
then driven by touch alone. That is why dragging has to scroll properly: with
both keys claimed it is the only way to reach a widget below the fold.

### Picking the wrong board

Nothing gets stuck. The list puts boards that answered the companion probe
first, with a green dot, and dims the rest. If you pick one anyway:

- either key, or a tap, cancels while it is still showing "Linking";
- after `LINK_TIMEOUT_MS` (12 s) without a handshake it gives up on its own,
  says "that board has no companion running", and returns to the list;
- once linked, if the board stops answering for `RELINK_TIMEOUT_MS` (30 s) it
  goes back and rescans.

### Screen idle

The backlight is the largest single load on the battery, and `LCD_BACKLIGHT` is
an expander pin — on or off, with no PWM to dim it. So the screen simply blanks
after a period with no touch and no key, and the redraw is skipped entirely
while it is dark, which saves the full-frame SPI push as well.

**The App decides how long**, like everything else: `screen_idle_s` rides along
in the handshake, from the `NESSO_SCREEN_IDLE_S` brick variable or the
`screen_idle_s` constructor argument, defaulting to 60 s. Zero keeps the screen
lit. `SCREEN_IDLE_MS` in `config.h` is only the fallback for a Nesso that has
not linked yet, and the `idle` console command overrides locally until the next
handshake.

**Never during Wi-Fi setup.** That screen carries the SSID and the QR code, and
the way to wake it again would be to touch a screen you can no longer read, so
the idle timer is held off in that state entirely.

The two kinds of input are treated differently on purpose:

- **A key wakes the screen and still does its job.** This is a remote first; a
  press that only turned the backlight on would lose you a slide.
- **A touch on a dark screen only wakes it.** Reaching for a device you cannot
  see should not press whatever happens to be under your finger.

`idle <seconds>` on the console changes it, and `idle 0` disables it.

### Touch targets

The panel is 135x240, which is small enough that "compact" quickly becomes
"unusable". Rows are 46px, and every control is drawn exactly where its hit zone
is: a stepper's `-` and `+` are the left and right thirds of the row, a slider
takes the whole row width and snaps to its step, a switch and a button take the
whole card. Verify a change with the `tap` console command, which reports which
widget a coordinate resolves to.

## Build and flash

The board package is `esp32:esp32` (the Nesso N1 is an ESP32-C6, RISC-V).

```bash
arduino-cli core install esp32:esp32
# libraries: see the table at the top, and read that warning before installing
arduino-cli lib install Arduino_Nesso_N1 WebSockets ArduinoJson Arduino_Modulino

arduino-cli compile -b esp32:esp32:arduino_nesso_n1 NessoCompanion
arduino-cli upload  -b esp32:esp32:arduino_nesso_n1 -p /dev/ttyACM0 NessoCompanion
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200
```

To ship a new build with the brick, export the compiled binaries
(`arduino-cli compile --export-binaries ...`), copy the bootloader, partitions,
`boot_app0` and application images into [`../firmware`](../firmware) under the
names used there, and write the new `NESSO_FIRMWARE_VERSION` from `config.h`
into `firmware/version.txt`. If the two disagree, autoflash updates forever or
never.

## Tuning

Everything adjustable is in `config.h`.

**Touch orientation.** If a tap lands somewhere other than where you pressed,
flip `TOUCH_SWAP_XY`, `TOUCH_INVERT_X` or `TOUCH_INVERT_Y`. The firmware prints
one line per press over serial to make this quick:

```
[touch] raw=64,180 -> screen=64,180
```

**Screen orientation.** `SCREEN_ROTATION 0` gives the 135x240 portrait the
dashboard is laid out for. Rotating it needs the row geometry in `ui.cpp`
adjusted to match.

**Protocol version.** `NESSO_PROTOCOL_VERSION` must match `PROTOCOL_VERSION` in
`protocol.py` in the brick root. A mismatch is refused at the handshake
rather than misbehaving later.

## Performance

`status` reports where the loop time goes, which is how the numbers below were
found rather than guessed:

```
[status] loop avg 80 us, worst 25674 us, over 50099 iterations
[status] spent: ws 261ms  stream 422ms  modulino 310ms  draw 686ms  i2c-poll 1528ms
```

Four things were making the UI feel sticky, in order of how much they cost:

1. **Serial blocked the loop.** The Nesso is normally plugged into the board's
   USB, so the CDC endpoint is enumerated but nothing reads it. With the default
   100 ms TX timeout, every `Serial.print` blocks for 100 ms once the ring
   buffer fills - and `pollTouch()` logs on every press, so each touch began
   with a 100 ms stall. `Serial.setTxTimeoutMs(0)` drops output instead.
2. **The I2C bus ran at 100 kHz.** The Nesso library never sets a clock, so the
   bus sat at the ESP32 default, and `Modulino.begin()` explicitly sets 100 kHz
   too. The datasheet allows 400 kHz, and the keys, touch controller, IMU and
   fuel gauge are all polled continuously. See `I2C_CLOCK_HZ`.
3. **The socket was polled every iteration**, thousands of times a second at
   ~50 us each, for a link carrying tens of frames per second. `WS_POLL_MS`
   gates it to every 2 ms; that alone was 30% of the loop.
4. **A dozen Arduino Strings were built per iteration** in `streamDueSensors()`,
   which now passes pointers.

Together: **450 us average loop down to 80 us**, and 8,900 iterations per four
seconds up to 50,000. The 26 ms worst case is the full-frame sprite push at
`SCREEN_REFRESH_MS`, which is inherent to pushing 135x240x16bpp over SPI.

What turned out **not** to matter, despite looking suspicious: the Modulino
hot-plug rescan costs **3 us**, because probing an address that does not answer
gets a fast NAK rather than a timeout.

## Footprint

```
Sketch  1456126 bytes (46% of 3145728)
Globals   52848 bytes (16% of 327680), 274832 free for locals
```

The dashboard renders through a full-screen 135x240x16bpp sprite (~64 KB, heap
allocated at boot) so it never flickers. If that allocation ever fails the UI
falls back to drawing straight to the panel, and says so over serial.
