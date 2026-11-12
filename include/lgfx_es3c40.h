// LCDwiki ES3C40P: ESP32-S3 (16 MB flash, 8 MB OPI PSRAM) + 4.0" IPS ST7796S 320x480 (SPI)
// + FT6336U capacitive touch (I2C 0x38). Pin map from the LCDwiki ES3C40P ESP-IDF demo:
//   LCD  CS 10  DC 46  SCK 12  MOSI 11  MISO 13  RST = EN (shared)  backlight 45
//   touch SDA 16  SCL 15  INT 17  RST 18
//   free for RS485: UART header RX 44 / TX 43
// Same panel driver and resolution as the CYD 3.5", so every screen of the CYD build is reused.
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#define BATTLAB_TOUCH_CAPACITIVE 1   // no calibration screen / button
#define BATTLAB_SCREEN_INV_DEFAULT true // IPS panel

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
  lgfx::Touch_FT5x06 touch_; // FT6336U speaks the FT5x06 protocol

public:
  void configure(bool invert, bool rgbOrder) {
    {
      auto cfg = bus_.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 12;
      cfg.pin_mosi = 11;
      cfg.pin_miso = 13;
      cfg.pin_dc = 46;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = 10;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = true;
      cfg.invert = invert;
      cfg.rgb_order = rgbOrder;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      panel_.config(cfg);
    }
    {
      auto cfg = light_.config();
      cfg.pin_bl = 45;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      light_.config(cfg);
      panel_.setLight(&light_);
    }
    {
      auto cfg = touch_.config();
      cfg.x_min = 0;
      cfg.x_max = 319;
      cfg.y_min = 0;
      cfg.y_max = 479;
      cfg.pin_int = 17;
      cfg.pin_rst = 18;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = 1;
      cfg.i2c_addr = 0x38;
      cfg.pin_sda = 16;
      cfg.pin_scl = 15;
      cfg.freq = 400000;
      touch_.config(cfg);
      panel_.setTouch(&touch_);
    }
    setPanel(&panel_);
  }
};
