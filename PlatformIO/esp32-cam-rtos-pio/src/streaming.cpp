#include "streaming.h"

const char* HEADER = "HTTP/1.1 200 OK\r\n" \
                      "Access-Control-Allow-Origin: *\r\n" \
                      "Content-Type: multipart/x-mixed-replace; boundary=+++===123454321===+++\r\n";
const char* BOUNDARY = "\r\n--+++===123454321===+++\r\n";
const char* CTNTTYPE = "Content-Type: image/jpeg\r\nContent-Length: ";
const int hdrLen = strlen(HEADER);
const int bdrLen = strlen(BOUNDARY);
const int cntLen = strlen(CTNTTYPE);
volatile uint32_t frameNumber;

frameChunck_t* fstFrame = NULL;  // first frame
frameChunck_t* curFrame = NULL;  // current frame being captured by the camera

const char*  STREAMING_URL = "/mjpeg/1";
const char*  CONFIG_GET_URL = "/config/get";
const char*  CONFIG_SET_URL = "/config/set";

void mjpegCB(void* pvParameters) {
  TickType_t xLastWakeTime;
  const TickType_t xFrequency = pdMS_TO_TICKS(WSINTERVAL);

  // Creating frame synchronization semaphore and initializing it
  frameSync = xSemaphoreCreateBinary();
  xSemaphoreGive( frameSync );

  //  Creating RTOS task for grabbing frames from the camera
  xTaskCreatePinnedToCore(
      camCB,        // callback
      "cam",        // name
      16 * KILOBYTE, // stack size
      NULL,         // parameters
      tskIDLE_PRIORITY + 2, // priority
      &tCam,        // RTOS task handle
      APP_CPU);     // core

  //  Registering webserver handling routines
  server.on(STREAMING_URL, HTTP_GET, handleJPGSstream);
  server.on(CONFIG_GET_URL, HTTP_GET, handleGetConfig);
  server.on(CONFIG_SET_URL, HTTP_GET, handleSetConfig);
  server.onNotFound(handleNotFound);

  server.begin();

  Log.trace("mjpegCB: Starting streaming service\n");
  Log.verbose ("mjpegCB: free heap (start)  : %d\n", ESP.getFreeHeap());

  //=== loop() section  ===================
  xLastWakeTime = xTaskGetTickCount();
  for (;;) {
    server.handleClient();

    //  After every server client handling request, we let other tasks run and then pause
    if ( xTaskDelayUntil(&xLastWakeTime, xFrequency) != pdTRUE ) taskYIELD();
  }
}

// ==== Memory allocator that takes advantage of PSRAM if present =======================
char* allocatePSRAM(size_t aSize) {
  if ( psramFound() && ESP.getFreePsram() > aSize ) {
    return (char*) ps_malloc(aSize);
  }
  return NULL;
}

char* allocateMemory(char* aPtr, size_t aSize, bool fail, bool psramOnly) {
  char* ptr = NULL;
  //  Since current buffer is too smal, free it
  if (aPtr != NULL) {
    free(aPtr);
    aPtr = NULL;
  }
  
  if ( psramOnly ) {
    ptr = allocatePSRAM(aSize);
  }
  else {
    // If memory requested is more than 2/3 of the currently free heap, try PSRAM immediately
    if ( aSize > ESP.getFreeHeap() * 2 / 3 ) {
      ptr = allocatePSRAM(aSize);
    }
    else {
      //  Enough free heap - let's try allocating fast RAM as a buffer
      ptr = (char*) malloc(aSize);

      //  If allocation on the heap failed, let's give PSRAM one more chance:
      if ( ptr == NULL ) ptr = allocatePSRAM(aSize);
    }
  }
  // Finally, if the memory pointer is NULL, we were not able to allocate any memory, and that is a terminal condition.
  if (fail && ptr == NULL) {
    Log.fatal("allocateMemory: Out of memory!");
    delay(5000);
    ESP.restart();
  }
  return ptr;
}

// ==== Handle invalid URL requests ============================================
void handleNotFound() {
  String message = "Server is running!\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";

  server.send(200, "text / plain", message);
}

void handleGetConfig() {
  sensor_t *s = esp_camera_sensor_get();
  String message = "";
  if(s != nullptr) {
    message = "{\"xclk\":";
    message += s->xclk_freq_hz / 1000000;
    message += ",\"pixformat\":";
    message += s->pixformat;
    message += ",\"status\":{\"colorbar\":";
    message += String(s->status.colorbar);
    message += ",\"dcw\":";
    message += String(s->status.dcw);
    message += ",\"vflip\":";
    message += String(s->status.vflip);
    message += ",\"hmirror\":";
    message += String(s->status.hmirror);
    message += ",\"lenc\":";
    message += String(s->status.lenc);
    message += ",\"raw_gma\":";
    message += String(s->status.raw_gma);
    message += ",\"wpc\":";
    message += String(s->status.wpc);
    message += ",\"bpc\":";
    message += String(s->status.bpc);
    message += ",\"gainceiling\":";
    message += String(s->status.gainceiling);
    message += ",\"agc_gain\":";
    message += String(s->status.agc_gain);
    message += ",\"agc\":";
    message += String(s->status.agc);
    message += ",\"aec_value\":";
    message += String(s->status.aec_value);
    message += ",\"ae_level\":";
    message += String(s->status.ae_level);
    message += ",\"aec2\":";
    message += String(s->status.aec2);
    message += ",\"aec\":";
    message += String(s->status.aec);
    message += ",\"awb_gain\":";
    message += String(s->status.awb_gain);
    message += ",\"awb\":";
    message += String(s->status.awb);
    message += ",\"wb_mode\":";
    message += String(s->status.wb_mode);
    message += ",\"special_effect\":";
    message += String(s->status.special_effect);
    message += ",\"denoise\":";
    message += String(s->status.denoise);
    message += ",\"sharpness\":";
    message += String(s->status.sharpness);
    message += ",\"saturation\":";
    message += String(s->status.saturation);
    message += ",\"contrast\":";
    message += String(s->status.contrast);
    message += ",\"brightness\":";
    message += String(s->status.brightness);
    message += ",\"quality\":";
    message += String(s->status.quality);
    message += ",\"binning\":";
    message += String(s->status.binning);
    message += ",\"scale\":";
    message += String(s->status.scale);
    message += ",\"framesize\":";
    message += String(s->status.framesize);
    message += "}}";
  }
  message += "\n";
  server.send(200, "text / plain", message);
}

void handleSetConfig()
{
  sensor_t *s = esp_camera_sensor_get();
  String message = "";

  for(int i=0; i<server.args(); ++i) {
    if(server.argName(i) == "framesize") {
      s->set_framesize(s, (framesize_t ) server.arg(i).toInt());
    }
    else if(server.argName(i) == "scale") {
      s->status.scale = (bool) server.arg(i).toInt();
    }
    else if(server.argName(i) == "binning") {
      s->status.binning = (bool) server.arg(i).toInt();
    }
    else if(server.argName(i) == "quality") {
      s->set_quality(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "brightness") {
      s->set_brightness(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "contrast") {
      s->set_contrast(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "saturation") {
      s->set_saturation(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "sharpness") {
      s->set_sharpness(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "denoise") {
      s->set_denoise(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "special_effect") {
      s->set_special_effect(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "wb_mode") {
      s->set_wb_mode(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "awb") {
      s->status.awb = (bool) server.arg(i).toInt();
    }
    else if(server.argName(i) == "awb_gain") {
      s->set_awb_gain(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "aec") {
      s->status.aec = (bool) server.arg(i).toInt();
    }
    else if(server.argName(i) == "aec2") {
      s->set_aec2(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "ae_level") {
      s->set_ae_level(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "aec_value") {
      s->set_aec_value(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "agc") {
      s->status.agc = (bool) server.arg(i).toInt();
    }
    else if(server.argName(i) == "agc_gain") {
      s->set_agc_gain(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "gainceiling") {
      s->set_gainceiling(s, (gainceiling_t) server.arg(i).toInt());
    }
    else if(server.argName(i) == "bpc") {
      s->set_bpc(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "wpc") {
      s->set_wpc(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "raw_gma") {
      s->set_raw_gma(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "lenc") {
      s->set_lenc(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "hmirror") {
      s->set_hmirror(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "vflip") {
      s->set_vflip(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "dcw") {
      s->set_dcw(s, server.arg(i).toInt());
    }
    else if(server.argName(i) == "colorbar") {
      s->set_colorbar(s, server.arg(i).toInt());
    }
  }
}
