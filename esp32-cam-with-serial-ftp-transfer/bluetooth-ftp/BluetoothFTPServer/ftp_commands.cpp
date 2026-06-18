#include "ftp_commands.h"
#include "sd_lock.h"
#include "bt_pin.h"
#include "FS.h"
#include "SD_MMC.h"
#include <BluetoothSerial.h>

// ---------------------------------------------------------------------------
// External reference to the global BluetoothSerial instance
extern BluetoothSerial SerialBT;

// ---------------------------------------------------------------------------
// Chunk size for chunked transfers (keeps SD mutex hold time short)
#define CHUNK_SIZE 1024

// ASCII End-of-Transmission marker for READ
#define EOT 0x04

// ---------------------------------------------------------------------------
// Application-level authentication state
static char expectedPin[17] = {0};
static bool isAuthenticated = false;

void ftpSetExpectedPin(const char* pin) {
  if (pin) {
    strncpy(expectedPin, pin, sizeof(expectedPin) - 1);
    expectedPin[sizeof(expectedPin) - 1] = '\0';
  }
}

void ftpSetAuthenticated(bool state) {
  isAuthenticated = state;
}

// ---------------------------------------------------------------------------
// Check if the current command requires authentication.
// Returns true if access is allowed, false if blocked.
static bool checkAuth(const char* cmd) {
#if !FTP_AUTH_REQUIRED
  return true;  // auth disabled — allow everything
#endif

  // These commands are always allowed (even before AUTH)
  if (strcasecmp(cmd, "AUTH") == 0) return true;
  if (strcasecmp(cmd, "HELP") == 0) return true;
  if (strcasecmp(cmd, "BTINFO") == 0) return true;

  if (!isAuthenticated) {
    SerialBT.println("[ERROR] Not authenticated. Send AUTH <pin> first.");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Tokenize a line in-place: command token + remainder
static char* getToken(char* line, const char* delim) {
  return strtok(line, delim);
}

// ---------------------------------------------------------------------------
void ftpDispatch(const char* line) {
  if (!line || strlen(line) == 0) return;

  // Make a mutable copy for tokenization
  char buf[256];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* cmd = getToken(buf, " ");
  if (!cmd) return;

  // Auth gate (if FTP_AUTH_REQUIRED is true)
  if (!checkAuth(cmd)) {
    return;
  }

  // Case-insensitive dispatch
  if (strcasecmp(cmd, "AUTH") == 0) {
    char* pin = getToken(NULL, " ");
    if (pin) {
      cmdAuth(pin);
    } else {
      SerialBT.println("[ERROR] Usage: AUTH <pin>");
    }
  }
  else if (strcasecmp(cmd, "HELP") == 0) {
    cmdHelp();
  }
  else if (strcasecmp(cmd, "LIST") == 0) {
    char* path = getToken(NULL, "");
    if (path) {
      while (*path == ' ') path++;
    }
    cmdList(path ? path : "/");
  }
  else if (strcasecmp(cmd, "READ") == 0) {
    char* path = getToken(NULL, "");
    if (path) {
      while (*path == ' ') path++;
      cmdRead(path);
    } else {
      SerialBT.println("[ERROR] Usage: READ <path>");
    }
  }
  else if (strcasecmp(cmd, "STORE") == 0) {
    char* path = getToken(NULL, " ");
    char* sizeStr = getToken(NULL, " ");
    if (path && sizeStr) {
      size_t sz = atol(sizeStr);
      cmdStore(path, sz);
    } else {
      SerialBT.println("[ERROR] Usage: STORE <path> <size>");
    }
  }
  else if (strcasecmp(cmd, "DELETE") == 0) {
    char* path = getToken(NULL, "");
    if (path) {
      while (*path == ' ') path++;
      cmdDelete(path);
    } else {
      SerialBT.println("[ERROR] Usage: DELETE <path>");
    }
  }
  else if (strcasecmp(cmd, "SETPIN") == 0) {
    char* pin = getToken(NULL, " ");
    if (pin) {
      cmdSetPin(pin);
    } else {
      SerialBT.println("[ERROR] Usage: SETPIN <pin>");
    }
  }
  else if (strcasecmp(cmd, "BTINFO") == 0) {
    cmdBtInfo();
  }
  else {
    SerialBT.printf("[ERROR] Unknown command: %s\n", cmd);
  }
}

// ---------------------------------------------------------------------------
void cmdAuth(const char* pin) {
  if (!pin || strlen(pin) == 0) {
    SerialBT.println("[ERROR] Usage: AUTH <pin>");
    return;
  }

  if (strlen(expectedPin) == 0) {
    SerialBT.println("[ERROR] No PIN configured on device.");
    return;
  }

  if (strcmp(pin, expectedPin) == 0) {
    isAuthenticated = true;
    SerialBT.println("[OK] Authenticated.");
  } else {
    SerialBT.println("[ERROR] Invalid PIN.");
  }
}

// ---------------------------------------------------------------------------
void cmdHelp(void) {
  SerialBT.println("Available commands:");
#if FTP_AUTH_REQUIRED
  SerialBT.println("  AUTH   <pin>               - Authenticate before using FTP");
#endif
  SerialBT.println("  LIST   <path>              - List directory contents");
  SerialBT.println("  READ   <path>              - Read file and send binary");
  SerialBT.println("  STORE  <path> <size>       - Write file from binary data");
  SerialBT.println("  DELETE <path>              - Delete a file");
  SerialBT.println("  SETPIN <pin>               - Set new app-level PIN");
  SerialBT.println("  BTINFO                     - Show Bluetooth info");
  SerialBT.println("  HELP                       - Show this message");
}

// ---------------------------------------------------------------------------
void cmdList(const char* path) {
  sdLockTake();
  File root = SD_MMC.open(path);
  if (!root || !root.isDirectory()) {
    sdLockGive();
    SerialBT.printf("[ERROR] Failed to open directory: %s\n", path);
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      SerialBT.printf("[DIR]  %s\n", file.name());
    }
    else {
      SerialBT.printf("[FILE]  %8lu bytes  %s\n", (unsigned long)file.size(), file.name());
    }
    file.close();
    file = root.openNextFile();
  }
  root.close();
  sdLockGive();
}

// ---------------------------------------------------------------------------
void cmdRead(const char* path) {
  sdLockTake();
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) {
    sdLockGive();
    SerialBT.printf("[ERROR] Failed to open file: %s\n", path);
    return;
  }

  size_t fileSize = f.size();
  SerialBT.printf("SIZE: %lu\n", (unsigned long)fileSize);

  uint8_t chunk[CHUNK_SIZE];
  size_t remaining = fileSize;
  while (remaining > 0) {
    size_t toRead = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
    size_t readBytes = f.read(chunk, toRead);
    if (readBytes == 0) break;

    sdLockGive();
    SerialBT.write(chunk, readBytes);
    sdLockTake();

    remaining -= readBytes;
  }
  f.close();
  sdLockGive();

  SerialBT.write(EOT);
}

// ---------------------------------------------------------------------------
void cmdStore(const char* path, size_t size) {
  Serial.printf("[STORE] Opening '%s' for %lu bytes\n", path, (unsigned long)size);

  sdLockTake();
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    sdLockGive();
    Serial.printf("[STORE] FAILED to open file\n");
    SerialBT.printf("[ERROR] Failed to create file: %s\n", path);
    return;
  }
  sdLockGive();
  Serial.printf("[STORE] File opened OK\n");

  // Tell client we are ready.  The client must now send data in chunks
  // and wait for an ACK after each chunk (stop-and-wait flow control).
  SerialBT.println("[OK] Ready to receive");
  SerialBT.flush();
  Serial.printf("[STORE] Sent 'Ready to receive', waiting for %lu bytes\n", (unsigned long)size);

  const size_t RX_CHUNK = 512;           // safe size for BT serial buffer
  uint8_t buf[RX_CHUNK];
  size_t totalReceived = 0;
  size_t batchCount = 0;
  unsigned long lastDataMs = millis();
  const unsigned long TIMEOUT_MS = 10000; // abort if no data for 10 s
  bool timedOut = false;

  while (totalReceived < size) {
    size_t available = (size_t)SerialBT.available();

    if (available == 0) {
      if (millis() - lastDataMs > TIMEOUT_MS) {
        Serial.printf("[STORE] TIMEOUT: no data for %lu ms. Expected %lu, got %lu, missing %lu\n",
                      TIMEOUT_MS, (unsigned long)size, (unsigned long)totalReceived,
                      (unsigned long)(size - totalReceived));
        timedOut = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    // Data arrived — reset timeout and drain everything that is present
    lastDataMs = millis();

    size_t toRead = available;
    if (toRead > RX_CHUNK) toRead = RX_CHUNK;
    if (toRead > (size - totalReceived)) toRead = size - totalReceived;

    size_t readBytes = SerialBT.readBytes(buf, toRead);
    if (readBytes == 0) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    batchCount++;

    // --- SD write ---
    sdLockTake();
    size_t written = f.write(buf, readBytes);
    sdLockGive();

    if (written != readBytes) {
      Serial.printf("[STORE] SD write mismatch: wrote %lu of %lu\n",
                    (unsigned long)written, (unsigned long)readBytes);
    }

    totalReceived += written;

    // ACK the cumulative total so the client knows where to resume.
    // Sending over Bluetooth is safe here because the client is waiting.
    SerialBT.printf("[ACK] %lu\n", (unsigned long)totalReceived);
    SerialBT.flush();

    // Progress log to Serial monitor (USB/UART), NOT Bluetooth
    Serial.printf("[STORE] batch #%lu  read=%lu  avail=%lu  total=%lu  remaining=%lu\n",
                  (unsigned long)batchCount,
                  (unsigned long)readBytes,
                  (unsigned long)available,
                  (unsigned long)totalReceived,
                  (unsigned long)(size - totalReceived));

    // Brief yield every few batches to feed the BT stack
    if (batchCount % 8 == 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  Serial.printf("[STORE] Loop exited. total=%lu  expected=%lu  timedOut=%s\n",
                (unsigned long)totalReceived, (unsigned long)size, timedOut ? "YES" : "NO");

  sdLockTake();
  f.close();
  sdLockGive();
  Serial.printf("[STORE] File closed OK\n");

  if (timedOut) {
    SerialBT.printf("[ERROR] Store timeout — received %lu of %lu bytes\n",
                    (unsigned long)totalReceived, (unsigned long)size);
  } else {
    SerialBT.printf("[OK] File stored: %s (%lu bytes)\n", path, (unsigned long)size);
  }
}

// ---------------------------------------------------------------------------
void cmdDelete(const char* path) {
  sdLockTake();
  bool ok = SD_MMC.remove(path);
  sdLockGive();

  if (ok) {
    SerialBT.printf("[OK] Deleted: %s\n", path);
  } else {
    SerialBT.printf("[ERROR] Failed to delete: %s\n", path);
  }
}

// ---------------------------------------------------------------------------
void cmdSetPin(const char* pin) {
  if (strlen(pin) < 4 || strlen(pin) > 16) {
    SerialBT.println("[ERROR] PIN must be 4-16 characters");
    return;
  }

  if (btPinWrite(pin)) {
    ftpSetExpectedPin(pin);
    SerialBT.printf("[OK] PIN saved as '%s'. Reboot to apply.\n", pin);
  } else {
    SerialBT.println("[ERROR] Failed to save PIN");
  }
}

// ---------------------------------------------------------------------------
void cmdBtInfo(void) {
  String pin = btPinRead();
  if (pin.length() == 0) pin = "0000";

  SerialBT.printf("Device:        %s\n", "ESP32-FTP");
  SerialBT.printf("App PIN:       %s\n", pin.c_str());
  SerialBT.printf("Client:        %s\n", SerialBT.hasClient() ? "connected" : "none");
  SerialBT.printf("Authenticated: %s\n", isAuthenticated ? "yes" : "no");
  SerialBT.println("(MAC: use OS Bluetooth settings to view)");
}
