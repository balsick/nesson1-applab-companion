// What the board asked this Nesso for, plus the live link state.
//
// Everything here is filled in by the `config` frame the board sends during the
// handshake. Nothing is streamed and no Modulino is polled unless it shows up
// in one of these tables, which is what keeps an idle companion cheap.
#pragma once

#include <Arduino.h>

#include "config.h"

enum WidgetKind : uint8_t {
  WK_VALUE = 0,
  WK_BUTTON,
  WK_SWITCH,
  WK_SLIDER,
  WK_STEPPER,
};

// One standard value the board subscribed to, with its own send schedule.
struct SensorSub {
  String name;
  uint16_t rateHz = 10;
  uint32_t periodMs = 100;
  uint32_t nextDueMs = 0;
  float value = 0.0f;
  bool hasValue = false;
};

// One component drawn on the screen.
struct Widget {
  String id;
  String label;
  WidgetKind kind = WK_VALUE;
  String text;          // rendered value for WK_VALUE
  bool flag = false;    // WK_SWITCH
  float number = 0.0f;  // WK_SLIDER, WK_STEPPER
  float vmin = 0.0f;
  float vmax = 100.0f;
  float vstep = 1.0f;
  uint32_t highlightUntilMs = 0;  // brief flash after a tap
};

// A Modulino the board cares about. `wantsIn` means it subscribed to events,
// `wantsOut` means it will push values to it.
struct ModulinoSub {
  String kind;
  bool wantsIn = false;
  bool wantsOut = false;
  bool present = false;
};

// The whole handshake result.
struct CompanionConfig {
  bool valid = false;
  uint32_t revision = 0;
  String appName;

  SensorSub sensors[MAX_SENSORS];
  uint8_t sensorCount = 0;

  bool wantsButton[2] = {false, false};

  // How long the Nesso waits before blanking its screen, as asked for by the
  // App. Zero means never.
  uint32_t screenIdleMs = SCREEN_IDLE_MS;

  Widget widgets[MAX_WIDGETS];
  uint8_t widgetCount = 0;

  ModulinoSub modulinos[MAX_MODULINOS];
  uint8_t modulinoCount = 0;

  void clear();
  SensorSub* findSensor(const String& name);
  Widget* findWidget(const String& id);
  ModulinoSub* findModulino(const String& kind);
  bool wantsAnyButton() const { return wantsButton[0] || wantsButton[1]; }
};

// A board found over mDNS.
struct BoardEntry {
  String hostname;
  String ip;
  String appName;   // filled in by the /info probe
  uint16_t port = NESSO_DEFAULT_PORT;
  bool hasCompanion = false;
};

extern CompanionConfig gConfig;
extern BoardEntry gBoards[MAX_BOARDS];
extern uint8_t gBoardCount;

// Turns "accelerometerX" into the short label the dashboard shows.
String shortSensorLabel(const String& name);

// Formats a float the way a 135px wide screen wants it.
String formatValue(float v, uint8_t decimals = 2);
