// viewer_config.h — ILI9488 480x320 TFT + XPT2046 touch on ESP32-S3
#pragma once
#define LGFX_USE_V1

#include <Arduino.h>
#include <SPI.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <JPEGDEC.h>
#include <LovyanGFX.hpp>

#define WDT_TIMEOUT_SEC 15

// ---- Performance tracker: FPS + decode time ----
struct PerfTracker {
    float fps = 0, decodeMs = 0, minDecode = 999, maxDecode = 0;
    void frameDone(uint32_t decodeUs) {
        _frames++;
        float ms = decodeUs / 1000.0f;
        _decodeSum += ms;
        if (ms < minDecode) minDecode = ms;
        if (ms > maxDecode) maxDecode = ms;
        uint32_t now = millis();
        uint32_t dt = now - _lastT;
        if (dt >= 1000) {
            fps = _frames * 1000.0f / dt;
            decodeMs = _decodeSum / _frames;
            _frames = 0; _decodeSum = 0; _lastT = now;
        }
    }
    void reset() { minDecode = 999; maxDecode = 0; }
private:
    uint32_t _lastT = 0, _frames = 0;
    float _decodeSum = 0;
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
            c.spi_host = SPI2_HOST;  c.spi_mode = 3;
            c.freq_write = 60000000; c.freq_read = 20000000;
            c.spi_3wire = false;     c.use_lock = false;
            c.dma_channel = SPI_DMA_CH_AUTO;
            c.pin_mosi = 45; c.pin_miso = -1; c.pin_sclk = 3; c.pin_dc = 47;
            _bus.config(c);
            _panel.setBus(&_bus);
        }
        { // Panel
            auto c = _panel.config();
            c.pin_cs = 14;  c.pin_rst = 21;  c.pin_busy = -1;
            c.memory_width = 320;  c.memory_height = 480;
            c.panel_width  = 320;  c.panel_height  = 480;
            c.offset_rotation = 0;
            c.readable = false; c.invert = false;
            c.rgb_order = false; c.dlen_16bit = false; c.bus_shared = false;
            _panel.config(c);
        }
        { // Touch — XPT2046 on SPI3
            auto c = _touch.config();
            c.spi_host = SPI3_HOST;
            c.pin_sclk = 42; c.pin_mosi = 2; c.pin_miso = 41; c.pin_cs = 1;
            c.x_min = 240; c.x_max = 3860; c.y_min = 140; c.y_max = 3860;
            c.freq = 2500000; c.bus_shared = false;
            _touch.config(c);
            _panel.setTouch(&_touch);
        }
        setPanel(&_panel);
    }
};
