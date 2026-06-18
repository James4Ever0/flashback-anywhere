#ifndef CAMERA_CONFIG_H
#define CAMERA_CONFIG_H

#include <Arduino.h>
#include <FS.h>
#include <esp_camera.h>

// ========== Defaults ==========

#define DEFAULT_INTERVAL_SEC    30
#define DEFAULT_JPEG_QUALITY    10
#define DEFAULT_FRAME_SIZE      FRAMESIZE_SXGA
#define DEFAULT_PICTURE_NUMBER  0
#define CAM_CONFIG_PATH         "/cam_config.txt"

// ========== Config Struct ==========

struct CamConfig {
  uint32_t intervalSec;
  uint8_t  jpegQuality;
  uint16_t frameSize;
  uint32_t pictureNumber;
};

// ========== Functions ==========

bool loadCamConfig(fs::FS& fs, CamConfig& outConfig);
bool writeCamConfig(fs::FS& fs, const CamConfig& config);

String   frameSizeToString(uint16_t fs);
uint16_t stringToFrameSize(const String& s);

#endif
