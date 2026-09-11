// Wi-Fi provisioning (captive portal, credentials in NVS) and discovery of
// Arduino boards advertising `_arduino._tcp` over mDNS.
#pragma once

#include <Arduino.h>

#include "config.h"
#include "state.h"

namespace net {

// --- Credentials ---------------------------------------------------------
bool hasCredentials();
String storedSsid();
uint16_t storedPort();
void saveCredentials(const String& ssid, const String& pass, uint16_t port);
// Changes only the companion port, leaving the credentials alone.
void saveCompanionPort(uint16_t port);
void forgetCredentials();

// The board that last completed a handshake. Remembered so discovery can offer
// it even when an mDNS query misses it, which happens on a busy network.
void rememberBoard(const String& ip, const String& hostname);
String lastBoardIp();
String lastBoardName();

// --- Station mode --------------------------------------------------------
// Tries the stored network. Returns false on timeout.
bool connectStation();
bool isConnected();
String localIp();

// --- Setup portal --------------------------------------------------------
// Brings up the SoftAP and the captive portal. Call portalLoop() until it
// returns true, which means credentials were saved and the board should
// reconnect.
void startPortal();
bool portalLoop();
void stopPortal();
String portalSsid();
String portalUrl();

// --- Discovery -----------------------------------------------------------
// Fills gBoards/gBoardCount with every `_arduino._tcp` responder, then probes
// each one on the companion port to see whether a companion App is running.
// Returns the number of boards found.
uint8_t discoverBoards(uint16_t companionPort);

}  // namespace net
