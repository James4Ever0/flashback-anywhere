// try to use board as mass storage device.
// since this board is not doing good with mass storage, how about just use serial device for communication?

#include <SD.h>
#include <USB.h>
#include <USBMSC.h>

// const long comPortBaudRate = 115200;
const long comPortBaudRate = 2000000;

// could we implement some error correction code in serial transfer?
// since the serial could have trouble.

#if SOC_USB_OTG_SUPPORTED
#if CONFIG_TINYUSB_MSC_ENABLED

#define USE_MASS_STORAGE_CODE

#else
#warning "Your device does not support mass storage"
#endif

#else
#warning "Your device does not support USB OTG"
#endif

#ifdef USE_MASS_STORAGE_CODE
#pragma message("Using mass storage code")

const int chipSelect = 5;

USBMSC MSC;
File root;

void setup() {
  // put your setup code here, to run once:
  Serial.begin(comPortBaudRate);
  while (!Serial) {
    ;
  }
    
  if(!SD.begin(chipSelect)){
    Serial.println("SD Card Mount Failed");
    return;
  }
  
  if (!MSC.begin()){
    Serial.println("USB MSC failed to start");
    return;
  }

  MSC.mount(&SD);

  Serial.println("System ready.");
}

void loop() {
  // put your main code here, to run repeatedly:
  MSC.update();
}

#else
#pragma message("Using test file transfer code")

void setup(){

  Serial.begin(comPortBaudRate);
  while (!Serial) {
    ;
  }
  Serial.println("Test sending photo with serial.");
}

void loop(){
  
}

#endif