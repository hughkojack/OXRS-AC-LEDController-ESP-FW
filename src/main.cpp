/**
  GPIO LED controller firmware for the Open eXtensible Rack System
  
  See https://oxrs.io/docs/firmware/led-controller-esp32.html for documentation.
  Compile options:
    ESP32
    
  External dependencies. Install using the Arduino library manager:
    "PubSubClient" by Nick O'Leary
    "OXRS-IO-MQTT-ESP32-LIB" by OXRS Core Team
    "OXRS-IO-API-ESP32-LIB" by OXRS Core Team
    "OXRS-AC-I2CSensors-ESP-LIB" by Austins Creations
    "ledPWM" by Austins Creations
  Compatible with 5 channel LED controller shield for LilyGO found here:
    
  GitHub repository:
   
    
  Bugs/Features:
    See GitHub issues list
  Copyright 2021 Austins Creations
*/

#include <Arduino.h>

/*----------------------- Board Type --------------------------------*/
// #define esp8266  
#define esp32

/*----------------------- Connection Type --------------------------------*/
// select connection mode here - comment / uncomment the one needed
#define ethMode    // uses ethernet
//#define wifiMode   // uses wifi

/*----------------------- Modetype Type --------------------------------*/
#define gpioMode  // uses 5ch GPIO mosfet control
//#define pcaMode   // uses I2C PCA controller

/*--------------------------- Version ------------------------------------*/
#define FW_NAME       "OXRS-AC-LedController-ESP-FW"
#define FW_SHORT_NAME "LED Controller"
#define FW_MAKER      "Austin's Creations"
#define FW_VERSION    "2.0.0"

/*--------------------------- Libraries ----------------------------------*/

#if defined(esp8266)
#include <ESP8266WiFi.h>            // For networking
#if defined(ethMode)
  #include <SPI.h>                  // for ethernet
  #include <Ethernet.h>             // For networking
#endif
#elif defined(esp32)
#include <WiFi.h>                   // For networking
#if defined(ethMode)
  #include <SPI.h>                  // for ethernet
  // #include <ETH.h>                  // For networking <-- need to find
#endif
#endif

#include <Wire.h>                   // For I2C
#include <PubSubClient.h>           // For MQTT
#include <OXRS_MQTT.h>              // For MQTT
#include <OXRS_API.h>               // For REST API
#include <OXRS_SENSORS.h>           // For QWICC I2C sensors
#include <ledPWM.h>                 // For PWM LED controller

/*--------------------------- Configuration ------------------------------*/
// Should be no user configuration in this file, everything should be in;
#if defined(wifiMode)
#include "config.h"
#endif


void setup() {
  // put your setup code here, to run once:
}

void loop() {
  // put your main code here, to run repeatedly:
}