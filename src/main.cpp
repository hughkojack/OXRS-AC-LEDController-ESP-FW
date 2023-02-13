/**
  PWM LED controller firmware for the Open eXtensible Rack System

  Documentation:  
    https://oxrs.io/docs/hardware/output-devices/pwm-controllers.html
  
  GitHub repository:
    https://github.com/austinscreations/OXRS-AC-LEDController-ESP-FW

  Copyright 2022 Austins Creations
*/

/*------------------------ Board Type ---------------------------------*/
//#define MCU32
//#define MCU8266 
//#define MCULILY

/*----------------------- Connection Type -----------------------------*/
//#define ETHMODE
//#define WIFIMODE

/*------------------------- PWM Type ----------------------------------*/
//#define PCAMODE           // 16ch PCA9865 PWM controllers (I2C)
//#define GPIOMODE          // 5ch GPIO MOSFETs

/*------------------------- I2C pins ----------------------------------*/
//#define I2C_SDA   0
//#define I2C_SCL   1

//rack32   = 21  22
//LilyGO   = 33  32
//room8266 =  4   5
//D1 mini  =  4   0

/*--------------------------- Macros ----------------------------------*/
#define STRINGIFY(s) STRINGIFY1(s)
#define STRINGIFY1(s) #s

/*--------------------------- Libraries -------------------------------*/
#include <Arduino.h>
#include <Wire.h>                   // For I2C
#include <PubSubClient.h>           // For MQTT
#include <OXRS_MQTT.h>              // For MQTT
#include <OXRS_API.h>               // For REST API
#include <OXRS_SENSORS.h>           // For QWICC I2C sensors
#include <ledPWM.h>                 // For PWM LED controller
#include <WiFiManager.h>            // captive wifi AP config
#include <MqttLogger.h>             // for mqtt and serial logging
#include <LittleFS.h>               // For file system access

#if defined(MCU32)
#include <WiFi.h>                   // For networking
#if defined(ETHMODE)
#include <Ethernet.h>               // For networking
#include <SPI.h>                    // For ethernet
#endif
#endif

#if defined(MCU8266)
#include <ESP8266WiFi.h>            // For networking
#if defined(ETHMODE)
#include <Ethernet.h>               // For networking
#include <SPI.h>                    // For ethernet
#endif
#endif

#if defined(MCULILY)
#include <WiFi.h>                   // For networking
#if defined(ETHMODE)
#include <ETH.h>                    // For networking
#include <SPI.h>                    // For ethernet
#endif
#endif

// LCD screen
#if defined(RACK32)
#include <OXRS_LCD.h>               // For LCD runtime displays
#include "logo.h"                   // Embedded maker logo
#endif

/*--------------------------- Constants ----------------------------------*/
// Serial
#define SERIAL_BAUD_RATE            115200

// REST API
#define REST_API_PORT               80

// Only support up to 5 LEDs per strip
#define MAX_LED_COUNT               5

// Supported LED states
#define LED_STATE_OFF               0
#define LED_STATE_ON                1

// Default fade interval (microseconds)
#define DEFAULT_FADE_INTERVAL_US    500L;

// Number of PWM controllers and channels per controller
#if defined(PCAMODE)
// Each PCA9865 is a PWM controller
// See https://www.nxp.com/docs/en/data-sheet/PCA9685.pdf
const byte    PCA_I2C_ADDRESS[]     = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47 };
const uint8_t PWM_CONTROLLER_COUNT  = sizeof(PCA_I2C_ADDRESS);
// Each PCA9865 has 16 channels
#define PWM_CHANNEL_COUNT           16

#elif defined(GPIOMODE)
// Each GPIO device is a single controller
#define PWM_CONTROLLER_COUNT        1
// Only 5 GPIO channels
#define PWM_CHANNEL_COUNT           5

#endif

// Ethernet
#if defined(ETHMODE)
#define DHCP_TIMEOUT_MS             15000
#define DHCP_RESPONSE_TIMEOUT_MS    4000

#if defined(MCU32)
#define WIZNET_RST_PIN              13
#define ETHERNET_CS_PIN             26

#elif defined(MCU8266)
#define WIZNET_RST_PIN              2
#define ETHERNET_CS_PIN             15

#elif defined(MCULILY)
#define ETH_CLOCK_MODE              ETH_CLOCK_GPIO17_OUT   // Version with not PSRAM
#define ETH_PHY_TYPE                ETH_PHY_LAN8720        // Type of the Ethernet PHY (LAN8720 or TLK110)  
#define ETH_PHY_POWER               -1                     // Pin# of the enable signal for the external crystal oscillator (-1 to disable for internal APLL source)
#define ETH_PHY_MDC                 23                     // Pin# of the I²C clock signal for the Ethernet PHY
#define ETH_PHY_MDIO                18                     // Pin# of the I²C IO signal for the Ethernet PHY
#define ETH_PHY_ADDR                0                      // I²C-address of Ethernet PHY (0 or 1 for LAN8720, 31 for TLK110)
#define ETH_RST_PIN                 5

#endif
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

// stack size counter (for determine used heap size on ESP8266)
char * g_stack_start;

// updateintervall for temp display on screen
uint32_t _tempUpdateMs = 0;

// timer for temp display on screen
uint32_t _lastTempUpdate = 0;

/*--------------------------- Instantiate Global Objects -----------------*/
#if defined(ETHMODE)
#if defined(MCU8266) || defined(MCU32)
    EthernetClient client;
EthernetServer server(REST_API_PORT);
#elif defined(MCULILY)
    WiFiClient client;
WiFiServer server(REST_API_PORT);
#endif
#endif

#if defined(WIFIMODE)
WiFiClient client;
WiFiServer server(REST_API_PORT);
#endif

// MQTT
PubSubClient mqttClient(client);
OXRS_MQTT mqtt(mqttClient);

// REST API
OXRS_API api(mqtt);

// Logging
MqttLogger logger(mqttClient, "log", MqttLoggerMode::MqttAndSerial);

// I2C sensors
OXRS_SENSORS sensors(mqtt);

// PWM LED controllers
PWMDriver pwmDriver[PWM_CONTROLLER_COUNT];

// LED strip config (allow for a max of all single LED strips)
LEDStrip ledStrips[PWM_CONTROLLER_COUNT][PWM_CHANNEL_COUNT];

// LCD screen
#if defined(RACK32)
  #if defined(WIFIMODE)
  OXRS_LCD _screen(WiFi, mqtt);
  #else
  OXRS_LCD _screen(Ethernet, mqtt);
  #endif
#endif

/*--------------------------- JSON builders -----------------*/
uint32_t getStackSize()
{
  char stack;
  return (uint32_t)g_stack_start - (uint32_t)&stack;  
}

void getFirmwareJson(JsonVariant json)
{
  JsonObject firmware = json.createNestedObject("firmware");

  firmware["name"] = FW_NAME;
  firmware["shortName"] = FW_SHORT_NAME;
  firmware["maker"] = FW_MAKER;
  firmware["version"] = STRINGIFY(FW_VERSION);

#if defined(FW_GITHUB_URL)
  firmware["githubUrl"] = FW_GITHUB_URL;
#endif
}

void getSystemJson(JsonVariant json)
{
  JsonObject system = json.createNestedObject("system");

  system["flashChipSizeBytes"] = ESP.getFlashChipSize();
  system["heapFreeBytes"] = ESP.getFreeHeap();

  #if defined(MCU32) || defined(MCULILY)
  system["heapUsedBytes"] = ESP.getHeapSize();
  system["heapMaxAllocBytes"] = ESP.getMaxAllocHeap();
  
  #elif defined(MCU8266)
  system["heapUsedBytes"] = getStackSize();
  
  #endif

  system["sketchSpaceUsedBytes"] = ESP.getSketchSize();
  system["sketchSpaceTotalBytes"] = ESP.getFreeSketchSpace();

  #if defined(MCU32) || defined(MCULILY)
  system["fileSystemUsedBytes"] = LittleFS.usedBytes();
  system["fileSystemTotalBytes"] = LittleFS.totalBytes();

  #elif defined(MCU8266)
  FSInfo fsInfo;
  SPIFFS.info(fsInfo);  
  system["fileSystemUsedBytes"] = fsInfo.usedBytes;
  system["fileSystemTotalBytes"] = fsInfo.totalBytes;

  #endif
}

void getNetworkJson(JsonVariant json)
{
  JsonObject network = json.createNestedObject("network");
  
  #if defined(ETHMODE) && defined(MCULILY)
  network["mode"] = "ethernet";
  network["ip"] = ETH.localIP();
  network["mac"] = ETH.macAddress();

  #else
  byte mac[6];
  
  #if defined(ETHMODE)
  network["mode"] = "ethernet";
  Ethernet.MACAddress(mac);
  network["ip"] = Ethernet.localIP();
  #elif defined(WIFIMODE)
  network["mode"] = "wifi";
  WiFi.macAddress(mac);
  network["ip"] = WiFi.localIP();
  #endif
  
  char mac_display[18];
  sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  network["mac"] = mac_display;
  #endif
}

void getConfigSchemaJson(JsonVariant json)
{
  JsonObject configSchema = json.createNestedObject("configSchema");
  
  // Config schema metadata
  configSchema["$schema"] = JSON_SCHEMA_VERSION;
  configSchema["title"] = FW_SHORT_NAME;
  configSchema["type"] = "object";

  JsonObject properties = configSchema.createNestedObject("properties");

  JsonObject channels = properties.createNestedObject("channels");
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

  #if defined(RACK32)
  // LCD config
  JsonObject activeBrightnessPercent = properties.createNestedObject("activeBrightnessPercent");
  activeBrightnessPercent["title"] = "LCD Active Brightness (%)";
  activeBrightnessPercent["description"] = "Brightness of the LCD when active (defaults to 100%). Must be a number between 0 and 100.";
  activeBrightnessPercent["type"] = "integer";
  activeBrightnessPercent["minimum"] = 0;
  activeBrightnessPercent["maximum"] = 100;

  JsonObject inactiveBrightnessPercent = properties.createNestedObject("inactiveBrightnessPercent");
  inactiveBrightnessPercent["title"] = "LCD Inactive Brightness (%)";
  inactiveBrightnessPercent["description"] = "Brightness of the LCD when in-active (defaults to 10%). Must be a number between 0 and 100.";
  inactiveBrightnessPercent["type"] = "integer";
  inactiveBrightnessPercent["minimum"] = 0;
  inactiveBrightnessPercent["maximum"] = 100;

  JsonObject activeDisplaySeconds = properties.createNestedObject("activeDisplaySeconds");
  activeDisplaySeconds["title"] = "LCD Active Display Timeout (seconds)";
  activeDisplaySeconds["description"] = "How long the LCD remains 'active' after an event is detected (defaults to 10 seconds, setting to 0 disables the timeout). Must be a number between 0 and 600 (i.e. 10 minutes).";
  activeDisplaySeconds["type"] = "integer";
  activeDisplaySeconds["minimum"] = 0;
  activeDisplaySeconds["maximum"] = 600;

  JsonObject eventDisplaySeconds = properties.createNestedObject("eventDisplaySeconds");
  eventDisplaySeconds["title"] = "LCD Event Display Timeout (seconds)";
  eventDisplaySeconds["description"] = "How long the last event is displayed on the LCD (defaults to 3 seconds, setting to 0 disables the timeout). Must be a number between 0 and 600 (i.e. 10 minutes).";
  eventDisplaySeconds["type"] = "integer";
  eventDisplaySeconds["minimum"] = 0;
  eventDisplaySeconds["maximum"] = 600;
#endif

  JsonObject fadeIntervalUs = properties.createNestedObject("fadeIntervalUs");
  fadeIntervalUs["type"] = "integer";
  fadeIntervalUs["minimum"] = 0;
  fadeIntervalUs["description"] = "Default time to fade from off -> on (and vice versa), in microseconds (defaults to 500us)";

  // Add any sensor config
  sensors.setConfigSchema(properties);
}

void getCommandSchemaJson(JsonVariant json)
{
  JsonObject commandSchema = json.createNestedObject("commandSchema");
  
  // Command schema metadata
  commandSchema["$schema"] = JSON_SCHEMA_VERSION;
  commandSchema["title"] = FW_SHORT_NAME;
  commandSchema["type"] = "object";

  JsonObject properties = commandSchema.createNestedObject("properties");
  
  JsonObject channels = properties.createNestedObject("channels");
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

  JsonObject restart = properties.createNestedObject("restart");
  restart["type"] = "boolean";
  restart["description"] = "Restart the controller";

  // Add any sensor commands
  sensors.setCommandSchema(properties);
}

void apiAdopt(JsonVariant json)
{
  // Build device adoption info
  getFirmwareJson(json);
  getSystemJson(json);
  getNetworkJson(json);
  getConfigSchemaJson(json);
  getCommandSchemaJson(json);
}

/*--------------------------- Initialisation -------------------------------*/
void initialiseRestApi(void)
{
  // NOTE: this must be called *after* initialising MQTT since that sets
  //       the default client id, which has lower precendence than MQTT
  //       settings stored in file and loaded by the API

  // Set up the REST API
  api.begin();

  // Register our callbacks
  api.onAdopt(apiAdopt);

  server.begin();
}

#if defined(PCAMODE)
void initialisePwmDrivers()
{
  logger.println(F("[ledc] scanning for PWM drivers..."));

  for (uint8_t pca = 0; pca < sizeof(PCA_I2C_ADDRESS); pca++)
  {
    logger.print(F(" - 0x"));
    logger.print(PCA_I2C_ADDRESS[pca], HEX);
    logger.print(F("..."));

    // Check if there is anything responding on this address
    Wire.beginTransmission(PCA_I2C_ADDRESS[pca]);
    if (Wire.endTransmission() == 0)
    {
      bitWrite(g_pwms_found, pca, 1);

      // Initialise the PCA9685 driver for this address
      pwmDriver[pca].begin_i2c(PCA_I2C_ADDRESS[pca]);

      logger.println(F("PCA9685"));
    }
    else
    {
      logger.println(F("empty"));
    }
  }
}

#elif defined(GPIOMODE)
void initialisePwmDrivers()
{
  bitWrite(g_pwms_found, 0, 1);  

  #if defined(BULB)
  pwmDriver[0].begin_gpio(4,12,14,5,13);
  #elif defined(MCU8266)
  pwmDriver[0].begin_gpio(15,13,12,14,5);
  #elif defined(MCULILY)
  pwmDriver[0].begin_gpio(14,04,12,15,16);
  #endif
}

#endif

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

    #if defined(RACK32)
    // create bitfield of PCA status for screen display
    for (int ch = 0; ch < ledStrip->channels; ch++)
    {
      if ((ledStrip->state == LED_STATE_ON))
      {
        if (ledStrip->colour[ch] > 0)
          bitSet(pcaState, channelOffset + ch);
      }
    }
    #endif

    // increase offset
    channelOffset += ledStrip->channels;
  }

  #if defined(RACK32)
  // update port visualisation on screen
  _screen.process(controller, pcaState);
  #endif
}

/*--------------------------- MQTT/API -----------------*/
void mqttConnected() 
{
  // MqttLogger doesn't copy the logging topic to an internal
  // buffer so we have to use a static array here
  static char logTopic[64];
  logger.setTopic(mqtt.getLogTopic(logTopic));

  // Publish device adoption info
  DynamicJsonDocument json(JSON_ADOPT_MAX_SIZE);
  mqtt.publishAdopt(api.getAdopt(json.as<JsonVariant>()));

  // Log the fact we are now connected
  logger.println("[ledc] mqtt connected");
}

void mqttDisconnected(int state) 
{
  // Log the disconnect reason
  // See https://github.com/knolleary/pubsubclient/blob/2d228f2f862a95846c65a8518c79f48dfc8f188c/src/PubSubClient.h#L44
  switch (state)
  {
    case MQTT_CONNECTION_TIMEOUT:
      logger.println(F("[ledc] mqtt connection timeout"));
      break;
    case MQTT_CONNECTION_LOST:
      logger.println(F("[ledc] mqtt connection lost"));
      break;
    case MQTT_CONNECT_FAILED:
      logger.println(F("[ledc] mqtt connect failed"));
      break;
    case MQTT_DISCONNECTED:
      logger.println(F("[ledc] mqtt disconnected"));
      break;
    case MQTT_CONNECT_BAD_PROTOCOL:
      logger.println(F("[ledc] mqtt bad protocol"));
      break;
    case MQTT_CONNECT_BAD_CLIENT_ID:
      logger.println(F("[ledc] mqtt bad client id"));
      break;
    case MQTT_CONNECT_UNAVAILABLE:
      logger.println(F("[ledc] mqtt unavailable"));
      break;
    case MQTT_CONNECT_BAD_CREDENTIALS:
      logger.println(F("[ledc] mqtt bad credentials"));
      break;      
    case MQTT_CONNECT_UNAUTHORIZED:
      logger.println(F("[ledc] mqtt unauthorised"));
      break;      
  }
}

uint8_t getController(JsonVariant json)
{
  // If only one controller supported then shortcut
  if (PWM_CONTROLLER_COUNT == 1)
    return 1;
  
  if (!json.containsKey("controller"))
  {
    logger.println(F("[ledc] missing controller"));
    return 0;
  }
  
  uint8_t controller = json["controller"].as<uint8_t>();

  // Check the controller is valid for this device
  if (controller <= 0 || controller > PWM_CONTROLLER_COUNT)
  {
    logger.println(F("[ledc] invalid controller"));
    return 0;
  }

  return controller;
}

uint8_t getStrip(JsonVariant json)
{
  if (!json.containsKey("strip"))
  {
    logger.println(F("[ledc] missing strip"));
    return 0;
  }
  
  uint8_t strip = json["strip"].as<uint8_t>();

  // Check the strip is valid for this device
  if (strip <= 0 || strip > PWM_CHANNEL_COUNT)
  {
    logger.println(F("[ledc] invalid strip"));
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
      logger.println(F("[ledc] invalid state"));
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

  #if defined(RACK32)
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
  // show event with proportional font for mor chracters per line
  _screen.showEvent(buffer, FONT_PROP);
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

  // Let the sensors handle any commands
  sensors.cmnd(json);
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

  #if defined(RACK32)
  // LCD config
  if (json.containsKey("activeBrightnessPercent"))
  {
    _screen.setBrightnessOn(json["activeBrightnessPercent"].as<int>());
  }

  if (json.containsKey("inactiveBrightnessPercent"))
  {
    _screen.setBrightnessDim(json["inactiveBrightnessPercent"].as<int>());
  }

  if (json.containsKey("activeDisplaySeconds"))
  {
    _screen.setOnTimeDisplay(json["activeDisplaySeconds"].as<int>());
  }

  if (json.containsKey("eventDisplaySeconds"))
  {
    _screen.setOnTimeEvent(json["eventDisplaySeconds"].as<int>());
  }

  // get the sensor update intervall for screen.showTemp()
  if (json.containsKey("sensorUpdateSeconds"))
  {
    _tempUpdateMs = json["sensorUpdateSeconds"].as<uint32_t>() * 1000L;
    if (_tempUpdateMs == 0L)
    {
      // wipes recent temp display on screen
      _screen.hideTemp();
    }
  }
#endif

  // Let the sensors handle any config
  sensors.conf(json);
}

void mqttCallback(char * topic, uint8_t * payload, unsigned int length) 
{
  #if defined(RACK32)
  // Update screen
  _screen.triggerMqttRxLed();
  #endif
 
  // Pass this message down to our MQTT handler
  mqtt.receive(topic, payload, length);
}

void initialiseMqtt(byte * mac)
{
  // Set the default client id to the last 3 bytes of the MAC address
  char clientId[32];
  sprintf_P(clientId, PSTR("%02x%02x%02x"), mac[3], mac[4], mac[5]);  
  mqtt.setClientId(clientId);
  
  // Register our callbacks
  mqtt.onConnected(mqttConnected);
  mqtt.onDisconnected(mqttDisconnected);
  mqtt.onConfig(jsonConfig);
  mqtt.onCommand(jsonCommand);  

  // Start listening for MQTT messages
  mqttClient.setCallback(mqttCallback);  
}

/*--------------------------- Network -------------------------------*/
#if defined(WIFIMODE)
void initialiseWifi()
{
  // Ensure we are in the correct WiFi mode
  WiFi.mode(WIFI_STA);

  // Get WiFi base MAC address
  byte mac[6];
  WiFi.macAddress(mac);

  // Display the MAC address on serial
  char mac_display[18];
  sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  logger.print(F("[ledc] mac address: "));
  logger.println(mac_display);

  // Update OLED display
  sensors.oled(mac);

  // Connect using saved creds, or start captive portal if none found
  // Blocks until connected or the portal is closed
  WiFiManager wm;
  if (!wm.autoConnect("OXRS_WiFi", "superhouse"))
  {
    // If we are unable to connect then restart
    ESP.restart();
  }

  // Display IP address on serial
  logger.print(F("[ledc] ip address: "));
  logger.println(WiFi.localIP());

  // Update OLED display
  sensors.oled(WiFi.localIP());

  // Set up MQTT (don't attempt to connect yet)
  initialiseMqtt(mac);

  // Set up the REST API once we have an IP address
  initialiseRestApi();
}
#endif

#if defined(ETHMODE)
#if defined(MCULILY)
void ethernetEvent(WiFiEvent_t event)
{
  // Log the event to serial for debugging
  switch (event)
  {
    case ARDUINO_EVENT_ETH_START:
      // Get the ethernet MAC address
      byte mac[6];
      ETH.macAddress(mac);

      // Display the MAC address on serial
      char mac_display[18];
      sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      logger.print(F("[ledc] mac address: "));
      logger.println(mac_display);

      // Update OLED display
      sensors.oled(mac);

      // Set up MQTT (don't attempt to connect yet)
      initialiseMqtt(mac);
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      // Get the IP address assigned by DHCP
      IPAddress ip = ETH.localIP();

      logger.print(F("[ledc] ip address: "));
      logger.println(ip);

      // Update OLED display
      sensors.oled(ip);
      
      // Set up the REST API once we have an IP address
      initialiseRestApi();
      break;
  }
}
#endif

void initialiseEthernet()
{
  #if defined(MCULILY)
  // We continue initialisation inside this event handler
  WiFi.onEvent(ethernetEvent);

  // Reset the Ethernet PHY
  pinMode(ETH_RST_PIN, OUTPUT);
  digitalWrite(ETH_RST_PIN, 0);
  delay(200);
  digitalWrite(ETH_RST_PIN, 1);
  delay(200);
  digitalWrite(ETH_RST_PIN, 0);
  delay(200);
  digitalWrite(ETH_RST_PIN, 1);

  // Start the Ethernet PHY and wait for events
  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLOCK_MODE);
  
  #elif defined(MCU8266) || defined(MCU32)
  // Get ESP base MAC address
  byte mac[6];
  WiFi.macAddress(mac);
  
  // Ethernet MAC address is base MAC + 3
  // See https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/system.html#mac-address
  mac[5] += 3;

  // Display the MAC address on serial
  char mac_display[18];
  sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  logger.print(F("[ledc] mac address: "));
  logger.println(mac_display);

  // Update OLED display
  sensors.oled(mac);

  // Initialise ethernet library
  Ethernet.init(ETHERNET_CS_PIN);

  // Reset Wiznet W5500
  pinMode(WIZNET_RST_PIN, OUTPUT);
  digitalWrite(WIZNET_RST_PIN, HIGH);
  delay(250);
  digitalWrite(WIZNET_RST_PIN, LOW);
  delay(50);
  digitalWrite(WIZNET_RST_PIN, HIGH);
  delay(350);

  // Get an IP address via DHCP
  logger.print(F("[ledc] ip address: "));
  if (!Ethernet.begin(mac, DHCP_TIMEOUT_MS, DHCP_RESPONSE_TIMEOUT_MS))
  {
    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
      logger.println(F("ethernet shield not found"));
    } else if (Ethernet.linkStatus() == LinkOFF) {
      logger.println(F("ethernet cable not connected"));
    } else {
      logger.println(F("failed to setup ethernet using DHCP"));
    }
    return;
  }
  
  // Display IP address on serial
  logger.println(Ethernet.localIP());

  // Update OLED display
  sensors.oled(Ethernet.localIP());

  // Set up MQTT (don't attempt to connect yet)
  initialiseMqtt(mac);

  // Set up the REST API once we have an IP address
  initialiseRestApi();

  #endif
}
#endif

void initialiseSerial()
{
  Serial.begin(SERIAL_BAUD_RATE);
  delay(1000);
  
  logger.println(F("[ledc ] starting up..."));

  DynamicJsonDocument json(128);
  getFirmwareJson(json.as<JsonVariant>());

  logger.print(F("[ledc ] "));
  serializeJson(json, logger);
  logger.println();
}

#if defined(RACK32)
void _initialiseScreen(void)
{
  // Initialise the LCD
  _screen.begin();

  // Display the firmware and logo (either from SPIFFS or PROGMEM)
  int returnCode = _screen.drawHeader(FW_SHORT_NAME, FW_MAKER, STRINGIFY(FW_VERSION), "ESP32", FW_LOGO);

  switch (returnCode)
  {
  case LCD_INFO_LOGO_FROM_SPIFFS:
      logger.println(F("[ledc] logo loaded from SPIFFS"));
      break;
  case LCD_INFO_LOGO_FROM_PROGMEM:
      logger.println(F("[ledc] logo loaded from PROGMEM"));
      break;
  case LCD_INFO_LOGO_DEFAULT:
      logger.println(F("[ledc] no logo found, using default OXRS logo"));
      break;
  case LCD_ERR_NO_LOGO:
      logger.println(F("[ledc] no logo found"));
      break;
  }
}

void updateTempSensor(void)
{
  // Ignore if temp sensor not found or has been disabled
  if (_tempUpdateMs == 0)
  {
      return;
  }

  // Check if we need to get a new temp reading and update
  if ((millis() - _lastTempUpdate) > _tempUpdateMs)
  {
      // Read temp from onboard sensor
      float temperature = sensors.getTemperatureValue();
      if (!isnan(temperature))
      {
        // Display temp on screen
        _screen.showTemp(temperature, sensors.getTemperatureUnits() == TEMP_C ? 'C' : 'F');
      }

      // Reset our timer
      _lastTempUpdate = millis();
  }
}

#endif

/*--------------------------- Program -------------------------------*/
void setup()
{
  // Store the address of the stack at startup so we can determine
  // the stack size at runtime (see getStackSize())
  char stack;
  g_stack_start = &stack;

  // Set up serial
  initialiseSerial();  

  // Start the I2C bus
  Wire.begin(I2C_SDA, I2C_SCL);
  
  // Start the sensor library (scan for attached sensors)
  sensors.begin();

  // Initialise PWM drivers
  initialisePwmDrivers();

  // Initialise LED strips
  for (uint8_t pwm = 0; pwm < PWM_CONTROLLER_COUNT; pwm++)
  {
    if (bitRead(g_pwms_found, pwm) == 0)
      continue;

    initialiseStrips(pwm);
  }

  #if defined(RACK32)
  _initialiseScreen();
  _screen.drawPorts(PORT_LAYOUT_OUTPUT_AUTO, g_pwms_found);
  #endif

  // Set up network/MQTT/REST API
  #if defined(WIFIMODE)
  initialiseWifi();
  #elif defined(ETHMODE)
  initialiseEthernet();
  #endif
}

void loop()
{
  // Check our MQTT broker connection is still ok
  mqtt.loop();

  // Maintain DHCP lease
  #if defined(ETHMODE) && not defined(MCULILY)
  Ethernet.maintain();
  #endif
  
  // Handle any API requests
  #if defined(WIFIMODE) || defined(MCULILY)
  WiFiClient client = server.available();
  api.loop(&client);
  #elif defined(ETHMODE)
  EthernetClient client = server.available();
  api.loop(&client);
  #endif

  // Iterate through each PWM controller
  for (uint8_t pwm = 0; pwm < PWM_CONTROLLER_COUNT; pwm++)
  {
    if (bitRead(g_pwms_found, pwm) == 0)
      continue;
    
    processStrips(pwm);
  }

  // update OLED
  sensors.oled();
  
  // publish sensor telemetry
  sensors.tele();

  #if defined(RACK32)
  // Update screen
  _screen.loop();

  // update temperature display
  updateTempSensor();
  #endif
}