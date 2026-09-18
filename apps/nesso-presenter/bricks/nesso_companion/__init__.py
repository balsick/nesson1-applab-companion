"""Nesso N1 Companion - use an Arduino Nesso N1 as a companion for this App.

Import the brick from the App's ``python/main.py``::

    from nesso_companion import NessoCompanion

    companion = NessoCompanion()
    companion.on("accelerometerX", my_callback)

See ``README.md`` in this folder for the full API and the wire protocol.
"""

from .companion import Device, NessoCompanion
from .protocol import (
    ACCELEROMETER,
    BATTERY,
    BUTTON_ACTIONS,
    BUTTON_NAMES,
    DEFAULT_PORT,
    GYROSCOPE,
    ORIENTATION,
    PROTOCOL_VERSION,
    SENSOR_NAMES,
)

__all__ = [
    "ACCELEROMETER",
    "BATTERY",
    "BUTTON_ACTIONS",
    "BUTTON_NAMES",
    "DEFAULT_PORT",
    "GYROSCOPE",
    "ORIENTATION",
    "PROTOCOL_VERSION",
    "SENSOR_NAMES",
    "Device",
    "NessoCompanion",
]
