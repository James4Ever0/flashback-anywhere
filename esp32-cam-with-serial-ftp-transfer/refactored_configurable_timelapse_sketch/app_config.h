#pragma once

#include <Arduino.h>
#include <FS.h>
#include <esp_camera.h>

// ========== Defaults ==========

#define DEFAULT_INTERVAL_SEC    60
#define DEFAULT_JPEG_QUALITY    10
#define DEFAULT_FRAME_SIZE      FRAMESIZE_SXGA
#define DEFAULT_PICTURE_NUMBER  0
#define CONFIG_PATH             "/config.txt"

// ========== Config Struct ==========

struct Config {
  uint32_t intervalSec;
  uint8_t  jpegQuality;
  uint16_t frameSize;
  uint32_t pictureNumber;
};

extern Config g_config;

// ========== Functions ==========

bool loadConfig(fs::FS& fs, Config& outConfig);
bool writeConfig(fs::FS& fs, const Config& config);

String   frameSizeToString(uint16_t fs);
uint16_t stringToFrameSize(const String& s);
