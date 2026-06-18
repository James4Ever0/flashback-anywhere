#include "pins.h"
#include "app_config.h"
#include "camera_manager.h"
#include "storage_manager.h"
#include "photo_pipeline.h"

#include <SD_MMC.h>
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>

// Global config instance
Config g_config;

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(100);

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  // --- Initialize SD (external to the photo pipeline) ---
  if (!initSD()) {
    Serial.println("[FATAL] SD init failed. Halting.");
    return;
  }

  // --- Load config using the initialized SD filesystem ---
  loadConfig(SD_MMC, g_config);

  // --- Initialize camera ---
  if (!initCamera(g_config)) {
    Serial.println("[FATAL] Camera init failed. Halting.");
    return;
  }

  // --- Run one photo cycle, injecting the SD filesystem ---
  runPhotoCycle(SD_MMC, g_config);
}

void loop() {
  // Intentionally empty; deep sleep restarts from setup() each cycle.
}
