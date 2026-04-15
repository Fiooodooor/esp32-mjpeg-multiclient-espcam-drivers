// tft_viewer_config.h — ILI9488 480x320 TFT + XPT2046 touch on ESP32-S3
#pragma once
#define LGFX_USE_V1

#include "definitions.h"
#include "references.h"

#include <JPEGDEC.h>
#include <LovyanGFX.hpp>

// ---- Build-time defaults (override via -D flags) ----
#ifndef VIEWER_WIFI_SSID
#define VIEWER_WIFI_SSID  "Akwarium-2-4"
#endif
#ifndef VIEWER_WIFI_PASS
#define VIEWER_WIFI_PASS  "wifi2da2875277"
#endif
#ifndef VIEWER_STREAM_HOST
#define VIEWER_STREAM_HOST "192.168.1.100"
#endif
#ifndef VIEWER_STREAM_PORT
#define VIEWER_STREAM_PORT 80
#endif
#ifndef VIEWER_STREAM_PATH
#define VIEWER_STREAM_PATH "/mjpeg/1"
#endif

#define T_CLK 14
#define T_CS 33
#define T_DIN 13
#define T_DO 12
// Touch IRQ not used — polling mode

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS 15
#define TFT_DC 2
#define TFT_RST 32

// ---- Constants ----
enum : uint32_t {
    MAX_JPEG_BUF   = 120 * 1024,
    NUM_BUFS       = 3,
    DL_STACK       = 12 * 1024,
    CONN_TIMEOUT   = 5000,
    READ_TIMEOUT   = 10000,
    BACKOFF_INIT   = 500,
    BACKOFF_MAX    = 30000,
    OSD_INTERVAL   = 1000,
    STATS_INTERVAL = 3000,
    FRAME_TIMEOUT  = 5000,
    HDR_BUF        = 512,
    W = 480, H = 320,
};

const uint16_t ST_CLR[] = {
  TFT_RED, TFT_YELLOW, TFT_RED, TFT_YELLOW, TFT_GREEN
};

#define WDT_TIMEOUT_SEC 15

// ---- Performance tracker: FPS + decode time ----
struct PerfTracker {
    struct t_PerfTracker{
        uint32_t _frames = 0;
        uint32_t _lastT = 0;
        uint32_t now = 0;
        uint32_t dt = 0;

        float ms = 0;
        float fps = 0;
        float decodeMs = 0;
        float minDecode = 999;
        float maxDecode = 0;
        float _decodeSum = 0;
    } perf;
    void frameDone(uint32_t decodeUs) {
        perf._frames++;
        perf.ms = decodeUs / 1000.0f;
        perf._decodeSum += perf.ms;
        if (perf.ms < perf.minDecode) perf.minDecode = perf.ms;
        if (perf.ms > perf.maxDecode) perf.maxDecode = perf.ms;
        perf.now = millis();
        perf.dt = perf.now - perf._lastT;
        if (perf.dt >= 1000) {
            perf.fps = perf._frames * 1000.0f / perf.dt;
            perf.decodeMs = perf._decodeSum / perf._frames;
            perf._frames = 0; perf._decodeSum = 0; perf._lastT = perf.now;
        }
    }
    void reset() { 
        perf.minDecode = 999;
        perf.maxDecode = 0;
    }
};

// ---- Display driver ----
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Touch_XPT2046 _touch;
public:
  LGFX() {
    { // SPI bus — 80 MHz write, DMA auto
      auto c = _bus.config();
      c.spi_host = SPI2_HOST;
      c.spi_mode = 3;
      c.freq_write = 60000000;
      c.freq_read = 20000000;
      c.spi_3wire = false;
      c.use_lock = false;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_mosi = TFT_MOSI;
      c.pin_miso = TFT_MISO;
      c.pin_sclk = TFT_SCLK;
      c.pin_dc = TFT_DC;
      _bus.config(c);
      _panel.setBus(&_bus);
    } { // Panel
      auto c = _panel.config();
      c.pin_cs = TFT_CS;
      c.pin_rst = TFT_RST;
      c.pin_busy = -1;
      c.memory_width = 320;
      c.memory_height = 480;
      c.panel_width  = 320;
      c.panel_height  = 480;
      c.offset_rotation = 0;
      c.readable = false;
      c.invert = false;
      c.rgb_order = false;
      c.dlen_16bit = false;
      c.bus_shared = false;
      _panel.config(c);
    } { // Touch — XPT2046 shares SPI2 bus with TFT
      auto c = _touch.config();
      c.spi_host = SPI2_HOST;
      c.pin_sclk = T_CLK;
      c.pin_mosi = T_DIN;
      c.pin_miso = T_DO;
      c.pin_cs = T_CS;
      c.x_min = 240;
      c.x_max = 3860;
      c.y_min = 140;
      c.y_max = 3860;
      c.freq = 2500000;
      c.bus_shared = true;
      _touch.config(c);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
  void cfgLoad();
  void cfgSave();

  void setupTft();
  void frameReceiveTask();
  void pollSerial();
  void dlTask(void*);

  bool netReadExact(WiFiClient& c, uint8_t* buf, size_t len);
  bool skipHttpHeaders(WiFiClient& c);
  int parsePartHeader(WiFiClient& c, size_t& clen, uint8_t* jbuf);
  void ensureWiFi();
  void drawOSD();
  void drawSplash(const char* msg, uint16_t clr);
  void checkTouch();
};

