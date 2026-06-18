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

// ---------------------------------------------------------------------------
// Configuration defines
// ---------------------------------------------------------------------------
#define BT_DEVICE_NAME       "ESP32-FTP"
#define BT_SSP_AUTO_CONFIRM  true
#define BT_CONFIRM_BUTTON_GPIO  0
#define FTP_AUTH_REQUIRED    false

// ---------------------------------------------------------------------------
BluetoothSerial SerialBT;

static volatile bool sspPending = false;
static volatile uint32_t sspNumVal = 0;
static volatile bool btWasConnected = false;

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
void taskBootButton(void* parameter) {
  (void)parameter;
  pinMode(BT_CONFIRM_BUTTON_GPIO, INPUT_PULLUP);
  while (true) {
    if (sspPending) {
      unsigned long start = millis();
      bool confirmed = false;
      while (sspPending && (millis() - start < 30000)) {
        if (digitalRead(BT_CONFIRM_BUTTON_GPIO) == LOW) {
          delay(50);
          if (digitalRead(BT_CONFIRM_BUTTON_GPIO) == LOW) {
            SerialBT.confirmReply(true);
            sspPending = false;
            confirmed = true;
            break;
          }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      if (sspPending && !confirmed) {
        SerialBT.confirmReply(false);
        sspPending = false;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
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
    sspPending = true;
#if BT_SSP_AUTO_CONFIRM
    Serial.printf("[BT] Auto-confirming SSP code %06lu\n", (unsigned long)numVal);
    SerialBT.confirmReply(true);
    sspPending = false;
#else
    Serial.printf("\n[BT] PAIRING REQUEST — code: %06lu — press GPIO%d\n", (unsigned long)numVal, BT_CONFIRM_BUTTON_GPIO);
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
  xTaskCreatePinnedToCore(taskBootButton, "bootbtn", 2048, NULL, 1, NULL, 1);
}

// ---------------------------------------------------------------------------
void loop() {
  vTaskDelay(portMAX_DELAY);
}
