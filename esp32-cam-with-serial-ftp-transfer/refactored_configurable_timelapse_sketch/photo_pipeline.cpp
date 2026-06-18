#include "photo_pipeline.h"
#include "pins.h"
#include "camera_manager.h"
#include "storage_manager.h"
#include "app_config.h"
#include <driver/rtc_io.h>

// ---------- Step functions ----------

bool captureImage(camera_fb_t** outFb) {
  return captureImageBuffer(outFb, /*warmUpFrames=*/10, /*warmUpDelayMs=*/50);
}

bool storeImage(fs::FS& fs, const String& path, camera_fb_t* fb) {
  if (!fb || !fb->buf || fb->len == 0) {
    Serial.println("[ERROR] storeImage: invalid frame buffer.");
    return false;
  }
  return writeFile(fs, path.c_str(), fb->buf, fb->len);
}

bool persistConfig(fs::FS& fs, Config& config) {
  return writeConfig(fs, config);
}

void setFlash(bool on) {
  digitalWrite(FLASH_LED_PIN, on ? HIGH : LOW);
  if (on) {
    rtc_gpio_hold_en(GPIO_NUM_33);
  } else {
    rtc_gpio_hold_dis(GPIO_NUM_33);
  }
}

void enterDeepSleep(uint32_t seconds) {
  Serial.printf("[INFO] Sleeping %lu seconds...\n", seconds);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

// ---------- Orchestration ----------

void runPhotoCycle(fs::FS& storage, Config& config) {
  // Turn flash on
  setFlash(true);

  // Capture image (Function A)
  camera_fb_t* fb = nullptr;
  if (!captureImage(&fb)) {
    Serial.println("[ERROR] Image capture failed. Retrying in 5s.");
    setFlash(false);
    enterDeepSleep(5);
    return;  // unreachable after deep sleep, but keeps compiler happy
  }

  // Increment counter and build filename
  config.pictureNumber++;
  String path = "/picture" + String(config.pictureNumber) + ".jpg";

  // Store image (Function B)
  storeImage(storage, path, fb);

  // Release camera frame buffer
  releaseImageBuffer(fb);
  fb = nullptr;

  // Persist updated config (Function C)
  persistConfig(storage, config);

  // Turn flash off (Function D)
  setFlash(false);

  // Enter deep sleep (Function E)
  enterDeepSleep(config.intervalSec);
}
