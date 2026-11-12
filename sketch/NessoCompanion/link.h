// The WebSocket link to the companion brick running on the Arduino board:
// handshake, inbound frames, outbound frames.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"
#include "state.h"

namespace companion {

// Fires once the board's `config` frame has been parsed into gConfig.
typedef void (*HandshakeHandler)();
// Fires when the board pushes a new value for a widget.
typedef void (*WidgetHandler)(Widget& widget);
// Fires when the board pushes data for an output Modulino.
typedef void (*ModulinoOutHandler)(const String& kind, JsonVariantConst value);
// Fires on connect/disconnect so the UI can follow along.
typedef void (*StatusHandler)(bool connected);

void onHandshake(HandshakeHandler handler);
void onWidget(WidgetHandler handler);
void onModulinoOut(ModulinoOutHandler handler);
void onStatus(StatusHandler handler);

// Opens the socket to the chosen board. Non-blocking; drive it with loop().
void begin(const BoardEntry& board);
void loop();
void stop();

bool isConnected();
bool handshakeDone();
const String& boardLabel();

// --- Outbound ------------------------------------------------------------
// One `data` frame carrying every sensor that came due in this tick.
void sendValues(const String* const names[], const float* values, uint8_t count);
void sendButtonEvent(const char* name, const char* action);
void sendWidgetValue(const Widget& widget);
void sendModulinoEvent(const char* kind, const char* event, float value);
// Tells the board which of the Modulinos it asked for are actually plugged in.
void sendModulinoStatus();
void sendLog(const char* level, const String& message);

}  // namespace companion
