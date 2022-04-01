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

#if defined(MCU32)
#include <WiFi.h>                   // For networking
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
#include "esp_system.h"
#include "esp_eth.h"
#endif
#endif

/*--------------------------- Constants ----------------------------------*/
// Serial
#define SERIAL_BAUD_RATE            115200

// REST API
#define REST_API_PORT               80

// Only support up to 5 LEDs per strip
#define MAX_LED_COUNT               5

// Supported LED modes
#define LED_MODE_NONE               0
#define LED_MODE_COLOUR             1
#define LED_MODE_FADE               2
#define LED_MODE_FLASH              3

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
  uint8_t mode;
  uint8_t colour[MAX_LED_COUNT];

  uint32_t lastCrossfade;
};

/*--------------------------- Global Variables ---------------------------*/
// Each bit corresponds to a discovered PWM controller
uint8_t g_pwms_found = 0;

// How long between each fade step
uint32_t g_fade_interval_us = DEFAULT_FADE_INTERVAL_US;

// Flashing state for any strips in flash mode
uint8_t g_flash_state = LOW;

// stack size counter (for determine used heap size on ESP8266)
char * g_stack_start;

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

// I2C sensors
OXRS_SENSORS sensors(mqtt);

// PWM LED controllers
PWMDriver pwmDriver[PWM_CONTROLLER_COUNT];

// LED strip config (allow for a max of all single LED strips)
LEDStrip ledStrips[PWM_CONTROLLER_COUNT][PWM_CHANNEL_COUNT];

/*--------------------------- JSON builders -----------------*/
uint32_t getStackSize()
{
  char stack;
  return (uint32_t)g_stack_start - (uint32_t)&stack;  
}

void getFirmwareJson(JsonVariant json)
{
  JsonObject firmware = json.createNestedObject("firmware");

  firmware["name"] = STRINGIFY(FW_NAME);
  firmware["shortName"] = STRINGIFY(FW_SHORT_NAME);
  firmware["maker"] = STRINGIFY(FW_MAKER);
  firmware["version"] = STRINGIFY(FW_VERSION);
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
  system["fileSystemUsedBytes"] = SPIFFS.usedBytes();
  system["fileSystemTotalBytes"] = SPIFFS.totalBytes();

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
  network["ip"] = ETH.localIP();
  network["mac"] = ETH.macAddress();

  #else
  byte mac[6];
  
  #if defined(ETHMODE)
  Ethernet.MACAddress(mac);
  network["ip"] = Ethernet.localIP();
  #elif defined(WIFIMODE)
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
  configSchema["title"] = STRINGIFY(FW_SHORT_NAME);
  configSchema["type"] = "object";

  JsonObject properties = configSchema.createNestedObject("properties");

  JsonObject channels = properties.createNestedObject("channels");
  channels["type"] = "array";
  
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

  JsonObject count = channelProperties.createNestedObject("count");
  count["type"] = "integer";
  count["minimum"] = 1;
  count["maximum"] = MAX_LED_COUNT;

  JsonArray required = channelItems.createNestedArray("required");
  if (PWM_CONTROLLER_COUNT > 1)
  {
    required.add("controller");
  }
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
  commandSchema["title"] = STRINGIFY(FW_SHORT_NAME);
  commandSchema["type"] = "object";

  JsonObject properties = commandSchema.createNestedObject("properties");
  
  JsonObject channels = properties.createNestedObject("channels");
  channels["type"] = "array";
  
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
  if (PWM_CONTROLLER_COUNT > 1)
  {
    required.add("controller");
  }
  required.add("strip");

  JsonObject flash = properties.createNestedObject("flash");
  flash["type"] = "boolean";

  JsonObject restart = properties.createNestedObject("restart");
  restart["type"] = "boolean";

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
  Serial.println(F("[ledc] scanning for PWM drivers..."));

  for (uint8_t pca = 0; pca < sizeof(PCA_I2C_ADDRESS); pca++)
  {
    Serial.print(F(" - 0x"));
    Serial.print(PCA_I2C_ADDRESS[pca], HEX);
    Serial.print(F("..."));

    // Check if there is anything responding on this address
    Wire.beginTransmission(PCA_I2C_ADDRESS[pca]);
    if (Wire.endTransmission() == 0)
    {
      bitWrite(g_pwms_found, pca, 1);

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

void initialiseStrips(uint8_t controller)
{
  for (uint8_t strip = 0; strip < PWM_CHANNEL_COUNT; strip++)
  {
    LEDStrip * ledStrip = &ledStrips[controller][strip];

    // .index is immutable and shouldn't be changed
    ledStrip->index = strip;
    ledStrip->channels = 0;
    ledStrip->mode = LED_MODE_NONE;

    for (uint8_t colour = 0; colour < MAX_LED_COUNT; colour++)
    {
      ledStrip->colour[colour] = 0;
    }
    
    ledStrip->lastCrossfade = 0L;
  }
}

void processStrips(uint8_t controller)
{
  uint8_t channelOffset = 0;

  PWMDriver * driver = &pwmDriver[controller];

  for (uint8_t strip = 0; strip < PWM_CHANNEL_COUNT; strip++)
  {
    LEDStrip * ledStrip = &ledStrips[controller][strip];

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

/*--------------------------- MQTT/API -----------------*/
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

uint8_t getController(JsonVariant json)
{
  // If only one controller supported then shortcut
  if (PWM_CONTROLLER_COUNT == 1)
    return 1;
  
  if (!json.containsKey("controller"))
  {
    Serial.println(F("[ledc] missing controller"));
    return 0;
  }
  
  uint8_t controller = json["controller"].as<uint8_t>();

  // Check the controller is valid for this device
  if (controller <= 0 || controller > PWM_CONTROLLER_COUNT)
  {
    Serial.println(F("[ledc] invalid controller"));
    return 0;
  }

  return controller;
}

uint8_t getStrip(JsonVariant json)
{
  if (!json.containsKey("strip"))
  {
    Serial.println(F("[ledc] missing strip"));
    return 0;
  }
  
  uint8_t strip = json["strip"].as<uint8_t>();

  // Check the strip is valid for this device
  if (strip <= 0 || strip > PWM_CHANNEL_COUNT)
  {
    Serial.println(F("[ledc] invalid strip"));
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
  ledStrip->mode = LED_MODE_COLOUR;

  for (uint8_t colour = 0; colour < MAX_LED_COUNT; colour++)
  {
    ledStrip->colour[colour] = 0;
  }

  // clear any strip config our new config overwrites
  for (uint8_t i = strip; i < strip + count - 1; i++)
  {
    ledStrip = &ledStrips[controller - 1][i];
    
    ledStrip->channels = 0;
    ledStrip->mode = LED_MODE_NONE;

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

void mqttCallback(char * topic, uint8_t * payload, unsigned int length) 
{
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
#if defined(MCULILY)
uint8_t * getEthMacAddress(uint8_t* mac)
{
    esp_eth_get_mac(mac);
    return mac;
}

void wiFiEvent(WiFiEvent_t event)
{
  byte mac[6];

  // Log the event to serial for debugging
  switch (event)
  {
    case SYSTEM_EVENT_ETH_START:
      Serial.print(F("[ledc] ethernet started: "));
      Serial.println(ETH.macAddress());
      sensors.oled(getEthMacAddress(mac));
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
      sensors.oled(ETH.localIP());
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
    initialiseMqtt(getEthMacAddress(mac));

    // Set up the REST API once we have an IP address
    initialiseRestApi();
  }
}
#endif

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
  Serial.print(F("[ledc] mac address: "));
  Serial.println(mac_display);

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
  Serial.print(F("[ledc] ip address: "));
  Serial.println(WiFi.localIP());

  // Update OLED display
  sensors.oled(Ethernet.localIP());

  // Set up MQTT (don't attempt to connect yet)
  initialiseMqtt(mac);

  // Set up the REST API once we have an IP address
  initialiseRestApi();
}
#endif

#if defined(ETHMODE)
void initialiseEthernet()
{
  #if defined(MCULILY)
  // We continue initialisation inside this event handler
  WiFi.onEvent(wiFiEvent);

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
  Serial.print(F("[ledc] mac address: "));
  Serial.println(mac_display);

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
  Serial.print(F("[ledc] ip address: "));
  if (!Ethernet.begin(mac, DHCP_TIMEOUT_MS, DHCP_RESPONSE_TIMEOUT_MS))
  {
    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
      Serial.println(F("ethernet shield not found"));
    } else if (Ethernet.linkStatus() == LinkOFF) {
      Serial.println(F("ethernet cable not connected"));
    } else {
      Serial.println(F("failed to setup ethernet using DHCP"));
    }
    return;
  }
  
  // Display IP address on serial
  Serial.println(Ethernet.localIP());

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
  
  Serial.println(F("[ledc ] starting up..."));

  DynamicJsonDocument json(128);
  getFirmwareJson(json.as<JsonVariant>());

  Serial.print(F("[ledc ] "));
  serializeJson(json, Serial);
  Serial.println();
}

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
  #if defined(ETHMODE)
  Ethernet.maintain();
  #endif
  
  // Handle any API requests
  #if defined(WIFIMODE) || defined(MCULILY)
  WiFiClient client = server.available();
  api.checkWifi(&client);
  #elif defined(ETHMODE)
  EthernetClient client = server.available();
  api.checkEthernet(&client);
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
}