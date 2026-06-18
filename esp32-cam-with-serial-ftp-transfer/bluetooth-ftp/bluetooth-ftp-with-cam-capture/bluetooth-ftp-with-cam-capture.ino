/*
  Minimal Bluetooth Serial FTP Background Service
  ESP32-CAM (AI-Thinker) — Bluetooth Classic SPP only

  Board: AI-Thinker ESP32-CAM
  Core : >= 3.0.0 (tested on 3.3.7)
*/

#include "BluetoothSerial.h"
#include "FS.h"
#include "SD_MMC.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sd_lock.h"
#include "ftp_commands.h"
#include "bt_pin.h"
#include "camera_config.h"
#include "camera_capture.h"
#include "esp_camera.h"
#include "esp_gap_bt_api.h"

// ---------------------------------------------------------------------------
// Configuration defines
// ---------------------------------------------------------------------------
#define BT_DEVICE_NAME       "ESP32-FTP"
#define BT_SSP_AUTO_CONFIRM  true
#define FTP_AUTH_REQUIRED    false

// ---------------------------------------------------------------------------
BluetoothSerial SerialBT;

static volatile bool btWasConnected = false;

// SSP pairing state (serial-confirm mode)
static volatile bool sspPairingPending = false;
static volatile bool sspPairingConfirmed = false;
static volatile uint32_t sspNumVal = 0;
static uint32_t sspPairingDeadline = 0;

// Camera capture state
static CamConfig g_camConfig;
static unsigned long g_lastCaptureMs = 0;
static bool g_cameraOk = false;
static bool g_firstCaptureDone = false;
static uint32_t g_captureIntervalMs = 30000;  // default 30s

// ---------------------------------------------------------------------------
// Microsecond timestamp for precise debug
static inline unsigned long usNow() {
  return micros();
}

// ---------------------------------------------------------------------------
void taskBluetoothFTP(void* parameter) {
  (void)parameter;
  char lineBuf[256];
  size_t linePos = 0;

  while (true) {
    bool hasClient = SerialBT.hasClient();

    if (hasClient && !btWasConnected) {
      Serial.printf("[BT] Client connected\n");
      btWasConnected = true;
    }
    else if (!hasClient && btWasConnected) {
      Serial.printf("[BT] Client disconnected\n");
      btWasConnected = false;
      ftpSetAuthenticated(false);
      linePos = 0;
    }

    if (!hasClient) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    while (SerialBT.available()) {
      int c = SerialBT.read();
      if (c < 0) break;

      if (c == '\n') {
        lineBuf[linePos] = '\0';
        linePos = 0;

        unsigned long tRecv = usNow();
        Serial.printf("[CMD] Received: '%s' at %lu us\n", lineBuf, tRecv);

        unsigned long tDispatch0 = usNow();
        ftpDispatch(lineBuf);
        unsigned long tDispatch1 = usNow();
        Serial.printf("[CMD] Dispatch done in %lu us\n", tDispatch1 - tDispatch0);

        unsigned long tPrompt0 = usNow();
        SerialBT.print("> ");
        SerialBT.flush();   // force Bluedroid to transmit immediately
        unsigned long tPrompt1 = usNow();
        Serial.printf("[CMD] Prompt queued+flushed in %lu us\n", tPrompt1 - tPrompt0);
      }
      else if (c != '\r' && linePos < sizeof(lineBuf) - 1) {
        lineBuf[linePos++] = (char)c;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---------------------------------------------------------------------------
// Serial input drain helper — non-blocking, stops when buffer empty
static void drainSerialInput(size_t maxBytes, unsigned long maxMs) {
  unsigned long start = millis();
  size_t drained = 0;
  while (drained < maxBytes && (millis() - start) < maxMs) {
    if (!Serial.available()) break;
    Serial.read();
    drained++;
  }
  if (drained > 0) {
    Serial.printf("[BT] Drained %u bytes of stale serial input\n", (unsigned int)drained);
  }
}

// ---------------------------------------------------------------------------
static void cmdUnpairAll() {
  int devNum = esp_bt_gap_get_bond_device_num();
  if (devNum <= 0) {
    Serial.println("[BT] No bonded devices to remove.");
    return;
  }
  esp_bd_addr_t* devList = (esp_bd_addr_t*)malloc(devNum * sizeof(esp_bd_addr_t));
  if (!devList) {
    Serial.println("[BT] FAILED to allocate device list.");
    return;
  }
  esp_bt_gap_get_bond_device_list(&devNum, devList);
  int removed = 0;
  for (int i = 0; i < devNum; i++) {
    if (esp_bt_gap_remove_bond_device(devList[i]) == ESP_OK) {
      removed++;
    }
  }
  free(devList);
  Serial.printf("[BT] Removed %d of %d bonded devices.\n", removed, devNum);
}

// ---------------------------------------------------------------------------
static void serialCmdHelp() {
  Serial.println("Serial commands:");
  Serial.println("  CONFIRM xxxxxx  - Confirm pending Bluetooth pairing (xxxxxx = 6-digit code)");
  Serial.println("  UNPAIR_ALL      - Remove all bonded Bluetooth devices");
  Serial.println("  HELP            - Show this message");
}

// ---------------------------------------------------------------------------
static bool parseConfirmCode(const char* line, uint32_t* outCode) {
  // Expected format: "CONFIRM xxxxxx" where xxxxxx is exactly 6 digits
  const char* p = line + 7;  // skip "CONFIRM"
  while (*p == ' ') p++;
  if (strlen(p) != 6) return false;
  for (int i = 0; i < 6; i++) {
    if (p[i] < '0' || p[i] > '9') return false;
  }
  *outCode = (uint32_t)atol(p);
  return true;
}

// ---------------------------------------------------------------------------
void taskSerialHandler(void* parameter) {
  (void)parameter;
  char lineBuf[64];
  size_t linePos = 0;

  while (true) {
    // --- Read serial input ---
    while (Serial.available()) {
      int c = Serial.read();
      if (c < 0) break;
      if (c == '\n') {
        lineBuf[linePos] = '\0';
        linePos = 0;

        // --- Dispatch ---
        if (sspPairingPending) {
          if (strncasecmp(lineBuf, "CONFIRM", 7) == 0) {
            uint32_t enteredCode = 0;
            if (parseConfirmCode(lineBuf, &enteredCode) && enteredCode == sspNumVal) {
              sspPairingConfirmed = true;
              Serial.printf("[BT] Code %06lu matched. Pairing CONFIRMED.\n", (unsigned long)enteredCode);
            } else {
              Serial.printf("[BT] Code mismatch (expected %06lu). Pairing REJECTED.\n", (unsigned long)sspNumVal);
              sspPairingPending = false;
              SerialBT.confirmReply(false);
            }
          } else {
            Serial.println("[BT] Pairing REJECTED — expected CONFIRM xxxxxx.");
            sspPairingPending = false;
            SerialBT.confirmReply(false);
          }
        }
        else {
          if (strcasecmp(lineBuf, "UNPAIR_ALL") == 0) {
            cmdUnpairAll();
          }
          else if (strcasecmp(lineBuf, "HELP") == 0) {
            serialCmdHelp();
          }
          else {
            Serial.printf("[BT] Unknown command: '%s'. Type HELP for list.\n", lineBuf);
          }
        }
      }
      else if (c != '\r' && linePos < sizeof(lineBuf) - 1) {
        lineBuf[linePos++] = (char)c;
      }
    }

    // --- Handle confirmed pairing ---
    if (sspPairingPending && sspPairingConfirmed) {
      SerialBT.confirmReply(true);
      sspPairingPending = false;
      sspPairingConfirmed = false;
    }

    // --- Handle pairing timeout ---
    if (sspPairingPending && !sspPairingConfirmed && millis() > sspPairingDeadline) {
      SerialBT.confirmReply(false);
      sspPairingPending = false;
      Serial.println("[BT] Pairing timed out — rejected.");
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n========================================");
  Serial.println("[BOOT] Bluetooth FTP Server starting...");

  sdLockInit();

  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("[WARN] SD 4-bit mount failed, trying 1-bit...");
    if (!SD_MMC.begin()) {
      Serial.println("[ERROR] SD Card Mount Failed, Halting.");
      return;
    }
    else {
      Serial.println("[INFO] SD mounted in 1-bit mode.");
    }
  }
  else {
    Serial.println("[INFO] SD mounted in 4-bit mode.");
  }

  // --- Load camera config from SD ---
  sdLockTake();
  loadCamConfig(SD_MMC, g_camConfig);
  sdLockGive();
  g_captureIntervalMs = g_camConfig.intervalSec * 1000UL;

  // --- Initialize camera ---
  g_cameraOk = initCamera(g_camConfig);
  if (!g_cameraOk) {
    Serial.println("[WARN] Camera init failed. FTP will still work, but no captures.");
  }

  String appPin = btPinRead();
  if (appPin.length() > 0) {
    Serial.printf("[INFO] App-level PIN loaded: %s\n", appPin.c_str());
    ftpSetExpectedPin(appPin.c_str());
  }
  else {
    Serial.println("[INFO] No app-level PIN file found.");
  }

  SerialBT.begin(BT_DEVICE_NAME, false, true);

  int bondedCount = SerialBT.getNumberOfBondedDevices();
  Serial.printf("[INFO] Bonded devices: %d\n", bondedCount);

  SerialBT.onConfirmRequest([](uint32_t numVal) {
    sspNumVal = numVal;
#if BT_SSP_AUTO_CONFIRM
    Serial.printf("[BT] Auto-confirming SSP code %06lu\n", (unsigned long)numVal);
    SerialBT.confirmReply(true);
#else
    sspPairingPending = true;
    sspPairingConfirmed = false;
    sspPairingDeadline = millis() + 5000;

    // Drain any stale serial input before prompting
    drainSerialInput(4096, 200);

    Serial.printf("\n[BT] ════════════════════════════════════════\n");
    Serial.printf("[BT] PAIRING REQUEST — code: %06lu\n", (unsigned long)numVal);
    Serial.printf("[BT] Type CONFIRM %06lu to accept (5 second timeout)\n", (unsigned long)numVal);
    Serial.printf("[BT] ════════════════════════════════════════\n");
#endif
  });

  SerialBT.onAuthComplete([](bool success) {
    static int authCount = 0;
    authCount++;
    Serial.printf("[BT] Pairing %s (count=%d)\n", success ? "SUCCESS" : "FAILED", authCount);
  });

  String mac = SerialBT.getBtAddressString();
  Serial.printf("[INFO] Bluetooth '%s' ready. MAC: %s\n", BT_DEVICE_NAME, mac.c_str());
  Serial.println("========================================\n");

  xTaskCreatePinnedToCore(taskBluetoothFTP, "btftp", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(taskSerialHandler, "serial", 4096, NULL, 1, NULL, 1);
}

// ---------------------------------------------------------------------------
static inline unsigned long msNow() {
  return millis();
}

// ---------------------------------------------------------------------------
void loop() {
  if (!g_cameraOk) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    return;
  }

  unsigned long now = msNow();

  // First capture happens immediately after boot (g_lastCaptureMs == 0).
  // After that, wait for the configured interval between captures.
  unsigned long elapsed = now - g_lastCaptureMs;
  unsigned long targetInterval = g_firstCaptureDone ? g_captureIntervalMs : 0;

  if (elapsed >= targetInterval) {
    g_lastCaptureMs = now;
    g_firstCaptureDone = true;

    unsigned long tStart = msNow();
    Serial.printf("\n[CAM][%lu] === BEGIN photo cycle ===\n", tStart);

    // --- Step 1: Capture frame buffer ---
    unsigned long tCapture0 = msNow();
    Serial.printf("[CAM][%lu] Starting capture (warm-up + frame grab)...\n", tCapture0);
    camera_fb_t* fb = nullptr;
    bool captureOk = captureImageBuffer(&fb);
    unsigned long tCapture1 = msNow();
    if (!captureOk) {
      Serial.printf("[CAM][%lu] Capture FAILED after %lu ms. Aborting cycle.\n",
                    tCapture1, tCapture1 - tCapture0);
      return;
    }
    Serial.printf("[CAM][%lu] Capture SUCCESS in %lu ms. Buffer: %p, size: %u bytes\n",
                  tCapture1, tCapture1 - tCapture0, fb, (unsigned int)fb->len);

    // --- Step 2: Build filename ---
    g_camConfig.pictureNumber++;
    String path = "/picture" + String(g_camConfig.pictureNumber) + ".jpg";
    unsigned long tName = msNow();
    Serial.printf("[CAM][%lu] Filename: %s (pictureNumber=%lu)\n",
                  tName, path.c_str(), g_camConfig.pictureNumber);

    // --- Step 3: Store to SD ---
    unsigned long tStore0 = msNow();
    Serial.printf("[CAM][%lu] Acquiring SD lock...\n", tStore0);
    sdLockTake();
    unsigned long tLock = msNow();
    Serial.printf("[CAM][%lu] SD lock acquired in %lu ms. Opening file...\n",
                  tLock, tLock - tStore0);

    File f = SD_MMC.open(path, FILE_WRITE);
    bool written = false;
    size_t imgLen = 0;
    if (f) {
      imgLen = fb->len;
      Serial.printf("[CAM][%lu] File opened. Writing %u bytes...\n", msNow(), (unsigned int)imgLen);
      size_t bytesOut = f.write(fb->buf, fb->len);
      f.close();
      written = (bytesOut == imgLen);
      unsigned long tWrite = msNow();
      Serial.printf("[CAM][%lu] Write complete: %lu of %u bytes in %lu ms\n",
                    tWrite, (unsigned long)bytesOut, (unsigned int)imgLen, tWrite - tLock);
    } else {
      Serial.printf("[CAM][%lu] FAILED to open file for writing\n", msNow());
    }
    sdLockGive();
    unsigned long tStore1 = msNow();
    Serial.printf("[CAM][%lu] SD lock released. Total store time: %lu ms\n",
                  tStore1, tStore1 - tStore0);

    // --- Step 4: Release camera buffer ---
    releaseImageBuffer(fb);
    Serial.printf("[CAM][%lu] Camera buffer released\n", msNow());

    // --- Step 5: Persist config (pictureNumber) ---
    if (written) {
      unsigned long tConfig0 = msNow();
      Serial.printf("[CAM][%lu] Persisting updated config (pictureNumber=%lu)...\n",
                    tConfig0, g_camConfig.pictureNumber);
      sdLockTake();
      bool cfgOk = writeCamConfig(SD_MMC, g_camConfig);
      sdLockGive();
      unsigned long tConfig1 = msNow();
      Serial.printf("[CAM][%lu] Config persist %s in %lu ms\n",
                    tConfig1, cfgOk ? "OK" : "FAILED", tConfig1 - tConfig0);

      unsigned long tEnd = msNow();
      Serial.printf("[CAM][%lu] === END photo cycle. Total: %lu ms ===\n",
                    tEnd, tEnd - tStart);
      Serial.printf("[CAM][%lu] Next capture in ~%lu seconds\n",
                    tEnd, g_camConfig.intervalSec);
    } else {
      unsigned long tEnd = msNow();
      Serial.printf("[CAM][%lu] === END photo cycle (SAVE FAILED). Total: %lu ms ===\n",
                    tEnd, tEnd - tStart);
    }
  }

  vTaskDelay(pdMS_TO_TICKS(100));
}
