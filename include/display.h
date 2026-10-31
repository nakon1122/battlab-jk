#pragma once
#include "jk_protocol.h"
#include "bms_params.h"
#include <Arduino.h>

// Snapshot of everything the on-board display shows. Filled by main.cpp
// under its lock so the display task never touches shared state directly.
struct DisplaySnap {
  String state, detected, model, mac, probe, firmware;
  bool linked = false, live = false, scanning = false;
  jk::Sample sample;
  int rssi = 0;
  uint32_t rxBytes = 0, good = 0, uptimeMs = 0, ageMs = 0;
  int stations = 0;
  String devices = "[]", protocol, connAuth; // last scan result (JSON)
  bool automatic = false, wifiOn = true;
  const char *resetReason = "";
  bool resetAbnormal = false;
  uint32_t freeHeap = 0, minHeap = 0, maxBlock = 0;
  bmsw::JkSettings jk;
  bool jkFresh = false;
  uint8_t family = 0; // 0 unknown, 1 JK, 2 ANT, 3 JBD
  String lastWrite;
  bool radioMax = true;
  const char *bleTx = "";
};
// Home Wi-Fi + cloud (main.cpp)
struct NetInfo {
  String ssid, ip, cloud, pair, site;
  bool connected = false, configured = false, paired = false;
  int rssi = 0;
  long lastOkAgo = -1;
  uint32_t sent = 0;
};
NetInfo netInfo();
void netSetHomeWifi(const String &ssid, const String &pass);
void netForgetHomeWifi();
void netScanStart();
String netScanJson(); // "" while scanning, else JSON array [{ssid,rssi,lock}]
bool radioMaxPower();
void radioSetMaxPower(bool on); // MAX: BLE chip maximum + Wi-Fi 19.5 dBm

// BMS writes (validated, then queued to the Bluetooth task). Return nullptr
// when accepted, otherwise a short reason.
const char *bmsWriteParam(int idx, float value);
const char *bmsSetSwitch(int which, bool on);

// Actions shared by the web API and the on-screen UI (main.cpp).
void bmsScan();
bool bmsConnect(const String &mac, uint8_t type, const String &protocol);
void bmsAuto();
void bmsDisconnect();
bool wifiEnabled();
void wifiSetEnabled(bool on);

void takeDisplaySnapshot(DisplaySnap &out); // implemented in main.cpp
void displayStart();                        // no-op on boards without display
// Pause screen drawing (and its big sprite buffers) while TLS runs.
bool gfxHold(uint32_t waitMs);
void gfxRelease();

// RGB LED settings, shared by the web API, the on-screen controls and the
// LED task. Persisted in NVS.
#define LED_COLORS 8
#define LED_MODES 4 // 0 Rainbow, 1 Solid, 2 Breathe, 3 Status
struct LedCfg {
  bool on = true;
  uint8_t mode = 0;
  uint8_t color = 3;
  uint8_t level = 2; // 1..4
};
extern const uint8_t LED_PALETTE[LED_COLORS][3];
extern const char *const LED_COLOR_NAMES[LED_COLORS];
extern const char *const LED_MODE_NAMES[LED_MODES];
LedCfg ledGet();
void ledSet(const LedCfg &c);
