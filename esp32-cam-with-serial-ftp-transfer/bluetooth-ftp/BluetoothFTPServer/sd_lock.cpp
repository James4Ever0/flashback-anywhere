#include "sd_lock.h"

static SemaphoreHandle_t sdMutex = NULL;

void sdLockInit(void) {
  if (sdMutex == NULL) {
    sdMutex = xSemaphoreCreateMutex();
  }
}

void sdLockTake(void) {
  if (sdMutex != NULL) {
    xSemaphoreTake(sdMutex, portMAX_DELAY);
  }
}

void sdLockGive(void) {
  if (sdMutex != NULL) {
    xSemaphoreGive(sdMutex);
  }
}
