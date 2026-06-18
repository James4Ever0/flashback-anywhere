#pragma once

#include <Arduino.h>
#include <FS.h>

/**
 * Initialize the SD_MMC card.
 * Returns true if the card is mounted and detected.
 */
bool initSD();

/**
 * Write raw data to a file on the given filesystem.
 *
 * @param fs    Filesystem reference (e.g. SD_MMC).
 * @param path  Absolute file path.
 * @param data  Pointer to data buffer.
 * @param len   Number of bytes to write.
 * @return true on success.
 */
bool writeFile(fs::FS& fs, const char* path, const uint8_t* data, size_t len);

/**
 * Check whether a file exists on the given filesystem.
 */
bool fileExists(fs::FS& fs, const char* path);
