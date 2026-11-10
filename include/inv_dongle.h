// Inverters of other brands read from a home dongle (src/inv.cpp): Deye / Sofar (Solarman logger),
// Solis (S2-WL-ST) and PowMr & co. (Eybond / SmartESS collector). Up to 2 sources. Read only.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "lux_dongle.h"

namespace invd {
void start();                               // load settings, start the task
bool enabled();
bool fresh();                               // any inverter read in the last 60 s
void cloudJson(JsonArray arr);              // fresh inverters, for the cloud upload (p_data->'inv')
void statusJson(JsonObject o);              // settings + state, for the pages
void configure(const String &spec, bool on); // "deye,192.168.1.50,2712345678;powmr,192.168.1.61"
void scan();                                // look for dongles on the LAN
int views(luxd::View *out, int max);        // for the screen (same shape as the LuxPower view)
} // namespace invd
