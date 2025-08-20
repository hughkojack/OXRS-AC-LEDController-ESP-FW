/*
 * HSG_32_POE.cpp
 */

#include "Arduino.h"
#include "HSG_32_POE.h"

#include <Wire.h>                     // For I2C
#include <ETH.h>                      // For low-level Ethernet PHY initialisation
#include <Ethernet.h>                 // For networking
#include <WiFi.h>                     // Required for Ethernet to get MAC
#include <LittleFS.h>                 // For file system access
#include <MqttLogger.h>               // For logging

// Use our custom MQTT library
#include <HSG_MQTT.h>
#include <HSG_API.h>

// Define any constants missing from the libraries
#define MQTT_JSON_PATH "/mqtt.json"

// Macro for converting env vars to strings
#define STRINGIFY(s) STRINGIFY1(s)
#define STRINGIFY1(s) #s

// Network client (for MQTT)/server (for REST API)
WiFiClient _client;
WiFiServer _server(REST_API_PORT);

// MQTT client
PubSubClient _mqttClient(_client);
HSG_MQTT _mqtt(_mqttClient);

// REST API
HSG_API _api(_mqtt);

// Logging (topic updated once MQTT connects successfully)
MqttLogger _logger(_mqttClient, "log", MqttLoggerMode::SerialOnly);

// Firmware logo
const uint8_t * _fwLogo;
 
// Supported firmware config and command schemas
DynamicJsonDocument _fwConfigSchema(1024);
DynamicJsonDocument _fwCommandSchema(1024);

// MQTT callbacks wrapped by _mqttConfig/_mqttCommand
jsonCallback _onConfig;
jsonCallback _onCommand;

// Connection state flags
bool _ethernetConnected = false;
bool _mqttClientConnected = false;

// Stored MQTT config
char _topicPrefix[64];

// Forward declaration for our helper
bool _publishWithCorrectTopic(const char *, JsonVariant);

// Ethernet event handler
void _ethernetEvent(WiFiEvent_t event)
{
  switch (event)
  {
    case ARDUINO_EVENT_ETH_START:
      _logger.println(F("[poe] eth started"));
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      _logger.println(F("[poe] eth connected"));
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      _logger.print(F("[poe] eth got ip address: "));
      _logger.println(ETH.localIP());
      _ethernetConnected = true;

      // Start the web server now that we have an IP address
      _server.begin();
      _logger.println(F("[poe] web server started"));
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      _logger.println(F("[poe] eth disconnected"));
      _ethernetConnected = false;
      break;
    case ARDUINO_EVENT_ETH_STOP:
      _logger.println(F("[poe] eth stopped"));
      _ethernetConnected = false;
      break;
    default:
      break;
  }
}

/* JSON helpers */
void _mergeJson(JsonVariant dst, JsonVariantConst src)
{
  if (src.is<JsonObjectConst>())
  {
    for (JsonPairConst kvp : src.as<JsonObjectConst>())
    {
      if (dst[kvp.key()])
      {
        _mergeJson(dst[kvp.key()], kvp.value());
      }
      else
      {
        dst[kvp.key()] = kvp.value();
      }
    }
  }
  else
  {
    dst.set(src);
  }
}

/* Adoption info builders */
void _getFirmwareJson(JsonVariant json)
{
  JsonObject firmware = json["firmware"].to<JsonObject>();

  firmware["name"] = FW_NAME;
  firmware["shortName"] = FW_SHORT_NAME;
  firmware["maker"] = FW_MAKER;
  firmware["version"] = STRINGIFY(FW_VERSION);
  
#if defined(FW_GITHUB_URL)
  firmware["githubUrl"] = FW_GITHUB_URL;
#endif
}

void _getSystemJson(JsonVariant json)
{
  JsonObject system = json["system"].to<JsonObject>();

  system["heapUsedBytes"] = ESP.getHeapSize();
  system["heapFreeBytes"] = ESP.getFreeHeap();
  system["heapMaxAllocBytes"] = ESP.getMaxAllocHeap();
  system["flashChipSizeBytes"] = ESP.getFlashChipSize();

  system["sketchSpaceUsedBytes"] = ESP.getSketchSize();
  system["sketchSpaceTotalBytes"] = ESP.getFreeSketchSpace();

  system["fileSystemUsedBytes"] = LittleFS.usedBytes();
  system["fileSystemTotalBytes"] = LittleFS.totalBytes();
}

void _getNetworkJson(JsonVariant json)
{
  JsonObject network = json["network"].to<JsonObject>();

  network["mode"] = "ethernet";
  network["ip"] = ETH.localIP();
  network["mac"] = ETH.macAddress();
}

void _getI2CJson(JsonVariant json)
{
  JsonArray pca9685 = json["i2c"]["pca9685"].to<JsonArray>();

  for (byte i = 1; i < 127; i++)
  {
    Wire.beginTransmission(i);
    if (Wire.endTransmission() == 0)
    {
      pca9685.add(i);
    }
  }
}

void _getI2cConfigJson(JsonVariant json)
{
  JsonObject properties = json["properties"].to<JsonObject>();

  DynamicJsonDocument pcaJson(1024);
  _getI2CJson(pcaJson.as<JsonVariant>());
  JsonArray pcaArray = pcaJson["i2c"]["pca9685"].as<JsonArray>();

  if (pcaArray.size() > 0)
  {
    JsonObject i2c = properties["i2c"].to<JsonObject>();
    i2c["title"] = "I2C Device Configuration";
    i2c["type"] = "object";
    
    JsonObject i2cProperties = i2c["properties"].to<JsonObject>();
    
    JsonObject pca9685 = i2cProperties["pca9685"].to<JsonObject>();
    pca9685["title"] = "PCA9685 Output Mapping";
    pca9685["type"] = "object";

    JsonObject pcaProperties = pca9685["properties"].to<JsonObject>();

    for (JsonVariant addr : pcaArray)
    {
      char key[5];
      sprintf(key, "0x%02X", addr.as<int>());
      
      JsonObject device = pcaProperties[key].to<JsonObject>();
      device["title"] = String("Device at ") + key;
      device["type"] = "array";
      
      JsonObject items = device["items"].to<JsonObject>();
      items["type"] = "integer";
      items["default"] = 0;
    }
  }
}

void _getGroupConfigJson(JsonVariant json)
{
    JsonObject properties = json["properties"].to<JsonObject>();
    JsonObject groups = properties["groups"].to<JsonObject>();
    groups["title"] = "Group Definitions";
    groups["description"] = "Define groups of outputs that can be controlled together.";
    groups["type"] = "object";

    JsonObject additionalProperties = groups["additionalProperties"].to<JsonObject>();
    additionalProperties["type"] = "array";
    
    JsonObject items = additionalProperties["items"].to<JsonObject>();
    items["type"] = "integer";
}


void _getConfigSchemaJson(JsonVariant json)
{
  JsonObject configSchema = json["configSchema"].to<JsonObject>();
  
  configSchema["$schema"] = JSON_SCHEMA_VERSION;
  configSchema["title"] = FW_SHORT_NAME;
  configSchema["type"] = "object";

  JsonObject properties = configSchema["properties"].to<JsonObject>();

  if (!_fwConfigSchema.isNull())
  {
    _mergeJson(properties, _fwConfigSchema.as<JsonVariant>());
  }

  _getI2cConfigJson(configSchema);
  _getGroupConfigJson(configSchema);
}

void _getCommandSchemaJson(JsonVariant json)
{
  JsonObject commandSchema = json["commandSchema"].to<JsonObject>();
  
  commandSchema["$schema"] = JSON_SCHEMA_VERSION;
  commandSchema["title"] = FW_SHORT_NAME;
  commandSchema["type"] = "object";

  JsonObject properties = commandSchema["properties"].to<JsonObject>();

  if (!_fwCommandSchema.isNull())
  {
    _mergeJson(properties, _fwCommandSchema.as<JsonVariant>());
  }

  JsonObject restart = properties["restart"].to<JsonObject>();
  restart["title"] = "Restart";
  restart["type"] = "boolean";
}

/* API callbacks */
void _apiAdopt(JsonVariant json)
{
  _getFirmwareJson(json);
  _getSystemJson(json);
  _getNetworkJson(json);
  _getI2CJson(json);
  _getConfigSchemaJson(json);
  _getCommandSchemaJson(json);
}

/* MQTT callbacks */
void _mqttConnected() 
{
  if (_mqttClientConnected)
  {
    return;
  }
  _mqttClientConnected = true;

  // Publish device adoption info with the correct structure
  DynamicJsonDocument json(1024);
  _apiAdopt(json.as<JsonVariant>());
  _publishWithCorrectTopic("adopt", json.as<JsonVariant>());

  _logger.println("[poe] mqtt connected");

  // Manually subscribe to the correctly formatted command topic
  static char commandTopic[128];
  sprintf(commandTopic, "%s%s/cmnd", _topicPrefix, _mqtt.getClientId());
  _mqttClient.subscribe(commandTopic);
  _logger.print(F("[poe] subscribed to command topic: "));
  _logger.println(commandTopic);
}

void _mqttDisconnected(int state) 
{
  if (_mqttClientConnected)
  {
    _mqttClientConnected = false;
    
    switch (state)
    {
      case MQTT_CONNECTION_TIMEOUT: _logger.println(F("[poe] mqtt connection timeout")); break;
      case MQTT_CONNECTION_LOST: _logger.println(F("[poe] mqtt connection lost")); break;
      case MQTT_CONNECT_FAILED: _logger.println(F("[poe] mqtt connect failed")); break;
      case MQTT_DISCONNECTED: _logger.println(F("[poe] mqtt disconnected")); break;
      case MQTT_CONNECT_BAD_PROTOCOL: _logger.println(F("[poe] mqtt bad protocol")); break;
      case MQTT_CONNECT_BAD_CLIENT_ID: _logger.println(F("[poe] mqtt bad client id")); break;
      case MQTT_CONNECT_UNAVAILABLE: _logger.println(F("[poe] mqtt unavailable")); break;
      case MQTT_CONNECT_BAD_CREDENTIALS: _logger.println(F("[poe] mqtt bad credentials")); break;      
      case MQTT_CONNECT_UNAUTHORIZED: _logger.println(F("[poe] mqtt unauthorised")); break;      
    }
  }
}

void _mqttConfig(JsonVariant json)
{
  // Update our stored topic prefix if it has changed
  if (json.containsKey("topicPrefix")) {
    strcpy(_topicPrefix, json["topicPrefix"]);
    // Ensure the prefix ends with a slash
    int len = strlen(_topicPrefix);
    if (len > 0 && _topicPrefix[len - 1] != '/') {
      strcat(_topicPrefix, "/");
    }
  }
  
  if (_onConfig) { _onConfig(json); }
}

void _mqttCommand(JsonVariant json)
{
  if (json.containsKey("restart") && json["restart"].as<bool>())
  {
    ESP.restart();
  }

  if (_onCommand) { _onCommand(json); }
}

void _mqttCallback(char * topic, byte * payload, int length) 
{
  // Create a clean, null-terminated buffer for the payload
  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';

  // Log the raw message for debugging
  _logger.println(F("--- MQTT Message Received ---"));
  _logger.print(F("Topic: "));
  _logger.println(topic);
  _logger.print(F("Payload: "));
  _logger.println(message);
  _logger.println(F("-----------------------------"));

  // Build our expected command topic
  static char commandTopic[128];
  sprintf(commandTopic, "%s%s/cmnd", _topicPrefix, _mqtt.getClientId());

  // Check if the received topic is our command topic
  if (strcmp(topic, commandTopic) == 0)
  {
    // It's a command for us, process it directly
    static StaticJsonDocument<1024> json;
    json.clear();
    
    DeserializationError error = deserializeJson(json, message);
    if (error)
    {
      _logger.println(F("[poe] failed to deserialise command json payload"));
      return;
    }
    _mqttCommand(json.as<JsonVariant>());
  }
  else
  {
    // Not our command topic, let the library handle it (for config, etc.)
    int state = _mqtt.receive(topic, payload, length);
    switch (state)
    {
      case MQTT_RECEIVE_ZERO_LENGTH: _logger.println(F("[poe] empty mqtt payload received")); break;
      case MQTT_RECEIVE_JSON_ERROR: _logger.println(F("[poe] failed to deserialise mqtt json payload")); break;
      case MQTT_RECEIVE_NO_CONFIG_HANDLER: _logger.println(F("[poe] no mqtt config handler")); break;
      case MQTT_RECEIVE_NO_COMMAND_HANDLER: _logger.println(F("[poe] no mqtt command handler")); break;
    }
  }
}

/* Main program */
HSG_32_POE::HSG_32_POE(const uint8_t * fwLogo)
{
  _fwLogo = fwLogo;
}

void HSG_32_POE::begin(jsonCallback config, jsonCallback command)
{
  if (!LittleFS.begin(true))
  {
    _logger.println(F("[poe] failed to initialise file system"));
  }

  Wire.begin(I2C_SDA, I2C_SCL);

  DynamicJsonDocument json(1024);
  _getFirmwareJson(json.as<JsonVariant>());

  _logger.print(F("[poe] "));
  serializeJson(json, _logger);
  _logger.println();

  _onConfig = config;
  _onCommand = command;
  
  byte mac[6];
  _initialiseNetwork(mac);

  _initialiseMqtt(mac);
  _initialiseRestApi();

  // Manually load the topic prefix from the mqtt.json file
  File file = LittleFS.open(MQTT_JSON_PATH, "r");
  if (file)
  {
    DynamicJsonDocument mqttConfig(1024);
    deserializeJson(mqttConfig, file);
    if (mqttConfig.containsKey("topicPrefix")) {
      strcpy(_topicPrefix, mqttConfig["topicPrefix"]);
    } else {
      strcpy(_topicPrefix, "hsg/"); // Default if not set
    }
    file.close();
  }
  else
  {
    strcpy(_topicPrefix, "hsg/"); // Default if file doesn't exist
  }

  // Ensure the prefix ends with a slash
  int len = strlen(_topicPrefix);
  if (len > 0 && _topicPrefix[len - 1] != '/') {
    strcat(_topicPrefix, "/");
  }
}

void HSG_32_POE::loop(void)
{
  if (_isNetworkConnected())
  {
    _mqtt.loop();
    
    WiFiClient client = _server.available();
    if (client)
    {
      _api.loop(&client);
    }
  }
}

void HSG_32_POE::setConfigSchema(JsonVariant json)
{
  _fwConfigSchema.clear();
  _mergeJson(_fwConfigSchema.as<JsonVariant>(), json);
}

void HSG_32_POE::setCommandSchema(JsonVariant json)
{
  _fwConfigSchema.clear();
  _mergeJson(_fwCommandSchema.as<JsonVariant>(), json);
}

HSG_MQTT * HSG_32_POE::getMQTT()
{
  return &_mqtt;
}

HSG_API * HSG_32_POE::getAPI()
{
  return &_api;
}

bool _publishWithCorrectTopic(const char * type, JsonVariant json)
{
  // Get the client ID from the MQTT library
  const char * clientId = _mqtt.getClientId();

  // Build the topic string in the correct format: prefix/clientId/type
  char topic[128];
  sprintf(topic, "%s%s/%s", _topicPrefix, clientId, type);

  // Serialize the JSON payload
  char payload[256];
  serializeJson(json, payload, sizeof(payload));

  // Publish the message, retaining the status and adopt messages
  bool retain = (strcmp(type, "stat") == 0) || (strcmp(type, "adopt") == 0);
  return _mqttClient.publish(topic, payload, retain);
}

bool HSG_32_POE::publishStatus(JsonVariant json)
{
  if (!_isNetworkConnected()) { return false; }
  return _publishWithCorrectTopic("stat", json);
}

bool HSG_32_POE::publishTelemetry(JsonVariant json)
{
  if (!_isNetworkConnected()) { return false; }
  return _publishWithCorrectTopic("tele", json);
}

size_t HSG_32_POE::write(uint8_t character)
{
  return _logger.write(character);
}

void HSG_32_POE::_initialiseNetwork(byte * mac)
{
  WiFi.macAddress(mac);
  mac[5] += 2;

  WiFi.onEvent(_ethernetEvent);
  
  _logger.println(F("[poe] Initialising Ethernet PHY..."));
  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETHERNET_MODE, ETH_CLK_MODE);
}

void HSG_32_POE::_initialiseMqtt(byte * mac)
{
  char clientId[32];
  sprintf(clientId, PSTR("%02x%02x%02x"), mac[3], mac[4], mac[5]);  
  _mqtt.setClientId(clientId);
  
  _mqtt.onConnected(_mqttConnected);
  _mqtt.onDisconnected(_mqttDisconnected);
  _mqtt.onConfig(_mqttConfig);
  _mqtt.onCommand(_mqttCommand);
  
  _mqttClient.setCallback(_mqttCallback);
}

void HSG_32_POE::_initialiseRestApi(void)
{
  _api.begin();
  _api.onAdopt(_apiAdopt);
}

bool HSG_32_POE::_isNetworkConnected(void)
{
  return _ethernetConnected;
}
