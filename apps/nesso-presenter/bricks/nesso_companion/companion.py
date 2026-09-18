"""NessoCompanion - pair an Arduino Nesso N1 with this App.

The brick owns a WebSocket server. A Nesso N1 finds the board over mDNS
(``_arduino._tcp``), probes ``GET /info`` to check a companion App is really
running here, then opens the socket and says hello. The board answers with a
``config`` frame built from the callbacks the App registered: that frame is the
whole point of the handshake, because the Nesso only spins up the sensors,
button handlers, Modulinos and screen widgets it actually appears in.

Typical use::

    companion = NessoCompanion()

    companion.on("accelerometerX", on_accel_x)       # asks for accelerometer X
    companion.on("button_1", on_main_button)         # asks for button 1 events
    companion.on_modulino("knob/press", on_knob)     # asks for a Modulino Knob

    companion.add_value("slide", "read-only")        # a read-only screen field
    companion.on_value("next", "button", on_next)    # a tappable screen button

    companion.send_value("slide", "3 / 10")          # push a new value to draw
    companion.send_modulino("pixels", [(255, 0, 0)] * 8)
"""

from __future__ import annotations

import asyncio
import inspect
import json
import os
import threading
import time
from collections.abc import Callable, Iterable, Mapping
from typing import Any

import websockets
from websockets.asyncio.server import ServerConnection, serve

from arduino.app_utils import Logger, brick

from .protocol import (
    BUTTON_ACTIONS,
    BUTTON_NAMES,
    DEFAULT_PORT,
    INFO_PATH,
    INTERACTIVE_WIDGETS,
    PROTOCOL_VERSION,
    normalize_widget_kind,
    split_modulino_spec,
)

logger = Logger("NessoCompanion")

# How long send_value() waits for the frame to reach the asyncio loop before
# giving up. Long enough to absorb a busy loop, short enough that an App calling
# it from a tight user loop never stalls noticeably.
_SEND_TIMEOUT_S = 2.0


def _env_flag(name: str, fallback: bool = False) -> bool:
    raw = os.getenv(name)
    if raw is None:
        return fallback
    return raw.strip().lower() in ("1", "true", "yes", "on")


def _env_int(name: str, fallback: int) -> int:
    raw = os.getenv(name)
    if raw is None or not raw.strip():
        return fallback
    try:
        return int(raw)
    except ValueError:
        logger.warning(f"{name}={raw!r} is not an integer, falling back to {fallback}")
        return fallback


def _arity(fn: Callable) -> int | None:
    """Number of positional arguments ``fn`` accepts, or None when variadic."""
    try:
        params = inspect.signature(fn).parameters.values()
    except (TypeError, ValueError):  # builtins and C callables
        return None
    count = 0
    for p in params:
        if p.kind is inspect.Parameter.VAR_POSITIONAL:
            return None
        if p.kind in (inspect.Parameter.POSITIONAL_ONLY, inspect.Parameter.POSITIONAL_OR_KEYWORD):
            count += 1
    return count


class _Callbacks:
    """A list of callbacks invoked with as many arguments as each one accepts.

    Callbacks are deliberately forgiving about arity: ``on("button_1", cb)``
    works whether ``cb`` is ``cb(action)``, ``cb(action, name)`` or ``cb()``.
    """

    def __init__(self, label: str) -> None:
        self._label = label
        self._fns: list[Callable] = []

    def add(self, fn: Callable) -> None:
        if not callable(fn):
            raise TypeError(f"callback for {self._label} must be callable, got {type(fn).__name__}")
        self._fns.append(fn)

    def __bool__(self) -> bool:
        return bool(self._fns)

    def fire(self, *args: Any) -> None:
        for fn in self._fns:
            n = _arity(fn)
            trimmed = args if n is None else args[:n]
            try:
                fn(*trimmed)
            except Exception:
                logger.exception(f"callback for {self._label} raised")


class Device:
    """One Nesso that is connected right now.

    Sensor readings and Modulino presence live here rather than on the brick,
    because they belong to a particular piece of hardware in a particular hand.
    Screen widgets deliberately do not: they are declared by the App, and every
    connected Nesso draws the same ones.
    """

    def __init__(self, companion: NessoCompanion, conn: ServerConnection, hello: dict[str, Any]):
        self._companion = companion
        self._conn = conn
        self.id: str = str(hello.get("id") or f"anon-{id(conn):x}")
        self.name: str = hello.get("name") or hello.get("device") or "Nesso N1"
        self.kind: str = hello.get("device", "nesso-n1")
        self.firmware: str | None = hello.get("fw")
        self.caps: dict[str, Any] = hello.get("caps") or {}
        peer = getattr(conn, "remote_address", None)
        self.address: str | None = peer[0] if peer else None
        self.connected_at: float = time.time()

        self.values: dict[str, Any] = {}
        self.modulinos: dict[str, bool] = {}

    # Sending to one device rather than all of them is the whole point of
    # having a handle on it.
    def send_value(self, name: str, value: Any) -> bool:
        return self._companion._send_value_to(self, name, value)

    def send_values(self, values: Mapping[str, Any]) -> bool:
        return self._companion._send_values_to(self, values)

    def send_modulino(self, kind: str, value: Any) -> bool:
        return self._companion._send_modulino_to(self, kind, value)

    def describe(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "name": self.name,
            "device": self.kind,
            "fw": self.firmware,
            "address": self.address,
            "caps": self.caps,
            "modulinos": dict(self.modulinos),
            "values": dict(self.values),
        }

    def __repr__(self) -> str:
        return f"<Device {self.name} ({self.id}) at {self.address}>"


class _Widget:
    """A component the Nesso draws on its screen."""

    def __init__(self, wid: str, kind: str, label: str | None, options: dict[str, Any]) -> None:
        self.id = wid
        self.kind = kind
        self.label = label if label is not None else wid
        self.options = options
        self.value: Any = options.pop("value", None)
        self.callbacks = _Callbacks(f"widget '{wid}'")

    def to_wire(self) -> dict[str, Any]:
        item = {"id": self.id, "kind": self.kind, "label": self.label, "value": self.value}
        item.update(self.options)
        return item


@brick
class NessoCompanion:
    """WebSocket endpoint an Arduino Nesso N1 connects to as a companion device.

    Args:
        port: TCP port to listen on. Defaults to the ``NESSO_PORT`` brick
            variable, then to 8077.
        name: Name shown on the Nesso screen. Defaults to the ``NESSO_NAME``
            brick variable, then to the App folder name.
        host: Bind address. Defaults to every interface.
        default_rate_hz: Streaming rate requested for sensors registered
            without an explicit ``rate_hz``.
        screen_idle_s: How long the Nesso waits before blanking its screen. 0
            keeps it lit. Defaults to the ``NESSO_SCREEN_IDLE_S`` brick
            variable, then to 60.
    """

    def __init__(
        self,
        port: int | None = None,
        name: str | None = None,
        host: str = "0.0.0.0",
        default_rate_hz: int | None = None,
        screen_idle_s: int | None = None,
        autoflash: bool | None = None,
        firmware_dir: str = "/app/nesso-firmware/build",
    ) -> None:
        self.port = int(port) if port is not None else _env_int("NESSO_PORT", DEFAULT_PORT)
        self.host = host
        self.name = name or os.getenv("NESSO_NAME") or os.path.basename(
            os.getenv("APP_HOME", "").rstrip("/")
        ) or "Arduino App"
        self.default_rate_hz = (
            int(default_rate_hz)
            if default_rate_hz is not None
            else _env_int("NESSO_DEFAULT_RATE_HZ", 10)
        )

        self.screen_idle_s = (
            int(screen_idle_s)
            if screen_idle_s is not None
            else _env_int("NESSO_SCREEN_IDLE_S", 60)
        )

        # Off unless asked for: probing resets whatever ESP32 is on the port,
        # and the USB identifiers cannot tell a Nesso from any other one.
        self.autoflash = (
            bool(autoflash) if autoflash is not None else _env_flag("NESSO_AUTOFLASH", False)
        )
        self.firmware_dir = os.getenv("NESSO_FIRMWARE_DIR", firmware_dir)

        # Registrations. Guarded by _lock because an App may register from its
        # own thread while the asyncio loop is serving a connection.
        self._lock = threading.RLock()
        self._sensors: dict[str, dict[str, Any]] = {}
        # Names registered with an explicit rate_hz. Without this, the first
        # registration would be stuck at default_rate_hz and a slower explicit
        # rate could never win.
        self._sensor_rate_set: set[str] = set()
        self._sensor_cbs: dict[str, _Callbacks] = {}
        self._buttons: dict[str, set[str]] = {}
        self._button_cbs: dict[str, _Callbacks] = {}
        self._modulinos: dict[str, dict[str, Any]] = {}
        self._modulino_cbs: dict[str, _Callbacks] = {}
        self._widgets: dict[str, _Widget] = {}

        self._connect_cbs = _Callbacks("on_connect")
        self._disconnect_cbs = _Callbacks("on_disconnect")

        # Runtime state.
        # Connected devices, keyed by the id each one sends in its hello.
        self._devices: dict[str, Device] = {}
        self._clients: set[ServerConnection] = set()
        self._config_revision = 0
        self._sent_revision: dict[int, int] = {}

        self._loop: asyncio.AbstractEventLoop | None = None
        self._shutdown: asyncio.Future | None = None
        self._thread: threading.Thread | None = None
        self._server_ready = threading.Event()
        self._stopping = threading.Event()

    # -- lifecycle (called by the App framework) ---------------------------

    def start(self) -> None:
        """Bring the WebSocket server up on its own asyncio loop thread."""
        if self._thread is not None:
            return
        if self.autoflash:
            # In the background: writing the images takes about fifteen seconds
            # and the App should not wait for a device that may not be there.
            threading.Thread(
                target=self._provision, name="NessoProvision", daemon=True
            ).start()
        self._stopping.clear()
        self._thread = threading.Thread(target=self._run_loop, name="NessoCompanion", daemon=True)
        self._thread.start()
        if not self._server_ready.wait(timeout=10):
            logger.warning("WebSocket server did not report ready within 10s")

    def _provision(self) -> None:
        """Bring an attached Nesso up to the firmware shipped with this App."""
        from . import provision

        version_file = os.path.join(self.firmware_dir, "version.txt")
        try:
            with open(version_file) as handle:
                expected = handle.read().strip()
        except OSError as err:
            logger.error(f"cannot read {version_file}: {err}; skipping the firmware check")
            return

        outcome = provision.ensure_firmware(self.firmware_dir, expected)
        logger.info(f"firmware check: {outcome}")

    def stop(self) -> None:
        """Close every client and shut the server down."""
        self._stopping.set()
        loop, shutdown = self._loop, self._shutdown
        if loop is not None and shutdown is not None and loop.is_running():
            loop.call_soon_threadsafe(lambda: shutdown.done() or shutdown.set_result(None))
        thread = self._thread
        if thread is not None:
            thread.join(timeout=5)
        self._thread = None
        self._loop = None
        self._server_ready.clear()

    # -- registration ------------------------------------------------------

    def on(
        self,
        name: str,
        callback: Callable | None = None,
        *,
        rate_hz: int | None = None,
    ) -> Callable:
        """Subscribe to a standard sensor value or button event from the Nesso.

        Registering is what makes the Nesso produce the value at all, so an App
        that never calls ``on('gyroscopeX', ...)`` leaves the gyroscope off.

        Sensor names are the standard ones (``accelerometerX``, ``gyroscopeZ``,
        ``pitch``, ``roll``, ``batteryLevel``, ...) and the callback receives
        ``(value)``, optionally followed by ``(name, timestamp_ms)``.

        Button names are ``button_1`` and ``button_2``; the callback receives
        ``(action)`` where action is one of press, release, click, double_click
        or long_press, optionally followed by ``(name, timestamp_ms)``.

        Args:
            name: Standard value or button name.
            callback: Handler. Omit it to use ``on`` as a decorator.
            rate_hz: Streaming rate for sensors. Ignored for buttons.

        Returns:
            The callback, so this works as a decorator.
        """
        if callback is None:
            return lambda fn: self.on(name, fn, rate_hz=rate_hz)

        key = str(name).strip()
        with self._lock:
            if key in BUTTON_NAMES:
                self._buttons.setdefault(key, set(BUTTON_ACTIONS))
                self._button_cbs.setdefault(key, _Callbacks(f"button '{key}'")).add(callback)
            else:
                entry = self._sensors.setdefault(key, {"name": key, "rate_hz": self.default_rate_hz})
                if rate_hz is not None:
                    rate = max(1, int(rate_hz))
                    # Several explicit handlers on one value: the fastest wins.
                    # The default rate never wins against an explicit one.
                    entry["rate_hz"] = max(rate, entry["rate_hz"]) if key in self._sensor_rate_set else rate
                    self._sensor_rate_set.add(key)
                self._sensor_cbs.setdefault(key, _Callbacks(f"sensor '{key}'")).add(callback)
            self._touch_config()
        return callback

    def on_modulino(self, spec: str, callback: Callable | None = None) -> Callable:
        """Subscribe to events from a Modulino attached to the Nesso.

        Args:
            spec: ``'<kind>/<event>'`` such as ``'knob/press'``, or just
                ``'<kind>'`` to receive every event from that Modulino.
            callback: Handler receiving ``(value)``, optionally followed by
                ``(event, kind)``. Omit it to use this as a decorator.

        Returns:
            The callback, so this works as a decorator.
        """
        if callback is None:
            return lambda fn: self.on_modulino(spec, fn)

        kind, event = split_modulino_spec(spec)
        with self._lock:
            entry = self._modulinos.setdefault(kind, {"kind": kind, "dir": "in", "events": set()})
            entry["dir"] = "both" if entry["dir"] == "out" else "in"
            if event:
                entry["events"].add(event)
            key = f"{kind}/{event}" if event else kind
            self._modulino_cbs.setdefault(key, _Callbacks(f"modulino '{key}'")).add(callback)
            self._touch_config()
        return callback

    def add_value(self, name: str, kind: str = "read-only", label: str | None = None, **options: Any) -> None:
        """Declare a value the Nesso shows on its screen but cannot change.

        Args:
            name: Widget id, also used with :meth:`send_value`.
            kind: Widget kind. Only read-only kinds make sense here.
            label: Text drawn next to the value. Defaults to ``name``.
            **options: Extra widget hints, e.g. ``unit='C'``.
        """
        wire_kind = normalize_widget_kind(kind)
        if wire_kind in INTERACTIVE_WIDGETS:
            raise ValueError(
                f"'{kind}' is interactive; register it with on_value('{name}', '{kind}', callback)"
            )
        self._register_widget(name, wire_kind, label, options)

    def on_value(
        self,
        name: str,
        kind: str,
        callback: Callable | None = None,
        label: str | None = None,
        **options: Any,
    ) -> Callable:
        """Declare an interactive widget on the Nesso screen and handle its input.

        Args:
            name: Widget id, also used with :meth:`send_value`.
            kind: One of ``button``, ``switch``, ``slider``, ``stepper``.
            callback: Handler receiving ``(value)``, optionally followed by
                ``(name)``. Omit it to use this as a decorator.
            label: Text drawn on the widget. Defaults to ``name``.
            **options: Widget hints: ``min``, ``max``, ``step``, ``value``.

        Returns:
            The callback, so this works as a decorator.
        """
        if callback is None:
            return lambda fn: self.on_value(name, kind, fn, label=label, **options)

        wire_kind = normalize_widget_kind(kind)
        widget = self._register_widget(name, wire_kind, label, options)
        with self._lock:
            widget.callbacks.add(callback)
        return callback

    def on_connect(self, callback: Callable) -> Callable:
        """Called with ``(device_info)`` when a Nesso completes the handshake."""
        self._connect_cbs.add(callback)
        return callback

    def on_disconnect(self, callback: Callable) -> Callable:
        """Called with ``(device_info)`` when a Nesso goes away."""
        self._disconnect_cbs.add(callback)
        return callback

    def _register_widget(
        self, name: str, wire_kind: str, label: str | None, options: dict[str, Any]
    ) -> _Widget:
        key = str(name).strip()
        with self._lock:
            widget = self._widgets.get(key)
            if widget is None:
                widget = _Widget(key, wire_kind, label, dict(options))
                self._widgets[key] = widget
            else:
                if widget.kind != wire_kind:
                    raise ValueError(
                        f"widget '{key}' is already registered as '{widget.kind}', "
                        f"cannot re-register it as '{wire_kind}'"
                    )
                if label is not None:
                    widget.label = label
                if "value" in options:
                    widget.value = options.pop("value")
                widget.options.update(options)
            self._touch_config()
            return widget

    # -- outgoing ----------------------------------------------------------

    def send_value(self, name: str, value: Any) -> bool:
        """Push a new value for a widget declared with add_value/on_value.

        Returns:
            True if the frame was handed to a connected Nesso.
        """
        key = str(name).strip()
        with self._lock:
            widget = self._widgets.get(key)
            if widget is None:
                logger.warning(
                    f"send_value('{key}', ...) ignored: no widget with that name. "
                    f"Declare it first with add_value() or on_value()."
                )
                return False
            widget.value = value
        return self._broadcast({"t": "value", "id": key, "value": value})

    def send_values(self, values: Mapping[str, Any]) -> bool:
        """Push several widget values in a single frame."""
        payload: dict[str, Any] = {}
        with self._lock:
            for key, value in values.items():
                widget = self._widgets.get(str(key).strip())
                if widget is None:
                    logger.warning(f"send_values: no widget named '{key}', skipped")
                    continue
                widget.value = value
                payload[widget.id] = value
        if not payload:
            return False
        return self._broadcast({"t": "value", "values": payload})

    def send_modulino(self, kind: str, value: Any) -> bool:
        """Drive a Modulino attached to the Nesso.

        Args:
            kind: Modulino kind, e.g. ``'pixels'``.
            value: Payload for that Modulino. For ``pixels`` this is a sequence
                of 8 ``(r, g, b)`` triples.

        Returns:
            True if the frame was handed to a connected Nesso.
        """
        key = str(kind).strip().lower()
        self._register_modulino_output(key)
        return self._broadcast({"t": "modulino", "kind": key, "value": _jsonable(value)})

    def _register_modulino_output(self, key: str) -> None:
        with self._lock:
            # Only a real change to the registration may bump the config
            # revision: this runs on every send, and re-sending the handshake
            # would make the Nesso tear down and re-initialise its Modulinos.
            entry = self._modulinos.get(key)
            if entry is None:
                self._modulinos[key] = {"kind": key, "dir": "out", "events": set()}
                self._touch_config()
            elif entry["dir"] == "in":
                entry["dir"] = "both"
                self._touch_config()

    # -- state -------------------------------------------------------------

    @property
    def connected(self) -> bool:
        """True while at least one Nesso is connected."""
        return bool(self._clients)

    @property
    def devices(self) -> list[Device]:
        """Every Nesso connected right now, in the order they arrived."""
        with self._lock:
            return sorted(self._devices.values(), key=lambda d: d.connected_at)

    @property
    def device(self) -> Device | None:
        """The Nesso that has been connected longest, or None.

        Convenient when there is only one, which is the usual case. With several
        connected, use :attr:`devices` or the ``device`` argument that every
        callback receives, rather than guessing that this is the right one.
        """
        found = self.devices
        return found[0] if found else None

    def device_by_id(self, device_id: str) -> Device | None:
        """Looks a Nesso up by the id it reported in its hello."""
        with self._lock:
            return self._devices.get(device_id)

    @property
    def values(self) -> dict[str, Any]:
        """Last value of every subscribed sensor, merged across devices.

        With one Nesso this is simply its readings. With several, later frames
        overwrite earlier ones key by key, which is rarely what you want — read
        ``device.values`` instead.
        """
        merged: dict[str, Any] = {}
        with self._lock:
            for device in sorted(self._devices.values(), key=lambda d: d.connected_at):
                merged.update(device.values)
        return merged

    @property
    def modulinos(self) -> dict[str, bool]:
        """Which subscribed Modulinos are attached, to any connected Nesso.

        Answers "is there a Pixels on this link at all". For "does *that* remote
        have one", read ``device.modulinos``.
        """
        merged: dict[str, bool] = {}
        with self._lock:
            for device in self._devices.values():
                for kind, present in device.modulinos.items():
                    merged[kind] = merged.get(kind, False) or present
        return merged

    @property
    def widget_values(self) -> dict[str, Any]:
        """Current value of every screen widget, keyed by widget id."""
        with self._lock:
            return {w.id: w.value for w in self._widgets.values()}

    @property
    def url(self) -> str:
        """WebSocket URL a Nesso on the same network should dial."""
        host = os.getenv("HOST_IP") or self.host
        return f"ws://{host}:{self.port}"

    def describe(self) -> dict[str, Any]:
        """Everything the App asked the Nesso for, as plain data (for a UI)."""
        with self._lock:
            return {
                "app": self.name,
                "port": self.port,
                "url": self.url,
                "connected": self.connected,
                "screen_idle_s": self.screen_idle_s,
                "device": self.device.describe() if self.device else None,
                "devices": [d.describe() for d in self.devices],
                "sensors": sorted(self._sensors),
                "buttons": sorted(self._buttons),
                "modulinos": sorted(self._modulinos),
                "modulino_presence": self.modulinos,
                "widgets": [w.to_wire() for w in self._widgets.values()],
            }

    # -- server ------------------------------------------------------------

    def _run_loop(self) -> None:
        try:
            asyncio.run(self._serve())
        except Exception:
            if not self._stopping.is_set():
                logger.exception("WebSocket server stopped unexpectedly")

    async def _serve(self) -> None:
        self._loop = asyncio.get_running_loop()
        self._shutdown = self._loop.create_future()
        async with serve(
            self._handle_client,
            self.host,
            self.port,
            process_request=self._process_request,
            ping_interval=20,
            ping_timeout=20,
            max_size=256 * 1024,
        ):
            logger.info(f"Nesso companion '{self.name}' listening on ws://{self.host}:{self.port}")
            self._server_ready.set()
            await self._shutdown  # serve until stop() resolves this

    def _process_request(self, connection: ServerConnection, request: Any) -> Any:
        """Answer the Nesso's discovery probe without upgrading to a WebSocket."""
        path = request.path.split("?", 1)[0]
        if path in (INFO_PATH, "/", "/nesso/info"):
            body = json.dumps(
                {
                    "service": "nesso-companion",
                    "proto": PROTOCOL_VERSION,
                    "app": self.name,
                    "port": self.port,
                    "connected": self.connected,
                }
            )
            return connection.respond(200, body + "\n")
        return None

    async def _handle_client(self, conn: ServerConnection) -> None:
        device: Device | None = None
        try:
            hello = await asyncio.wait_for(conn.recv(), timeout=15)
            frame = _decode(hello)
            if not frame or frame.get("t") != "hello":
                await conn.close(1002, "expected a hello frame")
                return
            if int(frame.get("proto", 0)) != PROTOCOL_VERSION:
                await conn.close(1002, f"protocol {frame.get('proto')} not supported")
                return

            device = Device(self, conn, frame)
            with self._lock:
                # A remote that reconnects keeps its identity, so an App holding
                # a reference by id does not end up with a stale one.
                self._devices[device.id] = device
            self._clients.add(conn)
            logger.info(
                f"Nesso '{device.name}' ({device.id}) connected from {device.address}"
                f" — {len(self._devices)} device(s) now"
            )

            await self._send_config(conn)
            await self._off_loop(self._connect_cbs.fire, device)

            async for raw in conn:
                # User callbacks run in a worker thread: an App handler that
                # blocks must never stall the socket or the outgoing frames.
                await self._off_loop(self._dispatch, _decode(raw), device)
                await self._resend_config_if_stale(conn)
        except asyncio.TimeoutError:
            logger.warning("client did not send a hello frame in time")
        except websockets.exceptions.ConnectionClosed:
            pass
        except Exception:
            logger.exception("error while serving a Nesso connection")
        finally:
            self._clients.discard(conn)
            self._sent_revision.pop(id(conn), None)
            if device is not None:
                with self._lock:
                    # Only forget it if this connection is still the live one:
                    # a reconnect may already have replaced it.
                    if self._devices.get(device.id) is device:
                        del self._devices[device.id]
                logger.info(
                    f"Nesso '{device.name}' disconnected — {len(self._devices)} device(s) left"
                )
                self._disconnect_cbs.fire(device)

    async def _off_loop(self, fn: Callable, *args: Any) -> None:
        await asyncio.get_running_loop().run_in_executor(None, fn, *args)

    # -- handshake ---------------------------------------------------------

    def _touch_config(self) -> None:
        """Mark the handshake stale so connected devices get an updated config."""
        self._config_revision += 1

    def _build_config(self) -> dict[str, Any]:
        with self._lock:
            return {
                "t": "config",
                "proto": PROTOCOL_VERSION,
                "rev": self._config_revision,
                "app": self.name,
                "screen_idle_s": self.screen_idle_s,
                "sensors": [dict(s) for s in self._sensors.values()],
                "buttons": [
                    {"name": name, "events": sorted(actions)}
                    for name, actions in self._buttons.items()
                ],
                "modulinos": [
                    {"kind": m["kind"], "dir": m["dir"], "events": sorted(m["events"])}
                    for m in self._modulinos.values()
                ],
                "ui": [w.to_wire() for w in self._widgets.values()],
            }

    async def _send_config(self, conn: ServerConnection) -> None:
        config = self._build_config()
        await conn.send(json.dumps(config))
        self._sent_revision[id(conn)] = config["rev"]
        logger.info(
            f"handshake sent: {len(config['sensors'])} sensor(s), {len(config['buttons'])} button(s), "
            f"{len(config['modulinos'])} modulino(s), {len(config['ui'])} widget(s)"
        )

    async def _resend_config_if_stale(self, conn: ServerConnection) -> None:
        if self._sent_revision.get(id(conn)) != self._config_revision:
            await self._send_config(conn)

    # -- inbound dispatch --------------------------------------------------

    def _dispatch(self, frame: dict[str, Any] | None, device: Device) -> None:
        """Routes one inbound frame, telling the callbacks which Nesso sent it.

        Every callback takes the sending device as its last argument. Handlers
        that do not declare it never see it, because _Callbacks trims the
        arguments to what each one accepts.
        """
        if not frame:
            return
        kind = frame.get("t")
        if kind == "data":
            ts = frame.get("ts")
            values = frame.get("v") or {}
            with self._lock:
                device.values.update(values)
            for name, value in values.items():
                cbs = self._sensor_cbs.get(name)
                if cbs:
                    cbs.fire(value, name, ts, device)
        elif kind == "event":
            name = frame.get("name", "")
            action = frame.get("action", "")
            cbs = self._button_cbs.get(name)
            if cbs:
                cbs.fire(action, name, frame.get("ts"), device)
        elif kind == "ui":
            wid = frame.get("id", "")
            with self._lock:
                widget = self._widgets.get(wid)
                if widget is not None:
                    widget.value = frame.get("value")
            if widget is not None:
                # Widgets belong to the App, not to a device: every Nesso draws
                # the same ones, so a change on one is a change for all. The
                # device is passed only so a handler can tell who did it.
                widget.callbacks.fire(frame.get("value"), wid, device)
        elif kind == "modulino":
            mod = str(frame.get("kind", "")).lower()
            event = str(frame.get("event", "")).lower()
            value = frame.get("value")
            for key in (f"{mod}/{event}", mod):
                cbs = self._modulino_cbs.get(key)
                if cbs:
                    cbs.fire(value, event, mod, device)
        elif kind == "status":
            presence = {
                str(item.get("kind")): bool(item.get("present"))
                for item in frame.get("modulinos") or []
                if item.get("kind")
            }
            if presence != device.modulinos:
                device.modulinos = presence
                attached = sorted(k for k, v in presence.items() if v)
                logger.info(f"{device.name}: Modulinos attached: {', '.join(attached) or 'none'}")
        elif kind == "log":
            level = str(frame.get("level", "info")).lower()
            msg = f"[{device.name}] {frame.get('msg', '')}"
            getattr(logger, level, logger.info)(msg)
        elif kind == "pong":
            pass
        else:
            logger.debug(f"ignoring unknown frame type {kind!r}")

    # -- outbound plumbing -------------------------------------------------

    # Per-device sends. Device.send_* delegates here so the socket plumbing
    # stays in one place.

    def _send_value_to(self, device: Device, name: str, value: Any) -> bool:
        key = str(name).strip()
        with self._lock:
            widget = self._widgets.get(key)
            if widget is None:
                logger.warning(f"send_value('{key}', ...) ignored: no widget with that name")
                return False
            # The widget belongs to the App, so its value is still shared even
            # when the frame goes to one device.
            widget.value = value
        return self._send_to(device, {"t": "value", "id": key, "value": value})

    def _send_values_to(self, device: Device, values: Mapping[str, Any]) -> bool:
        payload: dict[str, Any] = {}
        with self._lock:
            for key, value in values.items():
                widget = self._widgets.get(str(key).strip())
                if widget is None:
                    logger.warning(f"send_values: no widget named '{key}', skipped")
                    continue
                widget.value = value
                payload[widget.id] = value
        if not payload:
            return False
        return self._send_to(device, {"t": "value", "values": payload})

    def _send_modulino_to(self, device: Device, kind: str, value: Any) -> bool:
        key = str(kind).strip().lower()
        self._register_modulino_output(key)
        return self._send_to(device, {"t": "modulino", "kind": key, "value": _jsonable(value)})

    def _send_to(self, device: Device, message: dict[str, Any]) -> bool:
        loop = self._loop
        if loop is None or device._conn not in self._clients:
            return False
        payload = json.dumps(message)
        future = asyncio.run_coroutine_threadsafe(
            self._send_all([device._conn], payload), loop
        )
        try:
            return bool(future.result(timeout=_SEND_TIMEOUT_S))
        except TimeoutError:
            logger.warning(f"timed out pushing a frame to {device.name}")
            return False
        except Exception:
            logger.exception(f"failed to push a frame to {device.name}")
            return False

    def _broadcast(self, message: dict[str, Any]) -> bool:
        loop = self._loop
        clients = list(self._clients)
        if loop is None or not clients:
            return False
        payload = json.dumps(message)
        future = asyncio.run_coroutine_threadsafe(self._send_all(clients, payload), loop)
        try:
            return bool(future.result(timeout=_SEND_TIMEOUT_S))
        except TimeoutError:
            logger.warning("timed out pushing a frame to the Nesso")
            return False
        except Exception:
            logger.exception("failed to push a frame to the Nesso")
            return False

    async def _send_all(self, clients: Iterable[ServerConnection], payload: str) -> bool:
        sent = False
        for conn in clients:
            try:
                await conn.send(payload)
                sent = True
            except websockets.exceptions.ConnectionClosed:
                self._clients.discard(conn)
        return sent


def _decode(raw: Any) -> dict[str, Any] | None:
    if isinstance(raw, bytes | bytearray):
        raw = raw.decode("utf-8", "replace")
    try:
        frame = json.loads(raw)
    except (TypeError, ValueError):
        logger.debug("dropping a frame that is not JSON")
        return None
    return frame if isinstance(frame, dict) else None


def _jsonable(value: Any) -> Any:
    """Turn tuples and nested sequences into something json.dumps accepts."""
    if isinstance(value, str | bytes | bytearray):
        return value.decode() if isinstance(value, bytes | bytearray) else value
    if isinstance(value, Mapping):
        return {str(k): _jsonable(v) for k, v in value.items()}
    if isinstance(value, Iterable):
        return [_jsonable(v) for v in value]
    return value
