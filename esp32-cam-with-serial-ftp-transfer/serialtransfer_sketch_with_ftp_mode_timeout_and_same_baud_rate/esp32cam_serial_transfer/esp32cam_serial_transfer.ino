/*
  ESP32-CAM Serial File Transfer — SerialTransfer Library Edition

  Uses PowerBroker2's SerialTransfer library for packetized, CRC-checked
  serial communication between ESP32-CAM and a Python host.

  NOTE ON DATA LOSS:
  SerialTransfer does CRC16 validation internally. When a packet fails CRC,
  it is silently discarded (available() returns false). The file transfer
  protocol below streams DATA_CHUNK packets without per-chunk ACKs. If even
  one chunk is dropped, the file is corrupt or the receiver hangs.

  Fixes:
    1. Lower TRANSFER_BAUD (try 921600, 460800, or 115200).
    2. Use a short, high-quality USB cable.
    3. Add per-chunk ACK to the protocol if drops persist.

  Libraries:
    - SerialTransfer  >= 3.1.1  (Arduino Library Manager)
    - esp32           >= 2.0.0  (ESP32 Arduino core)

  Protocol:
    All packets are COBS-framed + CRC16 by SerialTransfer.
    Byte 0 of every payload is a PacketType; the rest is command-specific.

  Modes:
    PHOTO   — capture JPEG, save to SD, deep sleep, repeat
    TRANSFER — serial command loop at 2 000 000 baud

  Board:  AI-Thinker ESP32-CAM
  Upload: 921600 baud, Huge APP partition
*/

#include "esp_camera.h"
#include "Arduino.h"
#include "FS.h"
#include "SD_MMC.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/rtc_io.h"

#include "SerialTransfer.h"   // PowerBroker2

// ============================================================================
// Pin Definitions  (AI-Thinker ESP32-CAM)
// ============================================================================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
#define FLASH_LED_PIN     33

// ============================================================================
// Configuration
// ============================================================================
enum DeviceMode { MODE_PHOTO = 0, MODE_TRANSFER = 1 };

// --- Transfer baud rate ---
// 2000000 is fast but may cause data loss with long/cheap USB cables.
// Try 921600, 460800, or 115200 if you see CRC errors or dropped packets.
// Must match the Python client (see client/esp32cam_client.py).
#define TRANSFER_BAUD          921600

#define DEFAULT_MODE           MODE_PHOTO
#define DEFAULT_INTERVAL_SEC   60
#define BOOT_SERIAL_TIMEOUT_MS 2000
#define PHOTO_BAUD             115200
#define RX_BUFFER_SIZE         8192
#define TX_BUFFER_SIZE         8192

// File chunk size — leave headroom for SerialTransfer overhead (~5 bytes)
// Default MAX_PACKET_SIZE in SerialTransfer is 254.
#define PAYLOAD_SIZE           248
#define FILE_CHUNK_SIZE        (PAYLOAD_SIZE - 1)   // 247 bytes per packet

// ============================================================================
// RTC-Persisted State (survives deep sleep)
// ============================================================================
RTC_DATA_ATTR uint8_t  rtcMode = DEFAULT_MODE;
RTC_DATA_ATTR uint32_t rtcPictureNumber = 0;
RTC_DATA_ATTR uint32_t rtcIntervalSec = DEFAULT_INTERVAL_SEC;

// ============================================================================
// Runtime State
// ============================================================================
DeviceMode currentMode;
uint32_t   pictureNumber;
uint32_t   intervalSec;
bool       sdReady = false;
bool       cameraReady = false;

SerialTransfer myTransfer;

// Text command buffer for boot-time serial trigger
String bootBuffer = "";

// ----- Upload state (receives file from host) -----
File storeFile;
uint32_t storeExpected = 0;
uint32_t storeReceived = 0;
bool storeActive = false;

// ----- Download state (sends file to host) -----
File readFile;
uint32_t readRemaining = 0;
bool readActive = false;

// ============================================================================
// Packet Protocol
// ============================================================================
enum PacketType : uint8_t {
  // --- Host → Device commands ---
  CMD_PING      = 0x00,   // payload: none  (host probes for transfer mode)
  CMD_LIST      = 0x10,   // payload: char path[]
  CMD_READ      = 0x11,   // payload: char path[]
  CMD_STORE     = 0x12,   // payload: char path[] + uint32_t size
  CMD_DELETE    = 0x13,   // payload: char path[]
  CMD_MODE      = 0x14,   // payload: uint8_t mode (0=photo, 1=transfer)

  // --- Device → Host responses ---
  RESP_PONG     = 0x01,   // payload: none
  RESP_ENTRY    = 0x20,   // payload: uint8_t isDir + uint32_t size + char name[]
  RESP_ERROR    = 0x31,   // payload: char message[]
  RESP_OK       = 0x30,   // payload: char message[] or none

  // --- File data ---
  DATA_CHUNK    = 0x40,   // payload: uint8_t data[]
  END_OF_DATA   = 0x41,   // payload: none
};

void sendNextReadChunk() {
  uint8_t* tx = myTransfer.packet.txBuff;
  if (readRemaining == 0) {
    tx[0] = END_OF_DATA;
    myTransfer.sendData(1);
    readFile.close();
    readActive = false;
    Serial.println("[READ] END_OF_DATA");
    return;
  }
  uint8_t buf[FILE_CHUNK_SIZE];
  size_t want = (readRemaining > FILE_CHUNK_SIZE) ? FILE_CHUNK_SIZE : readRemaining;
  size_t got = readFile.read(buf, want);
  tx[0] = DATA_CHUNK;
  memcpy(tx + 1, buf, got);
  myTransfer.sendData(1 + got);
  readRemaining -= got;
}

// ============================================================================
// Forward Declarations
// ============================================================================
bool initCamera();
bool initSD();
bool readConfigFromSD();
bool saveConfigToSD();
void checkModeTriggersAtBoot();
void enterPhotoMode();
void enterTransferMode();
void takePhotoAndSleep();
void transferModeLoop();

// ============================================================================
// Setup
// ============================================================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(PHOTO_BAUD);
  delay(100);

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  currentMode   = (rtcMode == MODE_TRANSFER) ? MODE_TRANSFER : MODE_PHOTO;
  pictureNumber = rtcPictureNumber;
  intervalSec   = rtcIntervalSec;

  Serial.printf("\n[BOOT] Wakeup cause: %d\n", esp_sleep_get_wakeup_cause());
  Serial.printf("[BOOT] Persisted mode: %s\n",
                (currentMode == MODE_TRANSFER) ? "transfer" : "photo");
  Serial.printf("[BOOT] Picture number: %lu\n", pictureNumber);
  Serial.printf("[BOOT] Interval: %lu sec\n", intervalSec);

  if (!initSD()) {
    Serial.println("[FATAL] SD Card init failed. Halting.");
    return;
  }

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
    readConfigFromSD();
  }

  checkModeTriggersAtBoot();

  if (!initCamera()) {
    Serial.println("[FATAL] Camera init failed. Halting.");
    return;
  }

  if (currentMode == MODE_TRANSFER) {
    enterTransferMode();
  } else {
    enterPhotoMode();
  }
}

// ============================================================================
// Loop
// ============================================================================
void loop() {
  if (currentMode == MODE_TRANSFER) {
    transferModeLoop();
  }
}

// ============================================================================
// Initialization
// ============================================================================
bool initCamera() {
  camera_config_t config;
  config.ledc_channel  = LEDC_CHANNEL_0;
  config.ledc_timer    = LEDC_TIMER_0;
  config.pin_d0        = Y2_GPIO_NUM;
  config.pin_d1        = Y3_GPIO_NUM;
  config.pin_d2        = Y4_GPIO_NUM;
  config.pin_d3        = Y5_GPIO_NUM;
  config.pin_d4        = Y6_GPIO_NUM;
  config.pin_d5        = Y7_GPIO_NUM;
  config.pin_d6        = Y8_GPIO_NUM;
  config.pin_d7        = Y9_GPIO_NUM;
  config.pin_xclk      = XCLK_GPIO_NUM;
  config.pin_pclk      = PCLK_GPIO_NUM;
  config.pin_vsync     = VSYNC_GPIO_NUM;
  config.pin_href      = HREF_GPIO_NUM;
  config.pin_sscb_sda  = SIOD_GPIO_NUM;
  config.pin_sscb_scl  = SIOC_GPIO_NUM;
  config.pin_pwdn      = PWDN_GPIO_NUM;
  config.pin_reset     = RESET_GPIO_NUM;
  config.xclk_freq_hz  = 10000000;
  config.pixel_format  = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_SXGA;
    config.jpeg_quality = 10;
    config.fb_count     = 2;
  } else {
    config.frame_size   = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);
  s->set_special_effect(s, 0);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 0);
  s->set_ae_level(s, 0);
  s->set_aec_value(s, 300);
  s->set_gain_ctrl(s, 1);
  s->set_agc_gain(s, 0);
  s->set_gainceiling(s, (gainceiling_t)0);
  s->set_bpc(s, 0);
  s->set_wpc(s, 1);
  s->set_raw_gma(s, 1);
  s->set_lenc(s, 1);
  s->set_hmirror(s, 1);
  s->set_vflip(s, 1);
  s->set_dcw(s, 1);
  s->set_colorbar(s, 0);

  cameraReady = true;
  Serial.println("[INFO] Camera initialized.");
  return true;
}

bool initSD() {
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("[WARN] SD 4-bit mode failed, trying 1-bit...");
    if (!SD_MMC.begin()) {
      Serial.println("[ERROR] SD Card mount failed.");
      return false;
    }
    Serial.println("[INFO] SD mounted in 1-bit mode.");
  } else {
    Serial.println("[INFO] SD mounted in 4-bit mode.");
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[ERROR] No SD card detected.");
    return false;
  }

  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  Serial.printf("[INFO] SD Card: %s, Size: %llu MB\n",
                cardType == CARD_MMC  ? "MMC" :
                cardType == CARD_SD   ? "SDSC" :
                cardType == CARD_SDHC ? "SDHC" : "UNKNOWN",
                cardSize);

  sdReady = true;
  return true;
}

// ============================================================================
// Config File
// ============================================================================
bool readConfigFromSD() {
  File f = SD_MMC.open("/config.txt", FILE_READ);
  if (!f) {
    Serial.println("[INFO] No /config.txt found, using defaults.");
    return false;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;

    int eq = line.indexOf('=');
    if (eq == -1) continue;

    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim();
    val.trim();

    if (key.equalsIgnoreCase("mode")) {
      currentMode = val.equalsIgnoreCase("transfer") ? MODE_TRANSFER : MODE_PHOTO;
    } else if (key.equalsIgnoreCase("interval")) {
      intervalSec = val.toInt();
      if (intervalSec < 1) intervalSec = DEFAULT_INTERVAL_SEC;
    } else if (key.equalsIgnoreCase("pictureNumber")) {
      pictureNumber = val.toInt();
    }
  }
  f.close();

  rtcMode = currentMode;
  rtcIntervalSec = intervalSec;
  rtcPictureNumber = pictureNumber;

  Serial.println("[INFO] Config loaded from /config.txt");
  return true;
}

bool saveConfigToSD() {
  File f = SD_MMC.open("/config.txt", FILE_WRITE);
  if (!f) {
    Serial.println("[ERROR] Failed to open /config.txt for writing.");
    return false;
  }
  f.println("# ESP32-CAM SerialTransfer Config");
  f.printf("mode=%s\n", (currentMode == MODE_TRANSFER) ? "transfer" : "photo");
  f.printf("interval=%lu\n", intervalSec);
  f.printf("pictureNumber=%lu\n", pictureNumber);
  f.close();
  return true;
}

// ============================================================================
// Mode Switch Triggers at Boot
// ============================================================================
void checkModeTriggersAtBoot() {
  Serial.println("[BOOT] Waiting for serial trigger (2s)... Send 'MODE TRANSFER' to switch.");
  unsigned long start = millis();
  while (millis() - start < BOOT_SERIAL_TIMEOUT_MS) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        bootBuffer.trim();
        if (bootBuffer.length() > 0) {
          String cmd = bootBuffer;
          bootBuffer = "";
          if (cmd.equalsIgnoreCase("MODE TRANSFER")) {
            Serial.println("[TRIGGER] Serial command -> TRANSFER mode");
            currentMode = MODE_TRANSFER;
            rtcMode = MODE_TRANSFER;
            return;
          } else if (cmd.equalsIgnoreCase("MODE PHOTO")) {
            Serial.println("[TRIGGER] Serial command -> PHOTO mode");
            currentMode = MODE_PHOTO;
            rtcMode = MODE_PHOTO;
            return;
          }
        }
      } else {
        bootBuffer += c;
      }
    }
  }
  Serial.println("[BOOT] No serial trigger received.");
}

// ============================================================================
// Photo Mode
// ============================================================================
void enterPhotoMode() {
  Serial.println("\n========== PHOTO MODE ==========");
  currentMode = MODE_PHOTO;
  rtcMode = MODE_PHOTO;
  takePhotoAndSleep();
}

void takePhotoAndSleep() {
  // Warm-up frames
  for (int i = 0; i < 10; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(50);
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[ERROR] Camera capture failed.");
    esp_sleep_enable_timer_wakeup(5000000);  // retry in 5 sec
    esp_deep_sleep_start();
    return;
  }

  digitalWrite(FLASH_LED_PIN, HIGH);
  rtc_gpio_hold_en(GPIO_NUM_33);

  pictureNumber++;
  String path = "/picture" + String(pictureNumber) + ".jpg";

  File file = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (!file) {
    Serial.println("[ERROR] Failed to open file for writing.");
  } else {
    file.write(fb->buf, fb->len);
    Serial.printf("[INFO] Saved: %s (%u bytes)\n", path.c_str(), fb->len);
    file.close();
    rtcPictureNumber = pictureNumber;
    saveConfigToSD();
  }

  esp_camera_fb_return(fb);

  digitalWrite(FLASH_LED_PIN, LOW);
  rtc_gpio_hold_dis(GPIO_NUM_33);

  Serial.printf("[INFO] Deep sleeping for %lu seconds...\n", intervalSec);
  Serial.flush();

  esp_sleep_enable_timer_wakeup((uint64_t)intervalSec * 1000000ULL);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_15, 0);

  esp_deep_sleep_start();
}

// ============================================================================
// Transfer Mode
// ============================================================================
void enterTransferMode() {
  Serial.println("\n========== TRANSFER MODE ==========");
  Serial.println("[INFO] Switching to 2000000 baud...");

  currentMode = MODE_TRANSFER;
  rtcMode = MODE_TRANSFER;

  Serial.flush();
  Serial.end();
  delay(100);

  Serial.setRxBufferSize(RX_BUFFER_SIZE);
  Serial.setTxBufferSize(TX_BUFFER_SIZE);
  Serial.begin(TRANSFER_BAUD);
  delay(200);

  myTransfer.begin(Serial);

  // Announce over plain serial for fallback detection
  Serial.println("\nSerial Filesystem Ready (SerialTransfer)");
  Serial.flush();
}

void transferModeLoop() {
  if (!myTransfer.available()) return;

  uint8_t* rx = myTransfer.packet.rxBuff;
  uint8_t* tx = myTransfer.packet.txBuff;
  uint16_t rxLen = myTransfer.bytesRead;
  uint8_t type = rx[0];

  Serial.printf("[RX] type=0x%02X len=%u\n", type, rxLen);

  // ----- PING -----
  if (type == CMD_PING) {
    tx[0] = RESP_PONG;
    myTransfer.sendData(1);
    return;
  }

  // ----- LIST -----
  if (type == CMD_LIST) {
    char path[128] = {0};
    uint16_t n = rxLen - 1; if (n > 127) n = 127;
    memcpy(path, rx + 1, n);
    Serial.printf("[LIST] '%s'\n", path);

    File dir = SD_MMC.open(path);
    if (!dir || !dir.isDirectory()) {
      tx[0] = RESP_ERROR;
      memcpy(tx + 1, "Bad dir", 7);
      myTransfer.sendData(8);
      if (dir) dir.close();
      return;
    }
    File entry = dir.openNextFile();
    while (entry) {
      tx[0] = RESP_ENTRY;
      tx[1] = entry.isDirectory() ? 1 : 0;
      uint32_t sz = entry.size();
      memcpy(tx + 2, &sz, 4);
      const char* name = entry.name();
      size_t nl = strlen(name);
      if (nl > PAYLOAD_SIZE - 6) nl = PAYLOAD_SIZE - 6;
      memcpy(tx + 6, name, nl);
      myTransfer.sendData(6 + nl);
      entry.close();
      entry = dir.openNextFile();
    }
    dir.close();
    tx[0] = RESP_OK;
    myTransfer.sendData(1);
    return;
  }

  // ----- READ (host downloads file from board) -----
  if (type == CMD_READ) {
    if (readActive) { readFile.close(); readActive = false; }
    char path[128] = {0};
    uint16_t n = rxLen - 1; if (n > 127) n = 127;
    memcpy(path, rx + 1, n);
    Serial.printf("[READ] '%s'\n", path);

    readFile = SD_MMC.open(path);
    if (!readFile || readFile.isDirectory()) {
      tx[0] = RESP_ERROR;
      memcpy(tx + 1, "Bad file", 8);
      myTransfer.sendData(9);
      if (readFile) { readFile.close(); }
      return;
    }
    uint32_t fsz = readFile.size();
    tx[0] = RESP_OK;
    memcpy(tx + 1, &fsz, 4);
    myTransfer.sendData(5);
    Serial.printf("[READ] size=%u\n", fsz);

    readRemaining = fsz;
    readActive = true;
    sendNextReadChunk();   // send chunk 0, wait for host ACK
    return;
  }

  // Host ACKed previous read chunk → send next one
  if (readActive && (type == CMD_PING || type == RESP_OK)) {
    sendNextReadChunk();
    return;
  }

  // ----- STORE (host uploads file to board) -----
  if (type == CMD_STORE) {
    if (storeActive) { storeFile.close(); storeActive = false; }
    if (rxLen < 5) {
      tx[0] = RESP_ERROR;
      memcpy(tx + 1, "Short", 5);
      myTransfer.sendData(6);
      return;
    }
    uint16_t pathLen = rxLen - 5;
    if (pathLen > 127) pathLen = 127;
    char path[128] = {0};
    memcpy(path, rx + 1, pathLen);
    uint32_t fileSize = 0;
    memcpy(&fileSize, rx + 1 + pathLen, 4);
    Serial.printf("[STORE] '%s' %lu bytes\n", path, (unsigned long)fileSize);

    storeFile = SD_MMC.open(path, FILE_WRITE);
    if (!storeFile) {
      tx[0] = RESP_ERROR;
      memcpy(tx + 1, "No open", 7);
      myTransfer.sendData(8);
      return;
    }
    storeExpected = fileSize;
    storeReceived = 0;
    storeActive = true;
    tx[0] = RESP_OK;
    memcpy(tx + 1, "Ready", 5);
    myTransfer.sendData(6);
    return;
  }

  // Host sent DATA_CHUNK during upload → write it, ACK it
  if (storeActive && type == DATA_CHUNK) {
    uint16_t chunkLen = rxLen - 1;
    storeFile.write(rx + 1, chunkLen);
    storeReceived += chunkLen;
    tx[0] = RESP_OK;
    myTransfer.sendData(1);
    if (storeReceived >= storeExpected) {
      storeFile.close();
      storeActive = false;
      tx[0] = RESP_OK;
      char m[32]; snprintf(m, 32, "OK %lu", (unsigned long)storeReceived);
      memcpy(tx + 1, m, strlen(m));
      myTransfer.sendData(1 + strlen(m));
      Serial.printf("[STORE] OK %lu\n", (unsigned long)storeReceived);
    }
    return;
  }

  // Host sent END_OF_DATA → finalize upload
  if (storeActive && type == END_OF_DATA) {
    storeFile.close();
    storeActive = false;
    tx[0] = RESP_OK;
    char m[32]; snprintf(m, 32, "OK %lu", (unsigned long)storeReceived);
    memcpy(tx + 1, m, strlen(m));
    myTransfer.sendData(1 + strlen(m));
    Serial.printf("[STORE] EOF ok %lu\n", (unsigned long)storeReceived);
    return;
  }

  // ----- DELETE -----
  if (type == CMD_DELETE) {
    char path[128] = {0};
    uint16_t n = rxLen - 1; if (n > 127) n = 127;
    memcpy(path, rx + 1, n);
    Serial.printf("[DEL] '%s'\n", path);
    if (!SD_MMC.exists(path)) {
      tx[0] = RESP_ERROR; memcpy(tx + 1, "Missing", 7); myTransfer.sendData(8);
    } else if (SD_MMC.remove(path)) {
      tx[0] = RESP_OK;    memcpy(tx + 1, "Gone", 4);    myTransfer.sendData(5);
    } else {
      tx[0] = RESP_ERROR; memcpy(tx + 1, "Fail", 4);    myTransfer.sendData(5);
    }
    return;
  }

  // ----- MODE -----
  if (type == CMD_MODE) {
    uint8_t m = rx[1];
    Serial.printf("[MODE] %u\n", m);
    if (m == MODE_PHOTO) {
      tx[0] = RESP_OK; memcpy(tx + 1, "Photo", 5); myTransfer.sendData(6);
      Serial.flush(); delay(100);
      enterPhotoMode();
    } else if (m == MODE_TRANSFER) {
      tx[0] = RESP_OK; memcpy(tx + 1, "Here", 4); myTransfer.sendData(5);
    } else {
      tx[0] = RESP_ERROR; memcpy(tx + 1, "Bad", 3); myTransfer.sendData(4);
    }
    return;
  }

  // ----- unknown / stray -----
  tx[0] = RESP_ERROR;
  memcpy(tx + 1, "What?", 5);
  myTransfer.sendData(6);
}
