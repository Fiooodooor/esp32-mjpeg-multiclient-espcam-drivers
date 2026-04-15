#pragma once
#include "definitions.h"
#include "references.h"

typedef struct {
  uint32_t        frame;
  WiFiClient      *client;
  TaskHandle_t    task;
  char*           buffer;
  size_t          len;
} streamInfo_t;

typedef struct {
  uint8_t   cnt;  // served to clients counter. when equal to number of active clients, could be deleted
  void*     nxt;  // next chunck
  uint32_t  fnm;  // frame number
  uint32_t  siz;  // frame size
  uint8_t*  dat;  // frame pointer
} frameChunck_t;

struct FrameDesc {
    uint8_t* buf;
    size_t   len;
};

// ---- Connection state machine ----
enum ConnState : uint8_t {
  S_WIFI_DOWN, S_WIFI_UP, S_NO_STREAM, S_CONNECTING, S_STREAMING
};

const char* const ST_STR[] = {
  "WiFi Down", "WiFi Up", "No Stream", "Connecting", "Streaming"
};

void camCB(void* pvParameters);
void handleJPGSstream(void);
void handleNotFound(void);
void streamCB(void * pvParameters);
void mjpegCB(void * pvParameters);
void handleSetConfig(void);
void handleGetConfig(void);

#define FAIL_IF_OOM true
#define OK_IF_OOM   false
#define PSRAM_ONLY  true
#define ANY_MEMORY  false
char* allocateMemory(char* aPtr, size_t aSize, bool fail = FAIL_IF_OOM, bool psramOnly = ANY_MEMORY);

extern const char* HEADER;
extern const char* BOUNDARY;
extern const char* CTNTTYPE;
extern const int hdrLen;
extern const int bdrLen;
extern const int cntLen;
extern volatile uint32_t frameNumber;
extern volatile size_t camSize;
extern volatile char*  camBuf;

extern frameChunck_t* fstFrame;
extern frameChunck_t* curFrame; 
