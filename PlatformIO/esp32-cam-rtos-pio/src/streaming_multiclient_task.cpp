#include "streaming.h"

#if defined(CAMERA_MULTICLIENT_TASK)

volatile size_t   camSize;    // size of the current frame, byte
volatile char*    camBuf;      // pointer to the current frame

#if defined(BENCHMARK)
#include <AverageFilter.h>
#define BENCHMARK_PRINT_INT 1000
averageFilter<uint32_t> captureAvg(10);
uint32_t lastPrintCam = millis();
#endif

void camCB(void* pvParameters) {
  FrameDesc fd;
  TickType_t xLastWakeTime;
  const TickType_t xFrequency = pdMS_TO_TICKS(1000 / FPS);
  char* fbs[2] = { NULL, NULL };
  size_t fSize[2] = { 0, 0 };
  int ifb = 0;
  frameNumber = 0;

  //=== loop() section  ===================
  xLastWakeTime = xTaskGetTickCount();

  for (;;) {
    size_t s = 0;
    //  Grab a frame from the camera and query its size
    camera_fb_t* fb = NULL;

#if defined(BENCHMARK)
    uint32_t benchmarkStart = micros();
#endif

    s = 0;
    if (xQueueReceive(freeQ, &fd, pdMS_TO_TICKS(5000U)) == pdTRUE) {

      fb = esp_camera_fb_get();
      if (fb) {
        s = fb->len;
        char* fb_ptr = (char *)fb->buf;

        if (fb->len > fSize[ifb]) {
          fSize[ifb] = fb->len + fb->len/4;
          fbs[ifb] = allocateMemory(fbs[ifb], fSize[ifb], FAIL_IF_OOM, ANY_MEMORY);
        }
        memcpy(fbs[ifb], fb_ptr, fb->len);
        if (fd.buf) {
          memcpy(fd.buf, fb_ptr, fb->len);
          fd.len = fb->len;
        }
        esp_camera_fb_return(fb);
        if (xQueueSend(readyQ, &fd, 0) != pdTRUE) {
          // Queue full — drop this frame, return buffer
          xQueueSend(freeQ, &fd, 0);          
        }
      }
      else {
        Log.error("camCB: error capturing image for frame %d\n", frameNumber);
        vTaskDelay(10);
      }

#if defined(BENCHMARK)
    captureAvg.value(micros()-benchmarkStart);
#endif

      if ( xSemaphoreTake( frameSync, portMAX_DELAY ) ) {
        camBuf = fbs[ifb];
        camSize = s;
        ifb++;
        ifb &= 1;
        frameNumber++;
        xSemaphoreGive( frameSync );
      }

      if ( xTaskDelayUntil(&xLastWakeTime, xFrequency) != pdTRUE ) taskYIELD();

#if defined(LOCAL_CAMERA_PREVIEW)
      // When TFT preview is active, never suspend — the display needs
      // continuous frames even when no WiFi clients are connected.
      if ( noActiveClients == 0 ) {
        Log.verbose("mjpegCB: free heap           : %d\n", ESP.getFreeHeap());
        Log.verbose("mjpegCB: min free heap)      : %d\n", ESP.getMinFreeHeap());
        Log.verbose("mjpegCB: max alloc free heap : %d\n", ESP.getMaxAllocHeap());
        Log.verbose("mjpegCB: tCam stack wtrmark  : %d\n", uxTaskGetStackHighWaterMark(tCam));
      }
#else
      if ( noActiveClients == 0 ) {
        Log.verbose("mjpegCB: free heap           : %d\n", ESP.getFreeHeap());
        Log.verbose("mjpegCB: min free heap)      : %d\n", ESP.getMinFreeHeap());
        Log.verbose("mjpegCB: max alloc free heap : %d\n", ESP.getMaxAllocHeap());
        Log.verbose("mjpegCB: tCam stack wtrmark  : %d\n", uxTaskGetStackHighWaterMark(tCam));
        vTaskSuspend(NULL);  // passing NULL means "suspend yourself"
      }
#endif
    }
#if defined(BENCHMARK)
    if ( millis() - lastPrintCam > BENCHMARK_PRINT_INT ) {
      lastPrintCam = millis();
      Log.verbose("mjpegCB: average frame capture time: %d microseconds\n", captureAvg.currentValue() );
    }
#endif
  }
}


// ==== Handle connection request from clients ===============================
void handleJPGSstream(void)
{
  if ( noActiveClients >= MAX_CLIENTS ) return;
  Log.verbose("handleJPGSstream start: free heap  : %d\n", ESP.getFreeHeap());

  streamInfo_t* info = new streamInfo_t;
  if ( info == NULL ) {
    Log.error("handleJPGSstream: cannot allocate stream info - OOM\n");
    return;
  }

  WiFiClient* client = new WiFiClient();
  if ( client == NULL ) {
    Log.error("handleJPGSstream: cannot allocate WiFi client for streaming - OOM\n");
    free(info);
    return;
  }

  *client = server.client();

  info->frame = frameNumber - 1;
  info->client = client;
  info->buffer = NULL;
  info->len = 0;

  //  Creating task to push the stream to all connected clients
  int rc = xTaskCreatePinnedToCore(
             streamCB,
             "streamCB",
             8 * KILOBYTE,
             (void*) info,
             tskIDLE_PRIORITY + 2,
             &info->task,
             APP_CPU);
  if ( rc != pdPASS ) {
    Log.error("handleJPGSstream: error creating RTOS task. rc = %d\n", rc);
    Log.error("handleJPGSstream: free heap  : %d\n", ESP.getFreeHeap());
    //    Log.error("stk high wm: %d\n", uxTaskGetStackHighWaterMark(tSend));
    delete info;
  }

  noActiveClients++;

  // Wake up streaming tasks, if they were previously suspended:
  if ( eTaskGetState( tCam ) == eSuspended ) vTaskResume( tCam );
}

// ==== Actually stream content to all connected clients ========================
void streamCB(void * pvParameters) {
  char buf[16];
  TickType_t xLastWakeTime;
  TickType_t xFrequency;

  streamInfo_t* info = (streamInfo_t*) pvParameters;

  if ( info == NULL ) {
    Log.fatal("streamCB: a NULL pointer passed");
    delay(5000);
    ESP.restart();
  }

  xLastWakeTime = xTaskGetTickCount();
  xFrequency = pdMS_TO_TICKS(1000 / FPS);
  Log.trace("streamCB: Client Connected\n");

  //  Immediately send this client a header
  info->client->write(HEADER, hdrLen);
  info->client->write(BOUNDARY, bdrLen);

#if defined(BENCHMARK)
  averageFilter<int32_t> streamAvg(10);
  averageFilter<int32_t> waitAvg(10);
  averageFilter<uint32_t> frameAvg(10);
  averageFilter<uint32_t> fpsAvg(10);
  uint32_t streamStart = 0;
  streamAvg.initialize();
  waitAvg.initialize();
  frameAvg.initialize();
  fpsAvg.initialize();
  unsigned long lastPrint = millis();
  unsigned long lastFrame = millis();
#endif

  for (;;) {
    if ( info->client->connected() ) {

      if ( info->frame != frameNumber) { // do not send same frame twice

#if defined (BENCHMARK)
        streamStart = micros();
#endif        

        xSemaphoreTake( frameSync, portMAX_DELAY );
        // size_t currentSize = camSize;

#if defined (BENCHMARK)
        waitAvg.value(micros()-streamStart);
        frameAvg.value(currentSize);
        streamStart = micros();
#endif

        sprintf(buf, "%d\r\n\r\n", camSize);
        info->client->write(CTNTTYPE, cntLen);
        info->client->write(buf, strlen(buf));
        info->client->write((char*) camBuf, (size_t)camSize);

        xSemaphoreGive( frameSync );

        info->client->write(BOUNDARY, bdrLen);
// */
//  ====================================================================
        info->frame = frameNumber;
#if defined (BENCHMARK)
          streamAvg.value(micros()-streamStart);
#endif        
      }
    }
    else {
      //  client disconnected - clean up.
      noActiveClients--;
      Log.verbose("streamCB: Stream Task stack wtrmark  : %d\n", uxTaskGetStackHighWaterMark(info->task));
      info->client->stop();
      if ( info->buffer ) {
        free( info->buffer );
        info->buffer = NULL;
      }
      delete info->client;
      delete info;
      info = NULL;
      Log.trace("streamCB: Client disconnected\n");
      vTaskDelay(10);
      vTaskDelete(NULL);
    }
    //  Let other tasks run after serving every client
    if ( xTaskDelayUntil(&xLastWakeTime, xFrequency) != pdTRUE ) taskYIELD();

#if defined (BENCHMARK)
    fpsAvg.value((uint32_t) (millis()-lastFrame) );
    lastFrame = millis();
    if ( millis() - lastPrint > BENCHMARK_PRINT_INT ) {
      lastPrint = millis();
      Log.verbose("streamCB: wait avg=%d, stream avg=%d us, frame avg size=%d bytes, fps=%S\n", waitAvg.currentValue(), streamAvg.currentValue(), frameAvg.currentValue(), String((float)(1000.0) / (float)(fpsAvg.currentValue())));
    }
#endif
  }
}

#endif
