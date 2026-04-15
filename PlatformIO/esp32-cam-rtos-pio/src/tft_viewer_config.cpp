
#include "references.h"
#include "streaming.h"
#include "tft_viewer_config.h"

// ---- Globals ----
extern LGFX     tft;
static JPEGDEC  jpeg;
static PerfTracker perf;
Preferences prefs;

static uint8_t* jpegBuf[NUM_BUFS];
static volatile size_t frameSize;

// Triple-buffer queues: download pushes filled frames, display returns empty ones
QueueHandle_t freeQ  = nullptr;  // pool of free FrameDesc
QueueHandle_t readyQ = nullptr;  // filled frames awaiting display

static volatile uint32_t  totFrames  = 0;
static volatile uint32_t  totErrors  = 0;
static volatile uint32_t  totReconn  = 0;
static volatile uint32_t  droppedFr  = 0;

static volatile ConnState connSt     = S_WIFI_DOWN;
static volatile int8_t    rssi       = 0;
static volatile bool      osdOn      = true;

static char     cfgHost[64];
static uint16_t cfgPort;
static char     cfgPath[64];

// ---- Double-buffered bgr888 conversion for DMA overlap ----
// Pre-converting swap565→bgr888 lets LovyanGFX take the no_convert fast path:
// one DMA transfer per MCU block instead of 21-pixel chunked conversion.
static lgfx::bgr888_t __attribute__((aligned(4))) bgr888A[2048];
static lgfx::bgr888_t __attribute__((aligned(4))) bgr888B[2048];
static lgfx::bgr888_t* bgr888Cur = bgr888A;

// ---- JPEG decode callback — convert + async DMA push ----
static int onJpegDraw(JPEGDRAW* d) {
    const int n = d->iWidth * d->iHeight;
    const lgfx::swap565_t* src = (const lgfx::swap565_t*)d->pPixels;
    lgfx::bgr888_t* dst = bgr888Cur;
    for (int i = 0; i < n; i++) {
        dst[i] = src[i];   // LovyanGFX's correct swap565→bgr888 conversion
    }
    // pushImageDMA: DMA sends from bgr888Cur while JPEGDEC decodes next block
    tft.pushImageDMA(d->x, d->y, d->iWidth, d->iHeight, dst);
    // Swap buffer so DMA reads current while next callback writes to other
    bgr888Cur = (bgr888Cur == bgr888A) ? bgr888B : bgr888A;
    return 1;
}

// ---- Network I/O (taskYIELD for minimal latency) ----
bool LGFX::netReadExact(WiFiClient& c, uint8_t* buf, size_t len) {
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
bool LGFX::skipHttpHeaders(WiFiClient& c) {
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
int LGFX::parsePartHeader(WiFiClient& c, size_t& clen, uint8_t* jbuf) {
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
            *end = 0; // limit search to header portion
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
void LGFX::ensureWiFi() {
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

// ---- OSD ----
void LGFX::drawOSD() {
    if (!osdOn) return;
    // Bottom bar
    tft.fillRect(0, H - 16, W, 16, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextDatum(bottom_left);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(2, H - 2);
    tft.printf("%.1ffps %uKB dec:%.0fms", perf.perf.fps, (unsigned)(frameSize/1024), perf.perf.decodeMs);

    tft.setTextDatum(bottom_center);
    tft.setTextColor(ST_CLR[connSt], TFT_BLACK);
    tft.drawString(ST_STR[connSt], W / 2, H - 2);

    tft.setTextDatum(bottom_right);
    tft.setTextColor(rssi > -60 ? TFT_GREEN : (rssi > -75 ? TFT_YELLOW : TFT_RED), TFT_BLACK);
    tft.setCursor(W - 2, H - 2);
    uint32_t up = (millis()) / 1000;
    tft.printf("R:%d fr:%u %02u:%02u", rssi, (unsigned)totFrames, up / 60, up % 60);
}

void LGFX::drawSplash(const char* msg, uint16_t clr) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(middle_center);
    tft.setTextColor(clr, TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("MJPEG Viewer", W / 2, H / 2 - 30);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(msg, W / 2, H / 2 + 5);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    char u[196]; snprintf(u, sizeof(u), "%s:%d%s", cfgHost, cfgPort, cfgPath);
    tft.drawString(u, W / 2, H / 2 + 25);
}

// ---- Touch (throttled: every 8th frame) ----
void LGFX::checkTouch() {
    static uint8_t skip = 0;
    if (++skip < 8) return;
    skip = 0;
    int32_t x, y;
    if (tft.getTouch(&x, &y)) {
        while (tft.getTouch(&x, &y)) vTaskDelay(pdMS_TO_TICKS(10));
        osdOn = !osdOn;
        if (!osdOn) { tft.fillRect(0, H-16, W, 16, TFT_BLACK); tft.fillRect(0, 0, 160, 12, TFT_BLACK); }
    }
}

// ---- NVS config persistence ----
void LGFX::cfgLoad() {
    prefs.begin("v", true);
    strlcpy(cfgHost, prefs.getString("h", VIEWER_STREAM_HOST).c_str(), sizeof(cfgHost));
    cfgPort = prefs.getUShort("p", VIEWER_STREAM_PORT);
    strlcpy(cfgPath, prefs.getString("u", VIEWER_STREAM_PATH).c_str(), sizeof(cfgPath));
    prefs.end();
}
void LGFX::cfgSave() {
    prefs.begin("v", false);
    prefs.putString("h", cfgHost);
    prefs.putUShort("p", cfgPort);
    prefs.putString("u", cfgPath);
    prefs.end();
}

// ---- Serial commands (non-blocking) ----
static char serialBuf[128];
static uint8_t serialPos = 0;

void LGFX::pollSerial() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialPos == 0) continue;
            serialBuf[serialPos] = 0;
            serialPos = 0;

            if (strncmp(serialBuf, "SET ", 4) == 0) {
                // Parse host:port/path
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
                        this->cfgSave();
                        Serial.printf("[CFG] -> %s:%d%s (NVS saved)\n", cfgHost, cfgPort, cfgPath);
                    }
                }
            } else if (strcmp(serialBuf, "STATUS") == 0) {
                Serial.printf("[S] %s %.1ffps fr=%u err=%u rssi=%d heap=%u psram=%u\n",
                    ST_STR[connSt], perf.perf.fps, totFrames, totErrors,
                    rssi, ESP.getFreeHeap(), ESP.getFreePsram());
                Serial.printf("[S] dec=%.1f/%.1f/%.1fms %s:%d%s\n",
                    perf.perf.minDecode, perf.perf.decodeMs, perf.perf.maxDecode, cfgHost, cfgPort, cfgPath);
            } else if (strcmp(serialBuf, "RESET") == 0) {
                Serial.println("[CFG] Clearing NVS & restarting");
                prefs.begin("v", false); prefs.clear(); prefs.end();
                ESP.restart();
            } else {
                Serial.println("SET host:port/path | STATUS | RESET | HELP");
            }
        } else if (serialPos < sizeof(serialBuf) - 1) {
            serialBuf[serialPos++] = c;
        }
    }
}

// ---- Download task (Core 0) ----
void LGFX::dlTask(void*) {
    WiFiClient cli;
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

        // Get a free buffer (blocks only if all 3 are full — display backpressure)
        FrameDesc fd;
        if (xQueueReceive(freeQ, &fd, pdMS_TO_TICKS(FRAME_TIMEOUT)) != pdTRUE) {
            droppedFr++;
            continue;
        }

        // Read JPEG into buffer WHILE display renders previous frame
        size_t clen;
        int pre = parsePartHeader(cli, clen, fd.buf);
        if (pre < 0) {
            xQueueSend(freeQ, &fd, 0);  // return buffer
            if (!cli.connected()) { connSt = S_NO_STREAM; cli.stop(); }
            continue;
        }

        if ((size_t)pre < clen && !netReadExact(cli, fd.buf + pre, clen - pre)) {
            xQueueSend(freeQ, &fd, 0);  // return buffer
            totErrors++;
            connSt = S_NO_STREAM;
            cli.stop();
            continue;
        }

        // Post filled frame for display (if full, drop oldest)
        fd.len = clen;
        totFrames++;
        if (xQueueSend(readyQ, &fd, 0) != pdTRUE) {
            // Queue full — drop this frame, return buffer
            droppedFr++;
            xQueueSend(freeQ, &fd, 0);
        }
    }
}

void LGFX::frameReceiveTask()
{
    static uint8_t fpsSkip = 0;
    uint32_t t0;
    FrameDesc fd;
    if (xQueueReceive(readyQ, &fd, pdMS_TO_TICKS(FRAME_TIMEOUT)) == pdTRUE) {
        frameSize = fd.len;
        t0 = micros();
        tft.startWrite();
        if (jpeg.openRAM(fd.buf, fd.len, onJpegDraw)) {
            jpeg.setPixelType(RGB565_BIG_ENDIAN);
            jpeg.decode(0, 0, 0);
            jpeg.close();
        }

        tft.waitDMA();
        tft.endWrite();
        perf.frameDone(micros() - t0);

        xQueueSend(freeQ, &fd, 0);  // return buffer immediately

        if (osdOn && ++fpsSkip >= 4) {
            fpsSkip = 0;
            tft.setTextDatum(top_left); tft.setTextSize(1);
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.setCursor(2, 2);
            tft.printf("%.1ffps %uKB", perf.perf.fps, (unsigned)(frameSize / 1024));
        }
        static uint32_t lastOSD = 0;
        if (osdOn && millis() - lastOSD > OSD_INTERVAL) { drawOSD(); lastOSD = millis(); }

        static uint32_t lastLog = 0;
        if (millis() - lastLog > STATS_INTERVAL) {
            Serial.printf("[D] %.1ffps fr=%u e=%u dec=%u-%u-%ums heap=%u\n", perf.perf.fps, totFrames, totErrors, (unsigned)perf.perf.minDecode, (unsigned)perf.perf.decodeMs, (unsigned)perf.perf.maxDecode, ESP.getFreeHeap());
            lastLog = millis();
        }
    } else {
        if (connSt != S_STREAMING) drawSplash(ST_STR[connSt], TFT_YELLOW);
        Serial.println("[D] Frame timeout");
    }
}

void LGFX::setupTft()
{
    Serial.println("\n=== ESP32-S3 MJPEG Viewer ===");
    Serial.printf("Heap: %u  PSRAM: %u\n", ESP.getFreeHeap(), ESP.getFreePsram());

    this->cfgLoad();
    Serial.printf("Target: %s:%d%s\n", cfgHost, cfgPort, cfgPath);

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setSwapBytes(true);
    drawSplash("Connecting WiFi...", TFT_CYAN);

    // PSRAM triple buffers
    for (int i = 0; i < NUM_BUFS; i++) {
        jpegBuf[i] = (uint8_t*)ps_malloc(MAX_JPEG_BUF);
        if (!jpegBuf[i]) {
            Serial.println("FATAL: PSRAM alloc failed");
            drawSplash("PSRAM FAILED", TFT_RED);
            for (;;) vTaskDelay(1000);
        }
    }

    freeQ  = xQueueCreate(NUM_BUFS, sizeof(FrameDesc));
    readyQ = xQueueCreate(NUM_BUFS, sizeof(FrameDesc));
    for (int i = 0; i < NUM_BUFS; i++) {
        FrameDesc fd = { jpegBuf[i], 0 };
        xQueueSend(freeQ, &fd, 0);
    }
    esp_task_wdt_init(WDT_TIMEOUT_SEC, false);   
}
extern Preferences prefs;
extern QueueHandle_t freeQ;
extern QueueHandle_t readyQ;
