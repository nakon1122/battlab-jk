#pragma once
// Chamber camera of A1 / A1 mini / P1P / P1S: JPEG frames over TLS on port 6000.
// Only runs while somebody watches it on the dashboard; frames are uploaded
// to the cloud every ~2 s. Needs PSRAM (frames are 50-200 KB).
#include <Arduino.h>
#include <WiFiClientSecure.h>

class JpegCamera {
 public:
  bool begin();                 // allocate buffers (false = no PSRAM)
  bool available() const { return buf_ != nullptr; }
  void watch(const String &serial, const String &ip, const String &code);
  void stop();
  // keep reading frames; returns true when a new full frame is in frame()/len()
  bool loop();
  const uint8_t *frame() const { return buf_; }
  size_t len() const { return len_; }
  const String &serial() const { return serial_; }
  String lastErr;
  uint32_t frames = 0;

 private:
  WiFiClientSecure tls_;
  String serial_, ip_, code_;
  uint8_t *buf_ = nullptr, *tmp_ = nullptr;
  size_t len_ = 0;
  static const size_t CAP = 600 * 1024;
  bool connected_ = false;
  uint32_t nextTry_ = 0, lastRx_ = 0;
  bool connect_();
  bool readFull_(uint8_t *p, size_t n, uint32_t timeoutMs);
};
