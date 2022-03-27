/**
  PWM LED controller firmware for the Open eXtensible Rack System
  
  See https://oxrs.io/docs/hardware/output-devices/pwm-controllers.html for documentation.
  Compile options:
    ESP32   (rack32)     - wifi and ethernet
    ESP32   (LilyGO POE) - wifi and ethernet
    ESP8266 (D1 Mini)    - wifi
    ESP8266 (room8266)   - wifi and ethernet
    
  External dependencies. Install using the Arduino library manager:
    "PubSubClient" by Nick O'Leary
    "OXRS-IO-MQTT-ESP32-LIB" by OXRS Core Team
    "OXRS-IO-API-ESP32-LIB" by OXRS Core Team
    "OXRS-AC-I2CSensors-ESP-LIB" by Austins Creations
    "ledPWM" by Austins Creations
      
  GitHub repository:
   
    
  Bugs/Features:
    See GitHub issues list
  Copyright 2021 Austins Creations
*/

#include <Arduino.h>

/*------------------------ Board Type ---------------------------------*/
// #define MCU8266 
// #define MCU32
// #define MCULILY

/*----------------------- Connection Type -----------------------------*/
// select connection mode here - comment / uncomment the one needed
// #define ETHMODE    // uses ethernet
// #define WIFIMODE   // uses wifi

/*----------------------- Modetype Type -------------------------------*/
// #define GPIOMODE // uses 5ch GPIO mosfet control
// #define PCAMODE  // uses I2C PCA controller

/*------------------------- I2C pins ----------------------------------*/
//#define I2C_SDA   0
//#define I2C_SCL   1

//rack32   = 21  22
//LilyGO   = 33  32
//room8266 =  4   5
//D1 mini  =  4   0

/*--------------------------- Version ---------------------------------*/
#define FW_NAME       "OXRS-AC-LedController-ESP-FW"
#define FW_SHORT_NAME "LED Controller"
#define FW_MAKER      "Austin's Creations"
#define FW_VERSION    "2.0.0"

/*--------------------------- Libraries -------------------------------*/

#if defined(MCU8266)
#include <ESP8266WiFi.h>            // For networking
#if defined(ETHMODE)
  #include <SPI.h>                  // for ethernet
  #include <Ethernet.h>             // For networking
#endif
#endif

#if defined(MCULILY)
#include <WiFi.h>                   // For networking
#if defined(ETHMODE)
  #include <SPI.h>                  // for ethernet
  #include <ETH.h>                  // For networking
  #include "esp_system.h"
  #include "esp_eth.h"
#endif
#endif

#if defined(MCU32)
#include <WiFi.h>                   // For networking
#if defined(ETHMODE)
#endif
#endif

#include <Wire.h>                   // For I2C
#include <PubSubClient.h>           // For MQTT
#include <OXRS_MQTT.h>              // For MQTT
#include <OXRS_API.h>               // For REST API
#include <OXRS_SENSORS.h>           // For QWICC I2C sensors
#include <ledPWM.h>                 // For PWM LED controller
#include <WiFiManager.h>            // captive wifi AP config

/*--------------------------- Constants ----------------------------------*/
// Serial
#define SERIAL_BAUD_RATE            115200

// REST API
#define REST_API_PORT               80

// Number of channels on the device
#define PWM_CHANNEL_COUNT           5

// Only support up to 5 LEDs per strip
#define MAX_LED_COUNT               5

// Supported LED modes
#define LED_MODE_NONE               0
#define LED_MODE_COLOUR             1
#define LED_MODE_FADE               2
#define LED_MODE_FLASH              3

// Default fade interval (microseconds)
#define DEFAULT_FADE_INTERVAL_US    500L;

// Ethernet
#if defined(ETHMODE)
#if defined(MCU8266)
#define WIZNET_RST_PIN              2
#define ETHERNET_CS_PIN             15
#define DHCP_TIMEOUT_MS             15000
#define DHCP_RESPONSE_TIMEOUT_MS    4000
#endif
#if defined(MCULILY)
#define ETH_CLOCK_MODE              ETH_CLOCK_GPIO17_OUT   // Version with not PSRAM
#define ETH_PHY_TYPE                ETH_PHY_LAN8720        // Type of the Ethernet PHY (LAN8720 or TLK110)  
#define ETH_PHY_POWER               -1                     // Pin# of the enable signal for the external crystal oscillator (-1 to disable for internal APLL source)
#define ETH_PHY_MDC                 23                     // Pin# of the I²C clock signal for the Ethernet PHY
#define ETH_PHY_MDIO                18                     // Pin# of the I²C IO signal for the Ethernet PHY
#define ETH_PHY_ADDR                0                      // I²C-address of Ethernet PHY (0 or 1 for LAN8720, 31 for TLK110)
#define ETH_RST_PIN                 5
#endif
#if defined(MCU32)
#define WIZNET_RST_PIN              13
#define ETHERNET_CS_PIN             26
#define DHCP_TIMEOUT_MS             15000
#define DHCP_RESPONSE_TIMEOUT_MS    4000
#endif
#endif

#if defined(PCAMODE)
// PCA9865 - https://www.nxp.com/docs/en/data-sheet/PCA9685.pdf */
const byte    PCA_I2C_ADDRESS[]     = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47 };
const uint8_t PCA_COUNT             = sizeof(PCA_I2C_ADDRESS);

// Each PCA9865 has 16 channels
#define PCA_CHANNEL_COUNT           16
#endif

/*-------------------------- Internal datatypes --------------------------*/
struct LEDStrip
{
  uint8_t index;
  uint8_t channels;
  uint8_t mode;
  uint8_t colour[MAX_LED_COUNT];

  uint32_t lastCrossfade;
};

/*--------------------------- Global Variables ---------------------------*/
#if defined(PCAMODE)
// Each bit corresponds to a PCA9865 found on the IC2 bus
uint8_t g_pcas_found = 0;
#endif

// How long between each fade step
uint32_t g_fade_interval_us = DEFAULT_FADE_INTERVAL_US;

// Flashing state for any strips in flash mode
uint8_t g_flash_state = LOW;

// For saving mac address
byte mac[6];

/*--------------------------- Instantiate Global Objects -----------------*/
// client 
#if defined(ETHMODE)
#if defined(MCU8266) || defined(MCU32)
EthernetClient client;
EthernetServer server(REST_API_PORT);
#endif
#if defined(MCULILY)
WiFiClient client;
WiFiServer server(REST_API_PORT);
#endif
#endif

#if defined(WIFIMODE)
#if defined(MCULILY) || defined(MCU8266) || defined(MCU32)
WiFiManager wm; 
WiFiClient client;
WiFiServer server(REST_API_PORT);
#endif
#endif

#if defined(MCU8266) || defined(MCU32) || defined(MCULILY)
PubSubClient mqttClient(client);
OXRS_MQTT mqtt(mqttClient);
OXRS_API api(mqtt);
OXRS_SENSORS sensors(mqtt);
#endif

#if defined(PCAMODE)
// PWM LED controllers
PWMDriver pwmDriver[PCA_COUNT];

// LED strip config (allow for a max of all single LED strips)
LEDStrip ledStrips[PCA_COUNT][PCA_CHANNEL_COUNT];
#endif

#if defined(GPIOMODE)
// PWM LED controller
PWMDriver pwmDriver;

// LED strip config (allow for a max of all single LED strips)
LEDStrip ledStrips[PWM_CHANNEL_COUNT];
#endif

/*--------------------------- LilyGO MAC Address -----------------*/
#if defined(MCULILY)
uint8_t * LILYmacAddress(uint8_t* mac)
{
    esp_eth_get_mac(mac);
    return mac;
}
#endif

/*--------------------------- Adoption Routines -----------------*/

#if defined(MCU8266) || defined(MCU32) || defined(MCULILY)
/**
  Adoption info builders
*/
void getFirmwareJson(JsonVariant json)
{
  JsonObject firmware = json.createNestedObject("firmware");

  firmware["name"] = FW_NAME;
  firmware["shortName"] = FW_SHORT_NAME;
  firmware["maker"] = FW_MAKER;
  firmware["version"] = FW_VERSION;
}

void getMemoryJson(JsonVariant json)
{
  JsonObject memory = json.createNestedObject("memory");

  memory["sketchSizeBytes"] = ESP.getSketchSize();
  memory["freeSketchSpaceBytes"] = ESP.getFreeSketchSpace();
  memory["flashChipSizeBytes"] = ESP.getFlashChipSize();
  #if defined(MCU8266)
  FSInfo fs_info;
  LittleFS.info(fs_info);
  memory["fileSystemSizeBytes"] = fs_info.totalBytes;
  #elif defined(MCU32) || defined(MCULILY)
  memory["fileSystemSizeBytes"] = SPIFFS.totalBytes();
  #endif
}

void getNetworkJson(JsonVariant json)
{
  #if defined(MCULILY) && defined(ETHMODE)
  LILYmacAddress(mac);
  #endif
  #if defined(MCU8266) || defined(MCU32) || defined(MCULILY) && defined(WIFIMODE)
  WiFi.macAddress(mac);
  #endif
  
  char mac_display1[18];
  sprintf_P(mac_display1, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  JsonObject network = json.createNestedObject("network");
  
  #if defined(ETHMODE)
  #if defined(MCULILY)
   network["ip"] = ETH.localIP();
  #endif
  #if defined(MCU32) || defined(MCU8266)
   network["ip"] = Ethernet.localIP();
  #endif
  #endif
  #if defined(WIFIMODE) 
   network["ip"] = WiFi.localIP();
  #endif
  network["mac"] = mac_display1;
}

void getConfigSchemaJson(JsonVariant json)
{
  JsonObject configSchema = json.createNestedObject("configSchema");
  
  // Config schema metadata
  configSchema["$schema"] = JSON_SCHEMA_VERSION;
  configSchema["title"] = FW_NAME;
  configSchema["type"] = "object";

  JsonObject properties = configSchema.createNestedObject("properties");

  JsonObject channels = properties.createNestedObject("channels");
  channels["type"] = "array";
  
  JsonObject channelItems = channels.createNestedObject("items");
  channelItems["type"] = "object";

  JsonObject channelProperties = channelItems.createNestedObject("properties");

  #if defined(GPIOMODE)
  JsonObject strip = channelProperties.createNestedObject("strip");
  strip["type"] = "integer";
  strip["minimum"] = 1;
  strip["maximum"] = PWM_CHANNEL_COUNT;
  #endif

  #if defined(PCAMODE)
  JsonObject controller = channelProperties.createNestedObject("controller");
  controller["type"] = "integer";
  controller["minimum"] = 1;
  controller["maximum"] = PCA_COUNT;

  JsonObject strip = channelProperties.createNestedObject("strip");
  strip["type"] = "integer";
  strip["minimum"] = 1;
  strip["maximum"] = PCA_CHANNEL_COUNT;
  #endif

  JsonObject count = channelProperties.createNestedObject("count");
  count["type"] = "integer";
  count["minimum"] = 0;
  count["maximum"] = MAX_LED_COUNT;

  JsonArray required = channelItems.createNestedArray("required");
  #if defined(PCAMODE)
   required.add("controller");
  #endif
  required.add("strip");
  required.add("count");

  JsonObject fadeIntervalUs = properties.createNestedObject("fadeIntervalUs");
  fadeIntervalUs["type"] = "integer";
  fadeIntervalUs["minimum"] = 0;

  // Add any sensor config
  sensors.setConfigSchema(properties);
}

void getCommandSchemaJson(JsonVariant json)
{
  JsonObject commandSchema = json.createNestedObject("commandSchema");
  
  // Command schema metadata
  commandSchema["$schema"] = JSON_SCHEMA_VERSION;
  commandSchema["title"] = FW_NAME;
  commandSchema["type"] = "object";

  JsonObject properties = commandSchema.createNestedObject("properties");
  
  JsonObject channels = properties.createNestedObject("channels");
  channels["type"] = "array";
  
  JsonObject channelItems = channels.createNestedObject("items");
  channelItems["type"] = "object";

  JsonObject channelProperties = channelItems.createNestedObject("properties");

  #if defined(GPIOMODE)
  JsonObject strip = channelProperties.createNestedObject("strip");
  strip["type"] = "integer";
  strip["minimum"] = 1;
  strip["maximum"] = PWM_CHANNEL_COUNT;
  #endif

  #if defined(PCAMODE)
  JsonObject controller = channelProperties.createNestedObject("controller");
  controller["type"] = "integer";
  controller["minimum"] = 1;
  controller["maximum"] = PCA_COUNT;

  JsonObject strip = channelProperties.createNestedObject("strip");
  strip["type"] = "integer";
  strip["minimum"] = 1;
  strip["maximum"] = PCA_CHANNEL_COUNT;
  #endif

  JsonObject mode = channelProperties.createNestedObject("mode");
  mode["type"] = "string";
  JsonArray modeEnum = mode.createNestedArray("enum");
  modeEnum.add("colour");
  modeEnum.add("fade");
  modeEnum.add("flash");

  JsonObject colour = channelProperties.createNestedObject("colour");
  colour["type"] = "array";
  colour["minItems"] = 1;
  colour["maxItems"] = MAX_LED_COUNT;
  JsonObject colourItems = colour.createNestedObject("items");
  colourItems["type"] = "integer";

  JsonArray required = channelItems.createNestedArray("required");
  #if defined(PCAMODE)
   required.add("controller");
  #endif
  required.add("strip");

  JsonObject flash = properties.createNestedObject("flash");
  flash["type"] = "boolean";

  JsonObject restart = properties.createNestedObject("restart");
  restart["type"] = "boolean";

  // Add any sensor commands
  sensors.setCommandSchema(properties);
}

/**
  API callbacks
*/
void apiAdopt(JsonVariant json)
{
  // Build device adoption info
  getFirmwareJson(json);
  getMemoryJson(json);
  getNetworkJson(json);
  getConfigSchemaJson(json);
  getCommandSchemaJson(json);
}

/*
 REST API
 */
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
#endif

/*--------------------------- Base MCU Routines ------------------*/

#if defined(PCAMODE)
/**
  I2C bus
 */
void scanI2CBus()
{
  Serial.println(F("[ledc] scanning for PWM drivers..."));

  for (uint8_t pca = 0; pca < PCA_COUNT; pca++)
  {
    Serial.print(F(" - 0x"));
    Serial.print(PCA_I2C_ADDRESS[pca], HEX);
    Serial.print(F("..."));

    // Check if there is anything responding on this address
    Wire.beginTransmission(PCA_I2C_ADDRESS[pca]);
    if (Wire.endTransmission() == 0)
    {
      bitWrite(g_pcas_found, pca, 1);

      // Initialise the PCA9685 driver for this address
      pwmDriver[pca].begin_i2c(PCA_I2C_ADDRESS[pca]);

      Serial.println(F("PCA9685"));
    }
    else
    {
      Serial.println(F("empty"));
    }
  }
}
#endif

/*--------------------------- LED Routines -----------------*/

#if defined(MCU8266) || defined(MCU32) || defined(MCULILY)

void initialiseStrips(uint8_t pca)
{
  #if defined(PCAMODE)
  for (uint8_t strip = 0; strip < PCA_CHANNEL_COUNT; strip++)
  {
    LEDStrip * ledStrip = &ledStrips[pca][strip];

    // .index is immutable and shouldn't be changed
    ledStrip->index = strip;
    ledStrip->channels = 0;
    ledStrip->mode = LED_MODE_NONE;
    ledStrip->colour[MAX_LED_COUNT] = {};
    
    ledStrip->lastCrossfade = 0L;
  }
  #endif
  #if defined(GPIOMODE)
  for (uint8_t strip = 0; strip < PWM_CHANNEL_COUNT; strip++)
  {
    LEDStrip * ledStrip = &ledStrips[strip];

    // .index is immutable and shouldn't be changed
    ledStrip->index = strip;
    ledStrip->channels = 0;
    ledStrip->mode = LED_MODE_NONE;
    ledStrip->colour[MAX_LED_COUNT] = {};
    
    ledStrip->lastCrossfade = 0L;
  }
  #endif
}

void ledColour(PWMDriver * driver, LEDStrip * strip, uint8_t channelOffset)
{
  if (strip->channels == 1) 
  {
    driver->colour(strip->index, channelOffset, strip->colour[0]);
  }
  else if (strip->channels == 2) 
  {
    driver->colour(strip->index, channelOffset, strip->colour[0], strip->colour[1]);
  }
  else if (strip->channels == 3) 
  {
    driver->colour(strip->index, channelOffset, strip->colour[0], strip->colour[1], strip->colour[2]);
  }
  else if (strip->channels == 4) 
  {
    driver->colour(strip->index, channelOffset, strip->colour[0], strip->colour[1], strip->colour[2], strip->colour[3]);
  }
  else if (strip->channels == 5) 
  {
    driver->colour(strip->index, channelOffset, strip->colour[0], strip->colour[1], strip->colour[2], strip->colour[3], strip->colour[4]);
  }  
}

void ledFade(PWMDriver * driver, LEDStrip * strip, uint8_t channelOffset)
{
  if ((micros() - strip->lastCrossfade) > g_fade_interval_us)
  {    
    if (strip->channels == 1) 
    {
      driver->crossfade(strip->index, channelOffset, strip->colour[0]);
    }
    else if (strip->channels == 2) 
    {
      driver->crossfade(strip->index, channelOffset, strip->colour[0], strip->colour[1]);
    }
    else if (strip->channels == 3) 
    {
      driver->crossfade(strip->index, channelOffset, strip->colour[0], strip->colour[1], strip->colour[2]);
    }
    else if (strip->channels == 4) 
    {
      driver->crossfade(strip->index, channelOffset, strip->colour[0], strip->colour[1], strip->colour[2], strip->colour[3]);
    }
    else if (strip->channels == 5) 
    {
      driver->crossfade(strip->index, channelOffset, strip->colour[0], strip->colour[1], strip->colour[2], strip->colour[3], strip->colour[4]);
    }  

    strip->lastCrossfade = micros();
  }
}

void ledFlash(PWMDriver * driver, LEDStrip * strip, uint8_t channelOffset)
{
  if (strip->channels == 1) 
  {
    if (g_flash_state == HIGH)
    {
      driver->colour(strip->index, channelOffset, strip->colour[0]);
    }
    else
    {
      driver->colour(strip->index, channelOffset, 0);
    }
  }
  else if (strip->channels == 2) 
  {
    if (g_flash_state == HIGH)
    {
      driver->colour(strip->index, channelOffset, strip->colour[0], strip->colour[1]);
    }
    else
    {
      driver->colour(strip->index, channelOffset, 0, 0);
    }
  }
  else if (strip->channels == 3) 
  {
    if (g_flash_state == HIGH)
    {
      driver->colour(strip->index, channelOffset, strip->colour[0], strip->colour[1], strip->colour[2]);
    }
    else
    {
      driver->colour(strip->index, channelOffset, 0, 0, 0);
    }
  }
  else if (strip->channels == 4) 
  {
    if (g_flash_state == HIGH)
    {
      driver->colour(strip->index, channelOffset, strip->colour[0], strip->colour[1], strip->colour[2], strip->colour[3]);
    }
    else
    {
      driver->colour(strip->index, channelOffset, 0, 0, 0, 0);
    }
  }
  else if (strip->channels == 5) 
  {
    if (g_flash_state == HIGH)
    {
      driver->colour(strip->index, channelOffset, strip->colour[0], strip->colour[1], strip->colour[2], strip->colour[3], strip->colour[4]);
    }
    else
    {
      driver->colour(strip->index, channelOffset, 0, 0, 0, 0, 0);
    }
  }  
}

void processStrips(uint8_t pca)
{
  uint8_t channelOffset = 0;

  #if defined(GPIOMODE)
  PWMDriver * driver = &pwmDriver;

  for (uint8_t strip = 0; strip < PWM_CHANNEL_COUNT; strip++)
  {
    LEDStrip * ledStrip = &ledStrips[strip];

  #endif
  #if defined(PCAMODE)
  PWMDriver * driver = &pwmDriver[pca];
  
  for (uint8_t strip = 0; strip < PCA_CHANNEL_COUNT; strip++)
  {
    LEDStrip * ledStrip = &ledStrips[pca][strip];
  #endif

    if (ledStrip->mode == LED_MODE_COLOUR)
    {
      // colour
      ledColour(driver, ledStrip, channelOffset);
    }
    else if (ledStrip->mode == LED_MODE_FADE)
    {
      // fade
      ledFade(driver, ledStrip, channelOffset);
    }
    else if (ledStrip->mode == LED_MODE_FLASH)
    {
      // flash
      ledFlash(driver, ledStrip, channelOffset);
    }
  
    // increase offset
    channelOffset += ledStrip->channels;
  }
}
#endif

/*--------------------------- API / MQTT Callbacks -----------------*/

#if defined(MCU8266) || defined(MCU32) || defined(MCULILY)
/**
  MQTT callbacks
*/
void mqttConnected() 
{
  // Publish device adoption info
  DynamicJsonDocument json(JSON_ADOPT_MAX_SIZE);
  mqtt.publishAdopt(api.getAdopt(json.as<JsonVariant>()));

  // Log the fact we are now connected
  Serial.println("[ledc] mqtt connected");
}

void mqttDisconnected(int state) 
{
  // Log the disconnect reason
  // See https://github.com/knolleary/pubsubclient/blob/2d228f2f862a95846c65a8518c79f48dfc8f188c/src/PubSubClient.h#L44
  switch (state)
  {
    case MQTT_CONNECTION_TIMEOUT:
      Serial.println(F("[ledc] mqtt connection timeout"));
      break;
    case MQTT_CONNECTION_LOST:
      Serial.println(F("[ledc] mqtt connection lost"));
      break;
    case MQTT_CONNECT_FAILED:
      Serial.println(F("[ledc] mqtt connect failed"));
      break;
    case MQTT_DISCONNECTED:
      Serial.println(F("[ledc] mqtt disconnected"));
      break;
    case MQTT_CONNECT_BAD_PROTOCOL:
      Serial.println(F("[ledc] mqtt bad protocol"));
      break;
    case MQTT_CONNECT_BAD_CLIENT_ID:
      Serial.println(F("[ledc] mqtt bad client id"));
      break;
    case MQTT_CONNECT_UNAVAILABLE:
      Serial.println(F("[ledc] mqtt unavailable"));
      break;
    case MQTT_CONNECT_BAD_CREDENTIALS:
      Serial.println(F("[ledc] mqtt bad credentials"));
      break;      
    case MQTT_CONNECT_UNAUTHORIZED:
      Serial.println(F("[ledc] mqtt unauthorised"));
      break;      
  }
}

#if defined(PCAMODE)
uint8_t getController(JsonVariant json)
{
  if (!json.containsKey("controller"))
  {
    Serial.println(F("[ledc] missing controller"));
    return 0;
  }
  
  uint8_t controller = json["controller"].as<uint8_t>();

  // Check the controller is valid for this device
  if (controller <= 0 || controller > PCA_COUNT)
  {
    Serial.println(F("[ledc] invalid controller"));
    return 0;
  }

  return controller;
}
#endif

uint8_t getStrip(JsonVariant json)
{
  if (!json.containsKey("strip"))
  {
    Serial.println(F("[ledc] missing strip"));
    return 0;
  }
  
  uint8_t strip = json["strip"].as<uint8_t>();

  // Check the strip is valid for this device
  #if defined(PCAMODE)
  if (strip <= 0 || strip > PCA_CHANNEL_COUNT)
  #endif
  #if defined(GPIOMODE)
  if (strip <= 0 || strip > PWM_CHANNEL_COUNT)
  #endif
  {
    Serial.println(F("[ledc] invalid strip"));
    return 0;
  }

  return strip;
}

void jsonChannelConfig(JsonVariant json)
{
  #if defined(PCAMODE)
  uint8_t controller = getController(json);
  if (controller == 0) return;
  #endif

  uint8_t strip = getStrip(json);
  if (strip == 0) return;

  uint8_t count = json["count"].as<uint8_t>();

  // controller/strip indexes are sent 1-based
  #if defined(PCAMODE)
  LEDStrip * ledStrip = &ledStrips[controller - 1][strip - 1];
  #endif
  #if defined(GPIOMODE)
  LEDStrip * ledStrip = &ledStrips[strip - 1];
  #endif

  // set the config for this strip
  ledStrip->channels = count;
  ledStrip->mode = LED_MODE_COLOUR;
  ledStrip->colour[MAX_LED_COUNT] = {};

  // clear any strip config our new config overwrites
  for (uint8_t i = strip; i < strip + count - 1; i++)
  {
    #if defined(PCAMODE)
    ledStrip = &ledStrips[controller - 1][i];
    #endif
    #if defined(GPIOMODE)
    ledStrip = &ledStrips[i];
    #endif
    
    ledStrip->channels = 0;
    ledStrip->mode = LED_MODE_NONE;
    ledStrip->colour[MAX_LED_COUNT] = {};
  }
}

void jsonChannelCommand(JsonVariant json)
{
  #if defined(PCAMODE)
  uint8_t controller = getController(json);
  if (controller == 0) return;
  #endif

  uint8_t strip = getStrip(json);
  if (strip == 0) return;

  // controller/strip indexes are sent 1-based
  #if defined(PCAMODE)
  LEDStrip * ledStrip = &ledStrips[controller - 1][strip - 1];
  #endif
  #if defined(GPIOMODE)
  LEDStrip * ledStrip = &ledStrips[strip - 1];
  #endif

  if (json.containsKey("mode"))
  {
    if (strcmp(json["mode"], "colour") == 0)
    {
      ledStrip->mode = LED_MODE_COLOUR;
    }
    else if (strcmp(json["mode"], "fade") == 0)
    {
      ledStrip->mode = LED_MODE_FADE;
    }
    else if (strcmp(json["mode"], "flash") == 0)
    {
      ledStrip->mode = LED_MODE_FLASH;
    }
    else 
    {
      Serial.println(F("[ledc] invalid mode"));
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

  if (json.containsKey("flash"))
  {
    g_flash_state = json["flash"].as<bool>() ? HIGH : LOW;
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

  // Let the sensors handle any config
  sensors.conf(json);
}
#endif

/*--------------------------- Sub Routines -----------------*/

#if defined(MCU8266) || defined(MCU32) || defined(MCULILY)
/*
  MQTT
*/
void mqttCallback(char * topic, uint8_t * payload, unsigned int length) 
{
  // Pass this message down to our MQTT handler
  mqtt.receive(topic, payload, length);
}

/*
 MQTT
 */
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
#endif

/*--------------------------- Ethernet -------------------------------*/

#if defined(MCULILY)
void WiFiEvent(WiFiEvent_t event)
{
  // Log the event to serial for debugging
  switch (event)
  {
    case SYSTEM_EVENT_ETH_START:
      Serial.print(F("[ledc] ethernet started: "));
      Serial.println(ETH.macAddress());
      break;
    case SYSTEM_EVENT_ETH_CONNECTED:
      Serial.print(F("[ledc] ethernet connected: "));
      if (ETH.fullDuplex()) { Serial.print(F("full duplex ")); }
      Serial.print(F("@ "));
      Serial.print(ETH.linkSpeed());
      Serial.println(F("mbps"));
      break;
    case SYSTEM_EVENT_ETH_GOT_IP:
      Serial.print(F("[ledc] ip assigned: "));
      Serial.println(ETH.localIP());
      sensors.oled(ETH.localIP()); // update screen - should show IP address
      break;
    case SYSTEM_EVENT_ETH_DISCONNECTED:
      Serial.println(F("[ledc] ethernet disconnected"));
      break;
    case SYSTEM_EVENT_ETH_STOP:
      Serial.println(F("[ledc] ethernet stopped"));
      break;
    default:
      break;
  }

  // Once our ethernet controller has started continue initialisation
  if (event == SYSTEM_EVENT_ETH_START)
  {
    // Set up MQTT (don't attempt to connect yet)
    initialiseMqtt(LILYmacAddress(mac));

    // Set up the REST API once we have an IP address
    initialiseRestApi();
  }
}
#endif

#if defined(WIFIMODE)
void initialiseWifi(byte * mac)
{
  // Ensure we are in the correct WiFi mode
  WiFi.mode(WIFI_STA);

  // Connect using saved creds, or start captive portal if none found
  // Blocks until connected or the portal is closed
  if (!wm.autoConnect("OXRS_WiFi", "superhouse"))
  {
    // If we are unable to connect then restart
    ESP.restart();
  }
  
  // Get ESP8266 base MAC address
  WiFi.macAddress(mac);

  // Format the MAC address for display
  char mac_display[18];
  sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // Display MAC/IP addresses on serial
  Serial.print(F("[ledc] mac address: "));
  Serial.println(mac_display);  
  Serial.print(F("[ledc] ip address: "));
  Serial.println(WiFi.localIP());
}
#endif

#if defined(ETHMODE)
void initialiseEthernet()
{
  #if defined(MCULILY)
  WiFi.onEvent(WiFiEvent);

  // Reset the Ethernet PHY
  pinMode(ETH_RST_PIN, OUTPUT);
  digitalWrite(ETH_RST_PIN, 0);
  delay(200);
  digitalWrite(ETH_RST_PIN, 1);
  delay(200);
  digitalWrite(ETH_RST_PIN, 0);
  delay(200);
  digitalWrite(ETH_RST_PIN, 1);

  // Start the Ethernet PHY
  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLOCK_MODE);
  #endif
  #if defined(MCU8266) || defined(MCU32)
  // Get ESP base MAC address
  WiFi.macAddress(mac);
  
  // Ethernet MAC address is base MAC + 3
  // See https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/system.html#mac-address
  mac[5] += 3;

  // Display the MAC address on serial
  char mac_display[18];
  sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print(F(" mac address: "));
  Serial.println(mac_display);

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

  // Get an IP address via DHCP and display on serial
  Serial.print(F("ip address: "));
  if (Ethernet.begin(mac, DHCP_TIMEOUT_MS, DHCP_RESPONSE_TIMEOUT_MS))
  {
    Serial.println(Ethernet.localIP());
    sensors.oled(Ethernet.localIP()); // update screen - should show IP address
  }
  else
  {
    Serial.println("Failed to configure Ethernet using DHCP");
    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
      Serial.println("Ethernet shield was not found.  Sorry, can't run without hardware. :(");
    } else if (Ethernet.linkStatus() == LinkOFF) {
      Serial.println("Ethernet cable is not connected.");
    }
  }
  #endif
}
#endif







void setup()
{
  #if defined(MCU8266) || defined(MCU32) || defined(MCULILY)
  // Startup logging to serial
  Serial.begin(SERIAL_BAUD_RATE);
  delay(250);
  Serial.println();
  Serial.println(F("\n========================================"));
  Serial.print  (F("FIRMWARE: ")); Serial.println(FW_NAME);
  Serial.print  (F("MAKER:    ")); Serial.println(FW_MAKER);
  Serial.print  (F("VERSION:  ")); Serial.println(FW_VERSION);
  Serial.println(F("========================================"));

  Wire.begin(I2C_SDA,I2C_SCL);           // Start the I2C bus - SDA/SCL pins)
  sensors.begin();                        // start sensor library
  #endif

  #if defined(GPIOMODE)
  #if defined(BULB)
  pwmDriver.begin_gpio(4,12,14,5,13);
  #else
  #if defined(MCU8266)
  pwmDriver.begin_gpio(15,13,12,14,5);
  #endif
  #if defined(MCULILY)
  pwmDriver.begin_gpio(14,04,12,15,16);
  #endif
  initialiseStrips(0);                    // Initialise LED strip config
  #endif
  #endif

  #if defined(PCAMODE)
  scanI2CBus();

  // Initialise LED strip config
  for (uint8_t pca = 0; pca < PCA_COUNT; pca++)
  {
    if (bitRead(g_pcas_found, pca) == 0)
      continue;

    initialiseStrips(pca);
  }
  #endif

  #if defined(ETHMODE)
  #if defined(MCULILY)
  sensors.oled(LILYmacAddress(mac));      // start screen - starts with MAC address showing
  #endif
  #if defined(MCU8266) || defined(MCU32)
  sensors.oled(WiFi.macAddress(mac));     // start screen - starts with MAC address showing
  #endif
  initialiseEthernet();
  #endif
  #if defined(WIFIMODE)
  sensors.oled(WiFi.macAddress(mac));    // start screen - starts with MAC address showing
  initialiseWifi(mac);
  sensors.oled(WiFi.localIP());           // update screen - should show IP address
  #endif

  #if defined(MCU8266) || defined(MCU32) || defined(MCULILY) && defined(WIFIMODE)
  WiFi.macAddress(mac);

  initialiseMqtt(mac);   // Set up MQTT (don't attempt to connect yet)

  initialiseRestApi();
  #endif
}

void loop()
{
#if defined(MCU8266) || defined(MCU32) || defined(MCULILY) && defined(WIFIMODE)

// Check our MQTT broker connection is still ok
  mqtt.loop();

#if defined(MCULILY) && defined(ETHMODE)
WiFiClient client = server.available();
api.checkWifi(&client);
#endif

#if defined(MCU8266) || defined(MCU32)
#if defined(ETHMODE)
Ethernet.maintain();
EthernetClient client = server.available();
api.checkEthernet(&client);
#endif
#endif

#if defined(MCU8266) || defined(MCU32) || defined(MCULILY) && defined(WIFIMODE)
#if defined(WIFIMODE)
WiFiClient client = server.available();
api.checkWifi(&client);
#endif
#endif

#if defined(PCAMODE)
// Iterate through each of the PCA9865s
  for (uint8_t pca = 0; pca < PCA_COUNT; pca++)
  {
    if (bitRead(g_pcas_found, pca) == 0)
      continue;
    
    processStrips(pca);
  }
#endif
#if defined(GPIOMODE)
// Handle any changes to our LED strips
  processStrips(0);
#endif

sensors.oled();  // update OLED
sensors.tele();  // update mqtt sensors
#endif
}