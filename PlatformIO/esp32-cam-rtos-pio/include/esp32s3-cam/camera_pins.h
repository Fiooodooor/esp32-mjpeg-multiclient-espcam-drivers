// ESP32-S3 SPK v1.2 board - from schematic
// Put this before esp_camera_init()
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    33   // MCLK
#define SIOD_GPIO_NUM    37   // SDA
#define SIOC_GPIO_NUM    36   // SCK

#define Y9_GPIO_NUM      47   // D7
#define Y8_GPIO_NUM      48   // D6
#define Y7_GPIO_NUM      42   // D5
#define Y6_GPIO_NUM       8   // D4
#define Y5_GPIO_NUM       6   // D3
#define Y4_GPIO_NUM       4   // D2
#define Y3_GPIO_NUM       5   // D1
#define Y2_GPIO_NUM       7   // D0

#define VSYNC_GPIO_NUM   35   // VS
#define HREF_GPIO_NUM    34   // HS
#define PCLK_GPIO_NUM    41
