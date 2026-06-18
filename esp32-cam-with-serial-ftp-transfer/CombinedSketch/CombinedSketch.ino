#include "esp_camera.h"
#include "Arduino.h"
#include "FS.h"
#include "SD_MMC.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/rtc_io.h"

// ========== Pin Definitions ==========

// Camera pins (AI-Thinker ESP32-CAM)
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

// Mode switch pins - adjust to your hardware wiring
// we do not know these pins.

#define CHARGING_PIN      -1   // HIGH when charging (connect to your charging IC status output)
#define BUTTON_PIN        -1   // Button to toggle mode (connect with pull-down or use internal pull-up)
#define FLASH_LED_PIN     33   // Onboard flash LED

// ========== Runtime State ==========

enum DeviceMode {
  MODE_PHOTO = 0,
  MODE_TRANSFER = 1
};

// ========== Configuration Defaults ==========

#define DEFAULT_MODE           MODE_PHOTO
#define DEFAULT_INTERVAL_SEC   60
#define BOOT_SERIAL_TIMEOUT_MS 2000   // Time window to receive serial trigger at boot
#define TRANSFER_BAUD          921600
// #define TRANSFER_BAUD          2000000
#define RX_BUFFER_SIZE         8192
#define TX_BUFFER_SIZE         8192
#define FILE_READ_CHUNK_SIZE   16384
#define FILE_WRITE_CHUNK_SIZE  4096
#define CHUNK_SIZE             2048

// ========== RTC-Persisted State (survives deep sleep) ==========

RTC_DATA_ATTR uint8_t rtcMode = DEFAULT_MODE;
RTC_DATA_ATTR uint32_t rtcPictureNumber = 0;
RTC_DATA_ATTR uint32_t rtcIntervalSec = DEFAULT_INTERVAL_SEC;

// ========== Runtime Variables ==========

DeviceMode currentMode;
uint32_t pictureNumber;
uint32_t intervalSec;

bool sdReady = false;
bool cameraReady = false;

// Serial command parsing
String inputBuffer = "";
bool commandReady = false;

// ========== Forward Declarations ==========

bool initCamera();
bool initSD();
bool readConfigFromSD();
bool saveConfigToSD();
void checkModeTriggersAtBoot();
void enterPhotoMode();
void enterTransferMode();
void takePhotoAndSleep();
void transferModeLoop();
void processTransferCommand(String cmd);
void listDirectory(String path);
void readFile(String path);
void storeFile(String path, size_t fileSize);
void deleteFile(String path);
void printPrompt();

// ========== Setup ==========

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(100);

  // Initialize mode-switch input pins
  pinMode(CHARGING_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  // Load persisted state
  currentMode = (rtcMode == MODE_TRANSFER) ? MODE_TRANSFER : MODE_PHOTO;
  pictureNumber = rtcPictureNumber;
  intervalSec = rtcIntervalSec;

  Serial.printf("\n[BOOT] Wakeup cause: %d\n", esp_sleep_get_wakeup_cause());
  Serial.printf("[BOOT] Persisted mode: %s\n", (currentMode == MODE_TRANSFER) ? "transfer" : "photo");
  Serial.printf("[BOOT] Picture number: %lu\n", pictureNumber);
  Serial.printf("[BOOT] Interval: %lu sec\n", intervalSec);

  // Initialize SD first (needed for config and both modes)
  if (!initSD()) {
    Serial.println("[FATAL] SD Card init failed. Halting.");
    return;
  }

  // Read config file from SD (overrides RTC defaults on first boot)
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
    readConfigFromSD();
  }

  // Check mode-switch triggers at boot
  checkModeTriggersAtBoot();

  // Initialize camera (shared resource)
  if (!initCamera()) {
    Serial.println("[FATAL] Camera init failed. Halting.");
    return;
  }

  // Enter the appropriate mode
  if (currentMode == MODE_TRANSFER) {
    enterTransferMode();
  } else {
    enterPhotoMode();
  }
}

void loop() {
  // loop() is only active in transfer mode.
  // Photo mode ends in setup() via deep sleep.
  if (currentMode == MODE_TRANSFER) {
    transferModeLoop();
  }
}

// ========== Initialization ==========

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
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
  // Try 4-bit mode first
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
                cardType == CARD_MMC ? "MMC" :
                cardType == CARD_SD  ? "SDSC" :
                cardType == CARD_SDHC ? "SDHC" : "UNKNOWN",
                cardSize);

  sdReady = true;
  return true;
}

// ========== Config File ==========

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
      if (val.equalsIgnoreCase("transfer")) {
        currentMode = MODE_TRANSFER;
      } else {
        currentMode = MODE_PHOTO;
      }
    } else if (key.equalsIgnoreCase("interval")) {
      intervalSec = val.toInt();
      if (intervalSec < 1) intervalSec = DEFAULT_INTERVAL_SEC;
    } else if (key.equalsIgnoreCase("pictureNumber")) {
      pictureNumber = val.toInt();
    }
  }
  f.close();

  // Persist to RTC so deep sleep retains these values
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
  f.println("# ESP32-CAM Combined Sketch Config");
  f.printf("mode=%s\n", (currentMode == MODE_TRANSFER) ? "transfer" : "photo");
  f.printf("interval=%lu\n", intervalSec);
  f.printf("pictureNumber=%lu\n", pictureNumber);
  f.close();
  return true;
}

// ========== Mode Switch Triggers ==========

void checkModeTriggersAtBoot() {
  // 1. Charging detection — highest priority
  // if (digitalRead(CHARGING_PIN) == HIGH) {
  //   Serial.println("[TRIGGER] Charging detected -> TRANSFER mode");
  //   currentMode = MODE_TRANSFER;
  //   rtcMode = MODE_TRANSFER;
  //   return;
  // }

  // // 2. Button pressed at boot
  // if (digitalRead(BUTTON_PIN) == LOW) {
  //   Serial.println("[TRIGGER] Button pressed at boot -> toggling mode");
  //   currentMode = (currentMode == MODE_PHOTO) ? MODE_TRANSFER : MODE_PHOTO;
  //   rtcMode = currentMode;
  //   // Debounce: wait for release
  //   while (digitalRead(BUTTON_PIN) == LOW) { delay(10); }
  //   delay(100);
  //   return;
  // }

  // 3. Serial command during boot window
  Serial.println("[BOOT] Waiting for serial trigger (2s)... Send 'MODE TRANSFER' to switch.");
  unsigned long start = millis();
  while (millis() - start < BOOT_SERIAL_TIMEOUT_MS) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        inputBuffer.trim();
        if (inputBuffer.length() > 0) {
          String cmd = inputBuffer;
          inputBuffer = "";
          if (cmd.equalsIgnoreCase("MODE TRANSFER")) {
            Serial.println("[TRIGGER] Serial command received -> TRANSFER mode");
            currentMode = MODE_TRANSFER;
            rtcMode = MODE_TRANSFER;
            return;
          } else if (cmd.equalsIgnoreCase("MODE PHOTO")) {
            Serial.println("[TRIGGER] Serial command received -> PHOTO mode");
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
  Serial.println("[BOOT] No serial trigger received.");
}

// ========== Photo Mode ==========

void enterPhotoMode() {
  Serial.println("\n========== ENTERING PHOTO MODE ==========");
  currentMode = MODE_PHOTO;
  rtcMode = MODE_PHOTO;

  takePhotoAndSleep();
}

void takePhotoAndSleep() {
  // Warm-up: discard first few frames
  for (int i = 0; i < 10; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(50);
  }

  // Capture
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[ERROR] Camera capture failed.");
    // Retry after short sleep
    esp_sleep_enable_timer_wakeup(5000000); // 5 sec
    esp_deep_sleep_start();
    return;
  }

  // Flash LED on during save
  digitalWrite(FLASH_LED_PIN, HIGH);
  rtc_gpio_hold_en(GPIO_NUM_33);

  // Build filename
  pictureNumber++;
  String path = "/picture" + String(pictureNumber) + ".jpg";

  fs::FS &fs = SD_MMC;
  File file = fs.open(path.c_str(), FILE_WRITE);
  if (!file) {
    Serial.println("[ERROR] Failed to open file for writing.");
  } else {
    file.write(fb->buf, fb->len);
    Serial.printf("[INFO] Saved: %s (%u bytes)\n", path.c_str(), fb->len);
    file.close();

    // Persist picture number
    rtcPictureNumber = pictureNumber;
    saveConfigToSD();
  }

  esp_camera_fb_return(fb);

  // Turn off flash
  digitalWrite(FLASH_LED_PIN, LOW);
  rtc_gpio_hold_dis(GPIO_NUM_33);

  Serial.printf("[INFO] Deep sleeping for %lu seconds...\n", intervalSec);
  Serial.flush();

  // Configure wakeup sources
  esp_sleep_enable_timer_wakeup((uint64_t)intervalSec * 1000000ULL);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_15, 0); // Button LOW wakes up

  esp_deep_sleep_start();
}

// ========== Transfer Mode ==========

void enterTransferMode() {
  Serial.println("\n========== ENTERING TRANSFER MODE ==========");
  Serial.println("\n[INFO] Remember to set baud rate to 2000000");
  currentMode = MODE_TRANSFER;
  rtcMode = MODE_TRANSFER;

  // Re-init serial with high baud for fast transfer
  Serial.flush();
  Serial.end();
  delay(100);
  Serial.setRxBufferSize(RX_BUFFER_SIZE);
  Serial.setTxBufferSize(TX_BUFFER_SIZE);
  Serial.begin(TRANSFER_BAUD);
  delay(200);

  Serial.println("\nSerial Filesystem Ready");
  Serial.println("Commands: LIST <path> | READ <path> | STORE <path> <size> | DELETE <path> | MODE PHOTO | HELP");
  printPrompt();
}

void transferModeLoop() {
  // Check button in transfer mode to switch back to photo
  // if (digitalRead(BUTTON_PIN) == LOW) {
  //   Serial.println("\n[TRIGGER] Button pressed -> switching to PHOTO mode");
  //   while (digitalRead(BUTTON_PIN) == LOW) { delay(10); }
  //   delay(100);
  //   enterPhotoMode();
  //   return;
  // }

  // Check if charging stopped (optional: auto-switch back to photo)
  // if (digitalRead(CHARGING_PIN) == LOW) { ... }

  // Read serial commands
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
      processTransferCommand(inputBuffer);
    }
    inputBuffer = "";
    commandReady = false;
    printPrompt();
  }
}

void printPrompt() {
  Serial.print("> ");
  Serial.flush();
}

void processTransferCommand(String cmd) {
  // Tokenize: command arg1 arg2 ...
  int firstSpace = cmd.indexOf(' ');
  String command = (firstSpace == -1) ? cmd : cmd.substring(0, firstSpace);
  String args = (firstSpace == -1) ? "" : cmd.substring(firstSpace + 1);
  command.toLowerCase();
  args.trim();

  if (command == "list") {
    if (args.length() == 0) {
      Serial.println("[ERROR] Usage: LIST <absolute path>");
      return;
    }
    listDirectory(args);
  }
  else if (command == "read") {
    if (args.length() == 0) {
      Serial.println("[ERROR] Usage: READ <absolute file path>");
      return;
    }
    readFile(args);
  }
  else if (command == "store") {
    // Format: STORE <path> <size>
    int lastSpace = args.lastIndexOf(' ');
    if (lastSpace == -1) {
      Serial.println("[ERROR] Usage: STORE <absolute file path> <size_in_bytes>");
      return;
    }
    String path = args.substring(0, lastSpace);
    String sizeStr = args.substring(lastSpace + 1);
    path.trim();
    sizeStr.trim();
    size_t fileSize = sizeStr.toInt();
    if (fileSize == 0) {
      Serial.println("[ERROR] Invalid file size.");
      return;
    }
    storeFile(path, fileSize);
  }
  else if (command == "delete") {
    if (args.length() == 0) {
      Serial.println("[ERROR] Usage: DELETE <absolute file path>");
      return;
    }
    deleteFile(args);
  }
  else if (command == "mode") {
    args.toLowerCase();
    if (args == "photo") {
      Serial.println("[OK] Switching to PHOTO mode...");
      Serial.println("[INFO] Remember to reset baud rate to 115200");
      enterPhotoMode();
    } else if (args == "transfer") {
      Serial.println("[OK] Already in TRANSFER mode.");
    } else {
      Serial.println("[ERROR] Usage: MODE PHOTO | MODE TRANSFER");
    }
  }
  else if (command == "help") {
    Serial.println("Available commands:");
    Serial.println("  LIST   <path>              - List directory contents");
    Serial.println("  READ   <file_path>         - Read file and send binary");
    Serial.println("  STORE  <file_path> <size>  - Write file from binary data");
    Serial.println("  DELETE <file_path>         - Delete a file");
    Serial.println("  MODE   PHOTO|TRANSFER      - Switch device mode");
    Serial.println("  HELP                       - Show this message");
  }
  else {
    Serial.println("[ERROR] Unknown command. Type HELP for available commands.");
  }
}

// ========== Protocol Helpers ==========

bool waitForAck(unsigned long timeoutMs = 5000) {
  String ack = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        ack.trim();
        return ack.equalsIgnoreCase("ACK");
      }
      ack += c;
    }
  }
  return false;
}

// djb2 hash checksum
uint32_t computeChecksum(uint8_t *data, size_t len) {
  uint32_t h = 5381;
  for (size_t i = 0; i < len; i++) {
    h = ((h << 5) + h) + data[i];
  }
  return h;
}

String readSerialLine(unsigned long timeoutMs) {
  String line = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        line.trim();
        return line;
      }
      line += c;
    }
  }
  return "";
}

// ========== File Operations ==========

void listDirectory(String path) {
  File dir = SD_MMC.open(path);
  if (!dir) {
    Serial.println("[ERROR] Failed to open directory: " + path);
    return;
  }
  if (!dir.isDirectory()) {
    Serial.println("[ERROR] Not a directory: " + path);
    dir.close();
    return;
  }

  String output = "";
  output.reserve(2048);

  File entry = dir.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      output += "[DIR]  ";
      output += entry.name();
      output += "\n";
    } else {
      char line[80];
      snprintf(line, sizeof(line), "[FILE] %8u bytes  %s\n", entry.size(), entry.name());
      output += line;
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  Serial.print(output);
}

void readFile(String path) {
  File file = SD_MMC.open(path);
  if (!file) {
    Serial.println("[ERROR] Failed to open file: " + path);
    return;
  }
  if (file.isDirectory()) {
    Serial.println("[ERROR] Path is a directory: " + path);
    file.close();
    return;
  }

  size_t fileSize = file.size();
  Serial.printf("SIZE: %u\n", fileSize);
  Serial.flush();

  // Wait for client ACK before sending binary data
  if (!waitForAck()) {
    Serial.println("[ERROR] No ACK received from client");
    file.close();
    return;
  }

  uint8_t *buf = (uint8_t *)malloc(CHUNK_SIZE);
  if (!buf) {
    Serial.println("[ERROR] Memory allocation failed.");
    file.close();
    return;
  }

  size_t remaining = fileSize;
  while (remaining > 0) {
    size_t chunkLen = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
    size_t bytesRead = file.read(buf, chunkLen);

    // Send checksum
    uint32_t checksum = computeChecksum(buf, bytesRead);
    Serial.printf("CHECKSUM: %lu\n", checksum);
    Serial.flush();

    // Wait for ACKCHECKSUM
    String ack = readSerialLine(5000);
    if (ack != "ACKCHECKSUM") {
      Serial.println("[ERROR] No ACKCHECKSUM, aborting.");
      free(buf);
      file.close();
      return;
    }

    // Send chunk data
    Serial.write(buf, bytesRead);
    Serial.flush();

    // Wait for ACKDATA or NACKDATA
    ack = readSerialLine(5000);
    if (ack == "ACKDATA") {
      remaining -= bytesRead;
    } else if (ack == "NACKDATA") {
      // Resend same chunk: seek back
      file.seek(file.position() - bytesRead);
    } else {
      Serial.println("[ERROR] Expected ACKDATA/NACKDATA, aborting.");
      free(buf);
      file.close();
      return;
    }
  }

  free(buf);
  file.close();

  // End-of-file marker
  Serial.write(0x04);
  Serial.flush();
}

void storeFile(String path, size_t fileSize) {
  // Acknowledge and tell host to send ACK then data
  Serial.println("[OK] Ready to receive " + String(fileSize) + " bytes. Send ACK then data.");
  Serial.flush();

  // Wait for client ACK before receiving binary data
  if (!waitForAck()) {
    Serial.println("[ERROR] No ACK received from client");
    return;
  }

  fs::FS &fs = SD_MMC;
  File file = fs.open(path.c_str(), FILE_WRITE);
  if (!file) {
    Serial.println("[ERROR] Failed to open file for writing: " + path);
    // Drain incoming data
    size_t drained = 0;
    while (drained < fileSize) {
      if (Serial.available()) {
        Serial.read();
        drained++;
      }
    }
    return;
  }

  uint8_t *buf = (uint8_t *)malloc(CHUNK_SIZE);
  if (!buf) {
    Serial.println("[ERROR] Memory allocation failed.");
    file.close();
    return;
  }

  size_t received = 0;
  while (received < fileSize) {
    size_t chunkLen = (fileSize - received > CHUNK_SIZE) ? CHUNK_SIZE : (fileSize - received);

    // Read checksum from client
    String checksumLine = readSerialLine(5000);
    if (!checksumLine.startsWith("CHECKSUM: ")) {
      Serial.println("[ERROR] Expected CHECKSUM line, aborting.");
      free(buf);
      file.close();
      return;
    }
    uint32_t expectedChecksum = strtoul(checksumLine.substring(10).c_str(), NULL, 10);

    // Send ACKCHECKSUM
    Serial.println("ACKCHECKSUM");
    Serial.flush();

    // Read exactly chunkLen bytes
    size_t chunkReceived = 0;
    unsigned long lastByteTime = millis();
    while (chunkReceived < chunkLen) {
      int avail = Serial.available();
      if (avail > 0) {
        size_t toRead = min((size_t)avail, chunkLen - chunkReceived);
        size_t bytesRead = Serial.read(buf + chunkReceived, toRead);
        chunkReceived += bytesRead;
        lastByteTime = millis();
      }
      if (millis() - lastByteTime > 10000) {
        Serial.println("[ERROR] Timeout receiving chunk data.");
        free(buf);
        file.close();
        return;
      }
    }

    // Verify checksum
    uint32_t actualChecksum = computeChecksum(buf, chunkLen);
    if (actualChecksum == expectedChecksum) {
      file.write(buf, chunkLen);
      received += chunkLen;
      Serial.println("ACKDATA");
      Serial.flush();
    } else {
      Serial.println("NACKDATA");
      Serial.flush();
      // Don't write, don't advance received — client will resend
    }
  }

  free(buf);
  file.close();

  if (received == fileSize) {
    Serial.println("[OK] File stored: " + path + " (" + String(received) + " bytes)");
  } else {
    Serial.println("[ERROR] Incomplete transfer. Expected " + String(fileSize) + ", got " + String(received));
  }
}

void deleteFile(String path) {
  if (!SD_MMC.exists(path)) {
    Serial.println("[ERROR] File not found: " + path);
    return;
  }
  if (SD_MMC.remove(path)) {
    Serial.println("[OK] Deleted: " + path);
  } else {
    Serial.println("[ERROR] Failed to delete: " + path);
  }
}