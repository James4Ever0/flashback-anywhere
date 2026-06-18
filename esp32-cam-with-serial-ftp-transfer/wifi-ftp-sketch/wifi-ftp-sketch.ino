#include "esp_camera.h"
#include "Arduino.h"
#include "FS.h"
#include "SD_MMC.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/rtc_io.h"
#include <WiFi.h>

// SimpleFTPServer library: edit FtpServerKey.h to set
// DEFAULT_STORAGE_TYPE_ESP32 = STORAGE_SD_MMC (done)
#include <SimpleFTPServer.h>

// ========== Camera Pins (AI-Thinker ESP32-CAM) ==========

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

// ========== Mode Switch Pins ==========

// we do not know these pins
#define CHARGING_PIN      -1
#define BUTTON_PIN        -1

#define FLASH_LED_PIN     33

// ========== Runtime State ==========

enum DeviceMode {
  MODE_PHOTO = 0,
  MODE_WIFI_FTP = 1
};

#define DEFAULT_MODE           MODE_PHOTO
#define DEFAULT_INTERVAL_SEC   60
#define BOOT_SERIAL_TIMEOUT_MS 2000
#define DEFAULT_FTP_PORT       21

// ========== RTC-Persisted State ==========

RTC_DATA_ATTR uint8_t rtcMode = DEFAULT_MODE;
RTC_DATA_ATTR uint32_t rtcPictureNumber = 0;
RTC_DATA_ATTR uint32_t rtcIntervalSec = DEFAULT_INTERVAL_SEC;

// ========== Runtime Variables ==========

DeviceMode currentMode;
uint32_t pictureNumber;
uint32_t intervalSec;

bool sdReady = false;
bool cameraReady = false;
bool wifiConnected = false;
bool ftpRunning = false;

String inputBuffer = "";
bool commandReady = false;

// WiFi/FTP configuration
String wifiSsid = "";
String wifiPass = "";
uint16_t ftpPort = DEFAULT_FTP_PORT;
String ftpUser = "esp32cam";
String ftpPass = "esp32cam";

// FTP server instance
FtpServer ftpSrv;

// ========== Forward Declarations ==========

bool initCamera();
bool initSD();
bool readPhotoConfigFromSD();
bool savePhotoConfigToSD();
bool readWiFiFtpConfigFromSD();
bool saveWiFiFtpConfigToSD();
void checkModeTriggersAtBoot();
void enterPhotoMode();
void enterWiFiFtpMode();
void takePhotoAndSleep();
void wifiFtpLoop();
void processCommand(String cmd);
void printPrompt();
void cmdWiFiSsid(String args);
void cmdWiFiPass(String args);
void cmdWiFiStart();
void cmdWiFiStop();
void cmdFtpPort(String args);
void cmdFtpUser(String args);
void cmdFtpPass(String args);
void cmdFtpStart();
void cmdFtpStop();
void cmdFtpStatus();
void cmdModePhoto();
void cmdHelp();

// ========== Setup ==========

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(100);

  // pinMode(CHARGING_PIN, INPUT);
  // pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  // Clear any persistent WiFi state from previous sketches.
  // ESP32 stores WiFi config in NVS flash; old AP settings can
  // override new ones unless explicitly erased.
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  currentMode = (rtcMode == MODE_WIFI_FTP) ? MODE_WIFI_FTP : MODE_PHOTO;
  pictureNumber = rtcPictureNumber;
  intervalSec = rtcIntervalSec;

  Serial.printf("\n[BOOT] Wakeup cause: %d\n", esp_sleep_get_wakeup_cause());
  Serial.printf("[BOOT] Persisted mode: %s\n", (currentMode == MODE_WIFI_FTP) ? "wifi_ftp" : "photo");
  Serial.printf("[BOOT] Picture number: %lu\n", pictureNumber);
  Serial.printf("[BOOT] Interval: %lu sec\n", intervalSec);

  if (!initSD()) {
    Serial.println("[FATAL] SD Card init failed. Halting.");
    return;
  }

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
    readPhotoConfigFromSD();
  }
  // Always load WiFi/FTP config (strings are not RTC-persisted)
  readWiFiFtpConfigFromSD();

  checkModeTriggersAtBoot();

  if (!initCamera()) {
    Serial.println("[FATAL] Camera init failed. Halting.");
    return;
  }

  if (currentMode == MODE_WIFI_FTP) {
    enterWiFiFtpMode();
  } else {
    enterPhotoMode();
  }
}

void loop() {
  if (currentMode == MODE_WIFI_FTP) {
    wifiFtpLoop();
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

// ========== Photo Config File ==========

bool readPhotoConfigFromSD() {
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
      if (val.equalsIgnoreCase("wifi_ftp")) {
        currentMode = MODE_WIFI_FTP;
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

  rtcMode = currentMode;
  rtcIntervalSec = intervalSec;
  rtcPictureNumber = pictureNumber;

  Serial.println("[INFO] Photo config loaded from /config.txt");
  return true;
}

bool savePhotoConfigToSD() {
  File f = SD_MMC.open("/config.txt", FILE_WRITE);
  if (!f) {
    Serial.println("[ERROR] Failed to open /config.txt for writing.");
    return false;
  }
  f.println("# ESP32-CAM Photo Config");
  f.printf("mode=%s\n", (currentMode == MODE_WIFI_FTP) ? "wifi_ftp" : "photo");
  f.printf("interval=%lu\n", intervalSec);
  f.printf("pictureNumber=%lu\n", pictureNumber);
  f.close();
  return true;
}

// ========== WiFi/FTP Config File ==========

bool readWiFiFtpConfigFromSD() {
  File f = SD_MMC.open("/wifi_ftp_config.txt", FILE_READ);
  if (!f) {
    Serial.println("[INFO] No /wifi_ftp_config.txt found, using defaults.");
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

    if (key.equalsIgnoreCase("wifiSsid")) {
      wifiSsid = val;
    } else if (key.equalsIgnoreCase("wifiPass")) {
      wifiPass = val;
    } else if (key.equalsIgnoreCase("ftpPort")) {
      ftpPort = val.toInt();
      if (ftpPort == 0) ftpPort = DEFAULT_FTP_PORT;
    } else if (key.equalsIgnoreCase("ftpUser")) {
      ftpUser = val;
    } else if (key.equalsIgnoreCase("ftpPass")) {
      ftpPass = val;
    }
  }
  f.close();

  Serial.println("[INFO] WiFi/FTP config loaded from /wifi_ftp_config.txt");
  return true;
}

bool saveWiFiFtpConfigToSD() {
  File f = SD_MMC.open("/wifi_ftp_config.txt", FILE_WRITE);
  if (!f) {
    Serial.println("[ERROR] Failed to open /wifi_ftp_config.txt for writing.");
    return false;
  }
  f.println("# ESP32-CAM WiFi/FTP Config");
  f.printf("wifiSsid=%s\n", wifiSsid.c_str());
  f.printf("wifiPass=%s\n", wifiPass.c_str());
  f.printf("ftpPort=%u\n", ftpPort);
  f.printf("ftpUser=%s\n", ftpUser.c_str());
  f.printf("ftpPass=%s\n", ftpPass.c_str());
  f.close();
  return true;
}

// ========== Mode Switch Triggers ==========

void checkModeTriggersAtBoot() {
  // if (digitalRead(CHARGING_PIN) == HIGH) {
  //   Serial.println("[TRIGGER] Charging detected -> WIFI_FTP mode");
  //   currentMode = MODE_WIFI_FTP;
  //   rtcMode = MODE_WIFI_FTP;
  //   return;
  // }

  // if (digitalRead(BUTTON_PIN) == LOW) {
  //   Serial.println("[TRIGGER] Button pressed at boot -> toggling mode");
  //   currentMode = (currentMode == MODE_PHOTO) ? MODE_WIFI_FTP : MODE_PHOTO;
  //   rtcMode = currentMode;
  //   while (digitalRead(BUTTON_PIN) == LOW) { delay(10); }
  //   delay(100);
  //   return;
  // }

  Serial.println("[BOOT] Waiting for serial trigger (2s)... Send 'MODE WIFI_FTP' to switch.");
  unsigned long start = millis();
  while (millis() - start < BOOT_SERIAL_TIMEOUT_MS) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        inputBuffer.trim();
        if (inputBuffer.length() > 0) {
          String cmd = inputBuffer;
          inputBuffer = "";
          if (cmd.equalsIgnoreCase("MODE WIFI_FTP")) {
            Serial.println("[TRIGGER] Serial command received -> WIFI_FTP mode");
            currentMode = MODE_WIFI_FTP;
            rtcMode = MODE_WIFI_FTP;
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

  if (ftpRunning) {
    cmdFtpStop();
  }
  if (wifiConnected) {
    cmdWiFiStop();
  }

  takePhotoAndSleep();
}

void takePhotoAndSleep() {
  for (int i = 0; i < 10; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(50);
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[ERROR] Camera capture failed.");
    esp_sleep_enable_timer_wakeup(5000000);
    esp_deep_sleep_start();
    return;
  }

  digitalWrite(FLASH_LED_PIN, HIGH);
  rtc_gpio_hold_en(GPIO_NUM_33);

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

    rtcPictureNumber = pictureNumber;
    savePhotoConfigToSD();
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

// ========== WiFi/FTP Mode ==========

void enterWiFiFtpMode() {
  Serial.println("\n========== ENTERING WIFI_FTP MODE ==========");
  currentMode = MODE_WIFI_FTP;
  rtcMode = MODE_WIFI_FTP;

  Serial.println("Commands: WIFI_SSID <ssid> | WIFI_PASS <pass> | WIFI_START | WIFI_STOP");
  Serial.println("          FTP_PORT <port> | FTP_USER <user> | FTP_PASS <pass>");
  Serial.println("          FTP_START | FTP_STOP | FTP_STATUS | MODE PHOTO | HELP");
  Serial.println("[INFO] WiFi runs as Access Point. Connect your client to the ESP network.");
  Serial.println("[INFO] IMPORTANT: In your FTP client, type 'binary' before transferring files!");

  // Auto-start AP if SSID is already configured
  if (wifiSsid.length() > 0) {
    Serial.println("[INFO] Auto-starting Access Point with saved credentials...");
    cmdWiFiStart();
  } else {
    Serial.println("[INFO] No AP credentials saved. Use WIFI_SSID and WIFI_PASS to configure.");
  }

  printPrompt();
}

void wifiFtpLoop() {
  // if (digitalRead(BUTTON_PIN) == LOW) {
  //   Serial.println("\n[TRIGGER] Button pressed -> switching to PHOTO mode");
  //   while (digitalRead(BUTTON_PIN) == LOW) { delay(10); }
  //   delay(100);
  //   enterPhotoMode();
  //   return;
  // }

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

  if (ftpRunning) {
    ftpSrv.handleFTP();
  }
}

void printPrompt() {
  Serial.print("> ");
  Serial.flush();
}

// ========== Command Processing ==========

void processCommand(String cmd) {
  int firstSpace = cmd.indexOf(' ');
  String command = (firstSpace == -1) ? cmd : cmd.substring(0, firstSpace);
  String args = (firstSpace == -1) ? "" : cmd.substring(firstSpace + 1);
  command.toLowerCase();
  args.trim();

  if (command == "wifi_ssid") {
    cmdWiFiSsid(args);
  } else if (command == "wifi_pass") {
    cmdWiFiPass(args);
  } else if (command == "wifi_start") {
    cmdWiFiStart();
  } else if (command == "wifi_stop") {
    cmdWiFiStop();
  } else if (command == "ftp_port") {
    cmdFtpPort(args);
  } else if (command == "ftp_user") {
    cmdFtpUser(args);
  } else if (command == "ftp_pass") {
    cmdFtpPass(args);
  } else if (command == "ftp_start") {
    cmdFtpStart();
  } else if (command == "ftp_stop") {
    cmdFtpStop();
  } else if (command == "ftp_status") {
    cmdFtpStatus();
  } else if (command == "mode") {
    args.toLowerCase();
    if (args == "photo") {
      cmdModePhoto();
    } else if (args == "wifi_ftp") {
      Serial.println("[OK] Already in WIFI_FTP mode.");
    } else {
      Serial.println("[ERROR] Usage: MODE PHOTO | MODE WIFI_FTP");
    }
  } else if (command == "help") {
    cmdHelp();
  } else {
    Serial.println("[ERROR] Unknown command. Type HELP for available commands.");
  }
}

// ========== WiFi Commands ==========

void cmdWiFiSsid(String args) {
  if (args.length() == 0) {
    Serial.println("[ERROR] Usage: WIFI_SSID <ssid>");
    return;
  }
  wifiSsid = args;
  saveWiFiFtpConfigToSD();
  Serial.println("[OK] WiFi SSID set.");
}

void cmdWiFiPass(String args) {
  if (args.length() == 0) {
    Serial.println("[ERROR] Usage: WIFI_PASS <password>");
    Serial.println("[INFO] Use WIFI_PASS with no args to create an open network (no password).");
    return;
  }
  // WPA2-PSK requires 8-63 ASCII characters. Shorter passwords are rejected
  // by the ESP32 WiFi driver and the AP will be created without encryption.
  if (args.length() > 0 && args.length() < 8) {
    Serial.println("[ERROR] WPA2 password must be at least 8 characters.");
    Serial.println("[INFO]  Use an empty password (WIFI_PASS with no args) for an open network.");
    return;
  }
  if (args.length() > 63) {
    Serial.println("[ERROR] WPA2 password must be at most 63 characters.");
    return;
  }
  wifiPass = args;
  saveWiFiFtpConfigToSD();
  Serial.println("[OK] WiFi password set.");
}

void cmdWiFiStart() {
  if (wifiSsid.length() == 0) {
    Serial.println("[ERROR] WiFi SSID not set. Use WIFI_SSID first.");
    return;
  }

  // Validate password before starting
  if (wifiPass.length() > 0 && wifiPass.length() < 8) {
    Serial.println("[ERROR] Password too short for WPA2 (need 8+ chars).");
    Serial.println("[INFO]  Set a longer password with WIFI_PASS or clear it for an open network.");
    return;
  }

  const char *pass = wifiPass.length() > 0 ? wifiPass.c_str() : NULL;

  Serial.printf("[INFO] Starting Access Point '%s' ...\n", wifiSsid.c_str());
  WiFi.softAP(wifiSsid.c_str(), pass);

  wifiConnected = true;
  Serial.print("[OK] Access Point started. IP: ");
  Serial.println(WiFi.softAPIP());
  if (wifiPass.length() == 0) {
    Serial.println("[WARN] No password set -- AP is OPEN.");
  }
}

void cmdWiFiStop() {
  if (ftpRunning) {
    cmdFtpStop();
  }
  WiFi.softAPdisconnect(true);
  wifiConnected = false;
  Serial.println("[OK] Access Point stopped.");
}

// ========== FTP Commands ==========

void cmdFtpPort(String args) {
  if (args.length() == 0) {
    Serial.println("[ERROR] Usage: FTP_PORT <port>");
    return;
  }
  int p = args.toInt();
  if (p <= 0 || p > 65535) {
    Serial.println("[ERROR] Invalid port number.");
    return;
  }
  ftpPort = p;
  saveWiFiFtpConfigToSD();
  Serial.printf("[OK] FTP port set to %u.\n", ftpPort);
}

void cmdFtpUser(String args) {
  if (args.length() == 0) {
    Serial.println("[ERROR] Usage: FTP_USER <username>");
    return;
  }
  ftpUser = args;
  saveWiFiFtpConfigToSD();
  if (ftpRunning) {
    Serial.println("[WARN] FTP server is running. Stop and restart it to use new username.");
  } else {
    Serial.println("[OK] FTP username set.");
  }
}

void cmdFtpPass(String args) {
  if (args.length() == 0) {
    Serial.println("[ERROR] Usage: FTP_PASS <password>");
    return;
  }
  ftpPass = args;
  saveWiFiFtpConfigToSD();
  if (ftpRunning) {
    Serial.println("[WARN] FTP server is running. Stop and restart it to use new password.");
  } else {
    Serial.println("[OK] FTP password set.");
  }
}

void cmdFtpStart() {
  if (!sdReady) {
    Serial.println("[ERROR] SD card not mounted. Cannot start FTP server.");
    return;
  }
  if (!wifiConnected) {
    Serial.println("[ERROR] WiFi not connected. Use WIFI_START first.");
    return;
  }
  if (ftpRunning) {
    Serial.println("[OK] FTP server already running.");
    return;
  }

  // SimpleFTPServer begin: username, password, welcome message
  // Filesystem is selected at compile time via STORAGE_TYPE macro (SD_MMC)
  ftpSrv.begin(ftpUser.c_str(), ftpPass.c_str());

  ftpRunning = true;
  Serial.println("[OK] FTP server started.");
  Serial.printf("       Address: %s:%u\n", WiFi.softAPIP().toString().c_str(), ftpPort);
  Serial.printf("       User:    %s\n", ftpUser.c_str());
  Serial.printf("       Pass:    %s\n", ftpPass.c_str());
}

void cmdFtpStop() {
  if (!ftpRunning) {
    Serial.println("[OK] FTP server already stopped.");
    return;
  }

  // SimpleFTPServer has no explicit stop() method in most versions.
  // We mark it stopped and it will no longer call handleFTP().
  ftpRunning = false;
  Serial.println("[OK] FTP server stopped.");
}

void cmdFtpStatus() {
  Serial.println("========== Status ==========");
  Serial.printf("AP SSID:     %s\n", wifiSsid.c_str());
  Serial.printf("AP State:    %s\n", wifiConnected ? "running" : "stopped");
  if (wifiConnected) {
    Serial.printf("AP IP:       %s\n", WiFi.softAPIP().toString().c_str());
  }
  Serial.printf("FTP Port:    %u\n", ftpPort);
  Serial.printf("FTP User:    %s\n", ftpUser.c_str());
  Serial.printf("FTP State:   %s\n", ftpRunning ? "running" : "stopped");
  Serial.printf("SD State:    %s\n", sdReady ? "mounted" : "not mounted");
  Serial.println("============================");
}

void cmdModePhoto() {
  Serial.println("[OK] Switching to PHOTO mode...");
  enterPhotoMode();
}

void cmdHelp() {
  Serial.println("Available commands:");
  Serial.println("  WIFI_SSID <ssid>        - Set Access Point SSID");
  Serial.println("  WIFI_PASS <password>    - Set Access Point password (8-63 chars, or empty for open)");
  Serial.println("  WIFI_START              - Start Access Point");
  Serial.println("  WIFI_STOP               - Stop Access Point");
  Serial.println("  FTP_PORT <port>         - Set FTP server port");
  Serial.println("  FTP_USER <username>     - Set FTP username (set BEFORE FTP_START)");
  Serial.println("  FTP_PASS <password>     - Set FTP password (set BEFORE FTP_START)");
  Serial.println("  FTP_START               - Start FTP server (needs SD + AP running)");
  Serial.println("  FTP_STOP                - Stop FTP server");
  Serial.println("  FTP_STATUS              - Show AP/FTP status");
  Serial.println("  MODE PHOTO              - Return to photo mode (deep sleep)");
  Serial.println("  HELP                    - Show this message");
  Serial.println("");
  Serial.println("FTP CLIENT TIP: Always type 'binary' before transferring files.");
  Serial.println("                 The default 'ascii' mode corrupts JPEG images.");
}
