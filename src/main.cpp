/**
 * HSG Light Controller Firmware
 * * A stateful, multi-channel PWM LED controller with support for smooth fading,
 * logical output mapping, groups, and a web UI for configuration.
 * * Built on the HSG framework.
 */

/*--------------------------- Libraries -------------------------------*/
#include <Arduino.h>
#include <Adafruit_PWMServoDriver.h> // For PCA9685
#include <OXRS_SENSORS.h>           // For QWICC I2C sensors

// Board support package chooser
#if defined(HSG_ESP32_POE)
#include <HSG_32_POE.h>              // Our custom ESP32 POE support
HSG_32_POE hsg;
#elif defined(OXRS_RACK32)
#include <OXRS_Rack32.h>              // Rack32 support
#include "logo.h"                     // Embedded maker logo
OXRS_Rack32 oxrs(FW_LOGO);
// Add other board definitions here if needed
#endif

/*--------------------------- Constants ----------------------------------*/
#define CONFIG_JSON_PATH "/config.json"

// PCA9685 details
#define MAX_PCA9685_BOARDS 10
Adafruit_PWMServoDriver pca[MAX_PCA9685_BOARDS];
byte pca_addr[MAX_PCA9685_BOARDS];
int pca_count = 0;

// Maximum number of logical outputs
#define MAX_OUTPUTS 160 // 10 boards * 16 channels
#define DEFAULT_FADE_MS 1000 // Default fade duration is 1 second

/*--------------------------- Global State ---------------------------*/
// This struct holds the complete state for each output, including fading information
struct OutputState {
  int startPwmValue = 0;
  int currentPwmValue = 0;
  int targetPwmValue = 0;
  unsigned long fadeStartTime = 0;
  unsigned long fadeDuration = DEFAULT_FADE_MS;
};
OutputState outputs[MAX_OUTPUTS];

// This array stores the last "ON" brightness (0-100) for stateful ON/OFF commands
int outputBrightness[MAX_OUTPUTS] = {0};

// This holds the device configuration in memory
JsonDocument g_config;

// I2C sensors
OXRS_SENSORS sensors;

// Forward declarations
void setOutput(int, int, int);
void processCommand(JsonVariant);
void processFades();
bool getPcaAddress(int, byte *, int *);
void loadConfig();
void scanI2cDevices(JsonVariant);

/*
 * Get the physical address (PCA board and channel) for a logical output
 */
bool getPcaAddress(int output, byte * addr, int * channel)
{
  JsonObject i2c = g_config["i2c"]["pca9685"];
  if (i2c)
  {
    for (JsonPair kv : i2c)
    {
      JsonArray mappings = kv.value().as<JsonArray>();
      for (int i = 0; i < mappings.size(); i++)
      {
        if (mappings[i].as<int>() == output)
        {
          *addr = (byte)strtol(kv.key().c_str(), NULL, 0);
          *channel = i;
          return true;
        }
      }
    }
  }
  return false;
}

/*
 * Kicks off a fade for a given output to a target brightness
 */
void setOutput(int output, int brightness, int fadeMs)
{
  int outputIndex = output - 1;
  if (outputIndex < 0 || outputIndex >= MAX_OUTPUTS) return;

  // Set the start and target values for the fade
  outputs[outputIndex].startPwmValue = outputs[outputIndex].currentPwmValue;
  outputs[outputIndex].targetPwmValue = map(brightness, 0, 100, 0, 4095);
  outputs[outputIndex].fadeStartTime = millis();
  outputs[outputIndex].fadeDuration = fadeMs;

  // Store the "ON" brightness (0-100) for stateful commands
  if (brightness > 0)
  {
    outputBrightness[outputIndex] = brightness;
  }
}

/*
 * This function runs in the main loop to handle smooth transitions
 */
void processFades()
{
  for (int i = 0; i < MAX_OUTPUTS; i++)
  {
    // Check if a fade is active for this output
    if (outputs[i].currentPwmValue != outputs[i].targetPwmValue)
    {
      unsigned long elapsedTime = millis() - outputs[i].fadeStartTime;

      int newPwmValue;
      if (elapsedTime >= outputs[i].fadeDuration)
      {
        // Fade is complete, snap to the target value
        newPwmValue = outputs[i].targetPwmValue;
      }
      else
      {
        // Fade is in progress, calculate the intermediate value (linear interpolation)
        float progress = (float)elapsedTime / (float)outputs[i].fadeDuration;
        newPwmValue = outputs[i].startPwmValue + (progress * (outputs[i].targetPwmValue - outputs[i].startPwmValue));
      }

      // Only update the physical PWM chip if the value has actually changed
      if (newPwmValue != outputs[i].currentPwmValue)
      {
        outputs[i].currentPwmValue = newPwmValue;
        
        byte addr;
        int channel;
        // i + 1 is the logical output number
        if (getPcaAddress(i + 1, &addr, &channel))
        {
          // Find the correct PCA9685 board and set the PWM value
          for (int j = 0; j < pca_count; j++)
          {
            if (pca_addr[j] == addr)
            {
              pca[j].setPWM(channel, 0, newPwmValue);
              break;
            }
          }
        }
      }

      // If the fade just completed, publish the final state to MQTT
      if (newPwmValue == outputs[i].targetPwmValue)
      {
        JsonDocument json;
        json["output"] = i + 1;
        json["brightness"] = map(newPwmValue, 0, 4095, 0, 100);
        json["state"] = (newPwmValue > 0) ? "ON" : "OFF";
        hsg.publishStatus(json.as<JsonVariant>());
      }
    }
  }
}

/*
 * Process a command for a single output or a group
 */
void processCommand(JsonVariant json)
{
  if (json.containsKey("group"))
  {
    // Command is for a group
    const char * groupName = json["group"];
    int fadeMs = json.containsKey("fade") ? json["fade"].as<int>() : DEFAULT_FADE_MS;
    
    JsonArray outputs = g_config["groups"][groupName];
    if (outputs)
    {
      for (JsonVariant output : outputs)
      {
        // Create a new command for each output in the group
        JsonDocument newCmd;
        newCmd["output"] = output.as<int>();
        if (json.containsKey("state")) newCmd["state"] = json["state"];
        if (json.containsKey("brightness")) newCmd["brightness"] = json["brightness"];
        newCmd["fade"] = fadeMs;
        processCommand(newCmd.as<JsonVariant>());
      }
    }
  }
  else if (json.containsKey("output"))
  {
    // Command is for a single output
    int output = json["output"];
    int fadeMs = json.containsKey("fade") ? json["fade"].as<int>() : DEFAULT_FADE_MS;

    if (json.containsKey("state"))
    {
      if (strcmp(json["state"], "ON") == 0)
      {
        // Turn ON to last known brightness
        setOutput(output, outputBrightness[output - 1], fadeMs);
      }
      else if (strcmp(json["state"], "OFF") == 0)
      {
        // Turn OFF
        setOutput(output, 0, fadeMs);
      }
    }
    else if (json.containsKey("brightness"))
    {
      // Set to a specific brightness
      setOutput(output, json["brightness"], fadeMs);
    }
  }
}

/*
 * MQTT command callback
 */
void mqttCommand(JsonVariant json)
{
  // Log the received command
  hsg.print(F("[main] received command: "));
  serializeJson(json, hsg);
  hsg.println();

  // Let the sensors handle any commands
  sensors.cmnd(json);

  // Process any lighting commands
  processCommand(json);
}

/*
 * MQTT config callback
 */
void mqttConfig(JsonVariant json)
{
  // Merge the new config into our global config
  for (JsonPair kv : json.as<JsonObject>())
  {
    g_config[kv.key()] = kv.value();
  }

  // Let the sensors handle any config
  sensors.conf(json);
}

/*
 * Scans the I2C bus and populates a JSON object with found devices
 */
void scanI2cDevices(JsonVariant json)
{
  JsonArray pca9685 = json["i2c"]["pca9685"].to<JsonArray>();

  for (byte i = 1; i < 127; i++)
  {
    Wire.beginTransmission(i);
    if (Wire.endTransmission() == 0)
    {
      // For this firmware, we assume any detected I2C device is a PCA9685
      pca9685.add(i);
    }
  }
}

void setup()
{
  // Start serial and let it settle
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("[main] starting up..."));

  // Start the board support package (which starts I2C and networking)
  hsg.begin(mqttConfig, mqttCommand);

  // Load our config from file
  loadConfig();

  // Start the sensor library (scan for attached sensors)
  sensors.begin();

  // Scan for PCA9685 boards
  JsonDocument doc;
  scanI2cDevices(doc.as<JsonVariant>());
  JsonArray pcaArray = doc["i2c"]["pca9685"];
  
  for (JsonVariant addr : pcaArray)
  {
    if (pca_count < MAX_PCA9685_BOARDS)
    {
      byte i2c_addr = addr.as<byte>();
      pca_addr[pca_count] = i2c_addr;
      pca[pca_count] = Adafruit_PWMServoDriver(i2c_addr);
      pca[pca_count].begin();
      pca[pca_count].setPWMFreq(1000);
      pca_count++;
      
      Serial.print(F("[main] found PCA9685 at 0x"));
      Serial.println(i2c_addr, HEX);
    }
  }
}

void loop()
{
  // Let the board support package handle networking, etc.
  hsg.loop();

  // Process any active fades
  processFades();

  // Publish sensor telemetry (if any)
  JsonDocument telemetry;
  sensors.tele(telemetry.as<JsonVariant>());

  if (!telemetry.isNull())
  {
    hsg.publishTelemetry(telemetry.as<JsonVariant>());
  }
}

/*
 * Load config from file
 */
void loadConfig()
{
  // Mount file system
  if (LittleFS.begin())
  {
    // Open config file
    File file = LittleFS.open(CONFIG_JSON_PATH, "r");
    if (file)
    {
      // Deserialize the JSON document
      deserializeJson(g_config, file);
      file.close();
    }
  }
}
