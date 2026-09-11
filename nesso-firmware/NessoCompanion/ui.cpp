#include "ui.h"

#include "hw.h"

namespace ui {

// Drawing goes through a full-screen sprite so the dashboard never flickers.
// If the allocation fails (it is 64 KB) everything falls back to the panel.
static LGFX_Sprite canvas(&display);
static bool buffered = false;

static const int16_t STATUS_H = 16;
static const int16_t HEADER_H = 26;
static const int16_t ROW_H = 46;
static const int16_t PICK_TOP = 44;
static const int16_t PICK_ROW_H = 30;

static int16_t scrollOffset = 0;
static int8_t selection = -1;

static LovyanGFX* target() { return buffered ? (LovyanGFX*)&canvas : (LovyanGFX*)&display; }

static void flush() {
  if (buffered) canvas.pushSprite(0, 0);
}

void begin() {
  canvas.setColorDepth(16);
  buffered = canvas.createSprite(SCREEN_W, SCREEN_H);
  Serial.printf("[ui] framebuffer %s\n", buffered ? "enabled" : "unavailable, drawing direct");
}

// Streams the framebuffer as base64 RGB565 between two markers. The host side
// is tools/nesso_screenshot.py; this is the only way to see what the 135x240
// panel is actually drawing without pointing a camera at it.
void dumpScreen() {
  if (!buffered) {
    Serial.println("[screen] no framebuffer, cannot dump");
    return;
  }
  static const char B64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  const uint8_t* data = (const uint8_t*)canvas.getBuffer();
  const uint32_t length = canvas.bufferLength();
  Serial.printf("[screen] begin %d %d rgb565 %u\n", SCREEN_W, SCREEN_H, length);

  char line[89];
  uint8_t col = 0;
  for (uint32_t i = 0; i < length; i += 3) {
    uint32_t chunk = data[i] << 16;
    if (i + 1 < length) chunk |= data[i + 1] << 8;
    if (i + 2 < length) chunk |= data[i + 2];

    line[col++] = B64[(chunk >> 18) & 0x3F];
    line[col++] = B64[(chunk >> 12) & 0x3F];
    line[col++] = (i + 1 < length) ? B64[(chunk >> 6) & 0x3F] : '=';
    line[col++] = (i + 2 < length) ? B64[chunk & 0x3F] : '=';

    if (col >= 88) {
      line[col] = 0;
      Serial.println(line);
      col = 0;
      delay(1);  // let the USB CDC drain
    }
  }
  if (col) {
    line[col] = 0;
    Serial.println(line);
  }
  Serial.println("[screen] end");
}

// --- Shared chrome -------------------------------------------------------

static void drawStatusBar(LovyanGFX* g, const String& left) {
  g->fillRect(0, 0, SCREEN_W, STATUS_H, COL_PANEL_ALT);
  g->setTextSize(1);
  g->setTextColor(COL_MUTED, COL_PANEL_ALT);
  g->setTextDatum(textdatum_t::middle_left);
  g->drawString(left.substring(0, 16), 4, STATUS_H / 2);

  float level = hw::batteryLevel();
  uint16_t colour = level > 40 ? COL_GREEN : (level > 15 ? COL_ORANGE : COL_RED);
  g->setTextColor(colour, COL_PANEL_ALT);
  g->setTextDatum(textdatum_t::middle_right);
  g->drawString(String((int)level) + "%", SCREEN_W - 4, STATUS_H / 2);
}

static void centeredText(LovyanGFX* g, const String& text, int16_t y, uint8_t size, uint16_t colour) {
  g->setTextSize(size);
  g->setTextColor(colour, COL_BG);
  g->setTextDatum(textdatum_t::middle_center);
  g->drawString(text, SCREEN_W / 2, y);
}

// --- Status screens ------------------------------------------------------

void showSplash() {
  LovyanGFX* g = target();
  g->fillScreen(COL_BG);
  centeredText(g, "Nesso N1", 96, 2, COL_TEAL);
  centeredText(g, "companion", 120, 1, COL_MUTED);
  centeredText(g, NESSO_FIRMWARE_VERSION, 140, 1, COL_PANEL);
  flush();
}

void showMessage(const String& title, const String& line1, const String& line2, uint16_t accent,
                 const String& hint) {
  LovyanGFX* g = target();
  g->fillScreen(COL_BG);
  drawStatusBar(g, "");
  centeredText(g, title, 100, 2, accent);
  if (line1.length()) centeredText(g, line1, 130, 1, COL_TEXT);
  if (line2.length()) centeredText(g, line2, 146, 1, COL_MUTED);
  if (hint.length()) centeredText(g, hint, SCREEN_H - 12, 1, COL_PANEL);
  flush();
}

// Escapes the characters that are special inside a WIFI: URI, so an SSID
// containing one of them still scans correctly.
static String escapeWifiField(const String& value) {
  String out;
  out.reserve(value.length() + 4);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out += '\\';
    out += c;
  }
  return out;
}

void showPortal(const String& ssid, const String& url) {
  LovyanGFX* g = target();
  g->fillScreen(COL_BG);
  drawStatusBar(g, "setup");

  centeredText(g, "Wi-Fi setup", 26, 1, COL_MUTED);
  centeredText(g, "Scan to join", 40, 1, COL_TEAL);

  // The setup access point is open, so the QR only needs the SSID. A phone
  // camera joins the network straight from it, and the captive portal then
  // pops up on its own.
  String payload = "WIFI:T:nopass;S:" + escapeWifiField(ssid) + ";;";

  // A white card behind the code, sized so the modules end up 4px wide with a
  // full 4-module quiet zone: 133 = 5 (card padding) + 11 (qrcode's own offset)
  // + 100 (25 modules x 4px) + 11 + 5. Scanners get flaky below 4 modules.
  const int16_t box = 133, pad = 5, top = 50;
  const int16_t bx = (SCREEN_W - box) / 2;
  g->fillRect(bx, top, box, box, TFT_WHITE);
  g->qrcode(payload.c_str(), bx + pad, top + pad, box - 2 * pad, 1, false);

  centeredText(g, ssid, top + box + 12, 1, COL_TEAL);
  centeredText(g, url, 209, 1, COL_ACCENT);
  centeredText(g, "or use USB serial", 226, 1, COL_PANEL);
  flush();
}

// --- Board picker --------------------------------------------------------

static int16_t pickScroll = 0;
static const int16_t PICK_BOTTOM = SCREEN_H - 28;  // leave room for two hint lines

// Keeps the highlighted row inside the visible window.
static void followPickSelection(uint8_t selected) {
  int16_t top = PICK_TOP + selected * PICK_ROW_H;
  if (top - pickScroll < PICK_TOP) pickScroll = top - PICK_TOP;
  if (top - pickScroll + PICK_ROW_H > PICK_BOTTOM) pickScroll = top + PICK_ROW_H - PICK_BOTTOM;
  int16_t maxPick = max((int16_t)0, (int16_t)(gBoardCount * PICK_ROW_H - (PICK_BOTTOM - PICK_TOP)));
  pickScroll = constrain(pickScroll, (int16_t)0, maxPick);
}

void drawPicker(uint8_t selected, const String& networkName) {
  LovyanGFX* g = target();
  g->fillScreen(COL_BG);
  drawStatusBar(g, networkName);

  g->setTextSize(1);
  g->setTextColor(COL_TEXT, COL_BG);
  g->setTextDatum(textdatum_t::top_left);
  g->drawString("Pick a board", 6, STATUS_H + 6);
  g->drawFastHLine(0, PICK_TOP - 6, SCREEN_W, COL_PANEL);

  if (gBoardCount == 0) {
    centeredText(g, "no boards", 120, 1, COL_MUTED);
    centeredText(g, "KEY2 to rescan", 140, 1, COL_PANEL);
    flush();
    return;
  }

  followPickSelection(selected);

  for (uint8_t i = 0; i < gBoardCount; i++) {
    int16_t y = PICK_TOP + i * PICK_ROW_H - pickScroll;
    if (y + PICK_ROW_H - 4 < PICK_TOP) continue;
    if (y + PICK_ROW_H - 4 > PICK_BOTTOM) break;

    bool active = (i == selected);
    const BoardEntry& board = gBoards[i];
    uint16_t bg = active ? COL_TEAL : COL_PANEL;
    g->fillRoundRect(3, y, SCREEN_W - 6, PICK_ROW_H - 4, 4, bg);

    String name = board.hostname;
    name.replace(".local.", "");
    name.replace(".local", "");

    // Boards without a companion App are dimmed: they are listed so you can see
    // them, not because you want to pick one.
    uint16_t nameColour = active ? COL_BG : (board.hasCompanion ? COL_TEXT : COL_MUTED);
    uint16_t subColour = active ? COL_BG : COL_PANEL_ALT;

    g->setTextSize(1);
    g->setTextDatum(textdatum_t::top_left);
    g->setTextColor(nameColour, bg);
    g->drawString(name.substring(0, 18), 8, y + 4);
    g->setTextColor(board.hasCompanion && !active ? COL_MUTED : subColour, bg);
    g->drawString(board.hasCompanion ? board.appName.substring(0, 18) : board.ip, 8, y + 15);

    g->fillCircle(SCREEN_W - 12, y + 12, 3, board.hasCompanion ? COL_GREEN : COL_PANEL_ALT);
  }

  g->setTextSize(1);
  g->setTextDatum(textdatum_t::bottom_center);
  g->setTextColor(COL_PANEL, COL_BG);
  g->drawString("KEY1 next  KEY2 pick", SCREEN_W / 2, SCREEN_H - 14);
  g->drawString("hold KEY2: wifi setup", SCREEN_W / 2, SCREEN_H - 3);
  flush();
}

int8_t pickerHit(int16_t x, int16_t y) {
  if (y < PICK_TOP || y > PICK_BOTTOM) return -1;
  int8_t index = (y + pickScroll - PICK_TOP) / PICK_ROW_H;
  return (index >= 0 && index < (int8_t)gBoardCount) ? index : -1;
}

void scrollPickerBy(int16_t pixels) {
  int16_t maxPick = max((int16_t)0, (int16_t)(gBoardCount * PICK_ROW_H - (PICK_BOTTOM - PICK_TOP)));
  pickScroll = constrain((int16_t)(pickScroll + pixels), (int16_t)0, maxPick);
}

// --- Dashboard -----------------------------------------------------------

static int16_t widgetY(uint8_t index) { return HEADER_H + index * ROW_H - scrollOffset; }

// Geometry shared by the drawing and the hit testing, so a control is always
// exactly as big as it looks.
static const int16_t CARD_X = 3;
static const int16_t CARD_W = SCREEN_W - 6;
static const int16_t CARD_H = ROW_H - 6;

static const int16_t TRACK_X = 10;
static const int16_t TRACK_W = SCREEN_W - 20;

static const int16_t STEP_BTN_W = 36;
static const int16_t STEP_BTN_H = 22;

static void drawWidget(LovyanGFX* g, Widget& widget, int16_t y, bool selected) {
  bool flash = widget.highlightUntilMs > millis();
  uint16_t bg = flash ? COL_TEAL : (selected ? COL_PANEL_ALT : COL_PANEL);
  uint16_t fg = flash ? COL_BG : COL_TEXT;

  g->fillRoundRect(CARD_X, y, CARD_W, CARD_H, 5, bg);
  if (selected && !flash) g->drawRoundRect(CARD_X, y, CARD_W, CARD_H, 5, COL_ACCENT);

  g->setTextSize(1);
  g->setTextDatum(textdatum_t::top_left);

  switch (widget.kind) {
    case WK_BUTTON: {
      // The whole card is the target.
      g->setTextColor(fg, bg);
      g->setTextDatum(textdatum_t::middle_center);
      g->setTextSize(2);
      g->drawString(widget.label.substring(0, 10), SCREEN_W / 2, y + CARD_H / 2);
      break;
    }
    case WK_SWITCH: {
      // The whole card toggles; the pill is just the state readout.
      g->setTextColor(COL_MUTED, bg);
      g->drawString(widget.label.substring(0, 11), 8, y + CARD_H / 2 - 4);
      int16_t pw = 46, ph = 24;
      int16_t px = SCREEN_W - 12 - pw, py = y + (CARD_H - ph) / 2;
      g->fillRoundRect(px, py, pw, ph, ph / 2, widget.flag ? COL_GREEN : COL_PANEL_ALT);
      g->fillCircle(widget.flag ? px + pw - ph / 2 : px + ph / 2, py + ph / 2, ph / 2 - 3, COL_TEXT);
      break;
    }
    case WK_SLIDER: {
      g->setTextColor(COL_MUTED, bg);
      g->drawString(widget.label.substring(0, 10), 8, y + 5);
      g->setTextColor(COL_TEXT, bg);
      g->setTextDatum(textdatum_t::top_right);
      g->drawString(formatValue(widget.number, 0), SCREEN_W - 10, y + 5);

      float span = widget.vmax - widget.vmin;
      float ratio = span > 0 ? (widget.number - widget.vmin) / span : 0;
      ratio = constrain(ratio, 0.0f, 1.0f);

      int16_t th = 12, ty = y + CARD_H - th - 6;
      g->fillRoundRect(TRACK_X, ty, TRACK_W, th, th / 2, COL_PANEL_ALT);
      g->fillRoundRect(TRACK_X, ty, (int16_t)(TRACK_W * ratio), th, th / 2, COL_ACCENT);
      // A knob big enough to aim at, and to read as draggable.
      int16_t kx = TRACK_X + (int16_t)(TRACK_W * ratio);
      kx = constrain(kx, (int16_t)(TRACK_X + 8), (int16_t)(TRACK_X + TRACK_W - 8));
      g->fillCircle(kx, ty + th / 2, 9, COL_TEXT);
      g->fillCircle(kx, ty + th / 2, 6, COL_ACCENT);
      break;
    }
    case WK_STEPPER: {
      g->setTextColor(COL_MUTED, bg);
      g->drawString(widget.label.substring(0, 11), 8, y + 4);

      int16_t by = y + CARD_H - STEP_BTN_H - 3;
      // Drawn exactly where the hit zones are: left and right thirds.
      g->fillRoundRect(CARD_X + 3, by, STEP_BTN_W, STEP_BTN_H, 5, COL_PANEL_ALT);
      g->fillRoundRect(SCREEN_W - CARD_X - 3 - STEP_BTN_W, by, STEP_BTN_W, STEP_BTN_H, 5, COL_PANEL_ALT);

      g->setTextSize(2);
      g->setTextDatum(textdatum_t::middle_center);
      g->setTextColor(COL_ACCENT, COL_PANEL_ALT);
      g->drawString("-", CARD_X + 3 + STEP_BTN_W / 2, by + STEP_BTN_H / 2);
      g->drawString("+", SCREEN_W - CARD_X - 3 - STEP_BTN_W / 2, by + STEP_BTN_H / 2);

      g->setTextColor(COL_TEXT, bg);
      g->drawString(formatValue(widget.number, 0), SCREEN_W / 2, by + STEP_BTN_H / 2);
      break;
    }
    default: {
      g->setTextColor(COL_MUTED, bg);
      g->drawString(widget.label.substring(0, 14), 8, y + 5);
      g->setTextDatum(textdatum_t::bottom_right);
      g->setTextColor(COL_TEXT, bg);
      g->setTextSize(2);
      g->drawString(widget.text.substring(0, 9), SCREEN_W - 10, y + CARD_H - 4);
      break;
    }
  }
}

// A compact readout of the live sensor values, so the screen shows something
// useful even when the App declared no widgets at all.
static void drawSensorStrip(LovyanGFX* g, int16_t y) {
  if (gConfig.sensorCount == 0) return;
  g->setTextSize(1);
  g->setTextDatum(textdatum_t::top_left);
  for (uint8_t i = 0; i < gConfig.sensorCount && y < SCREEN_H - 10; i++) {
    SensorSub& sub = gConfig.sensors[i];
    g->setTextColor(COL_MUTED, COL_BG);
    g->drawString(shortSensorLabel(sub.name).substring(0, 10), 8, y);
    g->setTextDatum(textdatum_t::top_right);
    g->setTextColor(COL_TEXT, COL_BG);
    g->drawString(sub.hasValue ? formatValue(sub.value, 2) : "-", SCREEN_W - 8, y);
    g->setTextDatum(textdatum_t::top_left);
    y += 11;
  }
}

void drawDashboard(bool linked, const String& boardName) {
  LovyanGFX* g = target();
  g->fillScreen(COL_BG);

  // Header: App name plus a dot for the link.
  g->fillRect(0, 0, SCREEN_W, HEADER_H - 4, COL_PANEL_ALT);
  g->setTextSize(1);
  g->setTextDatum(textdatum_t::middle_left);
  g->setTextColor(COL_TEXT, COL_PANEL_ALT);
  String title = gConfig.valid && gConfig.appName.length() ? gConfig.appName : boardName;
  g->drawString(title.substring(0, 16), 5, (HEADER_H - 4) / 2);
  g->fillCircle(SCREEN_W - 8, (HEADER_H - 4) / 2, 3, linked ? COL_GREEN : COL_RED);

  int16_t y = HEADER_H;
  for (uint8_t i = 0; i < gConfig.widgetCount; i++) {
    y = widgetY(i);
    if (y + ROW_H < HEADER_H) continue;
    if (y > SCREEN_H) break;
    drawWidget(g, gConfig.widgets[i], y, selection == (int8_t)i);
  }

  int16_t stripY = gConfig.widgetCount ? widgetY(gConfig.widgetCount) + 4 : HEADER_H + 4;
  if (stripY < SCREEN_H - 12) drawSensorStrip(g, stripY);

  if (!linked) {
    g->setTextSize(1);
    g->setTextColor(COL_RED, COL_BG);
    g->setTextDatum(textdatum_t::bottom_center);
    g->drawString("reconnecting...", SCREEN_W / 2, SCREEN_H - 3);
  }
  flush();
}

static int16_t maxScroll() {
  int16_t contentH = HEADER_H + gConfig.widgetCount * ROW_H + gConfig.sensorCount * 11 + 12;
  return max((int16_t)0, (int16_t)(contentH - SCREEN_H));
}

void scrollDashboardBy(int16_t pixels) {
  scrollOffset = constrain((int16_t)(scrollOffset + pixels), (int16_t)0, maxScroll());
}

// Maps an x inside the slider track onto the widget's value, snapped to step.
static void setSliderFromX(Widget& widget, int16_t x) {
  float ratio = constrain((float)(x - TRACK_X) / (float)TRACK_W, 0.0f, 1.0f);
  float raw = widget.vmin + ratio * (widget.vmax - widget.vmin);
  float snapped = widget.vmin + roundf((raw - widget.vmin) / widget.vstep) * widget.vstep;
  widget.number = constrain(snapped, widget.vmin, widget.vmax);
}

static Widget* applyTap(Widget& widget, int16_t x) {
  widget.highlightUntilMs = millis() + 150;
  switch (widget.kind) {
    case WK_BUTTON:
      return &widget;
    case WK_SWITCH:
      widget.flag = !widget.flag;
      return &widget;
    case WK_SLIDER: {
      float before = widget.number;
      setSliderFromX(widget, x);
      return widget.number != before ? &widget : nullptr;
    }
    case WK_STEPPER: {
      // The hit zones are the left and right thirds, which is exactly where the
      // - and + buttons are drawn.
      float delta = 0;
      if (x <= SCREEN_W / 3) delta = -widget.vstep;
      else if (x >= 2 * SCREEN_W / 3) delta = widget.vstep;
      if (delta == 0) return nullptr;
      float before = widget.number;
      widget.number = constrain(widget.number + delta, widget.vmin, widget.vmax);
      return widget.number != before ? &widget : nullptr;
    }
    default:
      return nullptr;
  }
}

// Returns the widget whose card contains y, or nullptr.
static Widget* widgetAt(int16_t y, uint8_t* indexOut = nullptr) {
  for (uint8_t i = 0; i < gConfig.widgetCount; i++) {
    int16_t top = widgetY(i);
    if (y >= top && y < top + CARD_H) {
      if (indexOut) *indexOut = i;
      return &gConfig.widgets[i];
    }
  }
  return nullptr;
}

Widget* dashboardTap(int16_t x, int16_t y) {
  uint8_t index = 0;
  Widget* widget = widgetAt(y, &index);
  if (!widget) return nullptr;
  selection = (int8_t)index;
  return applyTap(*widget, x);
}

Widget* dashboardDrag(const hw::Gesture& gesture) {
  // A sideways drag that started on a slider adjusts it; everything else
  // scrolls the list under the finger.
  if (gesture.horizontal) {
    Widget* widget = widgetAt(gesture.originY);
    if (widget && widget->kind == WK_SLIDER) {
      float before = widget->number;
      setSliderFromX(*widget, gesture.x);
      return widget->number != before ? widget : nullptr;
    }
  }
  scrollDashboardBy(-gesture.stepDy);
  return nullptr;
}

void moveSelection(int8_t delta) {
  if (gConfig.widgetCount == 0) {
    selection = -1;
    return;
  }
  int16_t next = selection + delta;
  if (next < 0) next = gConfig.widgetCount - 1;
  if (next >= gConfig.widgetCount) next = 0;
  selection = (int8_t)next;

  // Keep the selected row on screen.
  int16_t top = HEADER_H + selection * ROW_H;
  if (top - scrollOffset < HEADER_H) scrollOffset = top - HEADER_H;
  if (top - scrollOffset + ROW_H > SCREEN_H) scrollOffset = top + ROW_H - SCREEN_H;
  scrollOffset = constrain(scrollOffset, (int16_t)0, maxScroll());
}

Widget* activateSelection() {
  if (selection < 0 || selection >= (int8_t)gConfig.widgetCount) return nullptr;
  Widget& widget = gConfig.widgets[selection];
  // Without a touch position, act on the middle of the row: a stepper gets no
  // unambiguous direction there, so nudge it up by one step.
  if (widget.kind == WK_STEPPER) {
    widget.highlightUntilMs = millis() + 150;
    widget.number = constrain(widget.number + widget.vstep, widget.vmin, widget.vmax);
    return &widget;
  }
  if (widget.kind == WK_SLIDER) {
    widget.highlightUntilMs = millis() + 150;
    float next = widget.number + widget.vstep;
    widget.number = next > widget.vmax ? widget.vmin : next;
    return &widget;
  }
  return applyTap(widget, SCREEN_W / 2);
}

bool hasSelection() { return selection >= 0; }

}  // namespace ui
