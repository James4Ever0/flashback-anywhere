#ifndef BT_PIN_H
#define BT_PIN_H

#include <Arduino.h>

// Read PIN from /btpin.txt on SD card.
// Returns the PIN string, or empty string if file missing / unreadable.
String btPinRead(void);

// Write new PIN to /btpin.txt on SD card.
// Returns true on success.
bool btPinWrite(const char* pin);

#endif
