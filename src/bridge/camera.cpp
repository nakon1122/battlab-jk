#include "camera.h"

bool JpegCamera::begin() {
  if (!psramFound()) {
    lastErr = "no PSRAM on this board";
    return false;
  }
  buf_ = (uint8_t *)ps_malloc(CAP);
  tmp_ = (uint8_t *)ps_malloc(CAP);
  if (!buf_ || !tmp_) {
    lastErr = "PSRAM alloc failed";
    buf_ = tmp_ = nullptr;
    return false;
  }
  return true;
}

void JpegCamera::watch(const String &serial, const String &ip, const String &code) {
  if (serial == serial_ && connected_) return;
  stop();
  serial_ = serial;
  ip_ = ip;
  code_ = code;
  len_ = 0;
  nextTry_ = 0;
  lastErr = "";
}

void JpegCamera::stop() {
  if (connected_) tls_.stop();
  connected_ = false;
  serial_ = "";
}

bool JpegCamera::readFull_(uint8_t *p, size_t n, uint32_t timeoutMs) {
  uint32_t t = millis();
  while (n) {
    int a = tls_.available();
    if (a <= 0) {
      if (!tls_.connected() || millis() - t > timeoutMs) return false;
      delay(1);
      continue;
    }
    int r = tls_.read(p, n < (size_t)a ? n : (size_t)a);
    if (r <= 0) return false;
    p += r;
    n -= r;
  }
  return true;
}

bool JpegCamera::connect_() {
  tls_.setInsecure();
  tls_.setHandshakeTimeout(10);
  if (!tls_.connect(ip_.c_str(), 6000)) {
    lastErr = "camera: cannot connect (port 6000)";
    nextTry_ = millis() + 5000;
    return false;
  }
  // 80-byte auth: 0x40, 0x3000, 0, 0, user[32], access code[32]  (little endian)
  uint8_t a[80] = {0};
  a[0] = 0x40;
  a[5] = 0x30;
  memcpy(a + 16, "bblp", 4);
  memcpy(a + 48, code_.c_str(), code_.length() > 32 ? 32 : code_.length());
  tls_.write(a, sizeof a);
  connected_ = true;
  lastRx_ = millis();
  return true;
}

bool JpegCamera::loop() {
  if (!buf_ || !serial_.length()) return false;
  if (!connected_) {
    if ((int32_t)(millis() - nextTry_) < 0) return false;
    if (!connect_()) return false;
  }
  if (!tls_.connected() || millis() - lastRx_ > 12000) {
    lastErr = millis() - lastRx_ > 12000 ? "camera: no frames (wrong code or camera off?)" : "camera: connection closed";
    tls_.stop();
    connected_ = false;
    nextTry_ = millis() + 5000;
    return false;
  }
  bool got = false;
  // read every frame that is waiting, keep the newest
  for (int i = 0; i < 6 && tls_.available() >= 16; i++) {
    uint8_t h[16];
    if (!readFull_(h, 16, 3000)) break;
    size_t n = (size_t)h[0] | ((size_t)h[1] << 8) | ((size_t)h[2] << 16);
    if (n < 100 || n > CAP) {
      lastErr = "camera: bad frame header";
      tls_.stop();
      connected_ = false;
      nextTry_ = millis() + 3000;
      return false;
    }
    if (!readFull_(tmp_, n, 6000)) {
      lastErr = "camera: read timeout";
      tls_.stop();
      connected_ = false;
      nextTry_ = millis() + 3000;
      return false;
    }
    lastRx_ = millis();
    if (tmp_[0] == 0xFF && tmp_[1] == 0xD8) {
      uint8_t *t = buf_;
      buf_ = tmp_;
      tmp_ = t;
      len_ = n;
      frames++;
      got = true;
    }
  }
  return got;
}
