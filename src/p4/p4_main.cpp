// BATTLAB on ESP32-P4 + ESP32-C5 (KSDIY P4C5): phase 1 — bring up the new RISC-V
// toolchain and prove it compiles/boots. WiFi (via the C5 esp-hosted co-processor),
// RS485, the MIPI-DSI screen and BLE are added in later phases.
// Built only in the `p4c5` PlatformIO env (pioarduino platform); the ESP32-S3 / CYD
// firmware does not include this file.
#if defined(BATTLAB_P4)
#include <Arduino.h>

#define BATTLAB_FW "0.1.0-p4c5"

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.printf("BATTLAB %s on %s\n", BATTLAB_FW, ESP.getChipModel());
  Serial.printf("cores %d  flash %u MB  psram %u MB  heap %u KB\n",
                ESP.getChipCores(), ESP.getFlashChipSize() / (1024 * 1024),
                ESP.getPsramSize() / (1024 * 1024), ESP.getFreeHeap() / 1024);
  Serial.println("phase 1: toolchain up. next: Wi-Fi via C5, RS485, MIPI-DSI screen.");
}

uint32_t last = 0;
void loop() {
  if (millis() - last > 2000) {
    last = millis();
    Serial.printf("alive %lus  heap %u KB  psram free %u KB\n",
                  (unsigned long)(millis() / 1000), ESP.getFreeHeap() / 1024,
                  ESP.getFreePsram() / 1024);
  }
  delay(10);
}
#endif
