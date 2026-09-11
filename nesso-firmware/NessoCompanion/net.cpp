#include "net.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

namespace net {

static Preferences prefs;
static const char* PREFS_NS = "nesso";

static DNSServer dns;
static WebServer server(80);
static bool portalRunning = false;
static bool portalSaved = false;
static uint32_t portalStartedMs = 0;

// --- Credentials ---------------------------------------------------------

bool hasCredentials() {
  prefs.begin(PREFS_NS, true);
  bool has = prefs.isKey("ssid") && prefs.getString("ssid", "").length() > 0;
  prefs.end();
  return has;
}

String storedSsid() {
  prefs.begin(PREFS_NS, true);
  String ssid = prefs.getString("ssid", "");
  prefs.end();
  return ssid;
}

static String storedPass() {
  prefs.begin(PREFS_NS, true);
  String pass = prefs.getString("pass", "");
  prefs.end();
  return pass;
}

uint16_t storedPort() {
  prefs.begin(PREFS_NS, true);
  uint16_t port = prefs.getUShort("port", NESSO_DEFAULT_PORT);
  prefs.end();
  return port ? port : NESSO_DEFAULT_PORT;
}

void saveCredentials(const String& ssid, const String& pass, uint16_t port) {
  prefs.begin(PREFS_NS, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putUShort("port", port ? port : NESSO_DEFAULT_PORT);
  prefs.end();
  Serial.printf("[net] credentials saved for '%s', companion port %u\n", ssid.c_str(), port);
}

void saveCompanionPort(uint16_t port) {
  prefs.begin(PREFS_NS, false);
  prefs.putUShort("port", port ? port : NESSO_DEFAULT_PORT);
  prefs.end();
  Serial.printf("[net] companion port set to %u\n", port);
}

void rememberBoard(const String& ip, const String& hostname) {
  prefs.begin(PREFS_NS, false);
  prefs.putString("lastip", ip);
  prefs.putString("lastname", hostname);
  prefs.end();
}

String lastBoardIp() {
  prefs.begin(PREFS_NS, true);
  String ip = prefs.getString("lastip", "");
  prefs.end();
  return ip;
}

String lastBoardName() {
  prefs.begin(PREFS_NS, true);
  String name = prefs.getString("lastname", "");
  prefs.end();
  return name;
}

void forgetCredentials() {
  prefs.begin(PREFS_NS, false);
  prefs.clear();
  prefs.end();
  Serial.println("[net] credentials cleared");
}

// --- Station mode --------------------------------------------------------

bool connectStation() {
  String ssid = storedSsid();
  if (ssid.isEmpty()) return false;

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);  // the companion is idle most of the time
  WiFi.begin(ssid.c_str(), storedPass().c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(100);
  }
  bool ok = WiFi.status() == WL_CONNECTED;
  Serial.printf("[net] '%s' %s\n", ssid.c_str(), ok ? localIp().c_str() : "connection failed");
  return ok;
}

bool isConnected() { return WiFi.status() == WL_CONNECTED; }

String localIp() { return WiFi.localIP().toString(); }

// --- Setup portal --------------------------------------------------------

static const char PORTAL_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Nesso N1 setup</title><style>
body{font:16px system-ui,sans-serif;margin:0;background:#111a1c;color:#e8f2f3}
main{max-width:420px;margin:0 auto;padding:24px 18px}
h1{font-size:20px;margin:0 0 4px}p.sub{color:#7fa3a8;margin:0 0 22px;font-size:14px}
label{display:block;margin:14px 0 6px;font-size:13px;color:#9fc1c5}
input,select{width:100%;box-sizing:border-box;padding:11px;border-radius:8px;border:1px solid #2c4145;background:#0b1214;color:#e8f2f3;font-size:16px}
button{width:100%;margin-top:22px;padding:13px;border:0;border-radius:8px;background:#00979d;color:#fff;font-size:16px;font-weight:600}
small{display:block;margin-top:16px;color:#6b8c90;line-height:1.5}
</style></head><body><main>
<h1>Nesso N1 companion</h1><p class="sub">Pick the network your Arduino board is on.</p>
<form method="POST" action="/save">
<label for="ssid">Network</label>
<select id="ssid" name="ssid">%NETWORKS%</select>
<label for="pass">Password</label>
<input id="pass" name="pass" type="password" autocomplete="off" placeholder="leave empty for an open network">
<label for="port">Companion port</label>
<input id="port" name="port" type="number" value="%PORT%" min="1" max="65535">
<button type="submit">Save and connect</button>
</form>
<small>The port must match NESSO_PORT in the Nesso N1 Companion brick. Default is 8077.</small>
</main></body></html>)HTML";

static const char SAVED_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Saved</title><style>body{font:16px system-ui,sans-serif;background:#111a1c;color:#e8f2f3;
display:flex;height:100vh;margin:0;align-items:center;justify-content:center;text-align:center}</style>
</head><body><div><h1>Saved</h1><p>The Nesso is reconnecting.<br>You can close this page.</p></div></body></html>)HTML";

static String buildNetworkOptions() {
  int found = WiFi.scanNetworks();
  String options;
  for (int i = 0; i < found && i < 20; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    ssid.replace("\"", "&quot;");
    options += "<option value=\"" + ssid + "\">" + ssid + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  WiFi.scanDelete();
  if (options.isEmpty()) options = "<option value=\"\">no networks found</option>";
  return options;
}

static void handleRoot() {
  String page = FPSTR(PORTAL_PAGE);
  page.replace("%NETWORKS%", buildNetworkOptions());
  page.replace("%PORT%", String(storedPort()));
  server.send(200, "text/html", page);
}

static void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  uint16_t port = (uint16_t)server.arg("port").toInt();
  if (ssid.isEmpty()) {
    server.send(400, "text/plain", "a network is required");
    return;
  }
  saveCredentials(ssid, pass, port);
  server.send(200, "text/html", FPSTR(SAVED_PAGE));
  portalSaved = true;
}

void startPortal() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);  // STA stays up so the network scan works
  WiFi.softAP(SETUP_AP_SSID);
  delay(200);

  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);  // anything else lands on the form (captive portal)
  server.begin();

  portalRunning = true;
  portalSaved = false;
  portalStartedMs = millis();
  Serial.printf("[net] setup portal at http://%s (SSID %s)\n",
                WiFi.softAPIP().toString().c_str(), SETUP_AP_SSID);
}

bool portalLoop() {
  if (!portalRunning) return false;
  dns.processNextRequest();
  server.handleClient();
  if (portalSaved) {
    delay(300);  // let the browser receive the confirmation page
    return true;
  }
  // Fall back to retrying the stored network rather than sitting in the portal
  // forever, in case the user walked away mid-setup.
  return hasCredentials() && (millis() - portalStartedMs) > SETUP_AP_TIMEOUT_MS;
}

void stopPortal() {
  if (!portalRunning) return;
  server.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  portalRunning = false;
}

String portalSsid() { return String(SETUP_AP_SSID); }

String portalUrl() { return "http://" + WiFi.softAPIP().toString(); }

// --- Discovery -----------------------------------------------------------

// Asks the board whether a companion App is listening, and what it is called.
static bool probeCompanion(const String& ip, uint16_t port, String& appName) {
  WiFiClient client;
  client.setTimeout(PROBE_TIMEOUT_MS);
  if (!client.connect(ip.c_str(), port, PROBE_TIMEOUT_MS)) return false;

  client.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                NESSO_INFO_PATH, ip.c_str());

  uint32_t deadline = millis() + PROBE_TIMEOUT_MS;
  String body;
  bool inBody = false;
  // Keep draining after the server closes: the descriptor is small and arrives
  // with Connection: close, so the buffer often still holds the body.
  while ((client.connected() || client.available()) && millis() < deadline) {
    while (client.available()) {
      String line = client.readStringUntil('\n');
      if (!inBody) {
        line.trim();
        if (line.isEmpty()) inBody = true;
      } else {
        body += line;
      }
      deadline = millis() + PROBE_TIMEOUT_MS;
    }
    delay(1);
  }
  client.stop();

  int brace = body.indexOf('{');
  if (brace < 0) return false;

  JsonDocument doc;
  if (deserializeJson(doc, body.substring(brace))) return false;
  if (String(doc["service"] | "") != "nesso-companion") return false;

  appName = String(doc["app"] | "Companion App");
  return true;
}

static bool alreadyListed(const String& ip) {
  for (uint8_t i = 0; i < gBoardCount; i++) {
    if (gBoards[i].ip == ip) return true;
  }
  return false;
}

static void addBoard(const String& hostname, const String& ip, uint16_t port) {
  if (gBoardCount >= MAX_BOARDS || alreadyListed(ip)) return;
  BoardEntry& entry = gBoards[gBoardCount];
  entry.hostname = hostname;
  entry.ip = ip;
  entry.port = port;
  entry.appName = "";
  entry.hasCompanion = probeCompanion(ip, port, entry.appName);
  Serial.printf("[net]   %s (%s) companion=%s %s\n", entry.hostname.c_str(), entry.ip.c_str(),
                entry.hasCompanion ? "yes" : "no", entry.appName.c_str());
  gBoardCount++;
}

uint8_t discoverBoards(uint16_t companionPort) {
  gBoardCount = 0;
  if (!isConnected()) return 0;

  // mDNS needs restarting after a reconnect, and it is cheap.
  MDNS.end();
  if (!MDNS.begin("nesso-n1")) {
    Serial.println("[net] mDNS failed to start");
    return 0;
  }

  // Two passes: one query regularly misses responders on a busy network, and
  // the board you actually want going missing is the worst possible failure.
  for (uint8_t pass = 0; pass < MDNS_QUERY_PASSES && gBoardCount < MAX_BOARDS; pass++) {
    int found = MDNS.queryService(MDNS_SERVICE, MDNS_PROTO);
    Serial.printf("[net] mDNS pass %u found %d '_%s._%s' responder(s)\n", pass + 1, found,
                  MDNS_SERVICE, MDNS_PROTO);
    for (int i = 0; i < found && gBoardCount < MAX_BOARDS; i++) {
      addBoard(MDNS.hostname(i), MDNS.address(i).toString(), companionPort);
    }
  }

  // Whatever mDNS says, the board we linked to last time is worth a look.
  String lastIp = net::lastBoardIp();
  if (lastIp.length() && !alreadyListed(lastIp)) {
    Serial.printf("[net] mDNS missed the last board, probing %s directly\n", lastIp.c_str());
    addBoard(net::lastBoardName(), lastIp, companionPort);
  }

  // Boards actually running a companion App come first: those are the ones you
  // are here to pick. Insertion sort keeps the rest in discovery order.
  for (uint8_t i = 1; i < gBoardCount; i++) {
    BoardEntry entry = gBoards[i];
    if (!entry.hasCompanion) continue;
    int8_t j = i - 1;
    while (j >= 0 && !gBoards[j].hasCompanion) {
      gBoards[j + 1] = gBoards[j];
      j--;
    }
    gBoards[j + 1] = entry;
  }

  return gBoardCount;
}

}  // namespace net
