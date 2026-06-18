/*
  ESP32-CAM Dual-Mode Serial File Transfer

  Modes:
    PHOTO    — capture JPEG, save to SD, repeat on interval
    TRANSFER — serial command loop for file operations

  Both modes use the same baud rate (921600).
  The board always listens for serial commands, even during photo capture.
  Auto-switches from TRANSFER back to PHOTO after 30 seconds of inactivity.

  Board:  AI-Thinker ESP32-CAM
  Upload: 921600 baud, Huge APP partition
*/

#include "pins.h"
#include "app_config.h"
#include "camera_manager.h"
#include "storage_manager.h"
#include "photo_pipeline.h"

#include <SD_MMC.h>
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>

// ============================================================================
// Global config instance
// ============================================================================
Config g_config;

// ============================================================================
// Runtime State
// ============================================================================
DeviceMode  currentMode;
unsigned long lastTransferActivity = 0;
String      serialBuffer = "";

// ----- Photo cycle timing -----
unsigned long nextPhotoTime = 0;
bool          photoPending = false;

// ----- Store receive state -----
bool          storeActive = false;
File          storeFile;
uint32_t      storeExpected = 0;
uint32_t      storeReceived = 0;
String        storePath = "";

// ============================================================================
// Forward Declarations
// ============================================================================
void processSerialInput();
void handleCommand(const String& cmd);
void handleFTPCommand(const String& cmd);
void handlePhotoCommand(const String& cmd);
void switchMode(DeviceMode newMode);
void handleList(const String& path);
void handleRead(const String& path);
void handleStore(const String& path, uint32_t size);
void handleDelete(const String& path);
void processStoreData();
void runPhotoStateMachine();

// ============================================================================
// Setup
// ============================================================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.setRxBufferSize(8192);
  Serial.setTxBufferSize(8192);
  Serial.begin(BAUD_RATE);
  delay(100);

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  Serial.println("\n[BOOT] ESP32-CAM Dual-Mode Starting...");
  Serial.printf("[BOOT] Baud rate: %d\n", BAUD_RATE);

  // --- Initialize SD ---
  if (!initSD()) {
    Serial.println("[FATAL] SD init failed. Halting.");
    while (1) { delay(1000); }
  }

  // --- Load config ---
  loadConfig(SD_MMC, g_config);

  // --- Initialize camera ---
  if (!initCamera(g_config)) {
    Serial.println("[FATAL] Camera init failed. Halting.");
    while (1) { delay(1000); }
  }

  // --- Set initial mode from config ---
  currentMode = g_config.mode;
  lastTransferActivity = millis();

  Serial.println("\n========== BOOT COMPLETE ==========");
  Serial.printf("[BOOT] Mode: %s\n", deviceModeToString(currentMode).c_str());
  Serial.printf("[BOOT] Picture number: %lu\n", g_config.pictureNumber);
  Serial.printf("[BOOT] Interval: %lu sec\n", g_config.intervalSec);
  Serial.println("===================================\n");

  if (currentMode == MODE_TRANSFER) {
    Serial.println("========== TRANSFER MODE ==========");
  } else {
    Serial.println("========== PHOTO MODE ==========");
    nextPhotoTime = millis();  // take first photo soon
  }
}

// ============================================================================
// Loop
// ============================================================================
void loop() {
  // --- Always process serial input ---
  processSerialInput();

  if (currentMode == MODE_TRANSFER) {
    // --- Handle binary store data if active ---
    if (storeActive) {
      processStoreData();
    }

    // --- Check inactivity timeout ---
    if (millis() - lastTransferActivity > TRANSFER_INACTIVITY_MS) {
      Serial.println("[INFO] Transfer idle timeout, switching to PHOTO mode");
      switchMode(MODE_PHOTO);
    }
  } else {
    // --- Photo mode state machine ---
    runPhotoStateMachine();
  }
}

// ============================================================================
// Serial Input — Non-blocking, mode-agnostic, logs every byte
// ============================================================================
void processSerialInput() {
  while (Serial.available()) {
    char c = Serial.read();
    // Log every byte as hex + printable char
    Serial.printf("[RX] 0x%02X '%c'\n",
                  (uint8_t)c,
                  (c >= 32 && c < 127) ? c : '.');

    if (c == '\n' || c == '\r') {
      serialBuffer.trim();
      if (serialBuffer.length() > 0) {
        handleCommand(serialBuffer);
        serialBuffer = "";
      }
    } else {
      if (serialBuffer.length() < 256) {
        serialBuffer += c;
      }
    }
  }
}

// ============================================================================
// Command Dispatch — universal commands first, then mode-specific handlers
// ============================================================================
void handleCommand(const String& cmd) {
  Serial.printf("[CMD] '%s'\n", cmd.c_str());

  // --- Universal commands (work in any mode) ---
  if (cmd.equalsIgnoreCase("CURRENT_MODE")) {
    Serial.printf("[OK] mode: %s\n", deviceModeToString(currentMode).c_str());
    return;
  }

  // --- Mode switch commands ---
  if (cmd.equalsIgnoreCase("MODE TRANSFER")) {
    switchMode(MODE_TRANSFER);
    Serial.println("[OK] mode changed to TRANSFER");
    return;
  }

  if (cmd.equalsIgnoreCase("MODE PHOTO")) {
    switchMode(MODE_PHOTO);
    Serial.println("[OK] mode changed to PHOTO");
    return;
  }

  // --- Mode-specific dispatch ---
  if (currentMode == MODE_TRANSFER) {
    handleFTPCommand(cmd);
  } else {
    handlePhotoCommand(cmd);
  }
}

// ============================================================================
// Photo-Mode Command Handler
// ============================================================================
void handlePhotoCommand(const String& cmd) {
  if (cmd.equalsIgnoreCase("HELP")) {
    Serial.println("Available commands in PHOTO mode:");
    Serial.println("  CURRENT_MODE           - Get current mode (PHOTO or TRANSFER)");
    Serial.println("  MODE TRANSFER          - Switch to transfer mode");
    Serial.println("  HELP                   - Show this help");
    return;
  }

  Serial.println("[ERROR] In PHOTO mode, only CURRENT_MODE, MODE, and HELP are available");
}

// ============================================================================
// Transfer-Mode (FTP) Command Handler
// ============================================================================
void handleFTPCommand(const String& cmd) {
  lastTransferActivity = millis();

  if (cmd.equalsIgnoreCase("HELP")) {
    Serial.println("Available commands in TRANSFER mode:");
    Serial.println("  CURRENT_MODE           - Get current mode (PHOTO or TRANSFER)");
    Serial.println("  MODE PHOTO             - Switch to photo mode");
    Serial.println("  LIST <path>            - List directory contents");
    Serial.println("  READ <path>            - Download a file (raw binary)");
    Serial.println("  STORE <path> <size>    - Upload a file (followed by raw binary)");
    Serial.println("  DELETE <path>          - Delete a file");
    Serial.println("  HELP                   - Show this help");
    return;
  }

  // LIST
  if (cmd.startsWith("LIST ") || cmd.equalsIgnoreCase("LIST")) {
    String path = "/";
    int sp = cmd.indexOf(' ');
    if (sp >= 0) path = cmd.substring(sp + 1);
    path.trim();
    handleList(path);
    return;
  }

  // READ
  if (cmd.startsWith("READ ")) {
    String path = cmd.substring(5);
    path.trim();
    handleRead(path);
    return;
  }

  // STORE
  if (cmd.startsWith("STORE ")) {
    String rest = cmd.substring(6);
    rest.trim();
    int sp = rest.lastIndexOf(' ');
    if (sp < 0) {
      Serial.println("[ERROR] Usage: STORE <path> <size>");
      return;
    }
    String path = rest.substring(0, sp);
    String sizeStr = rest.substring(sp + 1);
    path.trim();
    sizeStr.trim();
    uint32_t size = (uint32_t)sizeStr.toInt();
    handleStore(path, size);
    return;
  }

  // DELETE
  if (cmd.startsWith("DELETE ")) {
    String path = cmd.substring(7);
    path.trim();
    handleDelete(path);
    return;
  }

  Serial.println("[ERROR] Unknown command. Type HELP for options.");
}

// ============================================================================
// Mode Switch
// ============================================================================
void switchMode(DeviceMode newMode) {
  currentMode = newMode;
  g_config.mode = newMode;
  writeConfig(SD_MMC, g_config);

  if (newMode == MODE_TRANSFER) {
    lastTransferActivity = millis();
    Serial.println("\n========== TRANSFER MODE ==========");
  } else {
    Serial.println("\n========== PHOTO MODE ==========");
    photoPending = false;
    nextPhotoTime = millis() + 1000;  // slight delay before first photo
  }
}

// ============================================================================
// Photo State Machine (non-blocking, no deep sleep)
// ============================================================================
void runPhotoStateMachine() {
  if (!photoPending) {
    if (millis() >= nextPhotoTime) {
      photoPending = true;
    } else {
      return;
    }
  }

  // Execute one photo cycle (blocking ~500-800ms for capture)
  // UART hardware FIFO buffers incoming commands during this time
  Serial.println("[PHOTO] Capturing...");
  bool ok = runPhotoCycle(SD_MMC, g_config);

  photoPending = false;

  if (ok) {
    // Normal interval
    nextPhotoTime = millis() + (g_config.intervalSec * 1000UL);
    Serial.printf("[PHOTO] Next capture in %lu sec\n", g_config.intervalSec);
  } else {
    // Capture failed — enforce minimum 30-second retry to avoid tight loop
    uint32_t retrySec = g_config.intervalSec;
    if (retrySec < 30) retrySec = 30;
    nextPhotoTime = millis() + (retrySec * 1000UL);
    Serial.printf("[PHOTO] Capture failed. Retry in %lu sec\n", retrySec);
  }
}

// ============================================================================
// LIST Handler
// ============================================================================
void handleList(const String& path) {
  File dir = SD_MMC.open(path.c_str());
  if (!dir || !dir.isDirectory()) {
    Serial.println("[ERROR] Not a directory");
    if (dir) dir.close();
    return;
  }

  File entry = dir.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      Serial.printf("[DIR]  %s\n", entry.name());
    } else {
      Serial.printf("[FILE] %lu %s\n", (unsigned long)entry.size(), entry.name());
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  Serial.println("[OK]");
}

// ============================================================================
// READ Handler (host downloads file from board)
// ============================================================================
void handleRead(const String& path) {
  File f = SD_MMC.open(path.c_str(), FILE_READ);
  if (!f || f.isDirectory()) {
    Serial.println("[ERROR] File not found");
    if (f) f.close();
    return;
  }

  uint32_t size = f.size();
  Serial.printf("SIZE:%lu\n", (unsigned long)size);
  Serial.flush();

  // Stream the entire file in one go
  uint8_t buf[512];
  while (f.available()) {
    int got = f.read(buf, sizeof(buf));
    Serial.write(buf, got);
  }
  f.close();

  Serial.write(0x04);  // EOT
  Serial.println();
  Serial.println("[OK]");
}

// ============================================================================
// STORE Handler (host uploads file to board)
// ============================================================================
void handleStore(const String& path, uint32_t size) {
  if (storeActive) {
    storeFile.close();
    storeActive = false;
  }

  storeFile = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (!storeFile) {
    Serial.println("[ERROR] Failed to open file for writing");
    return;
  }

  storePath = path;
  storeExpected = size;
  storeReceived = 0;
  storeActive = true;

  Serial.println("[OK] Ready to receive");
  Serial.flush();
}

void processStoreData() {
  if (!storeActive) return;

  // Read available bytes without blocking
  int avail = Serial.available();
  if (avail <= 0) return;

  // Don't read more than we expect
  uint32_t need = storeExpected - storeReceived;
  int toRead = (avail > (int)need) ? (int)need : avail;

  uint8_t buf[512];
  while (toRead > 0) {
    int chunk = (toRead > (int)sizeof(buf)) ? (int)sizeof(buf) : toRead;
    int got = Serial.readBytes(buf, chunk);
    if (got <= 0) break;

    storeFile.write(buf, got);
    storeReceived += got;
    toRead -= got;

    if (storeReceived >= storeExpected) {
      storeFile.close();
      storeActive = false;
      Serial.printf("[ACK] %lu\n", (unsigned long)storeReceived);
      Serial.printf("[OK] Stored %lu bytes\n", (unsigned long)storeReceived);
      lastTransferActivity = millis();
      return;
    }
  }

  // Send periodic ACK for progress
  static unsigned long lastAck = 0;
  if (millis() - lastAck > 500) {
    Serial.printf("[ACK] %lu\n", (unsigned long)storeReceived);
    lastAck = millis();
    lastTransferActivity = millis();
  }
}

// ============================================================================
// DELETE Handler
// ============================================================================
void handleDelete(const String& path) {
  if (!SD_MMC.exists(path.c_str())) {
    Serial.println("[ERROR] File not found");
    return;
  }
  if (SD_MMC.remove(path.c_str())) {
    Serial.println("[OK]");
  } else {
    Serial.println("[ERROR] Delete failed");
  }
}
