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
