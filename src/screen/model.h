#pragma once
// Data the remote screen shows. Plain C++ (no Arduino) so the same UI code
// also builds on a PC to render the preview images.
#include <stdint.h>
#include <time.h>

struct Tray {
  char type[10] = "";
  uint32_t rgb = 0;   // 0xRRGGBB
  int8_t remain = -1; // % or -1 unknown
  bool empty = true;
};

struct AmsUnit {
  int id = 0;
  int hum = -1;       // % (humidity_raw) or -1
  int humLvl = -1;    // 1..5 level when % is not reported
  float temp = -1000;
  int dryMin = 0;     // drying minutes left (AMS 2 Pro / HT)
  int ntray = 0;
  Tray tray[4];
};

struct Printer {
  char serial[24] = "";
  char name[24] = "";
  char model[10] = "";
  char state[12] = "";  // IDLE PREPARE RUNNING PAUSE FINISH FAILED
  char task[48] = "";
  char err[16] = "";     // print_error as XXXX_XXXX, "" none
  bool online = false;
  int pct = 0, remain = 0, layer = 0, tlayer = 0;
  float noz = 0, nozT = 0, noz2 = 0, noz2T = 0, bed = 0, bedT = 0, cham = -1000;
  bool dual = false;
  int active = 0;               // active nozzle on dual-nozzle machines
  int fanPart = -1, fanAux = -1, fanCham = -1;
  int spd = 2;                  // 1 silent 2 standard 3 sport 4 ludicrous
  int nhms = 0;
  char hms[3][24] = {"", "", ""};
  int nams = 0;
  AmsUnit ams[4];
  int trayNow = 255;
  long ageS = 0;                // seconds since the bridge last sent this printer
};

struct Event {
  long id = 0;
  char printer[24] = "";
  char kind[10] = "";
  char task[40] = "";
  char err[16] = "";
  time_t t = 0;
};

struct Model {
  bool wifi = false;
  bool apMode = false;          // setup hotspot running
  bool cloudOk = false;         // last cloud fetch worked
  bool haveData = false;
  bool bridgeOnline = false;
  long bridgeAgeS = -1;
  int devices = 0, devicesOnline = 0;  // bridge boards
  long fetchAgeS = -1;
  char bridgeName[24] = "";
  char cloudErr[40] = "";
  int np = 0;
  Printer p[16];
  int nev = 0;
  Event ev[10];
  time_t now = 0;               // local wall clock (0 = unknown)
  char ssid[33] = "";
  char ip[16] = "";
  int rssi = 0;
  char apName[24] = "BAMBU-SCREEN";
  char keyTail[8] = "";
  char fw[16] = "";
};

enum Page { PG_ALL, PG_ONE, PG_EVENTS, PG_SETUP, PG_COUNT };

struct UiState {
  int page = PG_ALL;
  int focus = 0;                // printer shown on PG_ONE
  int bright = 4;               // 1..5
  bool flip = false;
  long toastId = 0;             // event shown as a banner
  char toast[64] = "";
  uint16_t toastColor = 0;
};

// Touch actions returned by uiTouch()
enum Action { A_NONE, A_REDRAW, A_BRIGHT_DN, A_BRIGHT_UP, A_ROTATE, A_CALIBRATE, A_WIFI_SETUP, A_REFRESH };
