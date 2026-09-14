"""Wire protocol shared by the Nesso N1 Companion brick and the Nesso N1 sketch.

The transport is a WebSocket carrying one JSON object per frame. Every frame has
a ``t`` (type) key. The protocol is versioned with ``PROTOCOL_VERSION``; the
Nesso announces the version it speaks in its ``hello`` frame and the board
refuses anything it does not understand.

Direction legend: N -> Q is Nesso to board, Q -> N is board to Nesso.

    N -> Q  hello     {"t":"hello","proto":1,"device":"nesso-n1","id":"...",
                       "name":"...","fw":"...","caps":{...}}
    Q -> N  config    {"t":"config","proto":1,"app":"...","sensors":[...],
                       "buttons":[...],"modulinos":[...],"ui":[...]}
    N -> Q  data      {"t":"data","ts":123,"v":{"accelerometerX":0.01,...}}
    N -> Q  event     {"t":"event","name":"button_1","action":"click","ts":123}
    N -> Q  ui        {"t":"ui","id":"screen_button","action":"press","value":true}
    N -> Q  modulino  {"t":"modulino","kind":"knob","event":"press","value":null}
    N -> Q  status    {"t":"status","modulinos":[{"kind":"distance","present":true}]}
    N -> Q  log       {"t":"log","level":"info","msg":"..."}
    Q -> N  value     {"t":"value","id":"custom_name","value":42}
    Q -> N  modulino  {"t":"modulino","kind":"pixels","value":[[r,g,b],...]}
    Q -> N  ping   /   N -> Q  pong
"""

PROTOCOL_VERSION = 1

# Default TCP port for the companion WebSocket server. Kept in one place so the
# brick, app.yaml and the sketch can all be traced back to the same number.
DEFAULT_PORT = 8077

# Path the Nesso connects to. A plain HTTP GET on INFO_PATH is answered with a
# JSON descriptor, so that during mDNS discovery the Nesso can tell which of the
# boards it found is actually running a companion App.
WS_PATH = "/nesso"
INFO_PATH = "/info"


# --- Standard value names -------------------------------------------------
# These are the names an App passes to NessoCompanion.on(). Keeping them fixed
# is what makes an App portable across companion devices.

ACCELEROMETER = ("accelerometerX", "accelerometerY", "accelerometerZ")
GYROSCOPE = ("gyroscopeX", "gyroscopeY", "gyroscopeZ")
ORIENTATION = ("pitch", "roll")
BATTERY = ("batteryLevel", "batteryVoltage")

SENSOR_NAMES = frozenset(ACCELEROMETER + GYROSCOPE + ORIENTATION + BATTERY)

# Button names and the actions they can report.
BUTTON_NAMES = frozenset({"button_1", "button_2"})
BUTTON_ACTIONS = ("press", "release", "click", "double_click", "long_press")

# --- Screen widgets -------------------------------------------------------
# Widget kinds the Nesso can draw on its 135x240 screen. "value" is display
# only; the others are interactive and report back through a ``ui`` frame.
WIDGET_VALUE = "value"
WIDGET_BUTTON = "button"
WIDGET_SWITCH = "switch"
WIDGET_SLIDER = "slider"
WIDGET_STEPPER = "stepper"

INTERACTIVE_WIDGETS = frozenset({WIDGET_BUTTON, WIDGET_SWITCH, WIDGET_SLIDER, WIDGET_STEPPER})
ALL_WIDGETS = INTERACTIVE_WIDGETS | {WIDGET_VALUE}

# Aliases accepted by add_value()/on_value() so the App-facing vocabulary reads
# naturally ("read-only") while the wire stays terse ("value").
WIDGET_ALIASES = {
    "read-only": WIDGET_VALUE,
    "read_only": WIDGET_VALUE,
    "readonly": WIDGET_VALUE,
    "value": WIDGET_VALUE,
    "text": WIDGET_VALUE,
    "button": WIDGET_BUTTON,
    "switch": WIDGET_SWITCH,
    "toggle": WIDGET_SWITCH,
    "slider": WIDGET_SLIDER,
    "stepper": WIDGET_STEPPER,
}


def normalize_widget_kind(kind: str) -> str:
    """Map a user-facing widget kind onto its wire name.

    Raises:
        ValueError: if the kind is not one the Nesso knows how to draw.
    """
    key = str(kind).strip().lower()
    try:
        return WIDGET_ALIASES[key]
    except KeyError:
        raise ValueError(
            f"unknown widget kind {kind!r}; expected one of {sorted(set(WIDGET_ALIASES))}"
        ) from None


# Modulino kinds the Nesso firmware knows how to drive, and what each reports.
# The brick itself passes any kind straight through, so this is documentation
# rather than a whitelist.
MODULINO_KINDS = {
    "knob": ("rotate", "press", "release"),
    "buttons": ("a", "b", "c"),
    "movement": ("x", "y", "z"),
    "distance": ("mm",),
    "pixels": (),  # output only
}


def split_modulino_spec(spec: str) -> tuple[str, str | None]:
    """Split a ``'knob/press'`` style spec into ``('knob', 'press')``.

    A spec without a slash (``'knob'``) subscribes to every event of that
    Modulino and yields ``('knob', None)``.
    """
    text = str(spec).strip().lower()
    if "/" in text:
        kind, _, event = text.partition("/")
        return kind.strip(), (event.strip() or None)
    return text, None
