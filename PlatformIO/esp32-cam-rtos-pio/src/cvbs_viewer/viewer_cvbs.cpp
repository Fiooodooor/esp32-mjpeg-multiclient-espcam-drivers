/*
 * ESP32 MJPEG Viewer — CVBS Composite Video Output
 *
 * Core 0: WiFi download (PS off, bulk header parse, taskYIELD I/O)
 * Core 1: JPEG decode → CVBS output scaled to 480x320 via DAC (GPIO25/26)
 * Double-buffered MJPEG download with a 1/4-sized CVBS canvas memory footprint.
 *
 * Serial: SET host:port/path | STATUS | RESET
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include "cvbs_viewer/viewer_cvbs_config.h"

// ---- Build-time defaults (override via -D flags) ----
#ifndef VIEWER_WIFI_SSID
#define VIEWER_WIFI_SSID  "Akwarium-2-4"
#endif
#ifndef VIEWER_WIFI_PASS
#define VIEWER_WIFI_PASS  "wifi2da2875277"
#endif
#ifndef VIEWER_STREAM_HOST
#define VIEWER_STREAM_HOST "192.168.50.86"
#endif
#ifndef VIEWER_STREAM_PORT
#define VIEWER_STREAM_PORT 80
#endif
#ifndef VIEWER_STREAM_PATH
#define VIEWER_STREAM_PATH "/mjpeg/1"
#endif

// ---- Constants ----
enum : uint32_t {
    MAX_JPEG_BUF   = 24 * 1024,
    NUM_BUFS       = 2,
    DL_STACK       = 16 * 1024,
    CONN_TIMEOUT   = 5000,
    READ_TIMEOUT   = 10000,
    BACKOFF_INIT   = 500,
    BACKOFF_MAX    = 30000,
    OSD_INTERVAL   = 1000,
    STATS_INTERVAL = 3000,
    FRAME_TIMEOUT  = 5000,
    HDR_BUF        = 512,
    W = CFG_PANEL_WIDTH,
    H = CFG_PANEL_HEIGHT,
};

// ---- Connection state machine ----
enum ConnState : uint8_t {
    S_WIFI_DOWN, S_WIFI_UP, S_NO_STREAM, S_CONNECTING, S_STREAMING
};
static const char* const ST_STR[] = {
    "WiFi Down", "WiFi Up", "No Stream", "Connecting", "Streaming"
};
static const uint16_t ST_CLR[] = {
    TFT_RED, TFT_YELLOW, TFT_RED, TFT_YELLOW, TFT_GREEN
};

// ---- Frame descriptor for queue-based single buffer ----
struct FrameDesc {
    uint8_t* buf;
    size_t   len;
};

// ---- Globals ----
static LGFX            tft;
static JPEGDEC         jpeg;
static PerfTracker     perf;
static Preferences     prefs;
static WiFiClient      cli;
TaskHandle_t dlTaskHandle = NULL;

static uint8_t* jpegBuf[NUM_BUFS];
// static uint8_t jpegBuf[NUM_BUFS][MAX_JPEG_BUF];

static volatile size_t frameSize;

// Triple-buffer queues
static QueueHandle_t freeQ  = nullptr;
static QueueHandle_t readyQ = nullptr;

static volatile uint32_t  totFrames  = 0;
static volatile uint32_t  totErrors  = 0;
static volatile uint32_t  totReconn  = 0;
static volatile uint32_t  droppedFr  = 0;
static volatile ConnState connSt     = S_WIFI_DOWN;
static volatile int8_t    rssi       = 0;
static volatile bool      osdOn      = true;
static uint32_t           bootTime   = 0;

static char     cfgHost[32];
static char     cfgPath[32];
static uint16_t cfgPort = 80;

// ---- JPEG decode callback — direct CVBS framebuffer update ----
static int onJpegDraw(JPEGDRAW* d) {
    tft.pushImageDMA(d->x, d->y, d->iWidth, d->iHeight, (lgfx::swap565_t*)d->pPixels);
    return 1;
}

// ---- Network I/O (taskYIELD for minimal latency) ----
static bool netReadExact(WiFiClient& c, uint8_t* buf, size_t len) {
    size_t got = 0;
    uint32_t last = millis();
    while (got < len && c.connected()) {
        if (millis() - last > READ_TIMEOUT) return false;
        int a = c.available();
        if (a > 0) {
            int r = c.read(buf + got, min((size_t)a, len - got));
            if (r > 0) { got += r; last = millis(); }
        } else taskYIELD();
    }
    return got == len;
}

// Skip HTTP response headers (one-time after connect)
static bool skipHttpHeaders(WiFiClient& c) {
    char line[256];
    size_t pos = 0;
    uint32_t t0 = millis();
    while (c.connected() && millis() - t0 < 5000) {
        if (!c.available()) { taskYIELD(); continue; }
        char ch = c.read();
        if (ch == '\n') {
            if (pos == 0 || (pos == 1 && line[0] == '\r')) return true;
            pos = 0;
        } else if (pos < sizeof(line) - 1) line[pos++] = ch;
    }
    return false;
}

// Bulk-parse MJPEG part header. Returns pre-read JPEG bytes or -1 on error.
static int parsePartHeader(WiFiClient& c, size_t& clen, uint8_t* jbuf) {
    char hdr[HDR_BUF];
    size_t pos = 0;
    clen = 0;
    uint32_t t0 = millis();
    while (pos < sizeof(hdr) - 1 && c.connected() && millis() - t0 < 5000) {
        int a = c.available();
        if (a <= 0) { taskYIELD(); continue; }
        int r = c.read((uint8_t*)hdr + pos, min(a, (int)(sizeof(hdr) - 1 - pos)));
        if (r <= 0) continue;
        pos += r;
        hdr[pos] = 0;
        char* end = strstr(hdr, "\r\n\r\n");
        if (end) {
            *end = 0;
            char* cl = strstr(hdr, "ength:");
            if (!cl) cl = strstr(hdr, "ENGTH:");
            if (cl) clen = strtoul(cl + 6, nullptr, 10);
            if (clen == 0 || clen > MAX_JPEG_BUF) return -1;
            size_t hlen = (end + 4) - hdr;
            size_t extra = pos - hlen;
            if (extra > 0) memcpy(jbuf, hdr + hlen, extra);
            return (int)extra;
        }
    }
    return -1;
}

// ---- WiFi reconnect with exponential backoff ----
static void ensureWiFi() {
    if (WiFi.status() == WL_CONNECTED) { rssi = WiFi.RSSI(); return; }
    connSt = S_WIFI_UP;
    Serial.println("[W] Reconnecting...");
    WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(VIEWER_WIFI_SSID, VIEWER_WIFI_PASS);
    for (uint32_t bo = BACKOFF_INIT; WiFi.status() != WL_CONNECTED; ) {
        vTaskDelay(pdMS_TO_TICKS(bo));
        if (bo < BACKOFF_MAX) bo <<= 1;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    rssi = WiFi.RSSI();
    Serial.printf("[W] OK %s RSSI=%d ps=off\n", WiFi.localIP().toString().c_str(), rssi);
}

// ---- Download task (Core 0) ----
static void dlTask(void*) {
    cli.setNoDelay(true);
    cli.setTimeout(READ_TIMEOUT / 1000);
    uint32_t bo = BACKOFF_INIT;

    for (;;) {
        ensureWiFi();

        if (!cli.connected()) {
            connSt = S_NO_STREAM;
            Serial.printf("[DL] -> %s:%d%s\n", cfgHost, cfgPort, cfgPath);
            if (!cli.connect(cfgHost, cfgPort, CONN_TIMEOUT)) {
                totErrors++;
                vTaskDelay(pdMS_TO_TICKS(bo));
                if (bo < BACKOFF_MAX) bo <<= 1;
                continue;
            }
            connSt = S_CONNECTING;
            cli.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n",
                       cfgPath, cfgHost);
            if (!skipHttpHeaders(cli) || !cli.connected()) { cli.stop(); continue; }
            bo = BACKOFF_INIT;
            totReconn++;
            connSt = S_STREAMING;
            Serial.println("[DL] Streaming");
        }

        FrameDesc fd;
        if (xQueueReceive(freeQ, &fd, pdMS_TO_TICKS(FRAME_TIMEOUT)) != pdTRUE) {
            droppedFr++;
            continue;
        }

        size_t clen;
        int pre = parsePartHeader(cli, clen, fd.buf);
        if (pre < 0) {
            xQueueSend(freeQ, &fd, 0);
            if (!cli.connected()) { connSt = S_NO_STREAM; cli.stop(); }
            continue;
        }

        if ((size_t)pre < clen && !netReadExact(cli, fd.buf + pre, clen - pre)) {
            xQueueSend(freeQ, &fd, 0);
            totErrors++;
            connSt = S_NO_STREAM;
            cli.stop();
            continue;
        }

        fd.len = clen;
        totFrames++;
        if (xQueueSend(readyQ, &fd, 0) != pdTRUE) {
            droppedFr++;
            xQueueSend(freeQ, &fd, 0);
        }
    }
}

static void printMemInfo() {
    Serial.printf("PSRAM=%u HeapInt=%u HInt_max=%u HeapDef=%u HDef_max=%u\n", ESP.getFreePsram(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
    heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

// ---- OSD ----
static void drawOSD() {
    if (!osdOn) return;
    // tft.fillRect(0, H - 16, W, 16, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextDatum(top_left);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(0, 2);
    tft.printf("%.1ffps", perf.fps);
    tft.setTextSize(1);
    tft.setTextDatum(top_right);
    tft.setTextColor(ST_CLR[connSt], TFT_BLACK);
    tft.setCursor(W/2, 2);
    tft.printf("%s", ST_STR[connSt]);
}

static void drawSplash(const char* msg, uint16_t title_clr, uint16_t msg_clr = TFT_WHITE) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(middle_center);
    tft.setTextColor(title_clr, TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("MJPEG Viewer", W / 2, H / 2 - 30);
    tft.setTextSize(1);
    tft.setTextColor(msg_clr, TFT_BLACK);
    tft.drawString(msg, W / 2, H / 2 + 5);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    char u[96]; snprintf(u, sizeof(u), "%s:%d%s", cfgHost, cfgPort, cfgPath);
    tft.drawString(u, W / 2, H / 2 + 25);
}

// ---- NVS config persistence ----
static void cfgLoad() {
    if (!prefs.begin("v", true)) {
        strlcpy(cfgHost, VIEWER_STREAM_HOST, sizeof(cfgHost));
        cfgPort = VIEWER_STREAM_PORT;
        strlcpy(cfgPath, VIEWER_STREAM_PATH, sizeof(cfgPath));
        return;
    }
    strlcpy(cfgHost, prefs.getString("h", VIEWER_STREAM_HOST).c_str(), sizeof(cfgHost));
    cfgPort = prefs.getUShort("p", VIEWER_STREAM_PORT);
    strlcpy(cfgPath, prefs.getString("u", VIEWER_STREAM_PATH).c_str(), sizeof(cfgPath));
    prefs.end();
}
static void cfgSave() {
    if (!prefs.begin("v", false)) return;
    prefs.putString("h", cfgHost);
    prefs.putUShort("p", cfgPort);
    prefs.putString("u", cfgPath);
    prefs.end();
}

// ---- Serial commands (non-blocking) ----
static char serialBuf[128];
static uint8_t serialPos = 0;

static void pollSerial() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialPos == 0) continue;
            serialBuf[serialPos] = 0;
            serialPos = 0;

            if (strncmp(serialBuf, "SET ", 4) == 0) {
                char* col = strchr(serialBuf + 4, ':');
                char* sl  = strchr(serialBuf + 4, '/');
                if (col && sl && sl > col) {
                    *col = 0; *sl = 0;
                    uint16_t p = atoi(col + 1);
                    if (p > 0) {
                        strlcpy(cfgHost, serialBuf + 4, sizeof(cfgHost));
                        cfgPort = p;
                        *sl = '/';
                        strlcpy(cfgPath, sl, sizeof(cfgPath));
                        cfgSave();
                        Serial.printf("[CFG] -> %s:%d%s (NVS saved)\n", cfgHost, cfgPort, cfgPath);
                    }
                }
            } else if (strcmp(serialBuf, "STATUS") == 0) {
                Serial.printf("[S] %s %.1ffps fr=%u err=%u rssi=%d heap=%u psram=%u\n",
                    ST_STR[connSt], perf.fps, totFrames, totErrors,
                    rssi, ESP.getFreeHeap(), ESP.getFreePsram());
                Serial.printf("[S] dec=%.1f/%.1f/%.1fms %s:%d%s\n",
                    perf.minDecode, perf.decodeMs, perf.maxDecode, cfgHost, cfgPort, cfgPath);
            } else if (strcmp(serialBuf, "RESET") == 0) {
                Serial.println("[CFG] Clearing NVS & restarting");
                prefs.begin("v", false); prefs.clear(); prefs.end();
                ESP.restart();
            } else {
                Serial.println("SET host:port/path | STATUS | RESET");
            }
        } else if (serialPos < sizeof(serialBuf) - 1) {
            serialBuf[serialPos++] = c;
        }
    }
}

// ---- Setup ----
void setup() {
    Serial.begin(115200);
    delay(300);
    bootTime = millis();
    Serial.println("\n=== ESP32 MJPEG Viewer (CVBS) ===");
    printMemInfo();

    cfgLoad();
    Serial.printf("Target: %s:%d%s\n", cfgHost, cfgPort, cfgPath);

    // JPEG buffers for producer/consumer download + decode
    for (int i = 0; i < NUM_BUFS; i++) {
        jpegBuf[i] = (uint8_t*)heap_caps_malloc(MAX_JPEG_BUF, MALLOC_CAP_DEFAULT);
        if (!jpegBuf[i]) {
            jpegBuf[i] = (uint8_t*)heap_caps_malloc(MAX_JPEG_BUF, MALLOC_CAP_INTERNAL);
        }
    }
    tft.setColorDepth(16);
    tft.init();
    tft.fillScreen(TFT_BLACK);
    tft.setSwapBytes(false);

    for (int i = 0; i < NUM_BUFS; i++)
    {
        if (!jpegBuf[i]) {
            Serial.printf("FATAL: DRAM alloc failed (%u bytes)\n", MAX_JPEG_BUF);
            drawSplash("MEMORY FAILED", TFT_RED, TFT_RED);
            printMemInfo();
            for (;;) vTaskDelay(1000);
        }
    }

    drawSplash("Connecting WiFi...", TFT_CYAN, TFT_WHITE);
    connSt = S_WIFI_UP;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(VIEWER_WIFI_SSID, VIEWER_WIFI_PASS);
    for (int i = 0; i < 150 && WiFi.status() != WL_CONNECTED; i++) delay(100);

    Serial.printf("Target frame: %ux%u\n", W, H);
    printMemInfo();

    if (WiFi.status() == WL_CONNECTED) {
        esp_wifi_set_ps(WIFI_PS_NONE);
        rssi = WiFi.RSSI();
        Serial.printf("WiFi OK %s RSSI=%d ps=off\n", WiFi.localIP().toString().c_str(), rssi);
        connSt = S_NO_STREAM;
        drawSplash("WiFi OK", TFT_GREEN);
    } else {
        Serial.println("WiFi failed — will retry");
        connSt = S_WIFI_DOWN;
        drawSplash("WiFi retry...", TFT_YELLOW);
    }

    freeQ  = xQueueCreate(NUM_BUFS, sizeof(FrameDesc));
    readyQ = xQueueCreate(NUM_BUFS, sizeof(FrameDesc));
    for (int i = 0; i < NUM_BUFS; i++) {
        FrameDesc fd = { jpegBuf[i], 0 };
        xQueueSend(freeQ, &fd, 0);
    }

    if(esp_task_wdt_init(WDT_TIMEOUT_SEC, false) == ESP_OK) {
        printMemInfo();
        Serial.println("esp_task_wdt_init ESP_OK. Starting download task...");
        BaseType_t xReturned = xTaskCreatePinnedToCore(dlTask, "dl", DL_STACK, nullptr, 3, &dlTaskHandle, 0);
        if(xReturned == pdPASS) {
            Serial.println("xTaskCreatePinnedToCore pdPASS. Commands: SET host:port/path | STATUS | RESET");
        } else {
            Serial.printf("Failed to start download task. (code = %ld)\n", (long)xReturned);
            printMemInfo();
            drawSplash("DL Task Failed", TFT_RED);
            for (;;) vTaskDelay(1000);
        }
    } else {
        Serial.println("Failed to initialize WDT");
        drawSplash("WDT Failed", TFT_RED);
        for (;;) vTaskDelay(1000);
    }
}

// ---- Main loop: decode & display (Core 1) ----
void loop() {
    pollSerial();

    FrameDesc fd;
    if (xQueueReceive(readyQ, &fd, pdMS_TO_TICKS(FRAME_TIMEOUT)) == pdTRUE) {
        frameSize = fd.len;
        uint32_t t0 = micros();
        if (jpeg.openRAM(fd.buf, fd.len, onJpegDraw)) {
            tft.startWrite();
            jpeg.setPixelType(RGB565_BIG_ENDIAN);
            int opts = JPEG_SCALE_HALF;
            if (jpeg.getWidth() >= W * 2 && jpeg.getHeight() >= H * 2) {
                opts = JPEG_SCALE_HALF;
            }
            jpeg.decode(0, 0, opts);
            drawOSD();
            tft.endWrite();
            jpeg.close();
        }
        perf.frameDone(micros() - t0);
        xQueueSend(freeQ, &fd, 0);

        static uint32_t lastLog = 0;
        if (millis() - lastLog > STATS_INTERVAL) {
            Serial.printf("[D] %.1ffps fr=%u e=%u dec=%u-%u-%ums heap=%u fsize=%u \n", perf.fps, totFrames, totErrors, (unsigned)perf.minDecode, (unsigned)perf.decodeMs, (unsigned)perf.maxDecode, ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
            printMemInfo();
            lastLog = millis();
        }
    } else {
        if (connSt != S_STREAMING) drawSplash(ST_STR[connSt], TFT_YELLOW);
        Serial.println("[D] Frame timeout");
    }
}
