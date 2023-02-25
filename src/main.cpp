/**
  PWM LED controller firmware for the Open eXtensible Rack System

  Documentation:  
    https://oxrs.io/docs/hardware/output-devices/pwm-controllers.html
  
  GitHub repository:
    https://github.com/austinscreations/OXRS-AC-LEDController-ESP-FW

  Copyright 2022 Austins Creations
*/

/*------------------------- PWM Type ----------------------------------*/
//#define PCA_MODE          // 16ch PCA9865 PWM controllers (I2C)
//#define GPIO_PWM1-5       // 5ch GPIO MOSFETs

/*--------------------------- Libraries -------------------------------*/
#include <Arduino.h>
#include <ledPWM.h>                 // For PWM LED controller

#if defined(OXRS_ESP32)
#include <OXRS_32.h>                  // ESP32 support
OXRS_32 oxrs;

#elif defined(OXRS_ESP8266)
#include <OXRS_8266.h>                // ESP8266 support
OXRS_8266 oxrs;

#elif defined(OXRS_RACK32)
#include <OXRS_Rack32.h>              // Rack32 support
#include "logo.h"                     // Embedded maker logo
OXRS_Rack32 oxrs(FW_LOGO);

#elif defined(OXRS_ROOM8266)
#include <OXRS_Room8266.h>            // Room8266 support
OXRS_Room8266 oxrs;

#elif defined(OXRS_LILYGO)
#include <OXRS_LILYGOPOE.h>           // LilyGO T-ETH-POE support
OXRS_LILYGOPOE oxrs;
#endif

/*--------------------------- Constants ----------------------------------*/
// Serial
#define SERIAL_BAUD_RATE            115200

// Only support up to 5 LEDs per strip
#define MAX_LED_COUNT               5

// Supported LED states
#define LED_STATE_OFF               0
#define LED_STATE_ON                1

// Default fade interval (microseconds)
#define DEFAULT_FADE_INTERVAL_US    500L;

// Number of PWM controllers and channels per controller
#if defined(PCA_MODE)
// Each PCA9865 is a PWM controller
// See https://www.nxp.com/docs/en/data-sheet/PCA9685.pdf
const byte    PCA_I2C_ADDRESS[]     = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47 };
const uint8_t PWM_CONTROLLER_COUNT  = sizeof(PCA_I2C_ADDRESS);
// Each PCA9865 has 16 channels
#define PWM_CHANNEL_COUNT           16

#else
// Each GPIO device is a single controller
#define PWM_CONTROLLER_COUNT        1
// Only 5 GPIO channels
#define PWM_CHANNEL_COUNT           5
#endif

/*-------------------------- Internal datatypes --------------------------*/
struct LEDStrip
{
  uint8_t index;
  uint8_t channels;
  uint8_t state;
  uint8_t colour[MAX_LED_COUNT];

  uint32_t fadeIntervalUs;
  uint32_t lastFadeUs;
};

/*--------------------------- Global Variables ---------------------------*/
// Each bit corresponds to a discovered PWM controller
uint8_t g_pwms_found = 0;

// Fade interval used if no explicit interval defined in command payload
uint32_t g_fade_interval_us = DEFAULT_FADE_INTERVAL_US;

// PWM LED controllers
PWMDriver pwmDriver[PWM_CONTROLLER_COUNT];

// LED strip config (allow for a max of all single LED strips)
LEDStrip ledStrips[PWM_CONTROLLER_COUNT][PWM_CHANNEL_COUNT];

/*--------------------------- JSON builders -----------------*/
void setConfigSchema()
{
  // Define our config schema
  StaticJsonDocument<1024> json;

  JsonObject channels = json.createNestedObject("channels");
  channels["type"] = "array";
  channels["description"] = "Define what strips are connected where";
  
  JsonObject channelItems = channels.createNestedObject("items");
  channelItems["type"] = "object";

  JsonObject channelProperties = channelItems.createNestedObject("properties");

  if (PWM_CONTROLLER_COUNT > 1)
  {
    JsonObject controller = channelProperties.createNestedObject("controller");
    controller["type"] = "integer";
    controller["minimum"] = 1;
    controller["maximum"] = PWM_CONTROLLER_COUNT;
    controller["description"] = "The index of the PCA controller this strip is connected to";
  }
  
  JsonObject strip = channelProperties.createNestedObject("strip");
  strip["type"] = "integer";
  strip["minimum"] = 1;
  strip["maximum"] = PWM_CHANNEL_COUNT;
  strip["description"] = "Assigns an index to the strip, incrementing from 1, in the order it is wired to the controller";

  JsonObject count = channelProperties.createNestedObject("count");
  count["type"] = "integer";
  count["minimum"] = 1;
  count["maximum"] = MAX_LED_COUNT;
  count["description"] = "Number of channels that the strip uses";

  JsonArray required = channelItems.createNestedArray("required");
  if (PWM_CONTROLLER_COUNT > 1)
  {
    required.add("controller");
  }
  required.add("strip");
  required.add("count");

  JsonObject fadeIntervalUs = json.createNestedObject("fadeIntervalUs");
  fadeIntervalUs["type"] = "integer";
  fadeIntervalUs["minimum"] = 0;
  fadeIntervalUs["description"] = "Default time to fade from off -> on (and vice versa), in microseconds (defaults to 500us)";

  // Pass our config schema down to the hardware library
  oxrs.setConfigSchema(json.as<JsonVariant>());
}

void setCommandSchema()
{
  // Define our command schema
  StaticJsonDocument<1024> json;
  
  JsonObject channels = json.createNestedObject("channels");
  channels["type"] = "array";
  channels["description"] = "Set the levels for each channel on a specified strip. Strip is identified by the index defined when configuring the controller. Levels are a value between 0-255. Optionally set a fade interval for this command, otherwise uses the controller fade interval (defaults to 500us).";
  
  JsonObject channelItems = channels.createNestedObject("items");
  channelItems["type"] = "object";

  JsonObject channelProperties = channelItems.createNestedObject("properties");

  if (PWM_CONTROLLER_COUNT > 1)
  {
    JsonObject controller = channelProperties.createNestedObject("controller");
    controller["type"] = "integer";
    controller["minimum"] = 1;
    controller["maximum"] = PWM_CONTROLLER_COUNT;
  }

  JsonObject strip = channelProperties.createNestedObject("strip");
  strip["type"] = "integer";
  strip["minimum"] = 1;
  strip["maximum"] = PWM_CHANNEL_COUNT;

  JsonObject state = channelProperties.createNestedObject("state");
  state["type"] = "string";
  JsonArray stateEnum = state.createNestedArray("enum");
  stateEnum.add("on");
  stateEnum.add("off");

  JsonObject colour = channelProperties.createNestedObject("colour");
  colour["type"] = "array";
  colour["minItems"] = 1;
  colour["maxItems"] = MAX_LED_COUNT;
  JsonObject colourItems = colour.createNestedObject("items");
  colourItems["type"] = "integer";
  colourItems["minimum"] = 0;
  colourItems["maximum"] = 255;

  JsonObject fadeIntervalUs = channelProperties.createNestedObject("fadeIntervalUs");
  fadeIntervalUs["type"] = "integer";
  fadeIntervalUs["minimum"] = 0;

  JsonArray required = channelItems.createNestedArray("required");
  if (PWM_CONTROLLER_COUNT > 1)
  {
    required.add("controller");
  }
  required.add("strip");

  JsonObject restart = json.createNestedObject("restart");
  restart["type"] = "boolean";
  restart["description"] = "Restart the controller";

  // Pass our command schema down to the hardware library
  oxrs.setCommandSchema(json.as<JsonVariant>());
}

void initialisePwmDrivers()
{
  #if defined(PCA_MODE)
  // Start the I2C bus
  Wire.begin(I2C_SDA, I2C_SCL);
  
  oxrs.println(F("[ledc] scanning for PWM drivers..."));

  for (uint8_t pca = 0; pca < sizeof(PCA_I2C_ADDRESS); pca++)
  {
    oxrs.print(F(" - 0x"));
    oxrs.print(PCA_I2C_ADDRESS[pca], HEX);
    oxrs.print(F("..."));

    // Check if there is anything responding on this address
    Wire.beginTransmission(PCA_I2C_ADDRESS[pca]);
    if (Wire.endTransmission() == 0)
    {
      // Initialise the PCA9685 driver for this address
      bitWrite(g_pwms_found, pca, 1);
      pwmDriver[pca].begin_i2c(PCA_I2C_ADDRESS[pca]);

      oxrs.println(F("PCA9685"));
    }
    else
    {
      oxrs.println(F("empty"));
    }
  }

  #else
  oxrs.println(F("[ledc] using direct PWM control via GPIOs..."));

  for (uint8_t i = 0; i < PWM_CHANNEL_COUNT; i++)
  {
    oxrs.print(F("[ledc]  - GPIO_PWM1 -> "));
    oxrs.println(GPIO_PWM1);
    oxrs.print(F("[ledc]  - GPIO_PWM2 -> "));
    oxrs.println(GPIO_PWM2);
    oxrs.print(F("[ledc]  - GPIO_PWM3 -> "));
    oxrs.println(GPIO_PWM3);
    oxrs.print(F("[ledc]  - GPIO_PWM4 -> "));
    oxrs.println(GPIO_PWM4);
    oxrs.print(F("[ledc]  - GPIO_PWM5 -> "));
    oxrs.println(GPIO_PWM5);
  }

  // Initialise the direct PWM for this address
  bitWrite(g_pwms_found, 0, 1);  
  pwmDriver[0].begin_gpio(GPIO_PWM1, GPIO_PWM2, GPIO_PWM3, GPIO_PWM4, GPIO_PWM5);
  #endif
}

/*--------------------------- LED -----------------*/
void ledFade(PWMDriver * driver, LEDStrip * strip, uint8_t channelOffset, uint8_t colour[])
{
  if ((micros() - strip->lastFadeUs) > strip->fadeIntervalUs)
  {    
    if (strip->channels == 1) 
    {
      driver->crossfade(strip->index, channelOffset, colour[0]);
    }
    else if (strip->channels == 2) 
    {
      driver->crossfade(strip->index, channelOffset, colour[0], colour[1]);
    }
    else if (strip->channels == 3) 
    {
      driver->crossfade(strip->index, channelOffset, colour[0], colour[1], colour[2]);
    }
    else if (strip->channels == 4) 
    {
      driver->crossfade(strip->index, channelOffset, colour[0], colour[1], colour[2], colour[3]);
    }
    else if (strip->channels == 5) 
    {
      driver->crossfade(strip->index, channelOffset, colour[0], colour[1], colour[2], colour[3], colour[4]);
    }  

    strip->lastFadeUs = micros();
  }
}

void initialiseStrips(uint8_t controller)
{
  for (uint8_t strip = 0; strip < PWM_CHANNEL_COUNT; strip++)
  {
    LEDStrip * ledStrip = &ledStrips[controller][strip];

    // .index is immutable and shouldn't be changed
    ledStrip->index = strip;
    ledStrip->channels = 0;
    ledStrip->state = LED_STATE_OFF;

    for (uint8_t colour = 0; colour < MAX_LED_COUNT; colour++)
    {
      ledStrip->colour[colour] = 0;
    }

    ledStrip->fadeIntervalUs = g_fade_interval_us; 
    ledStrip->lastFadeUs = 0L;
  }
}

void processStrips(uint8_t controller)
{
  uint8_t OFF[MAX_LED_COUNT];
  memset(OFF, 0, sizeof(OFF));
  uint16_t pcaState = 0x0;

  PWMDriver *driver = &pwmDriver[controller];

  uint8_t channelOffset = 0;

  for (uint8_t strip = 0; strip < PWM_CHANNEL_COUNT; strip++)
  {
    LEDStrip *ledStrip = &ledStrips[controller][strip];

    if (ledStrip->state == LED_STATE_OFF)
    {
      // off
      ledFade(driver, ledStrip, channelOffset, OFF);
    }
    else if (ledStrip->state == LED_STATE_ON)
    {
      // fade
      ledFade(driver, ledStrip, channelOffset, ledStrip->colour);
    }

    #if defined(OXRS_RACK32)
    // create bitfield of PCA status for screen display
    for (int ch = 0; ch < ledStrip->channels; ch++)
    {
      if ((ledStrip->state == LED_STATE_ON))
      {
        if (ledStrip->colour[ch] > 0)
        {
          bitSet(pcaState, channelOffset + ch);
        }
      }
    }
    #endif

    // increase offset
    channelOffset += ledStrip->channels;
  }

  #if defined(OXRS_RACK32)
  // update port visualisation on screen
  oxrs.updateDisplayPorts(controller, pcaState);
  #endif
}

uint8_t getController(JsonVariant json)
{
  // If only one controller supported then shortcut
  if (PWM_CONTROLLER_COUNT == 1)
    return 1;
  
  if (!json.containsKey("controller"))
  {
    oxrs.println(F("[ledc] missing controller"));
    return 0;
  }
  
  uint8_t controller = json["controller"].as<uint8_t>();

  // Check the controller is valid for this device
  if (controller <= 0 || controller > PWM_CONTROLLER_COUNT)
  {
    oxrs.println(F("[ledc] invalid controller"));
    return 0;
  }

  return controller;
}

uint8_t getStrip(JsonVariant json)
{
  if (!json.containsKey("strip"))
  {
    oxrs.println(F("[ledc] missing strip"));
    return 0;
  }
  
  uint8_t strip = json["strip"].as<uint8_t>();

  // Check the strip is valid for this device
  if (strip <= 0 || strip > PWM_CHANNEL_COUNT)
  {
    oxrs.println(F("[ledc] invalid strip"));
    return 0;
  }

  return strip;
}

void jsonChannelConfig(JsonVariant json)
{
  uint8_t controller = getController(json);
  if (controller == 0) return;

  uint8_t strip = getStrip(json);
  if (strip == 0) return;

  uint8_t count = json["count"].as<uint8_t>();

  // controller/strip indexes are sent 1-based
  LEDStrip * ledStrip = &ledStrips[controller - 1][strip - 1];

  // set the config for this strip
  ledStrip->channels = count;
  ledStrip->state = LED_STATE_OFF;

  for (uint8_t colour = 0; colour < MAX_LED_COUNT; colour++)
  {
    ledStrip->colour[colour] = 0;
  }

  // clear any strip config our new config overwrites
  for (uint8_t i = strip; i < strip + count - 1; i++)
  {
    ledStrip = &ledStrips[controller - 1][i];
    
    ledStrip->channels = 0;
    ledStrip->state = LED_STATE_OFF;

    for (uint8_t colour = 0; colour < MAX_LED_COUNT; colour++)
    {
      ledStrip->colour[colour] = 0;
    }
  }
}

void jsonChannelCommand(JsonVariant json)
{
  uint8_t controller = getController(json);
  if (controller == 0) return;

  uint8_t strip = getStrip(json);
  if (strip == 0) return;

  // controller/strip indexes are sent 1-based
  LEDStrip * ledStrip = &ledStrips[controller - 1][strip - 1];

  if (json.containsKey("state"))
  {
    if (strcmp(json["state"], "on") == 0)
    {
      ledStrip->state = LED_STATE_ON;
    }
    else if (strcmp(json["state"], "off") == 0)
    {
      ledStrip->state = LED_STATE_OFF;
    }
    else 
    {
      oxrs.println(F("[ledc] invalid state"));
    }
  }

  if (json.containsKey("colour"))
  {
    JsonArray array = json["colour"].as<JsonArray>();
    uint8_t colour = 0;
    
    for (JsonVariant v : array)
    {
      ledStrip->colour[colour++] = v.as<uint8_t>();
    }
  }

  if (json.containsKey("fadeIntervalUs"))
  {
    ledStrip->fadeIntervalUs = json["fadeIntervalUs"].as<uint32_t>();
  }
  else
  {
    ledStrip->fadeIntervalUs = g_fade_interval_us;
  }

  #if defined(OXRS_RACK32)
  // update event display on screen 
  char buffer[40];
  sprintf(buffer,"c.%d s.%d ", controller, strip);
  strcat(buffer, ledStrip->state == LED_STATE_ON ? "on" : "off");
  for (int ch = 0; ch < ledStrip->channels; ch++)
  {
    char colour[5];
    sprintf(colour, " %d", ledStrip->colour[ch]);
    strcat (buffer, colour);
  }
  
  // show event with proportional font for more characters per line
  oxrs.getLCD()->showEvent(buffer, FONT_PROP);
  #endif
}

void jsonCommand(JsonVariant json)
{
  if (json.containsKey("channels"))
  {
    for (JsonVariant channel : json["channels"].as<JsonArray>())
    {
      jsonChannelCommand(channel);
    }
  }

  if (json.containsKey("restart") && json["restart"].as<bool>())
  {
    ESP.restart();
  }
}

void jsonConfig(JsonVariant json)
{
  if (json.containsKey("channels"))
  {
    for (JsonVariant channel : json["channels"].as<JsonArray>())
    {
      jsonChannelConfig(channel);
    }
  }

  if (json.containsKey("fadeIntervalUs"))
  {
    g_fade_interval_us = json["fadeIntervalUs"].as<uint32_t>();
  }
}

/*--------------------------- Program -------------------------------*/
void setup()
{
  Serial.begin(SERIAL_BAUD_RATE);
  delay(1000);  
  Serial.println(F("[ledc] starting up..."));

  // Initialise PWM drivers
  initialisePwmDrivers();

  // Initialise LED strips
  for (uint8_t pwm = 0; pwm < PWM_CONTROLLER_COUNT; pwm++)
  {
    if (bitRead(g_pwms_found, pwm) == 0)
      continue;

    initialiseStrips(pwm);
  }

  // Start hardware
  oxrs.begin(jsonConfig, jsonCommand);

  #if defined(OXRS_RACK32)
  oxrs.setDisplayPortLayout(g_pwms_found, PORT_LAYOUT_OUTPUT_AUTO);
  #endif

  // Set up the config/command schema (for self-discovery and adoption)
  setConfigSchema();
  setCommandSchema();
}

void loop()
{
  // Let hardware handle any events etc
  oxrs.loop();

  // Iterate through each PWM controller
  for (uint8_t pwm = 0; pwm < PWM_CONTROLLER_COUNT; pwm++)
  {
    if (bitRead(g_pwms_found, pwm) == 0)
      continue;
    
    processStrips(pwm);
  }
}