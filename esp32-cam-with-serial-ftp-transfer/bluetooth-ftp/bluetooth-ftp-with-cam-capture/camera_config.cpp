#include "camera_config.h"

// ---------- Internals ----------

static bool parseConfigLine(const String& line, CamConfig& cfg) {
  int eq = line.indexOf('=');
  if (eq == -1) return false;

  String key = line.substring(0, eq);
  String val = line.substring(eq + 1);
  key.trim();
  val.trim();

  if (key.equalsIgnoreCase("interval")) {
    uint32_t v = val.toInt();
    cfg.intervalSec = (v > 0) ? v : DEFAULT_INTERVAL_SEC;
  } else if (key.equalsIgnoreCase("jpegQuality")) {
    int v = val.toInt();
    cfg.jpegQuality = (v >= 0 && v <= 63) ? v : DEFAULT_JPEG_QUALITY;
  } else if (key.equalsIgnoreCase("frameSize")) {
    uint16_t fs = stringToFrameSize(val);
    cfg.frameSize = (fs != (uint16_t)-1) ? fs : DEFAULT_FRAME_SIZE;
  } else if (key.equalsIgnoreCase("pictureNumber")) {
    cfg.pictureNumber = val.toInt();
  } else {
    return false;
  }
  return true;
}

// ---------- Public ----------

bool loadCamConfig(fs::FS& fs, CamConfig& outConfig) {
  // Set defaults first
  outConfig.intervalSec    = DEFAULT_INTERVAL_SEC;
  outConfig.jpegQuality    = DEFAULT_JPEG_QUALITY;
  outConfig.frameSize      = DEFAULT_FRAME_SIZE;
  outConfig.pictureNumber  = DEFAULT_PICTURE_NUMBER;

  File f = fs.open(CAM_CONFIG_PATH, FILE_READ);
  if (!f) {
    Serial.println("[CAM_CONFIG] Config not found. Creating default.");
    writeCamConfig(fs, outConfig);
    return true;
  }

  bool anyParsed = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;
    if (parseConfigLine(line, outConfig)) anyParsed = true;
  }
  f.close();

  if (!anyParsed) {
    Serial.println("[WARN] Config file empty/invalid. Using defaults.");
  } else {
    Serial.println("[INFO] Config loaded from SD.");
  }

  Serial.printf("[CONFIG] interval=%lu, quality=%u, frameSize=%s, pictureNumber=%lu\n",
                outConfig.intervalSec, outConfig.jpegQuality,
                frameSizeToString(outConfig.frameSize).c_str(), outConfig.pictureNumber);
  return true;
}

bool writeCamConfig(fs::FS& fs, const CamConfig& config) {
  File f = fs.open(CAM_CONFIG_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[ERROR] Failed to write cam config.");
    return false;
  }

  f.println("# ESP32-CAM Capture Config");
  f.println("# interval: seconds between captures");
  f.println("# jpegQuality: 0-63 (lower = better quality, larger file)");
  f.println("# frameSize: QVGA, CIF, VGA, SVGA, XGA, SXGA, UXGA");
  f.println("# pictureNumber: starting counter for filenames");
  f.println();
  f.printf("interval=%lu\n", config.intervalSec);
  f.printf("jpegQuality=%u\n", config.jpegQuality);
  f.printf("frameSize=%s\n", frameSizeToString(config.frameSize).c_str());
  f.printf("pictureNumber=%lu\n", config.pictureNumber);
  f.close();

  Serial.println("[INFO] Cam config written to " CAM_CONFIG_PATH);
  return true;
}

String frameSizeToString(uint16_t fs) {
  switch (fs) {
    case FRAMESIZE_QVGA: return "QVGA";
    case FRAMESIZE_CIF:  return "CIF";
    case FRAMESIZE_VGA:  return "VGA";
    case FRAMESIZE_SVGA: return "SVGA";
    case FRAMESIZE_XGA:  return "XGA";
    case FRAMESIZE_SXGA: return "SXGA";
    case FRAMESIZE_UXGA: return "UXGA";
    default:             return "SXGA";
  }
}

uint16_t stringToFrameSize(const String& s) {
  if (s.equalsIgnoreCase("QVGA")) return FRAMESIZE_QVGA;
  if (s.equalsIgnoreCase("CIF"))  return FRAMESIZE_CIF;
  if (s.equalsIgnoreCase("VGA"))  return FRAMESIZE_VGA;
  if (s.equalsIgnoreCase("SVGA")) return FRAMESIZE_SVGA;
  if (s.equalsIgnoreCase("XGA"))  return FRAMESIZE_XGA;
  if (s.equalsIgnoreCase("SXGA")) return FRAMESIZE_SXGA;
  if (s.equalsIgnoreCase("UXGA")) return FRAMESIZE_UXGA;
  return (uint16_t)-1;
}
