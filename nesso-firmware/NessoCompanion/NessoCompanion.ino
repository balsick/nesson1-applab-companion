/*
  Nesso N1 Companion

  Turns an Arduino Nesso N1 into a companion device for an Arduino App running
  on a UNO Q or VentunoQ, paired with the `nesso_companion` Custom Brick.

  Flow:
    1. Wi-Fi credentials come from NVS, or from a captive portal on first boot.
    2. The Nesso browses mDNS for `_arduino._tcp` and probes each board on the
       companion port to see which ones run a companion App.
    3. You pick one on the touch screen (or with KEY1/KEY2).
    4. The board answers the handshake with exactly what it wants: which IMU
       values at which rate, which buttons, which Modulinos, and which widgets
       to draw. Nothing else is powered up or streamed.

  Recovery gestures:
    - hold both keys for 1.5 s      -> drop the link, back to the board picker
    - hold both keys while booting  -> forget the Wi-Fi credentials

  The serial console is an alternative to the captive portal when the Nesso is
  already on a USB cable. Type `help` at 115200 baud.

  Board: Arduino Nesso N1 (esp32:esp32:arduino_nesso_n1)
*/

#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"
#include "hw.h"
#include "link.h"
#include "net.h"
#include "state.h"
#include "ui.h"

enum AppState : uint8_t {
  ST_PROVISION,
  ST_CONNECTING,
  ST_DISCOVER,
  ST_PICK,
  ST_LINK,
};

static AppState state = ST_CONNECTING;
static bool stateEntered = false;
static uint8_t pickIndex = 0;
static uint8_t wifiAttempts = 0;
static uint16_t companionPort = NESSO_DEFAULT_PORT;
static uint32_t lastDrawMs = 0;
static bool exitGestureLatched = false;

// Screen idle. The keys are a remote, so they always act and wake the screen as
// a side effect; a touch on a dark screen only wakes it, because reaching for a
// device you cannot see should not press whatever happens to be under a finger.
static uint32_t lastInputMs = 0;
static uint32_t idleAfterMs = SCREEN_IDLE_MS;

// Returns true when this input was spent waking the screen.
static bool noteInput() {
  lastInputMs = millis();
  if (hw::backlightOn()) return false;
  hw::setBacklight(true);
  lastDrawMs = 0;
  return true;
}

// Loop timing, reported by the `status` command.
static uint32_t loopLastUs = 0;
static uint32_t loopMaxUs = 0;
static uint32_t loopSumUs = 0;
static uint32_t loopCount = 0;

// Where the time actually goes, in microseconds since the last `status`.
static uint32_t tWs = 0, tStream = 0, tModulino = 0, tDraw = 0, tSensors = 0;

#define TIME_INTO(bucket, call)      \
  do {                               \
    uint32_t _t0 = micros();         \
    call;                            \
    bucket += micros() - _t0;        \
  } while (0)

// Link supervision: a board can accept the socket and never finish the
// handshake (wrong port, App not running), and a linked board can vanish. Both
// end up back at the picker rather than retrying forever.
static uint32_t linkEnteredMs = 0;
static uint32_t linkLostSinceMs = 0;
static bool linkEverHandshaked = false;

static void enter(AppState next) {
  state = next;
  stateEntered = false;
}

// --- Handshake ------------------------------------------------------------

static void onHandshake() {
  // Only turn on the parts of the IMU the board actually subscribed to.
  bool wantAccel = false, wantGyro = false;
  for (uint8_t i = 0; i < gConfig.sensorCount; i++) {
    const String& name = gConfig.sensors[i].name;
    if (name.startsWith("accelerometer") || name == "pitch" || name == "roll") wantAccel = true;
    if (name.startsWith("gyroscope")) wantGyro = true;
  }
  hw::enableImu(wantAccel, wantGyro);
  hw::beginModulinos();
  companion::sendModulinoStatus();

  // Remember this board so discovery can still offer it if mDNS misses it.
  if (pickIndex < gBoardCount) {
    net::rememberBoard(gBoards[pickIndex].ip, gBoards[pickIndex].hostname);
  }
  companion::sendLog("info", "handshake applied on " + String(NESSO_FIRMWARE_VERSION));
  lastDrawMs = 0;  // force an immediate repaint
}

static void onWidgetUpdate(Widget& widget) {
  (void)widget;
  lastDrawMs = 0;
}

static void onLinkStatus(bool connected) {
  (void)connected;
  lastDrawMs = 0;
}

// --- Modulino output ------------------------------------------------------

static void onModulinoOut(const String& kind, JsonVariantConst value) {
  if (kind != "pixels") return;

  uint32_t colors[8] = {0};
  uint8_t count = 0;
  for (JsonVariantConst item : value.as<JsonArrayConst>()) {
    if (count >= 8) break;
    if (item.is<JsonArrayConst>()) {
      JsonArrayConst rgb = item.as<JsonArrayConst>();
      uint8_t r = rgb.size() > 0 ? (uint8_t)(rgb[0].as<int>()) : 0;
      uint8_t g = rgb.size() > 1 ? (uint8_t)(rgb[1].as<int>()) : 0;
      uint8_t b = rgb.size() > 2 ? (uint8_t)(rgb[2].as<int>()) : 0;
      colors[count++] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    } else {
      // Also accept a flat list of packed 0xRRGGBB values.
      colors[count++] = (uint32_t)item.as<uint32_t>();
    }
  }
  hw::writeModulinoPixels(colors, count);
}

static void onModulinoEvent(const char* kind, const char* event, float value) {
  companion::sendModulinoEvent(kind, event, value);
}

// A Modulino was plugged in (or stopped answering) after the handshake.
static void onModulinoPresence() {
  companion::sendModulinoStatus();
  lastDrawMs = 0;
}

// --- Keys -----------------------------------------------------------------

static void onKeyEvent(uint8_t index, const char* action) {
  // A key wakes the screen and still does its job: this is a remote first.
  noteInput();

  switch (state) {
    case ST_PICK:
      // Holding the select key is the way back to Wi-Fi setup without a cable.
      if (index == 1 && strcmp(action, "long_press") == 0) {
        enter(ST_PROVISION);
        return;
      }
      if (strcmp(action, "click") != 0) return;
      if (gBoardCount == 0) {
        enter(ST_DISCOVER);
      } else if (index == 0) {
        pickIndex = (pickIndex + 1) % gBoardCount;
        lastDrawMs = 0;
      } else {
        enter(ST_LINK);
      }
      return;

    case ST_LINK:
      // Before the handshake lands, either key backs out of a wrong choice.
      if (!linkEverHandshaked) {
        if (strcmp(action, "click") == 0) {
          companion::stop();
          enter(ST_PICK);
        }
        return;
      }
      // When the App subscribed to a key, that key belongs to the App and the
      // local dashboard is driven by touch instead.
      if (index < 2 && gConfig.wantsButton[index]) {
        companion::sendButtonEvent(index == 0 ? "button_1" : "button_2", action);
        return;
      }
      if (strcmp(action, "click") != 0) return;
      if (index == 0) {
        ui::moveSelection(1);
      } else {
        Widget* changed = ui::activateSelection();
        if (changed) companion::sendWidgetValue(*changed);
      }
      lastDrawMs = 0;
      return;

    default:
      return;
  }
}

// --- Sensor streaming -----------------------------------------------------

static void streamDueSensors() {
  if (!companion::handshakeDone() || gConfig.sensorCount == 0) return;

  uint32_t now = millis();
  // Pointers, not copies: this runs on every loop iteration and building a
  // dozen Arduino Strings each time churns the heap for nothing.
  const String* names[MAX_SENSORS];
  float values[MAX_SENSORS];
  uint8_t count = 0;

  for (uint8_t i = 0; i < gConfig.sensorCount; i++) {
    SensorSub& sub = gConfig.sensors[i];
    if ((int32_t)(now - sub.nextDueMs) < 0) continue;

    float value = 0;
    if (!hw::readStandardValue(sub.name, value)) {
      // Nothing on this Nesso produces that name; stop asking every tick.
      sub.nextDueMs = now + 5000;
      continue;
    }
    sub.value = value;
    sub.hasValue = true;
    sub.nextDueMs = now + sub.periodMs;
    names[count] = &sub.name;
    values[count] = value;
    count++;
  }
  if (count) companion::sendValues(names, values, count);
}

// --- States ---------------------------------------------------------------

static void runProvision() {
  if (!stateEntered) {
    stateEntered = true;
    net::startPortal();
    ui::showPortal(net::portalSsid(), net::portalUrl());
  }
  if (net::portalLoop()) {
    net::stopPortal();
    wifiAttempts = 0;
    enter(ST_CONNECTING);
  }
}

static void runConnecting() {
  if (!stateEntered) {
    stateEntered = true;
    if (!net::hasCredentials()) {
      enter(ST_PROVISION);
      return;
    }
    ui::showMessage("Wi-Fi", net::storedSsid(), "connecting...");
    companionPort = net::storedPort();
    if (net::connectStation()) {
      wifiAttempts = 0;
      enter(ST_DISCOVER);
    } else if (++wifiAttempts >= 3) {
      wifiAttempts = 0;
      ui::showMessage("Wi-Fi", "cannot connect", "starting setup", COL_ORANGE);
      delay(1500);
      enter(ST_PROVISION);
    } else {
      stateEntered = false;  // retry
    }
  }
}

static void runDiscover() {
  if (!stateEntered) {
    stateEntered = true;
    ui::showMessage("Scanning", "looking for", "Arduino boards");
    net::discoverBoards(companionPort);
    pickIndex = 0;
    // Jump straight to the first board that answered the companion probe.
    for (uint8_t i = 0; i < gBoardCount; i++) {
      if (gBoards[i].hasCompanion) {
        pickIndex = i;
        break;
      }
    }
    enter(ST_PICK);
  }
}

static void runPick() {
  if (!net::isConnected()) {
    enter(ST_CONNECTING);
    return;
  }
  uint32_t now = millis();
  if (hw::backlightOn() && now - lastDrawMs >= SCREEN_REFRESH_MS) {
    lastDrawMs = now;
    ui::drawPicker(pickIndex, net::storedSsid());
  }

  hw::Gesture drag;
  if (hw::takeDrag(drag) && !noteInput()) {
    ui::scrollPickerBy(-drag.stepDy);
    lastDrawMs = 0;
  }

  hw::TouchPoint tap;
  if (hw::takeTap(tap) && !noteInput()) {
    int8_t hit = ui::pickerHit(tap.x, tap.y);
    if (hit >= 0) {
      pickIndex = (uint8_t)hit;
      enter(ST_LINK);
    } else if (gBoardCount == 0) {
      enter(ST_DISCOVER);
    }
  }
}

static void runLink() {
  if (!stateEntered) {
    stateEntered = true;
    if (pickIndex >= gBoardCount) {
      enter(ST_DISCOVER);
      return;
    }
    linkEnteredMs = millis();
    linkLostSinceMs = 0;
    linkEverHandshaked = false;
    ui::showMessage("Linking", gBoards[pickIndex].hostname, gBoards[pickIndex].ip, COL_TEAL,
                    "tap or key: cancel");
    companion::begin(gBoards[pickIndex]);
    lastDrawMs = 0;
  }

  if (!net::isConnected()) {
    companion::stop();
    enter(ST_CONNECTING);
    return;
  }

  TIME_INTO(tWs, companion::loop());
  TIME_INTO(tStream, streamDueSensors());
  TIME_INTO(tModulino, hw::pollModulinos());

  uint32_t now = millis();

  // Supervise the link, so a wrong pick or a board that went away does not
  // leave the Nesso stuck on a "Linking..." screen forever.
  if (companion::handshakeDone()) {
    linkEverHandshaked = true;
    linkLostSinceMs = 0;
  } else if (!linkEverHandshaked) {
    if (now - linkEnteredMs > LINK_TIMEOUT_MS) {
      companion::stop();
      ui::showMessage("No App", "that board has no", "companion running", COL_ORANGE);
      delay(1600);
      enter(ST_PICK);
      return;
    }
  } else {
    if (linkLostSinceMs == 0) {
      linkLostSinceMs = now;
    } else if (now - linkLostSinceMs > RELINK_TIMEOUT_MS) {
      companion::stop();
      ui::showMessage("Lost", "the board stopped", "responding", COL_ORANGE);
      delay(1600);
      enter(ST_DISCOVER);
      return;
    }
  }

  hw::TouchPoint tap;
  if (hw::takeTap(tap) && !noteInput()) {
    if (!linkEverHandshaked) {
      // A tap cancels a link that has not come up yet.
      companion::stop();
      enter(ST_PICK);
      return;
    }
    Widget* changed = ui::dashboardTap(tap.x, tap.y);
    if (changed) companion::sendWidgetValue(*changed);
    lastDrawMs = 0;
  }

  // Dragging is the only way to scroll once the App has claimed both keys.
  hw::Gesture drag;
  if (hw::takeDrag(drag) && !noteInput()) {
    Widget* changed = ui::dashboardDrag(drag);
    if (changed) companion::sendWidgetValue(*changed);
    lastDrawMs = 0;
  }

  if (hw::backlightOn() && now - lastDrawMs >= SCREEN_REFRESH_MS) {
    lastDrawMs = now;
    if (companion::handshakeDone()) {
      TIME_INTO(tDraw, ui::drawDashboard(companion::isConnected(), companion::boardLabel()));
    } else {
      ui::showMessage("Linking", companion::boardLabel(),
                      companion::isConnected() ? "handshaking..." : "waiting...", COL_TEAL,
                      "tap or key: cancel");
    }
  }
}

// --- Serial console -------------------------------------------------------
// Provisioning over the USB cable, for when reaching for a phone to use the
// captive portal is the slower option.

static void printStatus() {
  static const char* NAMES[] = {"provisioning", "connecting", "discovering", "picking", "linked"};
  Serial.printf("[status] state=%s ssid='%s' ip=%s port=%u boards=%u link=%s handshake=%s\n",
                NAMES[state], net::storedSsid().c_str(),
                net::isConnected() ? net::localIp().c_str() : "-", companionPort, gBoardCount,
                companion::isConnected() ? "up" : "down",
                companion::handshakeDone() ? "done" : "pending");
  Serial.printf("[status] battery %.0f%% %.3fV charger=%s\n", hw::batteryLevel(),
                hw::batteryVoltage(), hw::chargeStatusName());
  Serial.printf("[status] screen %s, idle after %lus\n", hw::backlightOn() ? "awake" : "dark",
                (unsigned long)(idleAfterMs / 1000));

  Serial.printf("[status] loop avg %lu us, worst %lu us, over %lu iterations\n",
                loopCount ? (unsigned long)(loopSumUs / loopCount) : 0UL,
                (unsigned long)loopMaxUs, (unsigned long)loopCount);
  Serial.printf("[status] last modulino rescan %lu us\n",
                (unsigned long)hw::lastRescanMicros());
  Serial.printf("[status] spent: ws %lums  stream %lums  modulino %lums  draw %lums  i2c-poll %lums\n",
                (unsigned long)(tWs / 1000), (unsigned long)(tStream / 1000),
                (unsigned long)(tModulino / 1000), (unsigned long)(tDraw / 1000),
                (unsigned long)(tSensors / 1000));
  loopMaxUs = loopSumUs = loopCount = 0;
  tWs = tStream = tModulino = tDraw = tSensors = 0;

  if (gConfig.modulinoCount == 0) {
    Serial.println("[status] modulinos: none subscribed by this App");
    return;
  }
  for (uint8_t i = 0; i < gConfig.modulinoCount; i++) {
    const ModulinoSub& sub = gConfig.modulinos[i];
    Serial.printf("[status] modulino %-9s %s (%s)\n", sub.kind.c_str(),
                  sub.present ? "PRESENT" : "absent",
                  sub.wantsOut ? (sub.wantsIn ? "in+out" : "out") : "in");
  }
}

static void runCommand(String line) {
  line.trim();
  if (line.isEmpty()) return;
  // Host tools sometimes leave the tty in echo mode, which bounces our own log
  // back at us. Our log lines all start with a bracketed tag; ignore them.
  if (line.startsWith("[")) return;

  int space = line.indexOf(' ');
  String verb = space < 0 ? line : line.substring(0, space);
  String rest = space < 0 ? "" : line.substring(space + 1);
  rest.trim();

  if (verb == "wifi") {
    // Everything after the SSID is the password, so passwords may contain
    // spaces. SSIDs may not.
    int split = rest.indexOf(' ');
    String ssid = split < 0 ? rest : rest.substring(0, split);
    String pass = split < 0 ? "" : rest.substring(split + 1);
    if (ssid.isEmpty()) {
      Serial.println("usage: wifi <ssid> <password>");
      return;
    }
    net::saveCredentials(ssid, pass, net::storedPort());
    net::stopPortal();
    enter(ST_CONNECTING);
  } else if (verb == "port") {
    uint16_t port = (uint16_t)rest.toInt();
    if (!port) {
      Serial.println("usage: port <1-65535>");
      return;
    }
    net::saveCompanionPort(port);
    companionPort = port;
    if (net::isConnected()) enter(ST_DISCOVER);
  } else if (verb == "portal") {
    // Re-open the setup portal without wiping what is stored, to move the Nesso
    // to another network. It falls back to the saved one if left alone.
    enter(ST_PROVISION);
  } else if (verb == "forget") {
    net::forgetCredentials();
    enter(ST_PROVISION);
  } else if (verb == "scan") {
    enter(ST_DISCOVER);
  } else if (verb == "connect") {
    // Picking a board is normally a touch (or KEY2) action. This is the same
    // step over the cable, for scripting and for testing.
    if (gBoardCount == 0) {
      Serial.println("[console] no boards discovered, run 'scan' first");
      return;
    }
    if (rest.length()) {
      int index = rest.toInt();
      if (index < 0 || index >= (int)gBoardCount) {
        Serial.printf("[console] index out of range, 0..%u\n", gBoardCount - 1);
        return;
      }
      pickIndex = (uint8_t)index;
    }
    Serial.printf("[console] connecting to %s (%s)\n", gBoards[pickIndex].hostname.c_str(),
                  gBoards[pickIndex].ip.c_str());
    enter(ST_LINK);
  } else if (verb == "scroll") {
    // Test aid: drive the same scroll path a finger drag uses.
    int delta = rest.toInt();
    if (state == ST_PICK) ui::scrollPickerBy(delta);
    else ui::scrollDashboardBy(delta);
    lastDrawMs = 0;
  } else if (verb == "tap") {
    // Test aid: inject a tap at screen coordinates, through the same handlers
    // the touch controller feeds.
    int space2 = rest.indexOf(' ');
    if (space2 < 0) {
      Serial.println("usage: tap <x> <y>");
      return;
    }
    int16_t tx = rest.substring(0, space2).toInt();
    int16_t ty = rest.substring(space2 + 1).toInt();
    if (state == ST_LINK && companion::handshakeDone()) {
      Widget* changed = ui::dashboardTap(tx, ty);
      Serial.printf("[tap] %d,%d -> %s\n", tx, ty,
                    changed ? changed->id.c_str() : "nothing");
      if (changed) companion::sendWidgetValue(*changed);
    } else if (state == ST_PICK) {
      int8_t hit = ui::pickerHit(tx, ty);
      Serial.printf("[tap] %d,%d -> row %d\n", tx, ty, hit);
      if (hit >= 0) {
        pickIndex = (uint8_t)hit;
        enter(ST_LINK);
      }
    }
    lastDrawMs = 0;
  } else if (verb == "idle") {
    idleAfterMs = (uint32_t)rest.toInt() * 1000UL;
    Serial.printf("[screen] idle after %lus%s\n", (unsigned long)(idleAfterMs / 1000),
                  idleAfterMs ? "" : " (disabled)");
    noteInput();
  } else if (verb == "battery") {
    hw::dumpCharger();
  } else if (verb == "i2c") {
    hw::scanI2C();
  } else if (verb == "screen") {
    ui::dumpScreen();
  } else if (verb == "status") {
    printStatus();
  } else if (verb == "help") {
    Serial.println("commands: wifi <ssid> <pass> | port <n> | scan | connect [n] |");
    Serial.println("          status | screen | i2c | battery | idle <s> | portal | forget");
    Serial.println("  test:   tap <x> <y> | scroll <px>");
  } else {
    Serial.printf("unknown command '%s', try 'help'\n", verb.c_str());
  }
}

static void pollConsole() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length()) {
        runCommand(line);
        line = "";
      }
    } else if (line.length() < 160) {
      line += c;
    }
  }
}

// --- Arduino entry points -------------------------------------------------

void setup() {
  Serial.begin(115200);
  // The Nesso is normally plugged into the board's USB, so the CDC endpoint is
  // enumerated but nobody is reading it. With the default 100 ms TX timeout,
  // every print blocks for 100 ms once the ring buffer fills - which is enough
  // to stall a touch gesture, since pollTouch() logs on each press. Dropping
  // output instead of blocking keeps the loop real-time.
  Serial.setTxTimeoutMs(0);
  delay(300);
  Serial.println("\n[nesso] companion " NESSO_FIRMWARE_VERSION);
  Serial.println("[nesso] type 'help' for the serial console");

  hw::begin();
  ui::begin();
  ui::showSplash();

  hw::onKey(onKeyEvent);
  hw::onModulinoEvent(onModulinoEvent);
  hw::onModulinoPresence(onModulinoPresence);
  companion::onHandshake(onHandshake);
  companion::onWidget(onWidgetUpdate);
  companion::onModulinoOut(onModulinoOut);
  companion::onStatus(onLinkStatus);

  // Holding both keys through boot wipes the stored network.
  uint32_t until = millis() + 2000;
  while (millis() < until) {
    hw::pollKeys();
    if (hw::keyIsDown(0) && hw::keyIsDown(1)) {
      net::forgetCredentials();
      ui::showMessage("Wi-Fi", "credentials", "cleared", COL_ORANGE);
      delay(1500);
      break;
    }
    delay(10);
  }

  companionPort = net::storedPort();
  enter(net::hasCredentials() ? ST_CONNECTING : ST_PROVISION);
}

void loop() {
  uint32_t nowUs = micros();
  if (loopLastUs) {
    uint32_t dt = nowUs - loopLastUs;
    if (dt > loopMaxUs) loopMaxUs = dt;
    loopSumUs += dt;
    loopCount++;
  }
  loopLastUs = nowUs;

  pollConsole();
  // Blank the screen once it has been left alone for long enough. millis(), not
  // the micros() above: micros() wraps every 71 minutes, which is well inside
  // the length of a talk, and the two timebases diverge from that point on.
  if (idleAfterMs && hw::backlightOn() && millis() - lastInputMs > idleAfterMs) {
    hw::setBacklight(false);
  }

  TIME_INTO(tSensors, hw::pollKeys());
  TIME_INTO(tSensors, hw::pollTouch());
  TIME_INTO(tSensors, hw::pollBattery());
  TIME_INTO(tSensors, hw::pollImu());

  // Hold both keys to drop the link and go back to the board picker. Latched so
  // one long hold does not trigger it over and over.
  if (hw::bothKeysHeldFor(BOTH_KEYS_EXIT_MS)) {
    if (!exitGestureLatched && (state == ST_LINK || state == ST_PICK)) {
      exitGestureLatched = true;
      companion::stop();
      enter(ST_DISCOVER);
    }
  } else if (!hw::keyIsDown(0) && !hw::keyIsDown(1)) {
    exitGestureLatched = false;
  }

  switch (state) {
    case ST_PROVISION: runProvision(); break;
    case ST_CONNECTING: runConnecting(); break;
    case ST_DISCOVER: runDiscover(); break;
    case ST_PICK: runPick(); break;
    case ST_LINK: runLink(); break;
  }
}
