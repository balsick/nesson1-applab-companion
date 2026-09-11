#include "state.h"

CompanionConfig gConfig;
BoardEntry gBoards[MAX_BOARDS];
uint8_t gBoardCount = 0;

void CompanionConfig::clear() {
  valid = false;
  revision = 0;
  appName = "";
  sensorCount = 0;
  widgetCount = 0;
  modulinoCount = 0;
  wantsButton[0] = false;
  wantsButton[1] = false;
  for (uint8_t i = 0; i < MAX_SENSORS; i++) {
    sensors[i] = SensorSub();
  }
  for (uint8_t i = 0; i < MAX_WIDGETS; i++) {
    widgets[i] = Widget();
  }
  for (uint8_t i = 0; i < MAX_MODULINOS; i++) {
    modulinos[i] = ModulinoSub();
  }
}

SensorSub* CompanionConfig::findSensor(const String& name) {
  for (uint8_t i = 0; i < sensorCount; i++) {
    if (sensors[i].name == name) return &sensors[i];
  }
  return nullptr;
}

Widget* CompanionConfig::findWidget(const String& id) {
  for (uint8_t i = 0; i < widgetCount; i++) {
    if (widgets[i].id == id) return &widgets[i];
  }
  return nullptr;
}

ModulinoSub* CompanionConfig::findModulino(const String& kind) {
  for (uint8_t i = 0; i < modulinoCount; i++) {
    if (modulinos[i].kind == kind) return &modulinos[i];
  }
  return nullptr;
}

String shortSensorLabel(const String& name) {
  if (name.startsWith("accelerometer")) return "acc " + name.substring(13);
  if (name.startsWith("gyroscope")) return "gyr " + name.substring(9);
  if (name == "batteryLevel") return "batt %";
  if (name == "batteryVoltage") return "batt V";
  return name;
}

String formatValue(float v, uint8_t decimals) {
  char buf[16];
  dtostrf(v, 0, decimals, buf);
  return String(buf);
}
