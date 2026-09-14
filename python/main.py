"""Nesso Presenter - drive a slide deck from an Arduino Nesso N1.

The Nesso is the remote: KEY1 on its face advances, KEY2 on its edge goes back,
tilting it moves a pointer, and its screen carries a jump control, the clock and
a recentre button. The browser is the projected surface, and works on its own if
the remote is not around.

Two slides are live rather than drawn: one drives a Modulino Pixels while it is
on screen, so plugging one in mid-talk is the demonstration, and the closing one
reports what this very presentation asked of the Nesso.
"""

import colorsys
import os
import re
import time

from arduino.app_bricks.web_ui import WebUI
from arduino.app_utils import App, Logger

from nesso_companion import NessoCompanion

logger = Logger("Presenter")

# The browser is repainted at this rate; the pointer has to feel immediate.
UI_REFRESH_HZ = 20

# Tilt away from centre that reaches the edge of the slide, at the extremes of
# the sensitivity control. A larger span means the pointer moves less per degree.
SPAN_DEG_MIN = 10.0
SPAN_DEG_MAX = 60.0

# Exponential smoothing on the pointer. Hands shake; slides are large.
POINTER_SMOOTHING = 0.35

# Below this the remote is close to cutting out mid-talk, which is worth saying
# loudly rather than showing as a number nobody reads.
BATTERY_LOW_V = 3.55

# The LEDs are uncomfortably bright at full output in a dark room.
PIXELS_LEVEL = 22
PIXELS_MAX_MM = 400.0

ui = WebUI()
companion = NessoCompanion()


# --- The deck --------------------------------------------------------------

IMAGE_RE = re.compile(r"^!\[(?P<alt>[^\]]*)\]\((?P<src>[^)]+)\)$")
LIVE_RE = re.compile(r"^!live\s+(?P<kind>[a-z0-9_-]+)$", re.IGNORECASE)


def parse_slide(chunk: str) -> dict | None:
    """Turns one `---` separated block into a slide.

    The whole grammar is: a heading, an image, a `!live` panel, fenced code,
    `>` for a lead line, `-` for a bullet, anything else a paragraph. Keeping it
    this small is what keeps the deck editable by hand.
    """
    title = ""
    image = None
    live = None
    blocks: list[dict] = []
    code: list[str] | None = None

    for raw_line in chunk.strip().splitlines():
        line = raw_line.rstrip()
        stripped = line.strip()

        if stripped.startswith("```"):
            if code is None:
                code = []
            else:
                blocks.append({"kind": "code", "text": "\n".join(code)})
                code = None
            continue
        if code is not None:
            code.append(line)
            continue

        if not stripped:
            continue
        if stripped.startswith("# "):
            title = stripped[2:].strip()
        elif IMAGE_RE.match(stripped):
            found = IMAGE_RE.match(stripped)
            image = {"src": found["src"], "alt": found["alt"]}
        elif LIVE_RE.match(stripped):
            live = LIVE_RE.match(stripped)["kind"].lower()
        elif stripped.startswith("> "):
            blocks.append({"kind": "lead", "text": stripped[2:].strip()})
        elif stripped.startswith("- "):
            blocks.append({"kind": "bullet", "text": stripped[2:].strip()})
        else:
            blocks.append({"kind": "text", "text": stripped})

    if code:
        blocks.append({"kind": "code", "text": "\n".join(code)})
    if not (title or blocks or image or live):
        return None
    return {"title": title, "image": image, "live": live, "blocks": blocks}


def load_deck() -> list[dict]:
    # In the container the App is mounted at /app; APP_HOME is the path on the
    # host, which only resolves when running outside Docker. Try both.
    candidates = [
        "/app/assets/slides.md",
        os.path.join(os.getenv("APP_HOME", ""), "assets", "slides.md"),
        os.path.join(os.path.dirname(__file__), "..", "assets", "slides.md"),
    ]
    path = next((c for c in candidates if c and os.path.isfile(c)), None)
    if path is None:
        logger.warning(f"no slides.md found in any of {candidates}")
        return [{"title": "No slides", "image": None, "live": None, "blocks": []}]

    with open(path, encoding="utf-8") as handle:
        raw = handle.read()

    chunks = re.split(r"^---\s*$", raw, flags=re.MULTILINE)
    slides = [s for s in (parse_slide(c) for c in chunks) if s]
    logger.info(f"deck loaded: {len(slides)} slide(s) from {path}")
    return slides or [{"title": "Empty deck", "image": None, "live": None, "blocks": []}]


deck = load_deck()

show = {"index": 0, "pointer": True, "sensitivity": 50, "running": False, "elapsed": 0.0}
stats = {"from_remote": 0, "from_browser": 0, "recentres": 0}

_timer_started_at: float | None = None
_last_clock_pushed = ""
_pointer = {"x": 0.5, "y": 0.5}
_neutral = {"pitch": 0.0, "roll": 0.0}
modulino_live: dict[str, float | None] = {}


def current_slide() -> dict:
    return deck[show["index"]]


def live_kind() -> str | None:
    return current_slide().get("live")


def slide_label() -> str:
    return f"{show['index'] + 1} / {len(deck)}"


def elapsed_seconds() -> float:
    if show["running"] and _timer_started_at is not None:
        return show["elapsed"] + (time.monotonic() - _timer_started_at)
    return show["elapsed"]


def clock_label() -> str:
    total = int(elapsed_seconds())
    return f"{total // 60}:{total % 60:02d}"


def show_state() -> dict:
    return {
        **show,
        "elapsed": elapsed_seconds(),
        "count": len(deck),
        "label": slide_label(),
        "clock": clock_label(),
        "live": live_kind(),
    }


def broadcast() -> None:
    ui.send_message("show", show_state())


def sync_widgets() -> None:
    """Keeps the Nesso's own screen in step, whoever made the change."""
    companion.send_values(
        {
            "deck": slide_label(),
            "clock": clock_label(),
            "slide": show["index"] + 1,
            "laser": show["pointer"],
            "sensitivity": show["sensitivity"],
        }
    )


def goto(index: int, *, source: str = "app") -> None:
    target = max(0, min(len(deck) - 1, int(index)))
    if target == show["index"]:
        return
    show["index"] = target
    if source in ("KEY1", "KEY2", "Nesso screen"):
        stats["from_remote"] += 1
    elif source == "browser":
        stats["from_browser"] += 1
    logger.info(f"slide {slide_label()} ({source})")
    sync_widgets()
    broadcast()


def step(delta: int, *, source: str = "app") -> None:
    goto(show["index"] + delta, source=source)


def set_timer(running: bool) -> None:
    global _timer_started_at
    if running == show["running"]:
        return
    if running:
        _timer_started_at = time.monotonic()
    else:
        show["elapsed"] = elapsed_seconds()
        _timer_started_at = None
    show["running"] = running
    broadcast()


def reset_timer() -> None:
    global _timer_started_at
    show["elapsed"] = 0.0
    _timer_started_at = time.monotonic() if show["running"] else None
    broadcast()


# --- The Nesso as a remote -------------------------------------------------
# KEY1 is the big button on the face, so it gets the action you do most. KEY2 is
# the small one on the edge, so it goes back. Recentring the pointer is a button
# on the screen rather than a hold: a hold mid-sentence is awkward, and a screen
# button can say what it does.


@companion.on("button_1")
def on_key1(action: str) -> None:
    if action == "click":
        step(+1, source="KEY1")


@companion.on("button_2")
def on_key2(action: str) -> None:
    if action == "click":
        step(-1, source="KEY2")


# --- Pointer ---------------------------------------------------------------
# pitch and roll rather than raw acceleration: they are already gravity
# referenced, so they hold still when the hand does.

companion.on("pitch", rate_hz=25)(lambda value: None)
companion.on("roll", rate_hz=25)(lambda value: None)
companion.on("batteryVoltage", rate_hz=1)(lambda value: None)
companion.on("batteryLevel", rate_hz=1)(lambda value: None)


def recentre() -> None:
    """Takes the current attitude as the new centre of the slide."""
    values = companion.values
    _neutral["pitch"] = values.get("pitch") or 0.0
    _neutral["roll"] = values.get("roll") or 0.0
    _pointer["x"] = _pointer["y"] = 0.5
    stats["recentres"] += 1
    logger.info(f"pointer recentred at pitch {_neutral['pitch']:.1f} roll {_neutral['roll']:.1f}")


def update_pointer() -> None:
    values = companion.values
    pitch, roll = values.get("pitch"), values.get("roll")
    if pitch is None or roll is None:
        return

    # Sensitivity 0..100 maps onto the tilt that reaches the edge, inverted:
    # more sensitivity means a smaller tilt gets you there.
    span = SPAN_DEG_MAX - (show["sensitivity"] / 100.0) * (SPAN_DEG_MAX - SPAN_DEG_MIN)

    target_x = 0.5 + (roll - _neutral["roll"]) / span
    target_y = 0.5 - (pitch - _neutral["pitch"]) / span

    for axis, target in (("x", target_x), ("y", target_y)):
        clamped = max(0.0, min(1.0, target))
        _pointer[axis] += (clamped - _pointer[axis]) * POINTER_SMOOTHING


# --- The live Modulino slide -----------------------------------------------
# Registered so the Nesso brings them up, but driven only while that slide is on
# screen, which is the point being made.


@companion.on_modulino("distance")
def on_distance(value, event) -> None:
    modulino_live["distance"] = value


companion.send_modulino("pixels", [(0, 0, 0)] * 8)

_phase = 0.0
_pixels_bar: list[list[int]] = [[0, 0, 0]] * 8
_pixels_driving = False


def dim(rgb) -> list[int]:
    factor = PIXELS_LEVEL / 100.0
    return [int(channel * 255 * factor) for channel in rgb]


def comet(phase: float) -> list[list[int]]:
    """A hue that travels along the strip, trailing off behind it."""
    head = (phase * 3.0) % 8
    bar = []
    for i in range(8):
        behind = (head - i) % 8
        falloff = max(0.0, 1.0 - behind / 3.0)
        hue = ((phase * 0.12) + i / 16.0) % 1.0
        bar.append(dim([c * falloff for c in colorsys.hsv_to_rgb(hue, 1.0, 1.0)]))
    return bar


def proximity_bar(mm: float) -> list[list[int]]:
    nearness = 1.0 - min(max(mm, 0.0), PIXELS_MAX_MM) / PIXELS_MAX_MM
    lit = int(round(nearness * 8))
    bar = []
    for i in range(8):
        if i < lit:
            shade = i / 7
            bar.append(dim([shade, 1 - shade, 0]))
        else:
            bar.append([0, 0, 0])
    return bar


def drive_pixels() -> None:
    """Animates the strip while its slide is up, and darkens it when it is not."""
    global _phase, _pixels_bar, _pixels_driving

    wanted = live_kind() == "pixels" and companion.modulinos.get("pixels")
    if not wanted:
        if _pixels_driving:
            _pixels_driving = False
            _pixels_bar = [[0, 0, 0]] * 8
            companion.send_modulino("pixels", _pixels_bar)
            logger.info("left the Modulino slide, LEDs off")
        return

    if not _pixels_driving:
        _pixels_driving = True
        logger.info("on the Modulino slide, driving the LEDs")

    _phase += 1.0 / UI_REFRESH_HZ
    distance = modulino_live.get("distance")
    bar = proximity_bar(distance) if distance is not None else comet(_phase)

    if bar != _pixels_bar:
        _pixels_bar = bar
        companion.send_modulino("pixels", bar)


def credits() -> dict:
    """What this presentation actually asked of the Nesso, measured not claimed."""
    described = companion.describe()
    device = companion.device or {}
    return {
        "sensors": described["sensors"],
        "buttons": described["buttons"],
        "widgets": [w["label"] for w in described["widgets"]],
        "modulinos": companion.modulinos,
        "device": device.get("name"),
        "firmware": device.get("fw"),
        "slides": len(deck),
        "from_remote": stats["from_remote"],
        "from_browser": stats["from_browser"],
        "recentres": stats["recentres"],
    }


# --- Widgets on the Nesso's own screen -------------------------------------
# Six, deliberately: this is a remote you glance at, not a dashboard.

companion.add_value("deck", "read-only", label="Slide")
companion.add_value("clock", "read-only", label="Clock")


@companion.on_value("slide", "stepper", label="Jump", min=1, max=len(deck), step=1, value=1)
def on_slide_widget(value) -> None:
    goto(int(value) - 1, source="Nesso screen")


@companion.on_value("laser", "switch", label="Pointer", value=True)
def on_laser_widget(value) -> None:
    show["pointer"] = bool(value)
    broadcast()


@companion.on_value("recentre", "button", label="Recentre")
def on_recentre_widget(value) -> None:
    recentre()


@companion.on_value("sensitivity", "slider", label="Sensitivity", min=10, max=100, step=10, value=50)
def on_sensitivity_widget(value) -> None:
    show["sensitivity"] = int(value)
    broadcast()


# --- Link ------------------------------------------------------------------


def link_state() -> dict:
    state = companion.describe()
    state["show"] = show_state()
    state["values"] = companion.values
    state["deck"] = deck
    return state


@companion.on_connect
def on_nesso_connected(device: dict) -> None:
    logger.info(f"remote connected: {device.get('name')} at {device.get('address')}")
    recentre()
    sync_widgets()
    ui.send_message("link", link_state())


@companion.on_disconnect
def on_nesso_disconnected(device: dict) -> None:
    logger.warning("remote disconnected")
    ui.send_message("link", link_state())


# --- Browser ---------------------------------------------------------------


def on_show_message(sid: str, data) -> None:
    """Controls from the browser, so the deck works without the remote."""
    if not isinstance(data, dict):
        return
    action = data.get("action")
    if action == "next":
        step(+1, source="browser")
    elif action == "prev":
        step(-1, source="browser")
    elif action == "goto":
        goto(int(data.get("index", 0)), source="browser")
    elif action == "pointer":
        show["pointer"] = bool(data["value"]) if "value" in data else not show["pointer"]
        sync_widgets()
    elif action == "recentre":
        recentre()
    elif action == "timer":
        set_timer(not show["running"])
        return
    elif action == "reset":
        reset_timer()
        return
    broadcast()


ui.on_message("show", on_show_message)
ui.expose_api("GET", "/api/state", link_state)
ui.on_connect(lambda sid: ui.send_message("link", link_state()))


def loop() -> None:
    global _last_clock_pushed

    update_pointer()
    drive_pixels()
    values = companion.values

    ui.send_message(
        "tick",
        {
            "connected": companion.connected,
            "pointer": {**_pointer, "on": show["pointer"] and companion.connected},
            "battery": {
                "volts": values.get("batteryVoltage"),
                "level": values.get("batteryLevel"),
                "low": (values.get("batteryVoltage") or 99) < BATTERY_LOW_V,
            },
            "clock": clock_label(),
            "running": show["running"],
            "live": {
                "kind": live_kind(),
                "pixels": _pixels_bar,
                "driving": _pixels_driving,
                "attached": companion.modulinos,
                "distance": modulino_live.get("distance"),
            },
            "credits": credits() if live_kind() == "credits" else None,
        },
    )

    # The clock widget only changes once a second; pushing it at the UI rate
    # would put 20 frames a second on the link for nothing.
    now_label = clock_label()
    if companion.connected and now_label != _last_clock_pushed:
        _last_clock_pushed = now_label
        companion.send_value("clock", now_label)

    time.sleep(1 / UI_REFRESH_HZ)


logger.info(f"Presenter ready, {len(deck)} slides. Waiting for a Nesso on {companion.url}")

App.run(user_loop=loop)
