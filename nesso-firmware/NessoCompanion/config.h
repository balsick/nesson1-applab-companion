// Build-time settings for the Nesso N1 companion firmware.
// Anything a user is likely to want to change lives here.
#pragma once

#include <Arduino.h>

// --- Protocol -------------------------------------------------------------
// Must match PROTOCOL_VERSION in bricks/nesso_companion/protocol.py.
#define NESSO_PROTOCOL_VERSION 1
#define NESSO_FIRMWARE_VERSION "1.0.0"

// Default TCP port of the companion brick. Overridable from the setup portal
// and then stored in NVS, for Apps that move the brick to another port.
#define NESSO_DEFAULT_PORT 8077

// WebSocket path and the plain-HTTP descriptor used to recognise a board that
// is actually running a companion App.
#define NESSO_WS_PATH "/nesso"
#define NESSO_INFO_PATH "/info"

// --- Wi-Fi provisioning ---------------------------------------------------
#define SETUP_AP_SSID "Nesso-N1-Setup"
#define SETUP_AP_TIMEOUT_MS 600000UL  // give up and retry the stored network after 10 min
#define WIFI_CONNECT_TIMEOUT_MS 20000UL

// --- Discovery ------------------------------------------------------------
#define MDNS_SERVICE "arduino"
#define MDNS_PROTO "tcp"
#define MDNS_QUERY_TIMEOUT_MS 3000UL
// One query drops responders on a busy network; ask twice and merge.
#define MDNS_QUERY_PASSES 2
#define PROBE_TIMEOUT_MS 800UL
#define MAX_BOARDS 8

// --- Screen ---------------------------------------------------------------
// The panel is 135x240. The dashboard uses it in portrait so that widgets can
// be stacked as a scrollable list.
#define SCREEN_W 135
#define SCREEN_H 240
#define SCREEN_ROTATION 0

// Flip these if a touch lands somewhere other than where you pressed. The
// firmware prints raw touch coordinates over serial to make this easy to check.
#define TOUCH_SWAP_XY 0
#define TOUCH_INVERT_X 0
#define TOUCH_INVERT_Y 0

// --- Timing ---------------------------------------------------------------
#define BUTTON_POLL_MS 20
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_CLICK_MS 350    // max press length still counted as a click
#define BUTTON_LONG_MS 800     // press length that becomes a long_press
#define BUTTON_DOUBLE_MS 350   // max gap between two clicks to make a double
#define BOTH_KEYS_EXIT_MS 1500 // hold both keys this long to leave the link

// The Nesso library never sets a bus clock, so the bus runs at the ESP32
// default of 100 kHz, and the Modulino library explicitly drops it there too.
// The datasheet allows 400 kHz (Fast Mode) and every device on the bus supports
// it: the keys, touch, IMU and gauge are all polled continuously, so this is
// worth 4x on nearly half the loop time. Drop to 100000 if a long Qwiic chain
// misbehaves.
#define I2C_CLOCK_HZ 400000

#define TOUCH_POLL_MS 20
// The FT6336 drops a sample now and then mid-drag; require a few empty polls in
// a row before calling it a release, or a swipe breaks into several taps.
#define TOUCH_RELEASE_POLLS 3
// Movement below this stays a tap; above it the gesture becomes a drag.
#define TAP_SLOP_PX 7
#define IMU_POLL_MS 20  // the BMI270 runs at 25 Hz; polling faster just costs I2C
#define BATTERY_POLL_MS 2000
#define MODULINO_POLL_MS 40
// Keep looking for subscribed Modulinos that were not plugged in at handshake
// time, so plugging one in later just works.
#define MODULINO_RESCAN_MS 3000
// A Modulino Movement is read continuously; only report axes that actually
// moved, and never faster than this, or it would swamp the link on its own.
#define MOVEMENT_DELTA_G 0.02f
#define MOVEMENT_MIN_INTERVAL_MS 100
// Same idea for a Modulino Distance: report a change of at least this many
// millimetres, no faster than this.
#define DISTANCE_DELTA_MM 8.0f
#define DISTANCE_MIN_INTERVAL_MS 100
// The backlight is the largest single load on the battery, and it is on/off
// through the I2C expander - there is no PWM on that pin. So the lever is to
// blank the screen after this long with no touch and no key. 0 disables it.
#define SCREEN_IDLE_MS 60000
#define SCREEN_REFRESH_MS 100  // interaction forces an immediate repaint anyway
#define WS_RECONNECT_MS 3000
// Polling the socket every loop iteration costs ~50us a time and buys nothing
// on a link that carries tens of frames per second.
#define WS_POLL_MS 2
// Give up on a board that accepts the socket but never completes the handshake,
// and on one that goes away after it did.
#define LINK_TIMEOUT_MS 12000
#define RELINK_TIMEOUT_MS 30000

// --- Limits ---------------------------------------------------------------
#define MAX_SENSORS 12
#define MAX_WIDGETS 12
#define MAX_MODULINOS 6
#define JSON_RX_CAPACITY 4096
#define JSON_TX_CAPACITY 1024

// --- Colours (RGB565) -----------------------------------------------------
#define COL_BG 0x0000
#define COL_PANEL 0x18E3
#define COL_PANEL_ALT 0x2124
#define COL_TEAL 0x0410
#define COL_TEXT 0xFFFF
#define COL_MUTED 0x8410
#define COL_GREEN 0x1E85
#define COL_ORANGE 0xED03
#define COL_RED 0xE841
#define COL_ACCENT 0x05FF
