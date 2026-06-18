#include "camera_manager.h"
#include "pins.h"

bool initCamera(const Config& config) {
  camera_config_t camConfig;
  camConfig.ledc_channel = LEDC_CHANNEL_0;
  camConfig.ledc_timer   = LEDC_TIMER_0;
  camConfig.pin_d0       = Y2_GPIO_NUM;
  camConfig.pin_d1       = Y3_GPIO_NUM;
  camConfig.pin_d2       = Y4_GPIO_NUM;
  camConfig.pin_d3       = Y5_GPIO_NUM;
  camConfig.pin_d4       = Y6_GPIO_NUM;
  camConfig.pin_d5       = Y7_GPIO_NUM;
  camConfig.pin_d6       = Y8_GPIO_NUM;
  camConfig.pin_d7       = Y9_GPIO_NUM;
  camConfig.pin_xclk     = XCLK_GPIO_NUM;
  camConfig.pin_pclk     = PCLK_GPIO_NUM;
  camConfig.pin_vsync    = VSYNC_GPIO_NUM;
  camConfig.pin_href     = HREF_GPIO_NUM;
  camConfig.pin_sscb_sda = SIOD_GPIO_NUM;
  camConfig.pin_sscb_scl = SIOC_GPIO_NUM;
  camConfig.pin_pwdn     = PWDN_GPIO_NUM;
  camConfig.pin_reset    = RESET_GPIO_NUM;
  camConfig.xclk_freq_hz = 10000000;
  camConfig.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    camConfig.frame_size   = (framesize_t)config.frameSize;
    camConfig.jpeg_quality = config.jpegQuality;
    camConfig.fb_count     = 2;
  } else {
    camConfig.frame_size   = FRAMESIZE_SVGA;
    camConfig.jpeg_quality = 12;
    camConfig.fb_count     = 1;
  }

  esp_err_t err = esp_camera_init(&camConfig);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);
  s->set_special_effect(s, 0);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 0);
  s->set_ae_level(s, 0);
  s->set_aec_value(s, 300);
  s->set_gain_ctrl(s, 1);
  s->set_agc_gain(s, 0);
  s->set_gainceiling(s, (gainceiling_t)0);
  s->set_bpc(s, 0);
  s->set_wpc(s, 1);
  s->set_raw_gma(s, 1);
  s->set_lenc(s, 1);
  s->set_hmirror(s, 1);
  s->set_vflip(s, 1);
  s->set_dcw(s, 1);
  s->set_colorbar(s, 0);

  Serial.println("[INFO] Camera initialized.");
  return true;
}

bool captureImageBuffer(camera_fb_t** outFb, int warmUpFrames, int warmUpDelayMs) {
  // Warm-up: discard frames to let auto-exposure settle
  for (int i = 0; i < warmUpFrames; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(warmUpDelayMs);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[ERROR] Camera capture failed.");
    *outFb = nullptr;
    return false;
  }

  *outFb = fb;
  return true;
}

void releaseImageBuffer(camera_fb_t* fb) {
  if (fb) {
    esp_camera_fb_return(fb);
  }
}
