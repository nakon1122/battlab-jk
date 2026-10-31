#pragma once
#include "jk_protocol.h"
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
};

void takeDisplaySnapshot(DisplaySnap &out); // implemented in main.cpp
void displayStart();                        // no-op on boards without display

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
