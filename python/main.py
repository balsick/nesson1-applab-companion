"""Nesso Presenter - drive a slide deck from an Arduino Nesso N1.

The Nesso is the remote: its keys change slide, tilting it moves a pointer, and
its screen carries a jump control and the running clock. The browser is the
projected surface, and works on its own if the remote is not around.

Note how little this App asks of the Nesso compared with the companion demo:
pitch, roll and the battery, and nothing else. The accelerometer, the gyroscope
and every Modulino stay asleep, because nothing here registered them.
"""

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
# the sensitivity control. Larger span means the pointer moves less per degree.
SPAN_DEG_MIN = 10.0
SPAN_DEG_MAX = 60.0

# Exponential smoothing on the pointer. Hands shake; slides are large.
POINTER_SMOOTHING = 0.35

# Below this the remote is close to cutting out mid-talk, which is worth saying
# loudly rather than showing as a number nobody reads.
BATTERY_LOW_V = 3.55

ui = WebUI()
companion = NessoCompanion()


# --- The deck --------------------------------------------------------------


def load_deck() -> list[dict]:
    """Reads assets/slides.md, one slide per `---` separated block.

    Deliberately not a Markdown parser: a heading, bullets and paragraphs are
    all this needs, and keeping it dumb means the deck stays editable by hand.
    """
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
        return [
            {
                "title": "No slides",
                "blocks": [{"kind": "text", "text": "Add assets/slides.md and restart the App."}],
            }
        ]
    with open(path, encoding="utf-8") as handle:
        raw = handle.read()

    image_re = re.compile(r"^!\[(?P<alt>[^\]]*)\]\((?P<src>[^)]+)\)$")

    slides = []
    for chunk in re.split(r"^---\s*$", raw, flags=re.MULTILINE):
        title = ""
        image = None
        blocks = []
        for line in (l.strip() for l in chunk.strip().splitlines()):
            if not line:
                continue
            found = image_re.match(line)
            if line.startswith("# "):
                title = line[2:].strip()
            elif found:
                # One illustration per slide; it drives the layout.
                image = {"src": found["src"], "alt": found["alt"]}
            elif line.startswith("> "):
                blocks.append({"kind": "lead", "text": line[2:].strip()})
            elif line.startswith("- "):
                blocks.append({"kind": "bullet", "text": line[2:].strip()})
            else:
                blocks.append({"kind": "text", "text": line.strip()})
        if title or blocks or image:
            slides.append({"title": title, "image": image, "blocks": blocks})

    logger.info(f"deck loaded: {len(slides)} slide(s) from {path}")
    return slides or [{"title": "Empty deck", "blocks": []}]


deck = load_deck()

show = {
    "index": 0,
    "pointer": True,
    "blank": False,
    "sensitivity": 50,
    "running": False,
    "elapsed": 0.0,
}

_timer_started_at: float | None = None
_last_clock_pushed = ""
_pointer = {"x": 0.5, "y": 0.5}
_neutral = {"pitch": 0.0, "roll": 0.0}


def slide_label() -> str:
    return f"{show['index'] + 1} / {len(deck)}"


def elapsed_seconds() -> float:
    if show["running"] and _timer_started_at is not None:
        return show["elapsed"] + (time.monotonic() - _timer_started_at)
    return show["elapsed"]


def clock_label() -> str:
    total = int(elapsed_seconds())
    return f"{total // 60}:{total % 60:02d}"


def broadcast() -> None:
    ui.send_message("show", show_state())


def show_state() -> dict:
    return {
        **show,
        "elapsed": elapsed_seconds(),
        "count": len(deck),
        "label": slide_label(),
        "clock": clock_label(),
    }


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


@companion.on("button_1")
def on_key1(action: str) -> None:
    if action == "click":
        step(+1, source="KEY1")
    elif action == "long_press":
        show["blank"] = not show["blank"]
        broadcast()


@companion.on("button_2")
def on_key2(action: str) -> None:
    if action == "click":
        step(-1, source="KEY2")
    elif action == "long_press":
        recentre()


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
    logger.info(f"pointer recentred at pitch {_neutral['pitch']:.1f} roll {_neutral['roll']:.1f}")
    ui.send_message("event", {"ts": time.time(), "text": "pointer recentred"})


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


@companion.on_value("blank", "button", label="Blank")
def on_blank_widget(value) -> None:
    show["blank"] = not show["blank"]
    broadcast()


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
    elif action == "blank":
        # A bare toggle is what a keypress wants; an explicit value is what a
        # script or a re-sync wants, so accept both.
        show["blank"] = bool(data["value"]) if "value" in data else not show["blank"]
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
    update_pointer()
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
        },
    )
    # The clock widget only changes once a second; pushing it at the UI rate
    # would put 20 frames a second on the link for nothing.
    global _last_clock_pushed
    now_label = clock_label()
    if companion.connected and now_label != _last_clock_pushed:
        _last_clock_pushed = now_label
        companion.send_value("clock", now_label)
    time.sleep(1 / UI_REFRESH_HZ)


logger.info(f"Presenter ready, {len(deck)} slides. Waiting for a Nesso on {companion.url}")

App.run(user_loop=loop)
