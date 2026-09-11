// Everything drawn on the 135x240 screen: status screens, the board picker and
// the dashboard generated from the widgets the board asked for.
#pragma once

#include <Arduino.h>

#include "config.h"
#include "hw.h"
#include "state.h"

namespace ui {

void begin();

// Streams the framebuffer over serial as base64 RGB565, for tools/nesso_screenshot.py.
void dumpScreen();

// --- Status screens ------------------------------------------------------
void showSplash();
void showMessage(const String& title, const String& line1, const String& line2 = "",
                 uint16_t accent = COL_TEAL, const String& hint = "");
void showPortal(const String& ssid, const String& url);

// --- Board picker --------------------------------------------------------
void drawPicker(uint8_t selected, const String& networkName);
// Returns the row under (x, y), or -1.
int8_t pickerHit(int16_t x, int16_t y);
void scrollPickerBy(int16_t pixels);

// --- Dashboard -----------------------------------------------------------
// `linked` drives the connection dot in the header.
void drawDashboard(bool linked, const String& boardName);
// Scrolls by a pixel amount, so the list can follow a finger.
void scrollDashboardBy(int16_t pixels);
// Applies a tap to whatever widget sits under it. Returns the widget whose
// value changed, or nullptr.
Widget* dashboardTap(int16_t x, int16_t y);
// Applies a drag: a horizontal one over a slider changes its value, anything
// else scrolls the list. Returns the widget whose value changed, or nullptr.
Widget* dashboardDrag(const hw::Gesture& gesture);
// Moves the keyboard-style selection used when the board has not claimed the
// two keys for itself.
void moveSelection(int8_t delta);
Widget* activateSelection();
bool hasSelection();

}  // namespace ui
