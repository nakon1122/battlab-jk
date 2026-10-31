# Dependencies and protocol references

Protocol field layout and read command format were checked against syssi/esphome-jk-bms, components/jk_bms_ble/jk_bms_ble.cpp, retrieved 2026-10-30. That upstream project is Apache-2.0 licensed. This project has a separate Arduino/NimBLE implementation and does not bundle the ESPHome component.

Runtime dependencies fetched by PlatformIO:
- Arduino ESP32 core / Espressif platform: https://github.com/espressif/arduino-esp32
- NimBLE-Arduino 1.4.2: https://github.com/h2zero/NimBLE-Arduino/tree/1.4.2 (Apache-2.0)
- ArduinoJson 6.21.5: https://github.com/bblanchon/ArduinoJson/tree/v6.21.5 (MIT)

Consult each dependency's included license before redistributing firmware commercially. BATTLAB Cloud licensing and payment code are not included in this firmware release.
