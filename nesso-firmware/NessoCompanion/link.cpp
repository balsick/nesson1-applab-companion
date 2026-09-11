#include "link.h"

#include <WebSocketsClient.h>
#include <WiFi.h>

namespace companion {

static WebSocketsClient ws;
static bool started = false;
static bool connected = false;
static bool handshaked = false;
static String label;

static HandshakeHandler handshakeHandler = nullptr;
static WidgetHandler widgetHandler = nullptr;
static ModulinoOutHandler modulinoOutHandler = nullptr;
static StatusHandler statusHandler = nullptr;

void onHandshake(HandshakeHandler handler) { handshakeHandler = handler; }
void onWidget(WidgetHandler handler) { widgetHandler = handler; }
void onModulinoOut(ModulinoOutHandler handler) { modulinoOutHandler = handler; }
void onStatus(StatusHandler handler) { statusHandler = handler; }

bool isConnected() { return connected; }
bool handshakeDone() { return handshaked; }
const String& boardLabel() { return label; }

// --- Outbound ------------------------------------------------------------

static void send(const JsonDocument& doc) {
  if (!connected) return;
  String payload;
  serializeJson(doc, payload);
  ws.sendTXT(payload);
}

static void sendHello() {
  JsonDocument doc;
  doc["t"] = "hello";
  doc["proto"] = NESSO_PROTOCOL_VERSION;
  doc["device"] = "nesso-n1";
  doc["id"] = WiFi.macAddress();
  doc["name"] = "Nesso " + WiFi.macAddress().substring(12);
  doc["fw"] = NESSO_FIRMWARE_VERSION;

  JsonObject caps = doc["caps"].to<JsonObject>();
  JsonArray sensors = caps["sensors"].to<JsonArray>();
  for (const char* name : {"accelerometerX", "accelerometerY", "accelerometerZ", "gyroscopeX",
                           "gyroscopeY", "gyroscopeZ", "pitch", "roll", "batteryLevel",
                           "batteryVoltage"}) {
    sensors.add(name);
  }
  JsonArray buttons = caps["buttons"].to<JsonArray>();
  buttons.add("button_1");
  buttons.add("button_2");

  JsonArray screen = caps["screen"].to<JsonArray>();
  screen.add(SCREEN_W);
  screen.add(SCREEN_H);

  JsonArray widgets = caps["widgets"].to<JsonArray>();
  for (const char* kind : {"value", "button", "switch", "slider", "stepper"}) widgets.add(kind);

  JsonArray modulinos = caps["modulinos"].to<JsonArray>();
  for (const char* kind : {"knob", "buttons", "movement", "pixels"}) modulinos.add(kind);

  send(doc);
}

void sendValues(const String* const names[], const float* values, uint8_t count) {
  if (!handshaked || count == 0) return;
  JsonDocument doc;
  doc["t"] = "data";
  doc["ts"] = millis();
  JsonObject v = doc["v"].to<JsonObject>();
  for (uint8_t i = 0; i < count; i++) v[*names[i]] = values[i];
  send(doc);
}

void sendButtonEvent(const char* name, const char* action) {
  if (!handshaked) return;
  JsonDocument doc;
  doc["t"] = "event";
  doc["name"] = name;
  doc["action"] = action;
  doc["ts"] = millis();
  send(doc);
}

void sendWidgetValue(const Widget& widget) {
  if (!handshaked) return;
  JsonDocument doc;
  doc["t"] = "ui";
  doc["id"] = widget.id;
  switch (widget.kind) {
    case WK_SWITCH: doc["value"] = widget.flag; break;
    case WK_SLIDER:
    case WK_STEPPER: doc["value"] = widget.number; break;
    case WK_BUTTON: doc["value"] = true; break;
    default: doc["value"] = widget.text; break;
  }
  send(doc);
}

void sendModulinoEvent(const char* kind, const char* event, float value) {
  if (!handshaked) return;
  JsonDocument doc;
  doc["t"] = "modulino";
  doc["kind"] = kind;
  doc["event"] = event;
  if (isnan(value)) {
    doc["value"] = nullptr;
  } else {
    doc["value"] = value;
  }
  send(doc);
}

void sendModulinoStatus() {
  if (!handshaked) return;
  JsonDocument doc;
  doc["t"] = "status";
  JsonArray mods = doc["modulinos"].to<JsonArray>();
  for (uint8_t i = 0; i < gConfig.modulinoCount; i++) {
    JsonObject item = mods.add<JsonObject>();
    item["kind"] = gConfig.modulinos[i].kind;
    item["present"] = gConfig.modulinos[i].present;
  }
  send(doc);
}

void sendLog(const char* level, const String& message) {
  if (!connected) return;
  JsonDocument doc;
  doc["t"] = "log";
  doc["level"] = level;
  doc["msg"] = message;
  send(doc);
}

// --- Inbound -------------------------------------------------------------

static WidgetKind parseWidgetKind(const char* kind) {
  if (!strcmp(kind, "button")) return WK_BUTTON;
  if (!strcmp(kind, "switch")) return WK_SWITCH;
  if (!strcmp(kind, "slider")) return WK_SLIDER;
  if (!strcmp(kind, "stepper")) return WK_STEPPER;
  return WK_VALUE;
}

// Turns whatever the board sent into the widget's own representation.
static void applyWidgetValue(Widget& widget, JsonVariantConst value) {
  switch (widget.kind) {
    case WK_SWITCH:
      widget.flag = value.as<bool>();
      break;
    case WK_SLIDER:
    case WK_STEPPER:
      widget.number = value.as<float>();
      break;
    default:
      if (value.isNull()) {
        widget.text = "-";
      } else if (value.is<const char*>()) {
        widget.text = value.as<const char*>();
      } else if (value.is<bool>()) {
        widget.text = value.as<bool>() ? "true" : "false";
      } else if (value.is<float>()) {
        float f = value.as<float>();
        widget.text = (f == (long)f) ? String((long)f) : formatValue(f, 2);
      } else {
        widget.text = value.as<String>();
      }
      break;
  }
}

static void applyConfig(JsonDocument& doc) {
  gConfig.clear();
  gConfig.revision = doc["rev"] | 0;
  gConfig.appName = String(doc["app"] | "Arduino App");

  for (JsonObjectConst item : doc["sensors"].as<JsonArrayConst>()) {
    if (gConfig.sensorCount >= MAX_SENSORS) break;
    SensorSub& sub = gConfig.sensors[gConfig.sensorCount];
    sub.name = String(item["name"] | "");
    if (sub.name.isEmpty()) continue;
    sub.rateHz = item["rate_hz"] | 10;
    if (sub.rateHz == 0) sub.rateHz = 1;
    sub.periodMs = 1000UL / sub.rateHz;
    sub.nextDueMs = millis();
    gConfig.sensorCount++;
  }

  for (JsonObjectConst item : doc["buttons"].as<JsonArrayConst>()) {
    String name = String(item["name"] | "");
    if (name == "button_1") gConfig.wantsButton[0] = true;
    if (name == "button_2") gConfig.wantsButton[1] = true;
  }

  for (JsonObjectConst item : doc["modulinos"].as<JsonArrayConst>()) {
    if (gConfig.modulinoCount >= MAX_MODULINOS) break;
    ModulinoSub& sub = gConfig.modulinos[gConfig.modulinoCount];
    sub.kind = String(item["kind"] | "");
    if (sub.kind.isEmpty()) continue;
    String dir = String(item["dir"] | "in");
    sub.wantsIn = (dir == "in" || dir == "both");
    sub.wantsOut = (dir == "out" || dir == "both");
    gConfig.modulinoCount++;
  }

  for (JsonObjectConst item : doc["ui"].as<JsonArrayConst>()) {
    if (gConfig.widgetCount >= MAX_WIDGETS) break;
    Widget& widget = gConfig.widgets[gConfig.widgetCount];
    widget.id = String(item["id"] | "");
    if (widget.id.isEmpty()) continue;
    widget.label = String(item["label"] | widget.id.c_str());
    widget.kind = parseWidgetKind(item["kind"] | "value");
    widget.vmin = item["min"] | 0.0f;
    widget.vmax = item["max"] | 100.0f;
    widget.vstep = item["step"] | 1.0f;
    if (widget.vstep <= 0) widget.vstep = 1.0f;
    widget.text = "-";
    applyWidgetValue(widget, item["value"]);
    gConfig.widgetCount++;
  }

  gConfig.valid = true;
  handshaked = true;

  Serial.printf("[link] handshake rev %u: %u sensor(s), buttons %d/%d, %u modulino(s), %u widget(s)\n",
                gConfig.revision, gConfig.sensorCount, gConfig.wantsButton[0], gConfig.wantsButton[1],
                gConfig.modulinoCount, gConfig.widgetCount);

  if (handshakeHandler) handshakeHandler();
}

static void handleFrame(uint8_t* payload, size_t length) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[link] bad frame: %s\n", err.c_str());
    return;
  }

  String type = String(doc["t"] | "");
  if (type == "config") {
    applyConfig(doc);
  } else if (type == "value") {
    JsonObjectConst bulk = doc["values"];
    if (!bulk.isNull()) {
      for (JsonPairConst pair : bulk) {
        Widget* widget = gConfig.findWidget(String(pair.key().c_str()));
        if (!widget) continue;
        applyWidgetValue(*widget, pair.value());
        if (widgetHandler) widgetHandler(*widget);
      }
    } else {
      Widget* widget = gConfig.findWidget(String(doc["id"] | ""));
      if (widget) {
        applyWidgetValue(*widget, doc["value"]);
        if (widgetHandler) widgetHandler(*widget);
      }
    }
  } else if (type == "modulino") {
    if (modulinoOutHandler) modulinoOutHandler(String(doc["kind"] | ""), doc["value"]);
  } else if (type == "ping") {
    JsonDocument pong;
    pong["t"] = "pong";
    send(pong);
  }
}

static void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      connected = true;
      handshaked = false;
      Serial.printf("[link] connected to %s\n", label.c_str());
      sendHello();
      if (statusHandler) statusHandler(true);
      break;
    case WStype_DISCONNECTED:
      if (connected) Serial.println("[link] disconnected");
      connected = false;
      handshaked = false;
      gConfig.valid = false;
      if (statusHandler) statusHandler(false);
      break;
    case WStype_TEXT:
      handleFrame(payload, length);
      break;
    default:
      break;
  }
}

// --- Lifecycle -----------------------------------------------------------

void begin(const BoardEntry& board) {
  stop();
  label = board.hostname.isEmpty() ? board.ip : board.hostname;
  Serial.printf("[link] dialling ws://%s:%u%s\n", board.ip.c_str(), board.port, NESSO_WS_PATH);

  ws.begin(board.ip.c_str(), board.port, NESSO_WS_PATH);
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(WS_RECONNECT_MS);
  // Keep-alive so a board that vanishes is noticed within a few seconds.
  ws.enableHeartbeat(15000, 3000, 2);
  started = true;
}

void loop() {
  if (!started) return;
  // The socket does not need servicing thousands of times a second, and this
  // was the single biggest consumer of loop time when it was unthrottled.
  static uint32_t lastPollMs = 0;
  uint32_t now = millis();
  if (now - lastPollMs < WS_POLL_MS) return;
  lastPollMs = now;
  ws.loop();
}

void stop() {
  if (!started) return;
  ws.disconnect();
  started = false;
  connected = false;
  handshaked = false;
  gConfig.valid = false;
}

}  // namespace companion
