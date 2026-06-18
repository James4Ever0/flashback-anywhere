#include "Arduino.h"
#include "FS.h"
#include "SD_MMC.h"

// --- Configuration ---
#define SERIAL_BAUD 2000000
#define RX_BUFFER_SIZE 8192  // 8KB
#define TX_BUFFER_SIZE 8192  // 8KB
#define FILE_READ_CHUNK_SIZE 16384 // 16KB

String inputBuffer = "";
bool commandReady = false;

void setup() {
  // 1. Set larger buffers BEFORE Serial.begin()
  Serial.setRxBufferSize(RX_BUFFER_SIZE);
  Serial.setTxBufferSize(TX_BUFFER_SIZE);
  
  Serial.begin(SERIAL_BAUD);
  delay(200); // Allow time for serial to stabilize

  Serial.println("\nWelcome to serial filesystem");
  Serial.print("> ");

  // 2. Initialize SD_MMC in 4-bit mode if supported
  // The second argument 'true' enables 4-bit mode.
  // Change to 'false' if you encounter issues.
  if (!SD_MMC.begin("/sdcard", true)) { 
    Serial.println("\n[ERROR] SD Card Mount Failed. Trying 1-bit mode...");
    // If 4-bit mode fails, try 1-bit mode as a fallback
    if (!SD_MMC.begin()) {
        Serial.println("[ERROR] SD Card Mount Failed");
        while (1) { delay(1000); }
    } else {
        Serial.println("[INFO] SD Card mounted in 1-bit mode.");
    }
  } else {
    Serial.println("[INFO] SD Card mounted in 4-bit mode.");
  }

  // Optional: print card info
  uint8_t cardType = SD_MMC.cardType();
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
    Serial.println("Enter your command:");
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

  String output = "";               // Accumulator
  output.reserve(2048);            // Pre-allocate memory to reduce reallocs

  File entry = dir.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      output += "[DIR]  ";
      output += entry.name();
      output += "\n";
    } else {
      char line[80];
      snprintf(line, sizeof(line), "[FILE] %8u bytes  %s\n", entry.size(), entry.name());
      output += line;
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  // Send everything at once
  Serial.print(output);
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

  // 3. Use a larger buffer for reading the file
  uint8_t* buf = (uint8_t*)malloc(FILE_READ_CHUNK_SIZE);
  if (!buf) {
    Serial.println("[ERROR] Memory allocation failed for file buffer");
    file.close();
    return;
  }

  size_t remaining = fileSize;
  while (remaining > 0) {
    size_t toRead = (remaining > FILE_READ_CHUNK_SIZE) ? FILE_READ_CHUNK_SIZE : remaining;
    size_t bytesRead = file.read(buf, toRead);
    Serial.write(buf, bytesRead);
    remaining -= bytesRead;
  }

  free(buf); // Important: free allocated memory
  file.close();

  // Send end-of-file marker: ASCII EOT (0x04)
  Serial.write(0x04);
  Serial.flush();
}