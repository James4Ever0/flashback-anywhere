#ifndef SD_LOCK_H
#define SD_LOCK_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Initialize the SD card mutex. Call once in setup().
void sdLockInit(void);

// Acquire the SD mutex. Blocks until available.
void sdLockTake(void);

// Release the SD mutex.
void sdLockGive(void);

#endif
