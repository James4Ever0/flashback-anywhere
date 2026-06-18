#include "storage_manager.h"
#include <SD_MMC.h>

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

bool writeFile(fs::FS& fs, const char* path, const uint8_t* data, size_t len) {
  File file = fs.open(path, FILE_WRITE);
  if (!file) {
    Serial.printf("[ERROR] Failed to open %s for writing.\n", path);
    return false;
  }

  size_t written = file.write(data, len);
  file.close();

  if (written != len) {
    Serial.printf("[ERROR] Partial write to %s (%u/%u bytes).\n", path, written, len);
    return false;
  }

  Serial.printf("[INFO] Saved: %s (%u bytes)\n", path, len);
  return true;
}

bool fileExists(fs::FS& fs, const char* path) {
  return fs.exists(path);
}
