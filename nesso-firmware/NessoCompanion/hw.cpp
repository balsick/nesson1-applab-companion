#include "hw.h"

#include <Arduino_Modulino.h>
#include <math.h>

#include "state.h"

// Arduino_Nesso_N1.h defines IMU as the on-board BMI270, but Arduino_Modulino
// pulls in Arduino_LSM6DSOX, which redefines IMU as its own instance. Whoever
// is included last wins, so the on-board sensor is addressed by its real name
// (myIMU, declared extern by the Nesso library) rather than through the macro.
#undef IMU

NessoDisplay display;
NessoBattery battery;

namespace hw {

static NessoTouch touch;
static bool touchReady = false;

// --- Touch ---------------------------------------------------------------

static TouchPoint lastTouch;
static bool touchDown = false;
static uint8_t emptyPolls = 0;
static uint32_t lastTouchPollMs = 0;

static int16_t pressStartX = 0, pressStartY = 0;
static int16_t lastDragY = 0;
static bool dragging = false;
static Gesture pendingDrag;
static bool hasPendingDrag = false;

static TouchPoint pendingTap;
static bool hasPendingTap = false;

static void mapTouch(int16_t rawX, int16_t rawY, int16_t& x, int16_t& y) {
#if TOUCH_SWAP_XY
  int16_t tx = rawY, ty = rawX;
#else
  int16_t tx = rawX, ty = rawY;
#endif
#if TOUCH_INVERT_X
  tx = (SCREEN_W - 1) - tx;
#endif
#if TOUCH_INVERT_Y
  ty = (SCREEN_H - 1) - ty;
#endif
  x = constrain(tx, (int16_t)0, (int16_t)(SCREEN_W - 1));
  y = constrain(ty, (int16_t)0, (int16_t)(SCREEN_H - 1));
}

void pollTouch() {
  if (!touchReady) return;
  // The touch controller shares the I2C bus with the keys, the IMU and any
  // Modulino, so it gets a poll interval rather than every loop iteration.
  uint32_t now = millis();
  if (now - lastTouchPollMs < TOUCH_POLL_MS) return;
  lastTouchPollMs = now;

  int16_t rawX = 0, rawY = 0;
  if (touch.read(rawX, rawY)) {
    emptyPolls = 0;
    mapTouch(rawX, rawY, lastTouch.x, lastTouch.y);
    lastTouch.pressed = true;

    if (!touchDown) {
      touchDown = true;
      dragging = false;
      pressStartX = lastTouch.x;
      pressStartY = lastTouch.y;
      lastDragY = lastTouch.y;
      // One line per press keeps the calibration check cheap to eyeball.
      Serial.printf("[touch] raw=%d,%d -> screen=%d,%d\n", rawX, rawY, lastTouch.x, lastTouch.y);
      return;
    }

    int16_t dx = lastTouch.x - pressStartX;
    int16_t dy = lastTouch.y - pressStartY;
    if (!dragging && (abs(dx) > TAP_SLOP_PX || abs(dy) > TAP_SLOP_PX)) {
      dragging = true;
      // Commit to whichever axis moved more; a slider drag and a list scroll
      // must not fight each other halfway through the gesture.
      pendingDrag.horizontal = abs(dx) > abs(dy);
      pendingDrag.originX = pressStartX;
      pendingDrag.originY = pressStartY;
      pendingDrag.stepDy = 0;
    }
    if (dragging) {
      pendingDrag.x = lastTouch.x;
      pendingDrag.y = lastTouch.y;
      pendingDrag.stepDy += lastTouch.y - lastDragY;
      lastDragY = lastTouch.y;
      hasPendingDrag = true;
    }
    return;
  }

  if (!touchDown) return;

  // Tolerate the odd dropped sample rather than ending the gesture on it.
  if (++emptyPolls < TOUCH_RELEASE_POLLS) return;

  emptyPolls = 0;
  touchDown = false;
  lastTouch.pressed = false;
  if (!dragging) {
    pendingTap = lastTouch;
    pendingTap.pressed = false;
    hasPendingTap = true;
  }
  dragging = false;
}

bool takeTap(TouchPoint& out) {
  if (!hasPendingTap) return false;
  out = pendingTap;
  hasPendingTap = false;
  return true;
}

bool takeDrag(Gesture& out) {
  if (!hasPendingDrag) return false;
  out = pendingDrag;
  pendingDrag.stepDy = 0;
  hasPendingDrag = false;
  return true;
}

// --- Keys ----------------------------------------------------------------

struct KeyState {
  bool down = false;
  bool rawDown = false;
  uint32_t changedAtMs = 0;
  uint32_t pressedAtMs = 0;
  bool longSent = false;
  uint32_t pendingClickAtMs = 0;  // 0 = no click waiting to become a double
};

static KeyState keys[2];
static KeyHandler keyHandler = nullptr;
static uint32_t lastKeyPollMs = 0;

void onKey(KeyHandler handler) { keyHandler = handler; }

static void emit(uint8_t index, const char* action) {
  if (keyHandler) keyHandler(index, action);
}

static bool readKey(uint8_t index) {
  // The keys sit on the I2C expander and are wired active-low with a pull-up.
  return digitalRead(index == 0 ? KEY1 : KEY2) == LOW;
}

void pollKeys() {
  uint32_t now = millis();
  if (now - lastKeyPollMs < BUTTON_POLL_MS) return;
  lastKeyPollMs = now;

  for (uint8_t i = 0; i < 2; i++) {
    KeyState& k = keys[i];
    bool raw = readKey(i);
    if (raw != k.rawDown) {
      k.rawDown = raw;
      k.changedAtMs = now;
    }
    if (raw != k.down && (now - k.changedAtMs) >= BUTTON_DEBOUNCE_MS) {
      k.down = raw;
      if (k.down) {
        k.pressedAtMs = now;
        k.longSent = false;
        emit(i, "press");
      } else {
        emit(i, "release");
        uint32_t held = now - k.pressedAtMs;
        if (!k.longSent && held <= BUTTON_CLICK_MS) {
          if (k.pendingClickAtMs && (now - k.pendingClickAtMs) <= BUTTON_DOUBLE_MS) {
            k.pendingClickAtMs = 0;
            emit(i, "double_click");
          } else {
            k.pendingClickAtMs = now;
          }
        }
      }
    }
    if (k.down && !k.longSent && (now - k.pressedAtMs) >= BUTTON_LONG_MS) {
      k.longSent = true;
      emit(i, "long_press");
    }
    // A click that never grew into a double is reported once the window closes.
    if (k.pendingClickAtMs && (now - k.pendingClickAtMs) > BUTTON_DOUBLE_MS) {
      k.pendingClickAtMs = 0;
      emit(i, "click");
    }
  }
}

bool keyIsDown(uint8_t index) { return index < 2 && keys[index].down; }

bool bothKeysHeldFor(uint32_t ms) {
  if (!keys[0].down || !keys[1].down) return false;
  uint32_t now = millis();
  return (now - keys[0].pressedAtMs) >= ms && (now - keys[1].pressedAtMs) >= ms;
}

// --- IMU -----------------------------------------------------------------

static bool imuStarted = false;
static bool wantAccel = false;
static bool wantGyro = false;
static float ax = 0, ay = 0, az = 0;
static float gx = 0, gy = 0, gz = 0;
static float pitchDeg = 0, rollDeg = 0;

void enableImu(bool accel, bool gyro) {
  wantAccel = accel;
  wantGyro = gyro;
  if ((accel || gyro) && !imuStarted) {
    // The library reports the Bosch error code through this stream. Its
    // panic_led_trap() is a no-op on the Nesso, so this cannot hang the boot.
    myIMU.debug(Serial);
    imuStarted = myIMU.begin() != 0;
    Serial.printf("[imu] %s\n", imuStarted ? "started" : "NOT FOUND");
  }
}

bool imuReady() { return imuStarted; }

static uint32_t lastImuPollMs = 0;

void pollImu() {
  if (!imuStarted) return;
  uint32_t now = millis();
  if (now - lastImuPollMs < IMU_POLL_MS) return;
  lastImuPollMs = now;

  if (wantAccel && myIMU.accelerationAvailable()) {
    myIMU.readAcceleration(ax, ay, az);
    // Tilt from gravity. Good enough for a cursor or an orientation readout.
    pitchDeg = atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / PI;
    rollDeg = atan2f(ay, az) * 180.0f / PI;
  }
  if (wantGyro && myIMU.gyroscopeAvailable()) {
    myIMU.readGyroscope(gx, gy, gz);
  }
}

bool readStandardValue(const String& name, float& out) {
  if (name == "accelerometerX") { out = ax; return true; }
  if (name == "accelerometerY") { out = ay; return true; }
  if (name == "accelerometerZ") { out = az; return true; }
  if (name == "gyroscopeX") { out = gx; return true; }
  if (name == "gyroscopeY") { out = gy; return true; }
  if (name == "gyroscopeZ") { out = gz; return true; }
  if (name == "pitch") { out = pitchDeg; return true; }
  if (name == "roll") { out = rollDeg; return true; }
  if (name == "batteryLevel") { out = batteryLevel(); return true; }
  if (name == "batteryVoltage") { out = batteryVoltage(); return true; }
  return false;
}

// --- Battery -------------------------------------------------------------

static float battLevel = 0;
static float battVolts = 0;
static uint32_t lastBatteryMs = 0;

void pollBattery() {
  uint32_t now = millis();
  if (now - lastBatteryMs < BATTERY_POLL_MS) return;
  lastBatteryMs = now;
  battLevel = (float)battery.getChargeLevel();
  battVolts = battery.getVoltage();
}

float batteryLevel() { return battLevel; }
float batteryVoltage() { return battVolts; }

const char* chargeStatusName() {
  switch (battery.getChargeStatus()) {
    case NessoBattery::NOT_CHARGING: return "NOT_CHARGING";
    case NessoBattery::PRE_CHARGE:   return "PRE_CHARGE";
    case NessoBattery::CHARGING:     return "CHARGING";
    case NessoBattery::FULL_CHARGE:  return "FULL_CHARGE";
  }
  return "unknown";
}

static void writeChargerReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(NessoBattery::AW32001_I2C_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

static uint8_t readChargerReg(uint8_t reg) {
  Wire.beginTransmission(NessoBattery::AW32001_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom((uint8_t)NessoBattery::AW32001_I2C_ADDR, (uint8_t)1) != 1) return 0xFF;
  return Wire.read();
}

void dumpCharger() {
  static const char* NAMES[] = {"INPUT_SRC",  "POWER_ON_CFG",  "CHG_CURRENT", "TERM_CURRENT",
                                "CHG_VOLTAGE", "TIMER_WD",     "MAIN_CTRL",   "SYS_CTRL",
                                "SYS_STATUS", "FAULT_STATUS",  "CHIP_ID"};
  Serial.printf("[charger] status=%s  battery %.0f%% %.3fV\n", chargeStatusName(), battLevel,
                battVolts);
  for (uint8_t reg = 0; reg <= 0x0A; reg++) {
    uint8_t v = readChargerReg(reg);
    Serial.printf("[charger]   0x%02X %-13s = 0x%02X  %c%c%c%c%c%c%c%c\n", reg, NAMES[reg], v,
                  (v & 0x80) ? '1' : '0', (v & 0x40) ? '1' : '0', (v & 0x20) ? '1' : '0',
                  (v & 0x10) ? '1' : '0', (v & 0x08) ? '1' : '0', (v & 0x04) ? '1' : '0',
                  (v & 0x02) ? '1' : '0', (v & 0x01) ? '1' : '0');
  }
}

// --- Modulinos -----------------------------------------------------------

static ModulinoHandler modulinoHandler = nullptr;
static ModulinoKnob knob;
static ModulinoButtons mButtons;
static ModulinoMovement movement;
static ModulinoPixels pixels;
static ModulinoDistance distance;
static bool hasKnob = false, hasButtons = false, hasMovement = false, hasPixels = false;
static bool hasDistance = false;
static int16_t lastKnobValue = 0;
static bool lastKnobPressed = false;
static bool lastButtonState[3] = {false, false, false};
static float lastMoveX = NAN, lastMoveY = NAN, lastMoveZ = NAN;
static float lastDistanceMm = NAN;
static uint32_t lastDistanceSentMs = 0;
static uint32_t lastModulinoPollMs = 0;
static uint32_t lastModulinoScanMs = 0;
static uint32_t lastMovementSentMs = 0;
static uint32_t lastRescanUs = 0;

void onModulinoEvent(ModulinoHandler handler) { modulinoHandler = handler; }

static ModulinoPresenceHandler presenceHandler = nullptr;
void onModulinoPresence(ModulinoPresenceHandler handler) { presenceHandler = handler; }

static void emitModulino(const char* kind, const char* event, float value) {
  if (modulinoHandler) modulinoHandler(kind, event, value);
}

// Default I2C addresses the Modulino library probes for each kind. Probing them
// here first is what keeps the bus usable: when a module is absent, the
// library's own discovery leaves the shared bus wedged, and on the Nesso that
// same bus carries the IMU, the fuel gauge and the touch controller.
//
// These are 7-bit bus addresses. Careful: the Modulino library's own `match[]`
// tables hold 8-bit write addresses and it halves them (`scan()` does
// `beginTransmission(addr / 2)`), so a Pixels listed there as 0x6C answers at
// 0x36. Movement and Distance are plain sensors addressed directly, with no
// halving.
struct ModulinoProbe {
  const char* kind;
  uint8_t addresses[2];
};

static const ModulinoProbe MODULINO_PROBES[] = {
  {"knob", {0x3A, 0x3B}},      // library match[] 0x74 / 0x76
  {"buttons", {0x3E, 0x00}},   // library match[] 0x7C
  {"pixels", {0x36, 0x00}},    // library match[] 0x6C
  {"movement", {0x6A, 0x00}},  // LSM6DSOX on the Modulino Movement
  {"distance", {0x29, 0x00}},  // VL53L4CD/ED time-of-flight
};

static bool i2cPresent(uint8_t addr) {
  if (addr == 0x00) return false;
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static bool modulinoPresent(const String& kind) {
  for (const ModulinoProbe& probe : MODULINO_PROBES) {
    if (kind != probe.kind) continue;
    for (uint8_t addr : probe.addresses) {
      if (i2cPresent(addr)) return true;
    }
    return false;
  }
  return false;
}

// Puts the shared bus back into a known-good state: clock out any slave still
// holding SDA low, issue a STOP, then re-init the controller.
static void recoverI2C() {
  Wire.end();
  delay(2);

  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, OUTPUT);
  for (uint8_t i = 0; i < 9 && digitalRead(SDA) == LOW; i++) {
    digitalWrite(SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(SCL, HIGH);
    delayMicroseconds(5);
  }
  pinMode(SDA, OUTPUT);
  digitalWrite(SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(SCL, HIGH);
  delayMicroseconds(5);
  digitalWrite(SDA, HIGH);
  delayMicroseconds(5);

  Wire.begin();
  Wire.setClock(I2C_CLOCK_HZ);
}

static bool modulinoBusStarted = false;

// Brings up one module and returns whether it answered. Split out of
// beginModulinos() so the hot-plug rescan can reuse it.
static bool bringUpModulino(ModulinoSub& sub) {
  if (sub.kind == "knob") {
    hasKnob = knob.begin();
    if (hasKnob) lastKnobValue = knob.get();
    return hasKnob;
  }
  if (sub.kind == "buttons") {
    hasButtons = mButtons.begin();
    return hasButtons;
  }
  if (sub.kind == "movement") {
    hasMovement = movement.begin();
    return hasMovement;
  }
  if (sub.kind == "pixels") {
    hasPixels = pixels.begin();
    return hasPixels;
  }
  if (sub.kind == "distance") {
    // ModulinoDistance::begin() probes 0x29 itself before touching the ToF
    // driver, so a missing module cannot hang us here.
    hasDistance = distance.begin();
    return hasDistance;
  }
  return false;
}

uint8_t beginModulinos() {
  hasKnob = hasButtons = hasMovement = hasPixels = hasDistance = false;
  lastDistanceMm = NAN;
  modulinoBusStarted = false;
  if (gConfig.modulinoCount == 0) return 0;

  // Work out which of the requested modules are physically there before letting
  // the Modulino library touch the bus at all.
  bool anyPresent = false;
  for (uint8_t i = 0; i < gConfig.modulinoCount; i++) {
    gConfig.modulinos[i].present = modulinoPresent(gConfig.modulinos[i].kind);
    anyPresent |= gConfig.modulinos[i].present;
  }

  if (!anyPresent) {
    for (uint8_t i = 0; i < gConfig.modulinoCount; i++) {
      Serial.printf("[modulino] %s: absent (will keep looking)\n",
                    gConfig.modulinos[i].kind.c_str());
    }
    return 0;
  }

  // The Qwiic connector shares the internal I2C bus.
  Modulino.begin(Wire);
  modulinoBusStarted = true;

  uint8_t found = 0;
  for (uint8_t i = 0; i < gConfig.modulinoCount; i++) {
    ModulinoSub& sub = gConfig.modulinos[i];
    if (!sub.present) {
      Serial.printf("[modulino] %s: absent (will keep looking)\n", sub.kind.c_str());
      continue;
    }
    sub.present = bringUpModulino(sub);
    if (sub.present) found++;
    Serial.printf("[modulino] %s: %s\n", sub.kind.c_str(), sub.present ? "present" : "absent");
  }

  // Belt and braces: whatever the library did to the bus, hand it back clean.
  recoverI2C();
  return found;
}

// Re-probes the modules the App asked for that have not shown up yet.
static void rescanModulinos() {
  bool broughtUpAny = false;
  for (uint8_t i = 0; i < gConfig.modulinoCount; i++) {
    ModulinoSub& sub = gConfig.modulinos[i];
    if (sub.present) continue;
    // A bare address probe is harmless; only a real bring-up disturbs the bus.
    if (!modulinoPresent(sub.kind)) continue;

    if (!modulinoBusStarted) {
      Modulino.begin(Wire);
      modulinoBusStarted = true;
    }
    sub.present = bringUpModulino(sub);
    broughtUpAny = true;
    Serial.printf("[modulino] %s: %s\n", sub.kind.c_str(),
                  sub.present ? "plugged in" : "answered but would not start");
  }
  if (broughtUpAny) {
    recoverI2C();
    if (presenceHandler) presenceHandler();
  }
}

uint32_t lastRescanMicros() { return lastRescanUs; }

void pollModulinos() {
  uint32_t now = millis();

  if (now - lastModulinoScanMs >= MODULINO_RESCAN_MS) {
    lastModulinoScanMs = now;
    // Probing addresses that do not answer costs real time on a shared bus;
    // measure it so it can be ruled in or out when the UI feels sticky.
    uint32_t t0 = micros();
    rescanModulinos();
    lastRescanUs = micros() - t0;
  }

  if (now - lastModulinoPollMs < MODULINO_POLL_MS) return;
  lastModulinoPollMs = now;

  if (hasKnob) {
    int16_t value = knob.get();
    if (value != lastKnobValue) {
      lastKnobValue = value;
      emitModulino("knob", "rotate", (float)value);
    }
    bool pressed = knob.isPressed();
    if (pressed != lastKnobPressed) {
      lastKnobPressed = pressed;
      emitModulino("knob", pressed ? "press" : "release", NAN);
    }
  }

  if (hasButtons && mButtons.update()) {
    // update() only says something changed; report the buttons that actually
    // did, not all three.
    for (uint8_t i = 0; i < 3; i++) {
      bool pressed = mButtons.isPressed(i) == HIGH;
      if (pressed == lastButtonState[i]) continue;
      lastButtonState[i] = pressed;
      const char event[2] = {(char)('a' + i), 0};
      emitModulino("buttons", event, pressed ? 1.0f : 0.0f);
    }
  }

  if (hasMovement && now - lastMovementSentMs >= MOVEMENT_MIN_INTERVAL_MS && movement.update()) {
    // The accelerometer always has a fresh reading, so rate-limiting alone is
    // not enough: only send an axis that actually moved.
    float x = movement.getX(), y = movement.getY(), z = movement.getZ();
    bool sent = false;
    if (isnan(lastMoveX) || fabsf(x - lastMoveX) >= MOVEMENT_DELTA_G) {
      lastMoveX = x;
      emitModulino("movement", "x", x);
      sent = true;
    }
    if (isnan(lastMoveY) || fabsf(y - lastMoveY) >= MOVEMENT_DELTA_G) {
      lastMoveY = y;
      emitModulino("movement", "y", y);
      sent = true;
    }
    if (isnan(lastMoveZ) || fabsf(z - lastMoveZ) >= MOVEMENT_DELTA_G) {
      lastMoveZ = z;
      emitModulino("movement", "z", z);
      sent = true;
    }
    if (sent) lastMovementSentMs = now;
  }

  if (hasDistance && now - lastDistanceSentMs >= DISTANCE_MIN_INTERVAL_MS) {
    if (distance.available()) {
      float mm = distance.get();
      if (isnan(lastDistanceMm) || fabsf(mm - lastDistanceMm) >= DISTANCE_DELTA_MM) {
        lastDistanceMm = mm;
        lastDistanceSentMs = now;
        emitModulino("distance", "mm", mm);
      }
    } else if (!isnan(lastDistanceMm)) {
      // Nothing in range any more; say so once rather than going quiet.
      lastDistanceMm = NAN;
      lastDistanceSentMs = now;
      emitModulino("distance", "mm", NAN);
    }
  }
}

void writeModulinoPixels(const uint32_t* colors, uint8_t count) {
  if (!hasPixels) return;
  for (uint8_t i = 0; i < 8; i++) {
    if (i < count) {
      uint32_t c = colors[i];
      pixels.set(i, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 25);
    } else {
      pixels.clear(i);
    }
  }
  pixels.show();
}

void scanI2C() {
  Serial.println("[i2c] scanning 0x08..0x77");
  uint8_t found = 0;
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      const char* known = "";
      switch (addr) {
        case 0x38: known = " FT6336U touch"; break;
        case 0x43: known = " PI4IOE5V6408 expander 1"; break;
        case 0x44: known = " PI4IOE5V6408 expander 2"; break;
        case 0x49: known = " AW32001 charger"; break;
        case 0x55: known = " BQ27220 gauge"; break;
        case 0x68: known = " BMI270 IMU (primary)"; break;
        case 0x69: known = " BMI270 IMU (secondary)"; break;
        default: break;
      }
      Serial.printf("[i2c]   0x%02X%s\n", addr, known);
      found++;
    }
  }
  Serial.printf("[i2c] %u device(s)\n", found);
}

// --- Bring-up ------------------------------------------------------------

void begin() {
  display.begin();
  display.setRotation(SCREEN_ROTATION);
  display.setEpdMode(epd_mode_t::epd_fastest);
  display.fillScreen(COL_BG);

  pinMode(KEY1, INPUT_PULLUP);
  pinMode(KEY2, INPUT_PULLUP);

  battery.begin();

  // INPUT_SRC is written directly rather than through the library setters.
  // begin() never sets the input current limit at all, so the chip stays at its
  // power-on 260 mA - and since the charge current alone is 256 mA and the
  // Nesso draws about as much again with Wi-Fi and the backlight on, it reports
  // CHARGING while the battery steadily drains.
  //
  // setIinLimitCurrent() on its own would work, but setVinDPMVoltage() is buggy:
  // it clears bits 6-3 and writes bits 7-4, so the old bit 7 survives and the
  // DPM threshold ends up wrong (asking for 4200 mV yielded 4840 mV, above a
  // typical USB rail, which stops charging altogether).
  //
  //   bits 7-4  VINDPM = (mV - 3880) / 80  -> 8 = 4520 mV, the value that was
  //                                           observed charging on this supply
  //   bits 3-0  IINLIM = (mA - 50) / 30    -> 15 = 500 mA, the chip maximum
  writeChargerReg(NessoBattery::AW3200_INPUT_SRC, (8 << 4) | 15);
  battery.enableCharge();
  pollBattery();
  Serial.printf("[charger] %s at boot\n", chargeStatusName());

  touchReady = touch.begin();
  Serial.printf("[touch] %s\n", touchReady ? "ready" : "not found");

  // Everything above brought Wire up at the ESP32 default of 100 kHz. The keys,
  // touch controller, IMU and fuel gauge are all polled continuously, so run the
  // bus at the speed the board actually supports.
  Wire.setClock(I2C_CLOCK_HZ);
  Serial.printf("[i2c] bus at %d Hz\n", I2C_CLOCK_HZ);
}

}  // namespace hw
