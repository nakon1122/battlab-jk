// Bambu remote screen UI (landscape 320x240 CYD 2.8" or 480x320 CYD 3.5").
// Every position is written for 320x240 and scaled; fonts are picked per size.
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

using lgfx::LovyanGFX;
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return lgfx::color565(r, g, b); }

static const uint16_t C_BG = rgb(10, 13, 18), C_CARD = rgb(24, 30, 39), C_CARD2 = rgb(32, 40, 52),
                      C_LINE = rgb(48, 58, 72), C_TEXT = rgb(236, 240, 244), C_MUTED = rgb(136, 148, 162),
                      C_GREEN = rgb(0, 200, 90), C_BAMBU = rgb(0, 174, 66), C_YELLOW = rgb(250, 200, 40),
                      C_ORANGE = rgb(255, 140, 30), C_RED = rgb(240, 60, 60), C_BLUE = rgb(60, 160, 255),
                      C_CYAN = rgb(40, 210, 220), C_DIM = rgb(70, 80, 92);
uint16_t uiBg() { return C_BG; }

// ------------------------------------------------------------- drawing ----
struct D {
  LovyanGFX &g;
  int W, H, oy;
  float sx, sy;
  bool L;  // large screen (480x320)
  int X(float v) const { return (int)lroundf(v * sx); }
  int Y(float v) const { return (int)lroundf(v * sy); }
  void rect(float x, float y, float w, float h, uint16_t c) const { g.fillRect(X(x), Y(y) - oy, X(x + w) - X(x), Y(y + h) - Y(y), c); }
  void rrect(float x, float y, float w, float h, float r, uint16_t c) const {
    g.fillRoundRect(X(x), Y(y) - oy, X(x + w) - X(x), Y(y + h) - Y(y), X(r), c);
  }
  void rframe(float x, float y, float w, float h, float r, uint16_t c) const {
    g.drawRoundRect(X(x), Y(y) - oy, X(x + w) - X(x), Y(y + h) - Y(y), X(r), c);
  }
  void hline(float x, float y, float w, uint16_t c) const { g.drawFastHLine(X(x), Y(y) - oy, X(x + w) - X(x), c); }
  void circle(float x, float y, float r, uint16_t c) const { g.fillCircle(X(x), Y(y) - oy, X(r), c); }
  void arc(float x, float y, float r0, float r1, float a0, float a1, uint16_t c) const {
    // a0/a1 in degrees, 0 = top, clockwise
    float s = a0 + 270, e = a1 + 270;
    while (s >= 360) { s -= 360; e -= 360; }
    int cx = X(x), cy = Y(y) - oy, ri = X(r0), ro = X(r1);
    if (e <= 360) g.fillArc(cx, cy, ri, ro, s, e, c);
    else { g.fillArc(cx, cy, ri, ro, s, 360, c); g.fillArc(cx, cy, ri, ro, 0, e - 360, c); }
  }
  void tri(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c) const {
    g.fillTriangle(X(x0), Y(y0) - oy, X(x1), Y(y1) - oy, X(x2), Y(y2) - oy, c);
  }
  // fonts: 0 small, 1 small bold, 2 medium bold, 3 large bold, 4 huge, 5 tiny
  const lgfx::IFont *font(int f) const {
    switch (f) {
      case 5: return L ? (const lgfx::IFont *)&fonts::DejaVu12 : &fonts::DejaVu9;
      case 0: return L ? (const lgfx::IFont *)&fonts::DejaVu18 : &fonts::DejaVu12;
      case 1: return L ? (const lgfx::IFont *)&fonts::FreeSansBold12pt7b : &fonts::FreeSansBold9pt7b;
      case 2: return L ? (const lgfx::IFont *)&fonts::FreeSansBold18pt7b : &fonts::FreeSansBold12pt7b;
      case 3: return L ? (const lgfx::IFont *)&fonts::FreeSansBold24pt7b : &fonts::FreeSansBold18pt7b;
      default: return L ? (const lgfx::IFont *)&fonts::FreeSansBold24pt7b : &fonts::FreeSansBold24pt7b;
    }
  }
  float fscale(int f) const { return (f == 4 && L) ? 1.4f : 1.0f; }
  int tw(const char *s, int f) const {
    g.setFont(font(f));
    g.setTextSize(fscale(f));
    return g.textWidth(s);
  }
  // datum: 0 left, 1 center, 2 right; y is the text's vertical middle
  void text(float x, float y, const char *s, int f, uint16_t c, int datum = 0, float maxw = 0) const {
    g.setFont(font(f));
    g.setTextSize(fscale(f));
    g.setTextColor(c);
    g.setTextDatum(datum == 0 ? textdatum_t::middle_left : datum == 1 ? textdatum_t::middle_center : textdatum_t::middle_right);
    char buf[64];
    snprintf(buf, sizeof buf, "%s", s);
    if (maxw > 0) {
      int lim = X(maxw);
      if (g.textWidth(buf) > lim) {
        size_t n = strlen(buf);
        while (n > 1) {
          buf[--n] = 0;
          char t[68];
          snprintf(t, sizeof t, "%s..", buf);
          if (g.textWidth(t) <= lim) { snprintf(buf, sizeof buf, "%s", t); break; }
        }
      }
    }
    g.drawString(buf, X(x), Y(y) - oy);
    g.setTextSize(1);
  }
};

static uint16_t stateColor(const Printer &p) {
  if (!p.online) return C_DIM;
  if (p.err[0] || p.nhms) {
    if (!strcmp(p.state, "RUNNING") || !strcmp(p.state, "PREPARE")) return C_ORANGE;
  }
  if (!strcmp(p.state, "RUNNING") || !strcmp(p.state, "PREPARE")) return C_GREEN;
  if (!strcmp(p.state, "PAUSE")) return C_YELLOW;
  if (!strcmp(p.state, "FAILED")) return C_RED;
  if (!strcmp(p.state, "FINISH")) return C_BLUE;
  return C_MUTED;
}
static const char *stateText(const Printer &p) {
  if (!p.online) return "OFFLINE";
  if (!strcmp(p.state, "RUNNING")) return "PRINTING";
  if (!strcmp(p.state, "PREPARE")) return "PREPARING";
  if (!strcmp(p.state, "PAUSE")) return "PAUSED";
  if (!strcmp(p.state, "FINISH")) return "FINISHED";
  if (!strcmp(p.state, "FAILED")) return "FAILED";
  if (!p.state[0]) return "WAITING";
  return "IDLE";
}
static bool busy(const Printer &p) { return p.online && (!strcmp(p.state, "RUNNING") || !strcmp(p.state, "PREPARE") || !strcmp(p.state, "PAUSE")); }

static void fmtDur(int min, char *o, size_t n) {
  if (min <= 0) snprintf(o, n, "--");
  else if (min < 60) snprintf(o, n, "%dm", min);
  else if (min < 24 * 60) snprintf(o, n, "%dh %02dm", min / 60, min % 60);
  else snprintf(o, n, "%dd %dh", min / 1440, (min % 1440) / 60);
}
static void fmtEta(const Model &m, int min, char *o, size_t n) {
  if (!m.now || min <= 0) { snprintf(o, n, "--:--"); return; }
  time_t t = m.now + (time_t)min * 60;
  struct tm tm;
  localtime_r(&t, &tm);
  struct tm nw;
  localtime_r(&m.now, &nw);
  if (tm.tm_yday != nw.tm_yday) snprintf(o, n, "%02d:%02d+1", tm.tm_hour, tm.tm_min);
  else snprintf(o, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}
static void fmtAge(long s, char *o, size_t n) {
  if (s < 0) snprintf(o, n, "never");
  else if (s < 60) snprintf(o, n, "%lds ago", s);
  else if (s < 3600) snprintf(o, n, "%ldm ago", s / 60);
  else if (s < 86400) snprintf(o, n, "%ldh ago", s / 3600);
  else snprintf(o, n, "%ldd ago", s / 86400);
}
static uint16_t c565(uint32_t c) { return rgb((c >> 16) & 255, (c >> 8) & 255, c & 255); }
static bool lightColor(uint32_t c) { return ((c >> 16 & 255) * 3 + (c >> 8 & 255) * 6 + (c & 255)) / 10 > 150; }

// ------------------------------------------------------------- chrome ----
static const float HDR = 26, TABH = 28, TABY = 240 - 28;
static const char *const TABS[PG_COUNT] = {"ALL", "PRINTER", "EVENTS", "SETUP"};

static void header(const D &d, const Model &m, const char *title, uint16_t tc, float maxw = 190) {
  d.rect(0, 0, 320, HDR, C_BG);
  d.text(8, HDR / 2, title, 1, tc, 0, maxw);
  if (m.now) {
    struct tm tm;
    localtime_r(&m.now, &tm);
    char b[8];
    snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min);
    d.text(312, HDR / 2, b, 1, C_TEXT, 2);
  }
  // link dots: Wi-Fi, cloud, bridge
  uint16_t cw = m.wifi ? C_GREEN : C_RED;
  uint16_t cc = !m.wifi ? C_DIM : m.cloudOk ? C_GREEN : C_RED;
  uint16_t cb = !m.haveData ? C_DIM : m.bridgeOnline ? C_GREEN : C_ORANGE;
  d.circle(240, HDR / 2, 3, cw);
  d.circle(249, HDR / 2, 3, cc);
  d.circle(258, HDR / 2, 3, cb);
  d.hline(0, HDR - 1, 320, C_LINE);
}

static void tabs(const D &d, int page) {
  d.rect(0, TABY, 320, TABH, C_BG);
  d.hline(0, TABY, 320, C_LINE);
  for (int i = 0; i < PG_COUNT; i++) {
    float x = i * 80;
    bool on = i == page;
    if (on) d.rrect(x + 5, TABY + 4, 70, TABH - 8, 6, C_CARD2);
    d.text(x + 40, TABY + TABH / 2, TABS[i], 0, on ? C_BAMBU : C_MUTED, 1);
  }
}

static void bar(const D &d, float x, float y, float w, float h, int pct, uint16_t c) {
  d.rrect(x, y, w, h, h / 2, C_LINE);
  if (pct > 0) d.rrect(x, y, fmaxf(h, w * (pct > 100 ? 100 : pct) / 100.0f), h, h / 2, c);
}

static float pill(const D &d, float xr, float yc, const char *t, uint16_t c) {
  float w = d.tw(t, 5) / d.sx + 12;
  d.rrect(xr - w, yc - 7, w, 14, 7, c);
  d.text(xr - w / 2, yc, t, 5, C_BG, 1);
  return w;
}

// ------------------------------------------------------------ ALL page ----
static void card(const D &d, const Model &m, const Printer &p, float x, float y, float w, float h) {
  uint16_t sc = stateColor(p);
  d.rrect(x, y, w, h, 7, C_CARD);
  d.rrect(x, y, 4, h, 2, sc);
  bool big = h > 120;
  float pw = pill(d, x + w - 6, y + 12, stateText(p), sc);
  d.text(x + 10, y + 12, p.name, 1, p.online ? C_TEXT : C_MUTED, 0, w - pw - 20);
  if (!p.online) {
    char a[20], b[40];
    fmtAge(p.ageS, a, sizeof a);
    snprintf(b, sizeof b, "last seen %s", a);
    d.text(x + w / 2, y + h / 2 + 6, b, 0, C_MUTED, 1, w - 16);
    return;
  }
  bool b = busy(p) || !strcmp(p.state, "FINISH");
  float py = y + (big ? 52 : 37);
  if (b) {
    char pc[8];
    snprintf(pc, sizeof pc, "%d%%", p.pct);
    d.text(x + 10, py, pc, big ? 3 : 2, C_TEXT);
    char r[16], e[12];
    fmtDur(p.remain, r, sizeof r);
    fmtEta(m, p.remain, e, sizeof e);
    if (!strcmp(p.state, "FINISH")) snprintf(r, sizeof r, "done");
    d.text(x + w - 7, py - 6, r, 1, sc, 2);
    if (strcmp(p.state, "FINISH")) d.text(x + w - 7, py + 8, e, 5, C_MUTED, 2);
    bar(d, x + 10, py + (big ? 22 : 15), w - 17, 4, p.pct, sc);
  } else {
    d.text(x + 10, py, "Ready", 2, C_MUTED);
  }
  if (big && p.task[0]) d.text(x + 10, py + 40, p.task, 0, C_TEXT, 0, w - 20);
  // temps + layer / error
  char t[48];
  snprintf(t, sizeof t, "N %.0f  B %.0f", (p.dual && p.active == 1) ? p.noz2 : p.noz, p.bed);
  float ty = y + h - 10;
  d.text(x + 10, ty, t, 5, C_MUTED);
  if (p.err[0] || p.nhms) d.text(x + w - 7, ty, "ERROR", 5, C_RED, 2);
  else if (b && p.tlayer) {
    char l[20];
    snprintf(l, sizeof l, "L %d/%d", p.layer, p.tlayer);
    d.text(x + w - 7, ty, l, 5, C_MUTED, 2);
  }
}

// overview layout: up to 4 printers as big cards, more as a grid of small tiles
static void gridShape(int n, int &cols, int &rows) {
  if (n <= 1) cols = 1;
  else if (n <= 4) cols = 2;
  else if (n <= 9) cols = 3;
  else cols = 4;
  rows = (n + cols - 1) / cols;
  if (n == 2) rows = 1;
}
static const int MAX_TILES = 16;

static void shortDur(int min, char *o, size_t n) {
  if (min <= 0) snprintf(o, n, "--");
  else if (min < 60) snprintf(o, n, "%dm", min);
  else if (min < 24 * 60) snprintf(o, n, "%dh%02d", min / 60, min % 60);
  else snprintf(o, n, "%dd%dh", min / 1440, (min % 1440) / 60);
}

static void tile(const D &d, const Model &m, const Printer &p, float x, float y, float w, float h) {
  uint16_t sc = stateColor(p);
  bool err = p.online && (p.err[0] || p.nhms);
  d.rrect(x, y, w, h, 5, err ? C_RED : C_CARD);
  if (err) d.rrect(x + 1.5f, y + 1.5f, w - 3, h - 3, 4, C_CARD);
  d.rrect(x, y, 3, h, 1.5f, sc);
  d.text(x + 7, y + h * 0.27f, p.name, 1, p.online ? C_TEXT : C_MUTED, 0, w - 10);
  float ly = y + h * 0.62f;
  bool b = busy(p);
  if (b) {
    char pc[8], r[12];
    snprintf(pc, sizeof pc, "%d%%", p.pct);
    shortDur(p.remain, r, sizeof r);
    d.text(x + 7, ly, pc, 1, sc);
    d.text(x + w - 4, ly, r, 5, C_MUTED, 2);
    bar(d, x + 7, y + h - 6, w - 12, 3, p.pct, sc);
  } else {
    const char *t = !p.online ? "OFFLINE" : !strcmp(p.state, "FINISH") ? "DONE" : !strcmp(p.state, "FAILED") ? "FAILED" : "IDLE";
    d.text(x + 7, ly, t, 5, sc == C_MUTED ? C_MUTED : sc);
    if (err) d.text(x + w - 4, ly, "ERR", 5, C_RED, 2);
  }
}

static void allPage(const D &d, const Model &m) {
  int printing = 0, bad = 0, off = 0;
  for (int i = 0; i < m.np; i++) {
    printing += busy(m.p[i]);
    bad += m.p[i].online && (m.p[i].err[0] || m.p[i].nhms);
    off += !m.p[i].online;
  }
  char title[48];
  if (bad) snprintf(title, sizeof title, "%d/%d busy  %d error", printing, m.np, bad);
  else if (off) snprintf(title, sizeof title, "%d/%d busy  %d off", printing, m.np, off);
  else snprintf(title, sizeof title, "Printers  %d/%d busy", printing, m.np);
  header(d, m, m.np ? title : "Bambu Monitor", bad ? C_RED : C_TEXT);
  float top = HDR + 4, bot = TABY - 4;
  if (!m.np) {
    d.text(160, 100, m.haveData ? "No printers on the bridge yet" : "Waiting for cloud data...", 1, C_MUTED, 1);
    if (m.cloudErr[0]) d.text(160, 125, m.cloudErr, 0, C_RED, 1, 300);
    return;
  }
  int n = m.np > MAX_TILES ? MAX_TILES : m.np, cols, rows;
  gridShape(n, cols, rows);
  float gap = n > 4 ? 4 : 5;
  float cw = (320 - 8 - gap * (cols - 1)) / cols, ch = (bot - top - gap * (rows - 1)) / rows;
  for (int i = 0; i < n; i++) {
    float x = 4 + (i % cols) * (cw + gap), y = top + (i / cols) * (ch + gap);
    if (n <= 4) card(d, m, m.p[i], x, y, cw, ch);
    else tile(d, m, m.p[i], x, y, cw, ch);
  }
}

// -------------------------------------------------------- PRINTER page ----
static void tempTile(const D &d, float x, float y, float w, float h, const char *lbl, float cur, float tgt, uint16_t c) {
  d.rrect(x, y, w, h, 6, C_CARD);
  d.text(x + 6, y + 8, lbl, 5, C_MUTED);
  char v[16];
  if (cur <= -999) snprintf(v, sizeof v, "--");
  else snprintf(v, sizeof v, "%.0f", cur);
  d.text(x + 6, y + h - 10, v, 1, tgt > 0 ? c : C_TEXT);
  if (tgt > 0) {
    char t[12];
    snprintf(t, sizeof t, "/%.0f", tgt);
    d.text(x + w - 5, y + h - 9, t, 5, C_MUTED, 2);
  }
}

static void amsRow(const D &d, const Printer &p, float x, float y, float w, float h) {
  d.rrect(x, y, w, h, 6, C_CARD);
  if (!p.nams) {
    d.text(x + w / 2, y + h / 2, "No AMS", 0, C_MUTED, 1);
    return;
  }
  int u = 0;  // unit holding the active tray, else the first
  if (p.trayNow < 254)
    for (int i = 0; i < p.nams; i++)
      if (p.ams[i].id == p.trayNow / 4) u = i;
  const AmsUnit &a = p.ams[u];
  char lbl[24];
  if (p.nams > 1) snprintf(lbl, sizeof lbl, "AMS %d/%d", a.id + 1, p.nams);
  else snprintf(lbl, sizeof lbl, "AMS %d", a.id + 1);
  float lw = 58;
  d.text(x + 7, y + 11, lbl, 0, C_TEXT);
  char hb[24] = "";
  if (a.dryMin > 0) snprintf(hb, sizeof hb, "DRY %d:%02d", a.dryMin / 60, a.dryMin % 60);
  else if (a.hum >= 0) snprintf(hb, sizeof hb, "RH %d%%", a.hum);
  else if (a.humLvl > 0) snprintf(hb, sizeof hb, "RH lvl %d", a.humLvl);
  d.text(x + 7, y + h / 2 + 4, hb, 5, a.dryMin > 0 ? C_ORANGE : C_MUTED);
  if (a.temp > -999) {
    char tb[12];
    snprintf(tb, sizeof tb, "%.0fC", a.temp);
    d.text(x + 7, y + h - 8, tb, 5, C_MUTED);
  }
  int nt = a.ntray ? a.ntray : 4;
  float sw = (w - lw - 6) / nt;
  for (int i = 0; i < nt; i++) {
    const Tray &t = a.tray[i];
    float tx = x + lw + i * sw + 3, ty = y + 6, tw = sw - 6, th = h - 12;
    bool act = p.trayNow == a.id * 4 + i;
    if (act) d.rrect(tx - 3, ty - 3, tw + 6, th + 6, 7, C_TEXT);
    if (t.empty) {
      d.rrect(tx, ty, tw, th, 5, C_BG);
      d.rframe(tx, ty, tw, th, 5, C_LINE);
      d.text(tx + tw / 2, ty + th / 2, "empty", 5, C_DIM, 1);
    } else {
      d.rrect(tx, ty, tw, th, 5, c565(t.rgb));
      if (lightColor(t.rgb) && !act) d.rframe(tx, ty, tw, th, 5, C_MUTED);
      uint16_t fc = lightColor(t.rgb) ? C_BG : C_TEXT;
      d.text(tx + tw / 2, ty + th / 2 - (t.remain >= 0 ? 7 : 0), t.type, 5, fc, 1, tw - 2);
      if (t.remain >= 0) {
        char r[8];
        snprintf(r, sizeof r, "%d%%", t.remain);
        d.text(tx + tw / 2, ty + th / 2 + 8, r, 1, fc, 1);
      }
    }
  }
}

static void onePage(const D &d, const Model &m, const UiState &u) {
  if (!m.np) {
    header(d, m, "Printer", C_TEXT);
    d.text(160, 110, "No printers yet", 1, C_MUTED, 1);
    return;
  }
  int fi = u.focus % m.np;
  const Printer &p = m.p[fi];
  uint16_t sc = stateColor(p);
  char title[48];
  if (strcmp(p.name, p.model) && p.model[0]) snprintf(title, sizeof title, "%s (%s)", p.name, p.model);
  else snprintf(title, sizeof title, "%s", p.name);
  header(d, m, title, C_TEXT, 120);
  if (m.np > 1) {  // printer switch: < 1/4 >
    d.tri(142, 8, 142, 18, 136, 13, C_MUTED);
    char n[8];
    snprintf(n, sizeof n, "%d/%d", fi + 1, m.np);
    d.text(160, 13, n, 0, C_MUTED, 1);
    d.tri(178, 8, 178, 18, 184, 13, C_MUTED);
  }
  float top = HDR + 3;
  bool b = busy(p) || !strcmp(p.state, "FINISH");
  // ring
  float cx = 47, cy = top + 44, rad = 41;
  d.arc(cx, cy, rad - 8, rad, 0, 360, C_LINE);
  if (p.online && b && p.pct > 0) d.arc(cx, cy, rad - 8, rad, 0, p.pct * 3.6f, sc);
  char pc[8];
  if (p.online && b) snprintf(pc, sizeof pc, "%d", p.pct);
  else snprintf(pc, sizeof pc, "--");
  d.text(cx, cy - 4, pc, 3, C_TEXT, 1);
  d.text(cx, cy + 17, "%", 5, C_MUTED, 1);
  // right column
  float x = 98, xr = 315, w = xr - x;
  float pw = pill(d, xr, top + 9, stateText(p), sc);
  d.text(x, top + 9, p.task[0] ? p.task : "no job", 1, p.task[0] ? C_TEXT : C_MUTED, 0, w - pw - 6);
  char r[16], e[12], l[24];
  fmtDur(b ? p.remain : 0, r, sizeof r);
  if (!strcmp(p.state, "FINISH")) snprintf(r, sizeof r, "done");
  fmtEta(m, b && strcmp(p.state, "FINISH") ? p.remain : 0, e, sizeof e);
  float ry = top + 27;
  d.text(x, ry, "REMAINING", 5, C_MUTED);
  d.text(x, ry + 16, r, 2, sc == C_DIM ? C_MUTED : sc);
  d.text(xr, ry, "ETA", 5, C_MUTED, 2);
  d.text(xr, ry + 16, e, 2, C_TEXT, 2);
  float ly = ry + 36;
  if (p.tlayer) snprintf(l, sizeof l, "Layer %d/%d", p.layer, p.tlayer);
  else snprintf(l, sizeof l, "Layer --");
  d.text(x, ly, l, 0, C_TEXT);
  if (p.fanPart >= 0) {
    char f[24];
    snprintf(f, sizeof f, "Fan %d%%", p.fanPart);
    d.text(xr, ly, f, 0, C_MUTED, 2);
  }
  float by = ly + 10;
  if (!p.online) {
    char a[20], s[40];
    fmtAge(p.ageS, a, sizeof a);
    snprintf(s, sizeof s, "Offline - data from %s", a);
    d.rrect(x, by, w, 17, 5, C_CARD2);
    d.text(x + w / 2, by + 8.5f, s, 5, C_MUTED, 1, w - 6);
  } else if (p.err[0] || p.nhms) {
    char s[48];
    if (p.err[0]) snprintf(s, sizeof s, "ERROR %s", p.err);
    else snprintf(s, sizeof s, "HMS %s", p.hms[0]);
    d.rrect(x, by, w, 17, 5, C_RED);
    d.text(x + w / 2, by + 8.5f, s, 0, C_TEXT, 1, w - 6);
  }
  // temps
  float ty = top + 92, th = 30, gap = 5;
  int ntile = 2 + (p.dual ? 1 : 0) + (p.cham > -999 ? 1 : 0);
  float tw = (320 - 8 - gap * (ntile - 1)) / ntile;
  int i = 0;
  if (p.dual) {
    tempTile(d, 4 + (i++) * (tw + gap), ty, tw, th, p.active == 0 ? "LEFT *" : "LEFT", p.noz, p.nozT, C_ORANGE);
    tempTile(d, 4 + (i++) * (tw + gap), ty, tw, th, p.active == 1 ? "RIGHT *" : "RIGHT", p.noz2, p.noz2T, C_ORANGE);
  } else {
    tempTile(d, 4 + (i++) * (tw + gap), ty, tw, th, "NOZZLE", p.noz, p.nozT, C_ORANGE);
  }
  tempTile(d, 4 + (i++) * (tw + gap), ty, tw, th, "BED", p.bed, p.bedT, C_ORANGE);
  if (p.cham > -999) tempTile(d, 4 + (i++) * (tw + gap), ty, tw, th, "CHAMBER", p.cham, 0, C_CYAN);
  // AMS
  float ay = ty + th + 5;
  amsRow(d, p, 4, ay, 312, TABY - ay - 4);
}

// --------------------------------------------------------- EVENTS page ----
static uint16_t kindColor(const char *k) {
  if (!strcmp(k, "finished")) return C_BLUE;
  if (!strcmp(k, "failed") || !strcmp(k, "error")) return C_RED;
  if (!strcmp(k, "paused")) return C_YELLOW;
  if (!strcmp(k, "offline")) return C_DIM;
  if (!strcmp(k, "started") || !strcmp(k, "resumed") || !strcmp(k, "online")) return C_GREEN;
  return C_MUTED;
}
static const char *kindText(const char *k) {
  if (!strcmp(k, "finished")) return "DONE";
  if (!strcmp(k, "failed")) return "FAILED";
  if (!strcmp(k, "error")) return "ERROR";
  if (!strcmp(k, "paused")) return "PAUSED";
  if (!strcmp(k, "offline")) return "OFFLINE";
  if (!strcmp(k, "online")) return "ONLINE";
  if (!strcmp(k, "started")) return "START";
  if (!strcmp(k, "resumed")) return "RESUME";
  return k;
}
static void eventsPage(const D &d, const Model &m) {
  header(d, m, "Recent events", C_TEXT);
  if (!m.nev) {
    d.text(160, 110, "No events yet", 1, C_MUTED, 1);
    return;
  }
  float y = HDR + 2, rh = (TABY - 2 - y) / 8;
  int n = m.nev > 8 ? 8 : m.nev;
  for (int i = 0; i < n; i++) {
    const Event &e = m.ev[i];
    float yc = y + i * rh + rh / 2;
    if (i % 2 == 0) d.rect(0, y + i * rh, 320, rh, C_CARD);
    char tb[16] = "--:--";
    if (e.t) {
      struct tm tm, nw;
      localtime_r(&e.t, &tm);
      localtime_r(&m.now, &nw);
      if (m.now && tm.tm_yday != nw.tm_yday) snprintf(tb, sizeof tb, "%d/%d", tm.tm_mday, tm.tm_mon + 1);
      else snprintf(tb, sizeof tb, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
    d.text(6, yc, tb, 0, C_MUTED);
    uint16_t kc = kindColor(e.kind);
    d.rrect(48, yc - 7, 56, 14, 4, kc);
    d.text(76, yc, kindText(e.kind), 5, C_BG, 1);
    d.text(111, yc, e.printer, 1, C_TEXT, 0, 60);
    const char *s = e.err[0] ? e.err : e.task;
    d.text(162, yc, s, 0, e.err[0] ? C_RED : C_MUTED, 0, 152);
  }
}

// ---------------------------------------------------------- SETUP page ----
struct Btn { float x, y, w, h; };
static const float SET_ROW0 = HDR + 4, SET_RH = 21;
static const Btn B_DN = {150, SET_ROW0 + 1, 30, SET_RH - 2}, B_UP = {222, SET_ROW0 + 1, 30, SET_RH - 2};
static Btn setBtn(int i) {  // bottom buttons: ROTATE, TOUCH, WI-FI SETUP
  float w = (320 - 8 - 2 * 5) / 3.0f;
  return {4 + i * (w + 5), TABY - 31, w, 27};
}
static void button(const D &d, const Btn &b, const char *t, uint16_t c = 0) {
  d.rrect(b.x, b.y, b.w, b.h, 6, c ? c : C_CARD2);
  d.text(b.x + b.w / 2, b.y + b.h / 2, t, 0, C_TEXT, 1, b.w - 4);
}
static void setupPage(const D &d, const Model &m, const UiState &u) {
  header(d, m, "Setup", C_TEXT);
  char b[64], a[20];
  float y = SET_ROW0 + SET_RH / 2;
  auto row = [&](const char *k, const char *v, uint16_t vc) {
    d.text(8, y, k, 0, C_MUTED);
    d.text(84, y, v, 0, vc, 0, 230);
    y += SET_RH;
  };
  d.text(8, y, "Brightness", 0, C_MUTED);
  button(d, B_DN, "-");
  button(d, B_UP, "+");
  for (int i = 0; i < 5; i++) d.rrect(186 + i * 6.8f, y - 6, 5, 12, 1, i < u.bright ? C_BAMBU : C_LINE);
  y += SET_RH;
  if (m.wifi) snprintf(b, sizeof b, "%s  (%d dBm)", m.ssid, m.rssi);
  else snprintf(b, sizeof b, "%s", m.ssid[0] ? "connecting..." : "not set");
  row("Wi-Fi", b, m.wifi ? C_TEXT : C_ORANGE);
  row("IP", m.wifi ? m.ip : "-", C_TEXT);
  fmtAge(m.fetchAgeS, a, sizeof a);
  if (m.cloudOk) snprintf(b, sizeof b, "OK, updated %s", a);
  else snprintf(b, sizeof b, "%s", m.cloudErr[0] ? m.cloudErr : "no data yet");
  row("Cloud", b, m.cloudOk ? C_GREEN : C_ORANGE);
  fmtAge(m.bridgeAgeS, a, sizeof a);
  if (m.haveData && m.devices > 1) snprintf(b, sizeof b, "%d/%d boards online, %s", m.devicesOnline, m.devices, a);
  else if (m.haveData) snprintf(b, sizeof b, "%s %s, %s", m.bridgeName, m.bridgeOnline ? "online" : "OFFLINE", a);
  else snprintf(b, sizeof b, "-");
  row("Bridge", b, m.bridgeOnline ? C_GREEN : C_ORANGE);
  snprintf(b, sizeof b, "...%s", m.keyTail);
  row("View key", b, C_MUTED);
  snprintf(b, sizeof b, "%s", m.fw);
  row("Firmware", b, C_MUTED);
  button(d, setBtn(0), "ROTATE");
  button(d, setBtn(1), "TOUCH CAL");
  button(d, setBtn(2), "WI-FI SETUP", C_BAMBU);
}

// ------------------------------------------------------------- public ----
static D mk(LovyanGFX &g, int W, int H, int oy) { return D{g, W, H, oy, W / 320.0f, H / 240.0f, W >= 400}; }

static void toastBar(const D &d, const UiState &u) {
  if (!u.toast[0]) return;
  float y = TABY - 30;
  d.rrect(6, y, 308, 26, 7, u.toastColor ? u.toastColor : C_BLUE);
  d.text(160, y + 13, u.toast, 1, C_BG, 1, 300);
}

void uiDraw(LovyanGFX &g, int W, int H, int oy, const Model &m, const UiState &u) {
  D d = mk(g, W, H, oy);
  g.fillScreen(C_BG);
  switch (u.page) {
    case PG_ALL: allPage(d, m); break;
    case PG_ONE: onePage(d, m, u); break;
    case PG_EVENTS: eventsPage(d, m); break;
    default: setupPage(d, m, u); break;
  }
  if (u.page != PG_SETUP) toastBar(d, u);
  tabs(d, u.page);
}

void uiDrawSetupAp(LovyanGFX &g, int W, int H, int oy, const Model &m) {
  D d = mk(g, W, H, oy);
  g.fillScreen(C_BG);
  d.rect(0, 0, 320, 34, C_BAMBU);
  d.text(160, 17, "Wi-Fi setup", 2, C_TEXT, 1);
  float y = 58;
  d.text(16, y, "1. On your phone, join Wi-Fi", 0, C_MUTED);
  d.rrect(30, y + 12, 260, 28, 6, C_CARD);
  d.text(160, y + 26, m.apName, 2, C_TEXT, 1);
  y += 56;
  d.text(16, y, "2. Open this page (pops up by itself)", 0, C_MUTED);
  d.rrect(30, y + 12, 260, 28, 6, C_CARD);
  d.text(160, y + 26, "http://192.168.4.1", 2, C_BAMBU, 1);
  y += 56;
  d.text(16, y, "3. Pick your home Wi-Fi, enter password, Save", 0, C_MUTED, 0, 296);
  d.text(160, 224, m.ssid[0] ? "Still trying the saved Wi-Fi too..." : "No Wi-Fi saved yet", 0, C_DIM, 1);
}

static bool hit(const D &d, const Btn &b, int x, int y) {
  return x >= d.X(b.x) && x <= d.X(b.x + b.w) && y >= d.Y(b.y) && y <= d.Y(b.y + b.h);
}

void uiSwipe(int dir, const Model &m, UiState &u) {
  if (!m.np) return;
  u.focus = (u.focus + dir + m.np) % m.np;
}

Action uiTouch(int W, int H, int x, int y, const Model &m, UiState &u) {
  static lgfx::LGFX_Sprite dummy;
  D d = mk(dummy, W, H, 0);
  if (u.toast[0] && y >= d.Y(TABY - 32) && y < d.Y(TABY)) {
    u.toast[0] = 0;
    return A_REDRAW;
  }
  if (y >= d.Y(TABY)) {
    int t = x * PG_COUNT / W;
    if (t == PG_ONE && u.page == PG_ONE) uiSwipe(1, m, u);
    u.page = t;
    return A_REDRAW;
  }
  switch (u.page) {
    case PG_ALL: {
      int n = m.np > MAX_TILES ? MAX_TILES : m.np, cols, rows;
      if (!n || y < d.Y(HDR)) return A_NONE;
      gridShape(n, cols, rows);
      int c = x * cols / W, r = (y - d.Y(HDR)) * rows / (d.Y(TABY) - d.Y(HDR));
      int i = r * cols + c;
      if (i < n) {
        u.focus = i;
        u.page = PG_ONE;
        return A_REDRAW;
      }
      return A_NONE;
    }
    case PG_ONE:
      if (y < d.Y(HDR + 40)) {
        if (x < d.X(160)) uiSwipe(-1, m, u);
        else uiSwipe(1, m, u);
        return A_REDRAW;
      }
      uiSwipe(x < W / 2 ? -1 : 1, m, u);
      return A_REDRAW;
    case PG_EVENTS: return A_REFRESH;
    case PG_SETUP: {
      Btn dn = B_DN, up = B_UP;
      if (hit(d, dn, x, y)) { if (u.bright > 1) u.bright--; return A_BRIGHT_DN; }
      if (hit(d, up, x, y)) { if (u.bright < 5) u.bright++; return A_BRIGHT_UP; }
      if (hit(d, setBtn(0), x, y)) return A_ROTATE;
      if (hit(d, setBtn(1), x, y)) return A_CALIBRATE;
      if (hit(d, setBtn(2), x, y)) return A_WIFI_SETUP;
      return A_NONE;
    }
  }
  return A_NONE;
}

void uiMakeToast(const Event &e, UiState &u) {
  u.toastId = e.id;
  const char *k = e.kind;
  if (!strcmp(k, "finished")) snprintf(u.toast, sizeof u.toast, "%s finished %s", e.printer, e.task);
  else if (!strcmp(k, "failed")) snprintf(u.toast, sizeof u.toast, "%s print FAILED", e.printer);
  else if (!strcmp(k, "error")) snprintf(u.toast, sizeof u.toast, "%s error %s", e.printer, e.err);
  else if (!strcmp(k, "paused")) snprintf(u.toast, sizeof u.toast, "%s paused %s", e.printer, e.err);
  else if (!strcmp(k, "offline")) snprintf(u.toast, sizeof u.toast, "%s went offline", e.printer);
  else { u.toast[0] = 0; return; }
  u.toastColor = kindColor(k);
}
