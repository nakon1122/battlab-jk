// Renders every remote-screen page on a PC with the real UI code (ui.cpp)
// and LovyanGFX, so each build ships with preview images.
//   tools/render_screens.sh  ->  firmware/screens/*.ppm -> *.png
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <stdio.h>
#include <string.h>
#include <string>
#include "../src/screen/ui.h"
#include "../include/bb_config.h"
#define BB_FW BB_FW_VERSION

static void save(lgfx::LGFX_Sprite &s, int W, int H, const std::string &path) {
  FILE *f = fopen(path.c_str(), "wb");
  fprintf(f, "P6 %d %d 255\n", W, H);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint32_t c = s.readPixel(x, y);
      uint8_t r = (c >> 8) & 0xF8, g = (c >> 3) & 0xFC, b = (c << 3) & 0xF8;
      fputc(r | r >> 5, f);
      fputc(g | g >> 6, f);
      fputc(b | b >> 5, f);
    }
  fclose(f);
}

static void tray(Tray &t, const char *type, uint32_t rgb, int remain) {
  snprintf(t.type, sizeof t.type, "%s", type);
  t.rgb = rgb;
  t.remain = remain;
  t.empty = false;
}

static Model demo() {
  Model m;
  m.wifi = m.cloudOk = m.haveData = m.bridgeOnline = true;
  m.bridgeAgeS = 8;
  m.fetchAgeS = 2;
  snprintf(m.bridgeName, sizeof m.bridgeName, "Home");
  snprintf(m.ssid, sizeof m.ssid, "MyHome_5G");
  snprintf(m.ip, sizeof m.ip, "192.168.1.57");
  m.rssi = -58;
  snprintf(m.keyTail, sizeof m.keyTail, "9e71");
  snprintf(m.fw, sizeof m.fw, BB_FW);
  struct tm tm = {};
  tm.tm_year = 126; tm.tm_mon = 10; tm.tm_mday = 1; tm.tm_hour = 15; tm.tm_min = 42;
  m.now = mktime(&tm);
  m.np = 4;
  {  // A1 printing
    Printer &p = m.p[0];
    snprintf(p.name, sizeof p.name, "A1");
    snprintf(p.model, sizeof p.model, "A1");
    snprintf(p.state, sizeof p.state, "RUNNING");
    snprintf(p.task, sizeof p.task, "battery bracket");
    p.online = true; p.pct = 88; p.remain = 3; p.layer = 76; p.tlayer = 87;
    p.noz = 250; p.nozT = 250; p.bed = 60; p.bedT = 60; p.fanPart = 100;
    p.nams = 1; p.ams[0].id = 0; p.ams[0].ntray = 4; p.ams[0].humLvl = 4;
    tray(p.ams[0].tray[0], "PETG", 0xF2F2F2, 72);
    tray(p.ams[0].tray[1], "PETG", 0x222222, 40);
    tray(p.ams[0].tray[2], "PLA", 0x1E88E5, 95);
    p.trayNow = 0;
  }
  {  // P2S paused with error
    Printer &p = m.p[1];
    snprintf(p.name, sizeof p.name, "P2S");
    snprintf(p.model, sizeof p.model, "P2S");
    snprintf(p.state, sizeof p.state, "PAUSE");
    snprintf(p.task, sizeof p.task, "gear housing v3");
    snprintf(p.err, sizeof p.err, "0700_8019");
    p.online = true; p.pct = 41; p.remain = 126; p.layer = 112; p.tlayer = 274;
    p.noz = 218; p.nozT = 220; p.bed = 65; p.bedT = 65; p.cham = 34; p.fanPart = 0;
    p.nams = 1; p.ams[0].id = 0; p.ams[0].ntray = 4; p.ams[0].hum = 46; p.ams[0].temp = 28;
    tray(p.ams[0].tray[0], "PLA", 0xE53935, 12);
    tray(p.ams[0].tray[1], "PLA", 0xFFD600, 60);
    tray(p.ams[0].tray[2], "PLA-S", 0x43A047, -1);
    tray(p.ams[0].tray[3], "TPU", 0xFF6F00, 88);
    p.trayNow = 0;
  }
  {  // X2D finished, dual nozzle
    Printer &p = m.p[2];
    snprintf(p.name, sizeof p.name, "X2D");
    snprintf(p.model, sizeof p.model, "X2D");
    snprintf(p.state, sizeof p.state, "FINISH");
    snprintf(p.task, sizeof p.task, "enclosure lid");
    p.online = true; p.pct = 100; p.remain = 0; p.layer = 310; p.tlayer = 310;
    p.dual = true; p.noz = 42; p.noz2 = 38; p.bed = 41; p.cham = 31; p.active = 1;
    p.nams = 2;
    p.ams[0].id = 0; p.ams[0].ntray = 4; p.ams[0].hum = 21; p.ams[0].dryMin = 0;
    tray(p.ams[0].tray[0], "ABS", 0x37474F, 50);
    tray(p.ams[0].tray[1], "ASA", 0xFAFAFA, 77);
    p.ams[1].id = 1; p.ams[1].ntray = 4; p.ams[1].hum = 15;
    p.trayNow = 1;
  }
  {  // H2C printing, AMS drying
    Printer &p = m.p[3];
    snprintf(p.name, sizeof p.name, "H2C");
    snprintf(p.model, sizeof p.model, "H2C");
    snprintf(p.state, sizeof p.state, "RUNNING");
    snprintf(p.task, sizeof p.task, "drone frame top plate");
    p.online = true; p.pct = 17; p.remain = 312; p.layer = 22; p.tlayer = 140;
    p.dual = true; p.noz = 260; p.nozT = 260; p.noz2 = 140; p.noz2T = 0; p.bed = 90; p.bedT = 90; p.cham = 48;
    p.fanPart = 30;
    p.nams = 1; p.ams[0].id = 0; p.ams[0].ntray = 4; p.ams[0].hum = 12; p.ams[0].dryMin = 336;
    tray(p.ams[0].tray[0], "PA-CF", 0x212121, 64);
    tray(p.ams[0].tray[1], "PETG", 0x00AE42, 30);
    tray(p.ams[0].tray[2], "PETG", 0x8E24AA, 81);
    p.trayNow = 0;
  }
  const char *ev[][5] = {{"X2D", "finished", "enclosure lid", "", "15:31"},
                         {"P2S", "paused", "gear housing v3", "0700_8019", "15:20"},
                         {"H2C", "started", "drone frame top plate", "", "14:58"},
                         {"A1", "started", "battery bracket", "", "13:05"},
                         {"P2S", "error", "", "0300_400C", "11:47"},
                         {"A1", "finished", "phone stand", "", "10:12"},
                         {"X2D", "offline", "", "", "08:30"},
                         {"X2D", "online", "", "", "08:33"}};
  // 12 more printers -> 16 like the real farm
  const char *models[] = {"A1", "A1", "P2S", "A1", "X2D", "A1 mini", "P2S", "A1", "H2C", "A1", "P2S", "A1"};
  const char *states[] = {"RUNNING", "IDLE", "RUNNING", "RUNNING", "FINISH", "RUNNING", "IDLE", "RUNNING", "RUNNING", "FAILED", "RUNNING", "IDLE"};
  int pcts[] = {63, 0, 8, 95, 100, 37, 0, 71, 52, 22, 14, 0};
  int rem[] = {48, 0, 402, 6, 0, 85, 0, 33, 140, 0, 610, 0};
  for (int i = 0; i < 12; i++) {
    Printer &p = m.p[4 + i];
    p = m.p[i % 4 == 2 ? 2 : 0];
    snprintf(p.model, sizeof p.model, "%s", models[i]);
    snprintf(p.name, sizeof p.name, "%s-%02d", strncmp(models[i], "A1", 2) == 0 ? "A1" : models[i], i + 2);
    snprintf(p.state, sizeof p.state, "%s", states[i]);
    p.pct = pcts[i];
    p.remain = rem[i];
    p.err[0] = 0;
    p.nhms = 0;
    p.online = true;
  }
  m.p[10].online = false;
  m.p[10].ageS = 3000;
  snprintf(m.p[13].err, sizeof m.p[13].err, "0300_400C");
  snprintf(m.p[0].name, sizeof m.p[0].name, "A1-01");
  snprintf(m.p[1].name, sizeof m.p[1].name, "P2S-01");
  snprintf(m.p[2].name, sizeof m.p[2].name, "X2D-01");
  snprintf(m.p[3].name, sizeof m.p[3].name, "H2C-01");
  m.np = 16;
  m.devices = 4;
  m.devicesOnline = 4;
  m.nev = 8;
  for (int i = 0; i < 8; i++) {
    Event &e = m.ev[i];
    e.id = 100 - i;
    snprintf(e.printer, sizeof e.printer, "%s", ev[i][0]);
    snprintf(e.kind, sizeof e.kind, "%s", ev[i][1]);
    snprintf(e.task, sizeof e.task, "%s", ev[i][2]);
    snprintf(e.err, sizeof e.err, "%s", ev[i][3]);
    struct tm t = tm;
    sscanf(ev[i][4], "%d:%d", &t.tm_hour, &t.tm_min);
    e.t = mktime(&t);
  }
  return m;
}

int main(int argc, char **argv) {
  std::string out = argc > 1 ? argv[1] : ".";
  setenv("TZ", "UTC", 1);
  tzset();
  struct Size { int W, H; const char *tag; } sizes[] = {{320, 240, "cyd28"}, {480, 320, "cyd35"}};
  for (auto &s : sizes) {
    lgfx::LGFX_Sprite sp;
    sp.setColorDepth(16);
    sp.createSprite(s.W, s.H);
    Model m = demo();
    UiState u;
    {  // overview with only 4 printers (big cards)
      Model m4 = m;
      m4.np = 4;
      u.page = PG_ALL;
      uiDraw(sp, s.W, s.H, 0, m4, u);
      save(sp, s.W, s.H, out + "/" + s.tag + "_1_all_4printers.ppm");
    }
    const char *names[] = {"1_all_16printers", "2_printer", "3_events", "4_setup"};
    for (int pg = 0; pg < PG_COUNT; pg++) {
      u.page = pg;
      u.focus = 0;
      uiDraw(sp, s.W, s.H, 0, m, u);
      save(sp, s.W, s.H, out + "/" + s.tag + "_" + names[pg] + ".ppm");
    }
    // printer page for each printer
    u.page = PG_ONE;
    for (int i = 1; i < 4; i++) {
      u.focus = i;
      uiDraw(sp, s.W, s.H, 0, m, u);
      save(sp, s.W, s.H, out + "/" + s.tag + "_2_printer_" + m.p[i].name + ".ppm");
    }
    // event banner
    u.page = PG_ALL;
    uiMakeToast(m.ev[0], u);
    uiDraw(sp, s.W, s.H, 0, m, u);
    save(sp, s.W, s.H, out + "/" + s.tag + "_5_alert.ppm");
    u.toast[0] = 0;
    // one printer offline
    Model m2 = m;
    m2.np = 4;
    m2.p[2].online = false;
    m2.p[2].ageS = 1260;
    m2.bridgeOnline = true;
    uiDraw(sp, s.W, s.H, 0, m2, u);
    save(sp, s.W, s.H, out + "/" + s.tag + "_6_offline.ppm");
    // Wi-Fi setup screen
    uiDrawSetupAp(sp, s.W, s.H, 0, m);
    save(sp, s.W, s.H, out + "/" + s.tag + "_0_wifi_setup.ppm");
  }
  return 0;
}
