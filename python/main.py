"""Nesso N1 Companion - Phase 1.

Registers every callback the companion brick supports and mirrors whatever the
Nesso N1 sends onto a Web UI. Nothing here drives the Nesso beyond the screen
widgets it is asked to draw: Phase 1 is about seeing the link work end to end.

What gets asked of the Nesso during the handshake:
  - the three accelerometer axes, the three gyroscope axes, pitch and roll
  - battery level and voltage, slowly, since they barely move
  - both buttons, with every action
  - Knob, Buttons, Movement and Distance Modulino events, if one is plugged in
  - a Modulino Pixels as an output, driven as a proximity bar from Distance
  - a handful of screen widgets, read-only and interactive
"""

import colorsys
import time

from arduino.app_bricks.web_ui import WebUI
from arduino.app_utils import App, Logger

from nesso_companion import ACCELEROMETER, GYROSCOPE, ORIENTATION, NessoCompanion

logger = Logger("Phase1")

# How often the Web UI gets a fresh snapshot. The Nesso streams faster than a
# browser needs to repaint, so values are collected in the brick and pushed from
# the user loop rather than on every single frame.
UI_REFRESH_HZ = 10

ui = WebUI()
companion = NessoCompanion()

# Rolling log of discrete things that happened, newest first. A browser asks for
# it once on load and then receives each new entry as it arrives.
EVENT_LOG_SIZE = 60
events: list[dict] = []
browsers: set[str] = set()


def log_event(source: str, name: str, detail: str) -> None:
    """Record a discrete event and push it to every open browser tab."""
    entry = {"ts": time.time(), "source": source, "name": name, "detail": detail}
    events.insert(0, entry)
    del events[EVENT_LOG_SIZE:]
    ui.send_message("event", entry)


# --- Sensors ---------------------------------------------------------------
# Registering a name is what makes the Nesso turn that sensor on, so this list
# is exactly the power budget the App is asking the Nesso to spend.


def track(value: float, name: str) -> None:
    """Handler for every streamed value.

    The brick already keeps the latest reading in companion.values and the user
    loop ships that snapshot to the browser, so there is nothing to do per
    sample here. A real App would put its logic in a handler like this one.
    """


for axis in (*ACCELEROMETER, *GYROSCOPE):
    companion.on(axis, track, rate_hz=20)

for angle in ORIENTATION:
    companion.on(angle, track, rate_hz=10)

for reading in ("batteryLevel", "batteryVoltage"):
    companion.on(reading, track, rate_hz=1)


# --- Buttons ---------------------------------------------------------------


@companion.on("button_1")
def on_button_1(action: str) -> None:
    logger.info(f"button_1 {action}")
    log_event("button", "button_1", action)


@companion.on("button_2")
def on_button_2(action: str) -> None:
    logger.info(f"button_2 {action}")
    log_event("button", "button_2", action)


# --- Modulinos on the Nesso ------------------------------------------------
# These fire only when the matching Modulino is plugged into the Nesso's Qwiic
# connector; otherwise the Nesso reports it absent and stays quiet.


@companion.on_modulino("knob/press")
def on_knob_press(value) -> None:
    log_event("modulino", "knob/press", "pressed")


@companion.on_modulino("knob/rotate")
def on_knob_rotate(value) -> None:
    log_event("modulino", "knob/rotate", str(value))


@companion.on_modulino("buttons")
def on_modulino_buttons(value, event) -> None:
    log_event("modulino", f"buttons/{event}", "pressed" if value else "released")


# Movement and Distance stream continuously, so they go to a live readout rather
# than into the event log, which is for discrete things that happened.
modulino_live: dict[str, float | None] = {}


@companion.on_modulino("movement")
def on_modulino_movement(value, event) -> None:
    modulino_live[f"movement/{event}"] = value


# --- Modulino Pixels -------------------------------------------------------
# The Pixels module can be driven two ways: automatically, as a proximity bar
# fed by the Distance module, or by hand from the Nesso's own screen and from
# the Web UI. Both write through the same render_pixels().

PIXELS_MAX_MM = 400.0

# level is a percentage: the LEDs are uncomfortably bright at full output.
pixels_state = {"auto": True, "hue": 200, "count": 8, "level": 25}
_last_bar: list | None = None


def hue_rgb(hue: float, level: float) -> tuple[int, int, int]:
    """A fully saturated colour for a hue in degrees, dimmed to `level` percent."""
    value = max(0.0, min(100.0, float(level))) / 100.0
    r, g, b = colorsys.hsv_to_rgb((float(hue) % 360) / 360.0, 1.0, value)
    return int(r * 255), int(g * 255), int(b * 255)


def dim(rgb: tuple[int, int, int], level: float) -> tuple[int, int, int]:
    """Scales a colour down to `level` percent."""
    factor = max(0.0, min(100.0, float(level))) / 100.0
    return tuple(int(channel * factor) for channel in rgb)


def proximity_bar(mm: float | None, level: float) -> list[tuple[int, int, int]]:
    """Eight RGB triples for a distance, dark when nothing is in range."""
    if mm is None:
        return [(0, 0, 0)] * 8
    nearness = 1.0 - min(max(mm, 0.0), PIXELS_MAX_MM) / PIXELS_MAX_MM
    lit = int(round(nearness * 8))
    bar = []
    for i in range(8):
        if i < lit:
            shade = i / 7
            bar.append(dim((int(255 * shade), int(255 * (1 - shade)), 0), level))
        else:
            bar.append((0, 0, 0))
    return bar


def render_pixels(force: bool = False) -> None:
    """Push the current pattern, skipping frames that would change nothing."""
    global _last_bar

    level = pixels_state["level"]
    if pixels_state["auto"]:
        bar = proximity_bar(modulino_live.get("distance"), level)
    else:
        colour = hue_rgb(pixels_state["hue"], level)
        lit = int(pixels_state["count"])
        bar = [colour] * lit + [(0, 0, 0)] * (8 - lit)

    if force or bar != _last_bar:
        _last_bar = bar
        companion.send_modulino("pixels", bar)


def set_pixels_mode(auto: bool) -> None:
    """Switches mode and keeps the Nesso's own switch widget in step."""
    if pixels_state["auto"] == auto:
        return
    pixels_state["auto"] = auto
    companion.send_value("pixels_auto", auto)


def broadcast_pixels_state() -> None:
    ui.send_message("pixels", dict(pixels_state))


def sync_pixels_widgets() -> None:
    """Pushes the whole Pixels state onto the Nesso's own screen widgets.

    Needed when the change came from the Web UI: without it the Nesso would keep
    showing the previous hue and count while the LEDs did something else.
    """
    companion.send_values(
        {
            "pixels_auto": pixels_state["auto"],
            "pixels_hue": pixels_state["hue"],
            "pixels_count": pixels_state["count"],
            "pixels_level": pixels_state["level"],
        }
    )


@companion.on_modulino("distance")
def on_modulino_distance(value, event) -> None:
    modulino_live["distance"] = value
    if pixels_state["auto"]:
        render_pixels()


# Declaring the output up front is what gets a Modulino Pixels brought up during
# the handshake, rather than only after the first send.
companion.send_modulino("pixels", [(0, 0, 0)] * 8)


# --- Screen widgets on the Nesso ------------------------------------------
# Read-only fields the board pushes values into...

companion.add_value("board", "read-only", label="Board")
companion.add_value("uptime", "read-only", label="Uptime")
companion.add_value("browsers", "read-only", label="Web UI")

# ...and interactive ones the Nesso draws and reports back.


@companion.on_value("ping", "button", label="Ping board")
def on_ping(value) -> None:
    log_event("widget", "ping", "tapped")
    companion.send_value("board", "pong")


@companion.on_value("stream", "switch", label="Streaming", value=True)
def on_stream(value) -> None:
    log_event("widget", "stream", "on" if value else "off")


@companion.on_value("brightness", "slider", label="Brightness", min=0, max=100, step=5, value=70)
def on_brightness(value) -> None:
    log_event("widget", "brightness", str(value))


@companion.on_value("slide", "stepper", label="Slide", min=1, max=10, step=1, value=1)
def on_slide(value) -> None:
    log_event("widget", "slide", str(value))


# --- Driving the Modulino Pixels from the Nesso screen ---------------------
# Touching a manual control implies manual mode, and the switch on the Nesso is
# pushed back so the screen never disagrees with what the LEDs are doing.


@companion.on_value("pixels_auto", "switch", label="LED auto", value=True)
def on_pixels_auto(value) -> None:
    pixels_state["auto"] = bool(value)
    log_event("widget", "pixels_auto", "auto (distance)" if value else "manual")
    render_pixels(force=True)
    broadcast_pixels_state()


@companion.on_value("pixels_hue", "slider", label="LED hue", min=0, max=345, step=15, value=200)
def on_pixels_hue(value) -> None:
    pixels_state["hue"] = float(value)
    set_pixels_mode(False)
    log_event("widget", "pixels_hue", f"{int(value)} deg")
    render_pixels(force=True)
    broadcast_pixels_state()


@companion.on_value("pixels_count", "stepper", label="LEDs lit", min=0, max=8, step=1, value=8)
def on_pixels_count(value) -> None:
    pixels_state["count"] = int(value)
    set_pixels_mode(False)
    log_event("widget", "pixels_count", str(int(value)))
    render_pixels(force=True)
    broadcast_pixels_state()


# Brightness applies in both modes, so it does not force manual.
@companion.on_value("pixels_level", "slider", label="LED level", min=0, max=100, step=5, value=25)
def on_pixels_level(value) -> None:
    pixels_state["level"] = float(value)
    log_event("widget", "pixels_level", f"{int(value)}%")
    render_pixels(force=True)
    broadcast_pixels_state()


# --- Link lifecycle --------------------------------------------------------


def link_state() -> dict:
    """Everything a browser needs to describe the link, in one dict."""
    state = companion.describe()
    state["browsers"] = len(browsers)
    state["values"] = companion.values
    state["modulino_live"] = dict(modulino_live)
    state["pixels"] = dict(pixels_state)
    return state


@companion.on_connect
def on_nesso_connected(device: dict) -> None:
    logger.info(f"Nesso connected: {device.name} at {device.address}")
    log_event("link", "connected", f"{device.name} @ {device.address}")
    companion.send_value("board", companion.name)
    companion.send_value("browsers", len(browsers))
    ui.send_message("link", link_state())


@companion.on_disconnect
def on_nesso_disconnected(device: dict) -> None:
    log_event("link", "disconnected", device.name)
    ui.send_message("link", link_state())


# --- Web UI ----------------------------------------------------------------


def on_browser_connect(sid: str) -> None:
    browsers.add(sid)
    companion.send_value("browsers", len(browsers))
    ui.send_message("link", link_state())
    ui.send_message("backlog", {"events": events})
    broadcast_pixels_state()


def on_browser_disconnect(sid: str) -> None:
    browsers.discard(sid)
    companion.send_value("browsers", len(browsers))


def pixels_self_test() -> dict:
    """Sweeps the Modulino Pixels so the output path can be checked by eye."""
    if not companion.modulinos.get("pixels"):
        return {"ok": False, "reason": "no Modulino Pixels reported by the Nesso"}
    for lit in range(9):
        companion.send_modulino("pixels", [(0, 80, 160)] * lit + [(0, 0, 0)] * (8 - lit))
        time.sleep(0.12)
    render_pixels(force=True)
    return {"ok": True, "swept": 8}


ui.expose_api("GET", "/api/pixels/test", pixels_self_test)
ui.expose_api("GET", "/api/state", link_state)
ui.expose_api("GET", "/api/events", lambda: {"events": events})
def on_pixels_message(sid: str, data) -> None:
    """Pixels controls coming from the Web UI."""
    if not isinstance(data, dict):
        return
    if "hue" in data:
        pixels_state["hue"] = float(data["hue"])
    if "count" in data:
        pixels_state["count"] = max(0, min(8, int(data["count"])))
    if "level" in data:
        pixels_state["level"] = max(0.0, min(100.0, float(data["level"])))
    # An explicit auto flag wins; otherwise touching a colour implies manual.
    if "auto" in data:
        pixels_state["auto"] = bool(data["auto"])
    elif "hue" in data or "count" in data:
        pixels_state["auto"] = False
    render_pixels(force=True)
    sync_pixels_widgets()
    broadcast_pixels_state()


ui.on_message("pixels", on_pixels_message)
ui.on_connect(on_browser_connect)
ui.on_disconnect(on_browser_disconnect)


started_at = time.monotonic()
_last_uptime_push = 0.0


def loop() -> None:
    """Push a snapshot to the browser, and the uptime to the Nesso."""
    global _last_uptime_push

    ui.send_message(
        "values",
        {
            "values": companion.values,
            "widgets": companion.widget_values,
            "modulinos": companion.modulinos,
            "modulino_live": dict(modulino_live),
            "connected": companion.connected,
        },
    )

    now = time.monotonic()
    if companion.connected and now - _last_uptime_push >= 1.0:
        _last_uptime_push = now
        companion.send_value("uptime", f"{int(now - started_at)} s")

    time.sleep(1 / UI_REFRESH_HZ)


logger.info(f"Waiting for a Nesso N1 on {companion.url}")

App.run(user_loop=loop)
