#pragma once
#include <Arduino.h>
#include "ArduinoLog.h"
#include <WebServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiManager.h>

#include "logging.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <esp_task_wdt.h>
#include "esp_camera.h"
#include "ov5640.h"
#include <vector>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

extern SemaphoreHandle_t frameSync;
extern WebServer server;
extern TaskHandle_t tMjpeg;   // handles client connections to the webserver
extern TaskHandle_t tCam;     // handles getting picture frames from the camera and storing them locally
extern TaskHandle_t tStream;
extern uint8_t      noActiveClients;       // number of active clients

extern const char*  STREAMING_URL;

extern Preferences prefs;
extern QueueHandle_t freeQ;
extern QueueHandle_t readyQ;

