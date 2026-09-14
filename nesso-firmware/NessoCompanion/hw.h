// Nesso N1 hardware: screen, touch, the two keys, the IMU, the battery gauge
// and any Modulino hanging off the Qwiic connector.
#pragma once

#include <Arduino.h>
#include <Arduino_Nesso_N1.h>

#include "config.h"

// The display is shared with ui.cpp, which does all the drawing.
extern NessoDisplay display;
extern NessoBattery battery;

namespace hw {

void begin();

// Prints every address answering on the shared I2C bus. The IMU, touch
// controller, gauge, expanders and any Modulino all live there.
void scanI2C();

// --- Touch ---------------------------------------------------------------
struct TouchPoint {
  int16_t x = 0;
  int16_t y = 0;
  bool pressed = false;
};

// A finger being dragged across the screen. `stepDy` is the movement since the
// last call, so scrolling can follow the finger instead of jumping a row at a
// time; `horizontal` says which axis the gesture has committed to, which is how
// a slider drag is told apart from a scroll.
struct Gesture {
  int16_t originX = 0;
  int16_t originY = 0;
  int16_t x = 0;
  int16_t y = 0;
  int16_t stepDy = 0;
  bool horizontal = false;
};

// Returns true once per tap, on release, with the coordinates of the press
// mapped into screen space. A press that turns into a drag is reported through
// takeDrag() instead, so it never also registers as a tap.
bool takeTap(TouchPoint& out);
// Returns true while a drag is in progress and there is new movement to apply.
bool takeDrag(Gesture& out);
void pollTouch();

// --- Keys ----------------------------------------------------------------
// Actions are the protocol's own names: press, release, click, double_click,
// long_press. index is 0 for KEY1 and 1 for KEY2.
typedef void (*KeyHandler)(uint8_t index, const char* action);
void onKey(KeyHandler handler);
void pollKeys();
bool bothKeysHeldFor(uint32_t ms);
bool keyIsDown(uint8_t index);

// --- IMU -----------------------------------------------------------------
void enableImu(bool accel, bool gyro);
void pollImu();
bool imuReady();
// Reads one of the standard value names. Returns false if this Nesso cannot
// produce it.
bool readStandardValue(const String& name, float& out);

// --- Battery -------------------------------------------------------------
void pollBattery();
float batteryLevel();
float batteryVoltage();
// Charger state straight from the AW32001, plus a raw register dump, because
// "is it charging?" should be answerable without guessing.
const char* chargeStatusName();
void dumpCharger();

// --- Modulinos -----------------------------------------------------------
// Events are reported as (kind, event, value); value is NAN when the event
// carries no number.
typedef void (*ModulinoHandler)(const char* kind, const char* event, float value);
void onModulinoEvent(ModulinoHandler handler);
// Fires when a subscribed Modulino appears or disappears, so the board can be
// told which ones are actually plugged in.
typedef void (*ModulinoPresenceHandler)();
void onModulinoPresence(ModulinoPresenceHandler handler);
// Brings up only the Modulinos the board asked for. Returns how many answered.
uint8_t beginModulinos();
void pollModulinos();
// How long the last hot-plug rescan took, for the `status` report.
uint32_t lastRescanMicros();
// Drives an output Modulino. `pixels` expects up to 8 packed 0xRRGGBB values.
void writeModulinoPixels(const uint32_t* colors, uint8_t count);

}  // namespace hw
