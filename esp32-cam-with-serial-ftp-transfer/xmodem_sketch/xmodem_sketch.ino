/*
  ESP32-CAM Serial File Transfer with XMODEM-CRC (xmodem-lib)

  Arduino library: xmodem-lib by gilman88
    https://github.com/gilman88/xmodem-lib
    Install: clone/copy to ~/Arduino/libraries/xmodem-lib

  Protocol:
    - Text commands at BOOT_BAUD (115200) during boot window
    - Transfer mode at TRANSFER_BAUD (configurable, default 921600 for CH340)
    - LIST / DELETE / HELP / MODE use plain text
    - READ / STORE use XMODEM-CRC via xmodem-lib

  Board: ESP32 Dev Module (AI-Thinker ESP32-CAM)
  Partition: Huge APP (3 MB No OTA / 1 MB SPIFFS) recommended
*/

#include <XModem.h>
#include "esp_camera.h"
#include "Arduino.h"
#include "FS.h"
#include "SD_MMC.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/rtc_io.h"

// ========== Pin Definitions (AI-Thinker ESP32-CAM) ==========

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

// ========== Configurable Baud Rates ==========
// Override at compile time: -DTRANSFER_BAUD=115200
#define BOOT_BAUD              115200
#ifndef TRANSFER_BAUD
#define TRANSFER_BAUD          921600   // CH340 reliable max; 2000000 for FT232
#endif

// ========== Other Configuration ==========

#define BOOT_TIMEOUT_MS        3000
#define RX_BUFFER_SIZE         8192
#define TX_BUFFER_SIZE         8192
#define PROMPT                 "> "

// Use xmodem-lib's basic XMODEM preset (1-byte checksum).
// The CRC_XMODEM preset has known bugs (byte-order + other issues).
// For a USB serial link, 1-byte checksum is perfectly adequate.

// ========== Mode Enum ==========

enum DeviceMode {
  MODE_PHOTO = 0,
  MODE_TRANSFER = 1
};

// ========== RTC-Persisted State ==========

RTC_DATA_ATTR uint8_t  rtcMode = MODE_PHOTO;
RTC_DATA_ATTR uint32_t rtcPictureNumber = 0;
RTC_DATA_ATTR uint32_t rtcIntervalSec = 60;

// ========== Runtime State ==========

DeviceMode currentMode;
uint32_t   pictureNumber;
uint32_t   intervalSec;
bool       sdReady = false;
bool       cameraReady = false;
String     inputBuffer = "";
bool       commandReady = false;

// XModem instance
XModem xmodem;

// Context shared with XModem callbacks
struct XmodemCtx {
  File   file;
  size_t expectedSize;
  size_t written;
  bool   error;
};
XmodemCtx xmodemCtx;

// ========== Forward Declarations ==========

bool initCamera();
bool initSD();
void checkBootTriggers();
void enterPhotoMode();
void enterTransferMode();
void takePhotoAndSleep();
void transferModeLoop();
void processCommand(const String& cmd);
void cmdList(const String& path);
void cmdRead(const String& path);
void cmdStore(const String& path, size_t fileSize);
void cmdDelete(const String& path);
void cmdHelp();
void printPrompt();
void drainSerial();

// XModem callbacks
void xmodemBlockLookup(void *blk_id, size_t idSize, byte *send_data, size_t dataSize);
bool xmodemReceiveBlock(void *blk_id, size_t idSize, byte *data, size_t dataSize);
void xmodemBasicChksum(byte *data, size_t dataSize, byte *chksum);

// ========== Setup ==========

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(BOOT_BAUD);
  delay(100);

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  currentMode   = (rtcMode == MODE_TRANSFER) ? MODE_TRANSFER : MODE_PHOTO;
  pictureNumber = rtcPictureNumber;
  intervalSec   = rtcIntervalSec;

  Serial.printf("\n[BOOT] Wakeup cause: %d\n", esp_sleep_get_wakeup_cause());
  Serial.printf("[BOOT] Mode: %s\n", (currentMode == MODE_TRANSFER) ? "transfer" : "photo");
  Serial.printf("[BOOT] Picture#: %lu  Interval: %lu s\n", pictureNumber, intervalSec);
  Serial.printf("[BOOT] Transfer baud: %lu\n", (unsigned long)TRANSFER_BAUD);

  if (!initSD()) {
    Serial.println("[FATAL] SD init failed. Halting.");
    return;
  }

  if (!initCamera()) {
    Serial.println("[FATAL] Camera init failed. Halting.");
    return;
  }

  checkBootTriggers();

  if (currentMode == MODE_TRANSFER) {
    enterTransferMode();
  } else {
    enterPhotoMode();
  }
}

void loop() {
  if (currentMode == MODE_TRANSFER) {
    transferModeLoop();
  }
}

// ========== Initialization ==========

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;

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

  sensor_t* s = esp_camera_sensor_get();
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);
  s->set_hmirror(s, 1);
  s->set_vflip(s, 1);
  s->set_dcw(s, 1);

  cameraReady = true;
  Serial.println("[INFO] Camera ready.");
  return true;
}

bool initSD() {
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("[WARN] SD 4-bit failed, trying 1-bit...");
    if (!SD_MMC.begin()) {
      Serial.println("[ERROR] SD mount failed.");
      return false;
    }
    Serial.println("[INFO] SD 1-bit mode.");
  } else {
    Serial.println("[INFO] SD 4-bit mode.");
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[ERROR] No SD card.");
    return false;
  }

  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  Serial.printf("[INFO] SD: %s, %llu MB\n",
                cardType == CARD_MMC ? "MMC" :
                cardType == CARD_SD  ? "SDSC" : "SDHC",
                cardSize);

  sdReady = true;
  return true;
}

// ========== Boot Triggers ==========

void checkBootTriggers() {
  Serial.println("[BOOT] Waiting for serial trigger (3s)... Send 'MODE TRANSFER' to switch.");
  unsigned long start = millis();
  while (millis() - start < BOOT_TIMEOUT_MS) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        inputBuffer.trim();
        if (inputBuffer.length() > 0) {
          String cmd = inputBuffer;
          inputBuffer = "";
          if (cmd.equalsIgnoreCase("MODE TRANSFER")) {
            Serial.println("[TRIGGER] -> TRANSFER mode");
            currentMode = MODE_TRANSFER;
            rtcMode = MODE_TRANSFER;
            return;
          } else if (cmd.equalsIgnoreCase("MODE PHOTO")) {
            Serial.println("[TRIGGER] -> PHOTO mode");
            currentMode = MODE_PHOTO;
            rtcMode = MODE_PHOTO;
            return;
          }
        }
      } else {
        inputBuffer += c;
      }
    }
  }
  Serial.println("[BOOT] No trigger.");
}

// ========== Photo Mode ==========

void enterPhotoMode() {
  Serial.println("\n========== PHOTO MODE ==========");
  currentMode = MODE_PHOTO;
  rtcMode = MODE_PHOTO;
  takePhotoAndSleep();
}

void takePhotoAndSleep() {
  for (int i = 0; i < 10; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(50);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[ERROR] Capture failed.");
    esp_sleep_enable_timer_wakeup(5000000);
    esp_deep_sleep_start();
    return;
  }

  digitalWrite(FLASH_LED_PIN, HIGH);
  rtc_gpio_hold_en(GPIO_NUM_33);

  pictureNumber++;
  String path = "/picture" + String(pictureNumber) + ".jpg";

  File file = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (file) {
    file.write(fb->buf, fb->len);
    Serial.printf("[INFO] Saved: %s (%u bytes)\n", path.c_str(), fb->len);
    file.close();
    rtcPictureNumber = pictureNumber;
  } else {
    Serial.println("[ERROR] File open failed.");
  }

  esp_camera_fb_return(fb);
  digitalWrite(FLASH_LED_PIN, LOW);
  rtc_gpio_hold_dis(GPIO_NUM_33);

  Serial.printf("[INFO] Sleeping %lu s...\n", intervalSec);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)intervalSec * 1000000ULL);
  esp_deep_sleep_start();
}

// ========== Transfer Mode ==========

void enterTransferMode() {
  Serial.println("\n========== TRANSFER MODE ==========");
  Serial.printf("[INFO] Switching to %lu baud...\n", (unsigned long)TRANSFER_BAUD);
  currentMode = MODE_TRANSFER;
  rtcMode = MODE_TRANSFER;

  Serial.flush();
  Serial.end();
  delay(100);
  Serial.setRxBufferSize(RX_BUFFER_SIZE);
  Serial.setTxBufferSize(TX_BUFFER_SIZE);
  Serial.begin(TRANSFER_BAUD);
  delay(200);

  // Initialize xmodem-lib with basic XMODEM preset (1-byte checksum).
  xmodem.begin(Serial, XModem::ProtocolType::XMODEM);
  xmodem.setChksumHandler(xmodemBasicChksum);
  xmodem.setBlockLookupHandler(xmodemBlockLookup);
  xmodem.setRecieveBlockHandler(xmodemReceiveBlock);

  Serial.println("\nSerial Filesystem Ready (xmodem-lib XMODEM)");
  Serial.printf("Transfer baud: %lu\n", (unsigned long)TRANSFER_BAUD);
  Serial.println("Commands: LIST <path> | READ <path> | STORE <path> <size> | DELETE <path> | MODE PHOTO | HELP");
  printPrompt();
}

void transferModeLoop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (inputBuffer.length() > 0) {
        commandReady = true;
        break;
      }
    } else {
      inputBuffer += c;
    }
  }

  if (commandReady) {
    inputBuffer.trim();
    if (inputBuffer.length() > 0) {
      processCommand(inputBuffer);
    }
    inputBuffer = "";
    commandReady = false;
    printPrompt();
  }
}

void printPrompt() {
  Serial.print(PROMPT);
  Serial.flush();
}

void drainSerial() {
  while (Serial.available()) {
    Serial.read();
  }
}

// ========== Command Parser ==========

void processCommand(const String& cmd) {
  int firstSpace = cmd.indexOf(' ');
  String command = (firstSpace == -1) ? cmd : cmd.substring(0, firstSpace);
  String args    = (firstSpace == -1) ? "" : cmd.substring(firstSpace + 1);
  command.toLowerCase();
  args.trim();

  if (command == "list") {
    if (args.length() == 0) {
      Serial.println("[ERROR] Usage: LIST <path>");
      return;
    }
    cmdList(args);
  }
  else if (command == "read") {
    if (args.length() == 0) {
      Serial.println("[ERROR] Usage: READ <path>");
      return;
    }
    cmdRead(args);
  }
  else if (command == "store") {
    int lastSpace = args.lastIndexOf(' ');
    if (lastSpace == -1) {
      Serial.println("[ERROR] Usage: STORE <path> <size>");
      return;
    }
    String path = args.substring(0, lastSpace);
    String sizeStr = args.substring(lastSpace + 1);
    path.trim();
    sizeStr.trim();
    size_t fileSize = sizeStr.toInt();
    if (fileSize == 0) {
      Serial.println("[ERROR] Invalid size.");
      return;
    }
    cmdStore(path, fileSize);
  }
  else if (command == "delete") {
    if (args.length() == 0) {
      Serial.println("[ERROR] Usage: DELETE <path>");
      return;
    }
    cmdDelete(args);
  }
  else if (command == "mode") {
    args.toLowerCase();
    if (args == "photo") {
      Serial.println("[OK] Switching to PHOTO mode.");
      Serial.printf("[INFO] Reset baud to %lu after sleep.\n", (unsigned long)BOOT_BAUD);
      enterPhotoMode();
    } else if (args == "transfer") {
      Serial.println("[OK] Already in TRANSFER mode.");
    } else {
      Serial.println("[ERROR] Usage: MODE PHOTO | MODE TRANSFER");
    }
  }
  else if (command == "help") {
    cmdHelp();
  }
  else {
    Serial.println("[ERROR] Unknown command. Type HELP.");
  }
}

void cmdHelp() {
  Serial.println("Available commands:");
  Serial.println("  LIST   <path>              - List directory");
  Serial.println("  READ   <file_path>         - Download file via XMODEM-CRC");
  Serial.println("  STORE  <file_path> <size>  - Upload file via XMODEM-CRC");
  Serial.println("  DELETE <file_path>         - Delete a file");
  Serial.println("  MODE   PHOTO|TRANSFER      - Switch mode");
  Serial.println("  HELP                       - Show this message");
}

// ========== LIST ==========

void cmdList(const String& path) {
  File dir = SD_MMC.open(path);
  if (!dir || !dir.isDirectory()) {
    Serial.println("[ERROR] Not a directory: " + path);
    if (dir) dir.close();
    return;
  }

  String out;
  out.reserve(2048);
  File entry = dir.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      out += "[DIR]  ";
      out += entry.name();
      out += "\n";
    } else {
      char line[80];
      snprintf(line, sizeof(line), "[FILE] %8u bytes  %s\n", entry.size(), entry.name());
      out += line;
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  Serial.print(out);
}

// ========== DELETE ==========

void cmdDelete(const String& path) {
  if (!SD_MMC.exists(path)) {
    Serial.println("[ERROR] Not found: " + path);
    return;
  }
  if (SD_MMC.remove(path)) {
    Serial.println("[OK] Deleted: " + path);
  } else {
    Serial.println("[ERROR] Failed to delete: " + path);
  }
}

// ========== READ (device -> host via xmodem-lib) ==========

void cmdRead(const String& path) {
  File file = SD_MMC.open(path);
  if (!file || file.isDirectory()) {
    Serial.println("[ERROR] Cannot open: " + path);
    if (file) file.close();
    return;
  }

  size_t fileSize = file.size();

  // Allocate buffer for the entire file
  // ESP32-CAM usually has PSRAM; try that first for large files
  uint8_t* buf = nullptr;
  if (psramFound()) {
    buf = (uint8_t*)ps_malloc(fileSize);
  }
  if (!buf) {
    buf = (uint8_t*)malloc(fileSize);
  }
  if (!buf) {
    Serial.println("[ERROR] Out of memory. File too large for RAM.");
    file.close();
    return;
  }

  size_t n = file.read(buf, fileSize);
  file.close();

  if (n != fileSize) {
    Serial.println("[ERROR] File read incomplete.");
    free(buf);
    return;
  }

  // Tell the client the file size, then hand off to xmodem-lib
  Serial.printf("XMODEM %u\n", fileSize);
  Serial.flush();
  delay(50);  // let client parse the header

  // xmodem-lib takes over Serial for the transfer
  bool ok = xmodem.send(buf, fileSize);

  free(buf);
  drainSerial();  // discard any trailing protocol bytes before prompt

  if (!ok) {
    Serial.println("[ERROR] XMODEM send failed.");
  }
}

// ========== STORE (host -> device via xmodem-lib) ==========

void cmdStore(const String& path, size_t fileSize) {
  if (SD_MMC.exists(path)) {
    SD_MMC.remove(path);
  }

  File file = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (!file) {
    Serial.println("[ERROR] Cannot create: " + path);
    drainSerial();
    return;
  }

  // Set up context for the receive handler
  xmodemCtx.file = file;
  xmodemCtx.expectedSize = fileSize;
  xmodemCtx.written = 0;
  xmodemCtx.error = false;

  Serial.println("XMODEM READY");
  Serial.flush();
  delay(100);

  // xmodem-lib takes over Serial for the transfer
  bool ok = xmodem.receive();

  xmodemCtx.file.close();
  drainSerial();

  // xmodem-lib returns false on some EOT-handling quirks even when the
  // transfer succeeded. Verify by checking the actual file size on SD.
  size_t actualSize = 0;
  if (SD_MMC.exists(path)) {
    File verify = SD_MMC.open(path);
    if (verify) {
      actualSize = verify.size();
      verify.close();
    }
  }

  if (actualSize == fileSize) {
    Serial.printf("[OK] Stored: %s (%u bytes)\n", path.c_str(), fileSize);
  } else {
    Serial.printf("[ERROR] XMODEM receive failed. Expected %u bytes, got %u.\n",
                  fileSize, actualSize);
    if (SD_MMC.exists(path)) {
      SD_MMC.remove(path);
    }
  }
}

// =============================================================================
// xmodem-lib callbacks
// =============================================================================

/*
  Block Lookup Handler — called by xmodem-lib when sending.
  Loads data from the SD file into send_data based on block ID.
  Block IDs start at 1.  Blocks past EOF are padded with 0x1A (CPM EOF).
*/
void xmodemBlockLookup(void *blk_id, size_t idSize, byte *send_data, size_t dataSize) {
  (void)idSize;  // always 1 for standard XMODEM
  uint8_t blockNum = *(uint8_t*)blk_id;
  size_t offset = (blockNum - 1) * dataSize;

  xmodemCtx.file.seek(offset);
  size_t n = xmodemCtx.file.read(send_data, dataSize);
  if (n < dataSize) {
    memset(send_data + n, 0x1A, dataSize - n);
  }
}

/*
  Receive Block Handler — called by xmodem-lib when receiving.
  Writes received data to the SD file.
  Returns TRUE to continue, FALSE to abort the transfer.
*/
bool xmodemReceiveBlock(void *blk_id, size_t idSize, byte *data, size_t dataSize) {
  (void)blk_id;
  (void)idSize;
  (void)dataSize;  // library strips trailing 0x1A; do not trust this

  if (xmodemCtx.error) {
    return false;
  }

  // The library strips trailing 0x1A bytes in dataSize (documented edge case),
  // but the buffer always contains ALL received bytes including padding.
  // Use the full block size and let expectedSize cap the write.
  const size_t BLOCK_SIZE = 128;
  size_t toWrite = min(BLOCK_SIZE, xmodemCtx.expectedSize - xmodemCtx.written);
  if (toWrite > 0) {
    size_t n = xmodemCtx.file.write(data, toWrite);
    if (n != toWrite) {
      xmodemCtx.error = true;
      return false;
    }
    xmodemCtx.written += n;
  }
  return true;
}

/*
  Custom basic checksum handler — computes standard XMODEM 1-byte checksum.
  Forces the library to use our calculation, ruling out a buggy default.
*/
void xmodemBasicChksum(byte *data, size_t dataSize, byte *chksum) {
  byte sum = 0;
  for (size_t i = 0; i < dataSize; i++) {
    sum += data[i];
  }
  chksum[0] = sum;
}
