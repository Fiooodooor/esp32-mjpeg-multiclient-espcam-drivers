
#include "tft_viewer_config.h"

// ---- Setup ----
void setup() {
    Serial.begin(115200);
    delay(300);
    tft.setupTft();
    connSt = S_WIFI_UP;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(VIEWER_WIFI_SSID, VIEWER_WIFI_PASS);
    for (int i = 0; i < 150 && WiFi.status() != WL_CONNECTED; i++) delay(100);

    if (WiFi.status() == WL_CONNECTED) {
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
        rssi = WiFi.RSSI();
        Serial.printf("WiFi OK %s RSSI=%d ps=off bw=HT40\n", WiFi.localIP().toString().c_str(), rssi);
        connSt = S_NO_STREAM;
        drawSplash("WiFi OK", TFT_GREEN);
    } else {
        Serial.println("WiFi failed — will retry");
        connSt = S_WIFI_DOWN;
        drawSplash("WiFi retry...", TFT_YELLOW);
    }

    xTaskCreatePinnedToCore(dlTask, "dl", DL_STACK, nullptr, 3, nullptr, 0);
    Serial.println("Ready. Commands: SET host:port/path | STATUS | RESET");
}

// ---- Main loop: decode & display (Core 1) ----
void loop() {
    pollSerial();
    checkTouch();
    tft.frameReceiveTask();
}
