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

// ========== Defaults ==========

#define DEFAULT_INTERVAL_SEC    60
#define DEFAULT_JPEG_QUALITY    10
#define DEFAULT_FRAME_SIZE      FRAMESIZE_SXGA
#define DEFAULT_PICTURE_NUMBER  0
#define CONFIG_PATH             "/config.txt"

// ========== Config Struct ==========

struct Config {
  uint32_t intervalSec;
  uint8_t jpegQuality;
  uint16_t frameSize;
  uint32_t pictureNumber;
};

Config config;

// ========== Forward Declarations ==========

bool initCamera();
bool initSD();
bool loadConfig();
bool writeDefaultConfig();
bool parseConfigLine(const String& line);
String frameSizeToString(uint16_t fs);
uint16_t stringToFrameSize(const String& s);
void takePhotoAndSleep();

// ========== Setup ==========

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(100);

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  if (!initSD()) {
    Serial.println("[FATAL] SD init failed. Halting.");
    return;
  }

  loadConfig();

  if (!initCamera()) {
    Serial.println("[FATAL] Camera init failed. Halting.");
    return;
  }

  takePhotoAndSleep();
}

void loop() {
}

// ========== Initialization ==========

bool initCamera() {
  camera_config_t camConfig;
  camConfig.ledc_channel = LEDC_CHANNEL_0;
  camConfig.ledc_timer = LEDC_TIMER_0;
  camConfig.pin_d0 = Y2_GPIO_NUM;
  camConfig.pin_d1 = Y3_GPIO_NUM;
  camConfig.pin_d2 = Y4_GPIO_NUM;
  camConfig.pin_d3 = Y5_GPIO_NUM;
  camConfig.pin_d4 = Y6_GPIO_NUM;
  camConfig.pin_d5 = Y7_GPIO_NUM;
  camConfig.pin_d6 = Y8_GPIO_NUM;
  camConfig.pin_d7 = Y9_GPIO_NUM;
  camConfig.pin_xclk = XCLK_GPIO_NUM;
  camConfig.pin_pclk = PCLK_GPIO_NUM;
  camConfig.pin_vsync = VSYNC_GPIO_NUM;
  camConfig.pin_href = HREF_GPIO_NUM;
  camConfig.pin_sscb_sda = SIOD_GPIO_NUM;
  camConfig.pin_sscb_scl = SIOC_GPIO_NUM;
  camConfig.pin_pwdn = PWDN_GPIO_NUM;
  camConfig.pin_reset = RESET_GPIO_NUM;
  camConfig.xclk_freq_hz = 10000000;
  camConfig.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    camConfig.frame_size = (framesize_t)config.frameSize;
    camConfig.jpeg_quality = config.jpegQuality;
    camConfig.fb_count = 2;
  } else {
    camConfig.frame_size = FRAMESIZE_SVGA;
    camConfig.jpeg_quality = 12;
    camConfig.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&camConfig);
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

  Serial.println("[INFO] Camera initialized.");
  return true;
}

bool initSD() {
  if (!SD_MMC.begin("/sdcard", true)) {
    if (!SD_MMC.begin()) {
      Serial.println("[ERROR] SD Card mount failed.");
      return false;
    }
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[ERROR] No SD card detected.");
    return false;
  }

  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  Serial.printf("[INFO] SD Card size: %llu MB\n", cardSize);
  return true;
}

// ========== Config File ==========

bool loadConfig() {
  // Set defaults first
  config.intervalSec = DEFAULT_INTERVAL_SEC;
  config.jpegQuality = DEFAULT_JPEG_QUALITY;
  config.frameSize = DEFAULT_FRAME_SIZE;
  config.pictureNumber = DEFAULT_PICTURE_NUMBER;

  File f = SD_MMC.open(CONFIG_PATH, FILE_READ);
  if (!f) {
    Serial.println("[INFO] Config not found. Creating default.");
    writeDefaultConfig();
    return true;
  }

  bool anyParsed = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;
    if (parseConfigLine(line)) anyParsed = true;
  }
  f.close();

  if (!anyParsed) {
    Serial.println("[WARN] Config file empty/invalid. Using defaults.");
  } else {
    Serial.println("[INFO] Config loaded from SD.");
  }

  Serial.printf("[CONFIG] interval=%lu, quality=%u, frameSize=%s, pictureNumber=%lu\n",
                config.intervalSec, config.jpegQuality,
                frameSizeToString(config.frameSize).c_str(), config.pictureNumber);
  return true;
}

bool writeDefaultConfig() {
  File f = SD_MMC.open(CONFIG_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[ERROR] Failed to write default config.");
    return false;
  }

  f.println("# ESP32-CAM Timelapse Config");
  f.println("# interval: seconds between captures");
  f.println("# jpegQuality: 0-63 (lower = better quality, larger file)");
  f.println("# frameSize: QVGA, CIF, VGA, SVGA, XGA, SXGA, UXGA");
  f.println("# pictureNumber: starting counter for filenames");
  f.println();
  f.printf("interval=%lu\n", DEFAULT_INTERVAL_SEC);
  f.printf("jpegQuality=%u\n", DEFAULT_JPEG_QUALITY);
  f.printf("frameSize=%s\n", frameSizeToString(DEFAULT_FRAME_SIZE).c_str());
  f.printf("pictureNumber=%lu\n", DEFAULT_PICTURE_NUMBER);
  f.close();

  Serial.println("[INFO] Default config written to " CONFIG_PATH);
  return true;
}

bool parseConfigLine(const String& line) {
  int eq = line.indexOf('=');
  if (eq == -1) return false;

  String key = line.substring(0, eq);
  String val = line.substring(eq + 1);
  key.trim();
  val.trim();

  if (key.equalsIgnoreCase("interval")) {
    uint32_t v = val.toInt();
    config.intervalSec = (v > 0) ? v : DEFAULT_INTERVAL_SEC;
  } else if (key.equalsIgnoreCase("jpegQuality")) {
    int v = val.toInt();
    config.jpegQuality = (v >= 0 && v <= 63) ? v : DEFAULT_JPEG_QUALITY;
  } else if (key.equalsIgnoreCase("frameSize")) {
    uint16_t fs = stringToFrameSize(val);
    config.frameSize = (fs != (uint16_t)-1) ? fs : DEFAULT_FRAME_SIZE;
  } else if (key.equalsIgnoreCase("pictureNumber")) {
    config.pictureNumber = val.toInt();
  } else {
    return false;
  }
  return true;
}

String frameSizeToString(uint16_t fs) {
  switch (fs) {
    case FRAMESIZE_QVGA: return "QVGA";
    case FRAMESIZE_CIF:  return "CIF";
    case FRAMESIZE_VGA:  return "VGA";
    case FRAMESIZE_SVGA: return "SVGA";
    case FRAMESIZE_XGA:  return "XGA";
    case FRAMESIZE_SXGA: return "SXGA";
    case FRAMESIZE_UXGA: return "UXGA";
    default:             return "SXGA";
  }
}

uint16_t stringToFrameSize(const String& s) {
  if (s.equalsIgnoreCase("QVGA")) return FRAMESIZE_QVGA;
  if (s.equalsIgnoreCase("CIF"))  return FRAMESIZE_CIF;
  if (s.equalsIgnoreCase("VGA"))  return FRAMESIZE_VGA;
  if (s.equalsIgnoreCase("SVGA")) return FRAMESIZE_SVGA;
  if (s.equalsIgnoreCase("XGA"))  return FRAMESIZE_XGA;
  if (s.equalsIgnoreCase("SXGA")) return FRAMESIZE_SXGA;
  if (s.equalsIgnoreCase("UXGA")) return FRAMESIZE_UXGA;
  return (uint16_t)-1;
}

// ========== Photo Capture ==========

void takePhotoAndSleep() {
  // Warm-up frames
  for (int i = 0; i < 10; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(50);
  }

  // Capture
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[ERROR] Camera capture failed.");
    esp_sleep_enable_timer_wakeup(5000000);
    esp_deep_sleep_start();
    return;
  }

  // Flash on
  digitalWrite(FLASH_LED_PIN, HIGH);
  rtc_gpio_hold_en(GPIO_NUM_33);

  // Save
  config.pictureNumber++;
  String path = "/picture" + String(config.pictureNumber) + ".jpg";

  fs::FS &fs = SD_MMC;
  File file = fs.open(path.c_str(), FILE_WRITE);
  if (!file) {
    Serial.println("[ERROR] Failed to open file for writing.");
  } else {
    file.write(fb->buf, fb->len);
    Serial.printf("[INFO] Saved: %s (%u bytes)\n", path.c_str(), fb->len);
    file.close();

    // Update config with new picture number
    File cf = SD_MMC.open(CONFIG_PATH, FILE_WRITE);
    if (cf) {
      cf.println("# ESP32-CAM Timelapse Config");
      cf.println("# interval: seconds between captures");
      cf.println("# jpegQuality: 0-63 (lower = better quality, larger file)");
      cf.println("# frameSize: QVGA, CIF, VGA, SVGA, XGA, SXGA, UXGA");
      cf.println("# pictureNumber: starting counter for filenames");
      cf.println();
      cf.printf("interval=%lu\n", config.intervalSec);
      cf.printf("jpegQuality=%u\n", config.jpegQuality);
      cf.printf("frameSize=%s\n", frameSizeToString(config.frameSize).c_str());
      cf.printf("pictureNumber=%lu\n", config.pictureNumber);
      cf.close();
    }
  }

  esp_camera_fb_return(fb);

  // Flash off
  digitalWrite(FLASH_LED_PIN, LOW);
  rtc_gpio_hold_dis(GPIO_NUM_33);

  Serial.printf("[INFO] Sleeping %lu seconds...\n", config.intervalSec);
  Serial.flush();

  esp_sleep_enable_timer_wakeup((uint64_t)config.intervalSec * 1000000ULL);
  esp_deep_sleep_start();
}
