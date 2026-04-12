// viewer_cvbs_config.h — CVBS composite video output on ESP32 (GPIO25/26 DAC)
#pragma once
#define LGFX_USE_V1

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <JPEGDEC.h>
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32/Panel_CVBS.hpp>

#define WDT_TIMEOUT_SEC 15
#define CFG_PANEL_WIDTH 270
#define CFG_PANEL_HEIGHT 180
// 240*160=38400
// 270*180=48600
// 285*190=54150
// 300*200=60000
// 375*250=93750
// 405*270=109350

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

// ---- Display driver: CVBS composite video via DAC ----
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_CVBS _panel_instance;
public:
    LGFX() {
        {
            auto cfg = _panel_instance.config();
            cfg.memory_width  = CFG_PANEL_WIDTH;
            cfg.memory_height = CFG_PANEL_HEIGHT;
            cfg.panel_width   = CFG_PANEL_WIDTH;
            cfg.panel_height  = CFG_PANEL_HEIGHT;
            cfg.offset_x      = 0;
            cfg.offset_y      = 0;
            cfg.offset_rotation = 0;
            _panel_instance.config(cfg);
        }
        { // Signal + DAC settings
            auto cfg = _panel_instance.config_detail();
#ifdef VIEWER_CVBS_PAL
            cfg.signal_type  = cfg.signal_type_t::PAL;
#else
            cfg.signal_type  = cfg.signal_type_t::NTSC_J;
#endif
#ifdef VIEWER_CVBS_PIN
            cfg.pin_dac      = VIEWER_CVBS_PIN;
#else
            cfg.pin_dac      = 26;
#endif
            cfg.use_psram     = 0;     // no external PSRAM available
            cfg.output_level  = 128;
            cfg.chroma_level  = 128;
            _panel_instance.config_detail(cfg);
        }
        setPanel(&_panel_instance);
    }
};
