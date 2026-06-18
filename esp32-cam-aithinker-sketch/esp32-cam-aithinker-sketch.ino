#include "esp_camera.h"
#include "Arduino.h"
#include "FS.h"                // SD Card ESP32
#include "SD_MMC.h"            // SD Card ESP32
#include "soc/soc.h"           // Disable brownour problems
#include "soc/rtc_cntl_reg.h"  // Disable brownour problems
#include "driver/rtc_io.h"
#include <EEPROM.h>            // read and write from flash memory

// define the number of bytes you want to access
#define EEPROM_SIZE 1

// Pin definition for CAMERA_MODEL_AI_THINKER
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

int pictureNumber = 0;

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector
 
  Serial.begin(115200);
  //Serial.setDebugOutput(true);
  //Serial.println();
  
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG; 
  
  if(psramFound()){
    Serial.println("PSRAM found");
    config.frame_size = FRAMESIZE_SXGA; // FRAMESIZE_ + QVGA|CIF|VGA|SVGA|XGA|SXGA|UXGA
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }
  
  // Init Camera
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }


    sensor_t * s = esp_camera_sensor_get(); //以下参数根据场景进行适当调节
  s->set_brightness(s, 0);     // -2 to 2 设置亮度
  s->set_contrast(s, 0);       // -2 to 2  设置对比度
  s->set_saturation(s, 0);     // -2 to 2 设置饱和度
  s->set_special_effect(s, 0); // 0 to 6 (0-无效果，1-负面，2-灰度，3-红色涂料，4-绿色涂料，5-蓝色涂料，6- 防晒霜)
  s->set_whitebal(s, 1);       // 0 = disable , 1 = enable  /0=禁用，1=启用  设置_whitebal
  s->set_awb_gain(s, 1);       // 0 = disable , 1 = enable  /0=禁用，1=启用  设置_awb增益
  s->set_wb_mode(s, 0);        // 0 to 4 - if awb_gain enabled (0 - Auto, 1 - Sunny, 2 - Cloudy, 3 - Office, 4 - Home) (0-自动，1-晴天，2-多云，3-办公室，4-家)
  s->set_exposure_ctrl(s, 1);  // 0 = disable , 1 = enable /0=禁用，1=启用
  s->set_aec2(s, 0);           // 0 = disable , 1 = enable /0=禁用，1=启用
  s->set_ae_level(s, 0);       // -2 to 2 设置ae级别
  s->set_aec_value(s, 300);    // 0 to 1200  设置aec值
  s->set_gain_ctrl(s, 1);      // 0 = disable , 1 = enable /  设置增益控制  0=禁用，1=启用
  s->set_agc_gain(s, 0);       // 0 to 30
  s->set_gainceiling(s, (gainceiling_t)0);  // 0 to 6  感光自动增益控制
  s->set_bpc(s, 0);            // 0 = disable , 1 = enable /0=禁用，1=启用
  s->set_wpc(s, 1);            // 0 = disable , 1 = enable /0=禁用，1=启用
  s->set_raw_gma(s, 1);        // 0 = disable , 1 = enable /0=禁用，1=启用
  s->set_lenc(s, 1);           // 0 = disable , 1 = enable /0=禁用，1=启用
  s->set_hmirror(s, 1);        // 0 = disable , 1 = enable /0=禁用，1=启用
  s->set_vflip(s, 1);          // 0 = disable , 1 = enable /0=禁用，1=启用
  s->set_dcw(s, 1);            // 0 = disable , 1 = enable /0=禁用，1=启用
  s->set_colorbar(s, 0);       // 0 = disable , 1 = enable /0=禁用，1=启用
    delay(1000);
  Serial.println("Starting SD Card");
  if(!SD_MMC.begin()){
    Serial.println("SD Card Mount Failed");
    return;
  }
  
  uint8_t cardType = SD_MMC.cardType();
  if(cardType == CARD_NONE){
    Serial.println("No SD Card attached");
    return;
  }
    
  camera_fb_t * fb = NULL;

  for (int i = 0; i < 10; i++) {
fb = esp_camera_fb_get();
if (!fb) continue;
esp_camera_fb_return(fb);
delay(50);
}
  
  // Take Picture with Camera
  fb = esp_camera_fb_get();  
  if(!fb) {
    Serial.println("Camera capture failed");
    return;
  }
  // initialize EEPROM with predefined size
  EEPROM.begin(EEPROM_SIZE);
  pictureNumber = EEPROM.read(0) + 1;

  pinMode(33, OUTPUT);
  digitalWrite(33,  HIGH );
  rtc_gpio_hold_en(GPIO_NUM_33);

  
 
  // Path where new picture will be saved in SD Card
  String path = "/picture" + String(pictureNumber) +".jpg";
 //String result = 90;
 
  fs::FS &fs = SD_MMC; 
  Serial.printf("Picture file name: %s\n", path.c_str());
  
  File file = fs.open(path.c_str(), FILE_WRITE);
  if(!file){
    Serial.println("Failed to open file in writing mode");
  } 
  else {
    file.write(fb->buf, fb->len); // payload (image), payload length
    Serial.printf("Saved file to path: %s\n", path.c_str());
    EEPROM.write(0, pictureNumber);
    EEPROM.commit();
  }
  file.close();
 
  esp_camera_fb_return(fb); 
  
  // Turns off the ESP32-CAM white on-board LED (flash) connected to GPIO 4
//  delay(2000);
   pinMode(33, PULLDOWN );
  digitalWrite(33,  LOW );
  rtc_gpio_hold_dis(GPIO_NUM_33);
    
 
  Serial.println("Going to sleep now");
  //delay(2000);
  
  esp_deep_sleep_start();
  Serial.println("This will never be printed");
}

void loop() {
  
}