#ifndef CAMERA_CAPTURE_H
#define CAMERA_CAPTURE_H

#include <esp_camera.h>
#include "camera_config.h"

/**
 * Initialize the OV2640 camera with settings from the given CamConfig.
 * Returns true on success.
 */
bool initCamera(const CamConfig& config);

/**
 * Warm-up the camera and capture a single frame buffer.
 * Caller MUST call releaseImageBuffer() when done.
 *
 * @param outFb           Receives pointer to the frame buffer.
 * @param warmUpFrames    Number of discard frames before capture.
 * @param warmUpDelayMs   Delay between warm-up frames.
 * @return true if a frame was successfully captured.
 */
bool captureImageBuffer(camera_fb_t** outFb, int warmUpFrames = 3, int warmUpDelayMs = 100);

/**
 * Release a frame buffer previously obtained from captureImageBuffer().
 */
void releaseImageBuffer(camera_fb_t* fb);

#endif
