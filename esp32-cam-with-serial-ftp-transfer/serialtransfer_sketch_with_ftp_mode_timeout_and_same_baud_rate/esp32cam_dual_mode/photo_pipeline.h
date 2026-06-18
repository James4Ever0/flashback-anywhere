#pragma once

#include <Arduino.h>
#include <FS.h>
#include <esp_camera.h>
#include "app_config.h"

/**
 * Run one complete photo cycle:
 *   1. Turn on flash
 *   2. Capture image (warm-up + frame grab)
 *   3. Store image to the given filesystem
 *   4. Persist updated config (picture number)
 *   5. Turn off flash
 *
 * The filesystem reference is injected so the pipeline is not
 * hard-wired to SD_MMC.
 * Returns false if capture failed.
 */
bool runPhotoCycle(fs::FS& storage, Config& config);

// ---------- Decomposed step functions ----------

/**
 * Function A: capture image from camera.
 * Caller must release the returned buffer with esp_camera_fb_return().
 */
bool captureImage(camera_fb_t** outFb);

/**
 * Function B: store image buffer to filesystem.
 */
bool storeImage(fs::FS& fs, const String& path, camera_fb_t* fb);

/**
 * Function C: persist config with updated picture number to filesystem.
 */
bool persistConfig(fs::FS& fs, Config& config);

/**
 * Function D: control flash LED.
 */
void setFlash(bool on);
