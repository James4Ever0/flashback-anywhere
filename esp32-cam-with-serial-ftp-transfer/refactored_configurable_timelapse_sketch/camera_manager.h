#pragma once

#include <esp_camera.h>
#include "app_config.h"

/**
 * Initialize the OV2640 camera with settings from the given Config.
 * Returns true on success.
 */
bool initCamera(const Config& config);

/**
 * Warm-up the camera and capture a single frame buffer.
 * Caller MUST call releaseImageBuffer() when done.
 *
 * @param outFb           Receives pointer to the frame buffer.
 * @param warmUpFrames    Number of discard frames before capture.
 * @param warmUpDelayMs   Delay between warm-up frames.
 * @return true if a frame was successfully captured.
 */
bool captureImageBuffer(camera_fb_t** outFb, int warmUpFrames = 10, int warmUpDelayMs = 50);

/**
 * Release a frame buffer previously obtained from captureImageBuffer().
 */
void releaseImageBuffer(camera_fb_t* fb);
