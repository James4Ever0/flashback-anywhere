#include "bt_pin.h"
#include "FS.h"
#include "SD_MMC.h"
#include "sd_lock.h"

#define PIN_FILE_PATH  "/btpin.txt"

String btPinRead(void) {
  sdLockTake();
  File f = SD_MMC.open(PIN_FILE_PATH, FILE_READ);
  if (!f) {
    sdLockGive();
    return String("");
  }

  String pin = f.readStringUntil('\n');
  f.close();
  sdLockGive();

  pin.trim();
  return pin;
}

bool btPinWrite(const char* pin) {
  if (!pin || strlen(pin) == 0) {
    return false;
  }

  sdLockTake();
  File f = SD_MMC.open(PIN_FILE_PATH, FILE_WRITE);
  if (!f) {
    sdLockGive();
    return false;
  }

  f.println(pin);
  f.close();
  sdLockGive();
  return true;
}
