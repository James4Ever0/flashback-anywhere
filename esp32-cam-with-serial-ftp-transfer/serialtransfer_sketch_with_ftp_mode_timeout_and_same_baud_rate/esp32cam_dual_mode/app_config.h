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

#define BAUD_RATE               921600
#define BOOT_SERIAL_TIMEOUT_MS  3000
#define TRANSFER_INACTIVITY_MS  30000   // auto-switch back to PHOTO after 30s

// ========== Device Mode ==========

enum DeviceMode {
  MODE_PHOTO = 0,
  MODE_TRANSFER = 1
};

// ========== Config Struct ==========

struct Config {
  uint32_t   intervalSec;
  uint8_t    jpegQuality;
  uint16_t   frameSize;
  uint32_t   pictureNumber;
  DeviceMode mode;
};

extern Config g_config;

// ========== Functions ==========

bool loadConfig(fs::FS& fs, Config& outConfig);
bool writeConfig(fs::FS& fs, const Config& config);

String   frameSizeToString(uint16_t fs);
uint16_t stringToFrameSize(const String& s);

String   deviceModeToString(DeviceMode m);
DeviceMode stringToDeviceMode(const String& s);
