// LuxPower dongle reader task (src/lux.cpp). Reads up to 2 dongles on the home Wi-Fi.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace luxd {
// what the screen shows for one inverter
struct View {
  char sn[11];
  bool fresh;
  float soc, v, p, ppv, pload, pgrid, epv, eload;
  uint16_t status;
};
void start();                        // load settings, start the task
bool fresh();                        // any inverter read in the last 60 s
void cloudJson(JsonArray arr);       // fresh inverters, for the cloud upload
void statusJson(JsonObject o);       // settings + state, for the local page
void configure(const String &ips, bool on); // comma separated, max 2
void scan();                         // look for dongles on the LAN (port 8000)
int views(View *out, int max);       // for the screen
bool enabled();
} // namespace luxd
