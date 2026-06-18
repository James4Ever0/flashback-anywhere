#include "Arduino.h"
#include "FS.h"
#include "SD_MMC.h"

// ----------------------------------------------------------------------
// Serial command handling
// ----------------------------------------------------------------------
String inputBuffer = "";
bool commandReady = false;

void setup() {
  Serial.begin(115200);
  delay(100);  // Allow serial monitor to attach

  // Welcome message
  Serial.println("\nWelcome to serial filesystem");
  Serial.print("> ");

  // Initialize SD_MMC (1‑bit mode, uses GPIOs 2,4,12,13,14,15)
  if (!SD_MMC.begin()) {
    Serial.println("\n[ERROR] SD Card Mount Failed");
    while (1) { delay(1000); }
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("\n[ERROR] No SD Card attached");
    while (1) { delay(1000); }
  }

  // Optional: print card info
  Serial.printf("[INFO] SD Card Type: %s\n", 
                 cardType == CARD_MMC ? "MMC" : 
                 cardType == CARD_SD  ? "SDSC" : 
                 cardType == CARD_SDHC ? "SDHC" : "UNKNOWN");
  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  Serial.printf("[INFO] Card Size: %llu MB\n", cardSize);
}

void loop() {
  // Read incoming serial data
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (inputBuffer.length() > 0) {
        commandReady = true;
        break;
      }
    } else {
      inputBuffer += c;
    }
  }

  if (commandReady) {
    inputBuffer.trim();
    if (inputBuffer.length() > 0) {
      processCommand(inputBuffer);
    }
    inputBuffer = "";
    commandReady = false;
    Serial.print("> ");
  }
}

void processCommand(String cmd) {
  // Split into command and argument
  int spaceIdx = cmd.indexOf(' ');
  String command = (spaceIdx == -1) ? cmd : cmd.substring(0, spaceIdx);
  String arg = (spaceIdx == -1) ? "" : cmd.substring(spaceIdx + 1);
  command.toLowerCase();

  if (command == "list") {
    if (arg.length() == 0) {
      Serial.println("[ERROR] Usage: list <absolute path>");
      return;
    }
    listDirectory(arg);
  }
  else if (command == "read") {
    if (arg.length() == 0) {
      Serial.println("[ERROR] Usage: read <absolute file path>");
      return;
    }
    readFile(arg);
  }
  else {
    Serial.println("[ERROR] Unknown command. Available: list, read");
  }
}

void listDirectory(String path) {
  File dir = SD_MMC.open(path);
  if (!dir) {
    Serial.println("[ERROR] Failed to open directory: " + path);
    return;
  }
  if (!dir.isDirectory()) {
    Serial.println("[ERROR] Not a directory: " + path);
    dir.close();
    return;
  }

  File entry = dir.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      Serial.printf("[DIR]  %s\n", entry.name());
    } else {
      Serial.printf("[FILE] %8u bytes  %s\n", entry.size(), entry.name());
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
}

void readFile(String path) {
  File file = SD_MMC.open(path);
  if (!file) {
    Serial.println("[ERROR] Failed to open file: " + path);
    return;
  }
  if (file.isDirectory()) {
    Serial.println("[ERROR] Path is a directory, not a file: " + path);
    file.close();
    return;
  }

  size_t fileSize = file.size();
  Serial.printf("SIZE: %u\n", fileSize);

  // Send file content in chunks (ESP32 serial buffer friendly)
  const size_t bufSize = 1024;
  uint8_t buf[bufSize];
  size_t remaining = fileSize;
  while (remaining > 0) {
    size_t toRead = (remaining > bufSize) ? bufSize : remaining;
    size_t read = file.read(buf, toRead);
    Serial.write(buf, read);
    remaining -= read;
  }

  file.close();

  // Send end-of-file marker: ASCII EOT (0x04)
  Serial.write(0x04);
  // Optionally flush to ensure all data is transmitted
  Serial.flush();
}