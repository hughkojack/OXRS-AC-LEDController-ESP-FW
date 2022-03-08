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

/*------------------------ Board Type ---------------------------------*/
// #define esp8266  
// #define esp32
// #define lilyESP

/*----------------------- Connection Type -----------------------------*/
// select connection mode here - comment / uncomment the one needed
//#define ethMode    // uses ethernet
//#define wifiModes   // uses wifi

/*----------------------- Modetype Type -------------------------------*/
// #define gpioModes // uses 5ch GPIO mosfet control
// #define pcaModes  // uses I2C PCA controller

/*------------------------- I2C pins ----------------------------------*/
//#define I2C_SDA   
//#define I2C_SCL

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

#if defined(esp8266)
#include <ESP8266WiFi.h>            // For networking
#if defined(ethMode)
  #include <SPI.h>                  // for ethernet
  #include <Ethernet.h>             // For networking
#endif
#endif

#if defined(esp32)
#include <WiFi.h>                   // For networking
#if defined(ethMode)
  #include <SPI.h>                  // for ethernet
  #include <Ethernet.h>             // For networking <-- need to find
#endif
#endif

#if defined(lilyESP)
#include <WiFi.h>                   // For networking
#if defined(ethMode)
  #include <SPI.h>                  // for ethernet
  #include <ETH.h>                  // For networking
#endif
#endif

#include <Wire.h>                   // For I2C
#include <PubSubClient.h>           // For MQTT
#include <OXRS_MQTT.h>              // For MQTT
#include <OXRS_API.h>               // For REST API
#include <OXRS_SENSORS.h>           // For QWICC I2C sensors
#include <ledPWM.h>                 // For PWM LED controller

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
#if defined(ethMode)
#if defined(lilyESP)
#define ETH_CLOCK_MODE              ETH_CLOCK_GPIO17_OUT   // Version with not PSRAM
#define ETH_PHY_TYPE                ETH_PHY_LAN8720        // Type of the Ethernet PHY (LAN8720 or TLK110)  
#define ETH_PHY_POWER               -1                     // Pin# of the enable signal for the external crystal oscillator (-1 to disable for internal APLL source)
#define ETH_PHY_MDC                 23                     // Pin# of the I²C clock signal for the Ethernet PHY
#define ETH_PHY_MDIO                18                     // Pin# of the I²C IO signal for the Ethernet PHY
#define ETH_PHY_ADDR                0                      // I²C-address of Ethernet PHY (0 or 1 for LAN8720, 31 for TLK110)
#define ETH_RST_PIN                 5
#endif
#if defined(esp32)
#define WIZNET_RST_PIN              13
#define ETHERNET_CS_PIN             26
#define DHCP_TIMEOUT_MS             15000
#define DHCP_RESPONSE_TIMEOUT_MS    4000
#endif
#if defined(esp8266)
#define WIZNET_RST_PIN              2
#define ETHERNET_CS_PIN             15
#define DHCP_TIMEOUT_MS             15000
#define DHCP_RESPONSE_TIMEOUT_MS    4000
#endif
#endif

#if defined(pcaModes)
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
#if defined(pcaModes)
// Each bit corresponds to a PCA9865 found on the IC2 bus
uint8_t g_pcas_found = 0;
#endif

// How long between each fade step
uint32_t g_fade_interval_us = DEFAULT_FADE_INTERVAL_US;

// Flashing state for any strips in flash mode
uint8_t g_flash_state = LOW;

/*--------------------------- Instantiate Global Objects -----------------*/
// client 
#if defined(wifiModes)
WiFiClient client;
WiFiServer server(REST_API_PORT);
// PubSubClient mqttClient(client);
// OXRS_MQTT mqtt(mqttClient);
// OXRS_API api(mqtt);
// OXRS_SENSORS sensors(mqtt);
#endif

#if defined(ethMode)
#if defined(lilyESP)
// Ethernet client (even tho using WiFiClient type!)
WiFiClient client;
WiFiServer server(REST_API_PORT);
// PubSubClient mqttClient(client);
// OXRS_MQTT mqtt(mqttClient);
// OXRS_API api(mqtt);
// OXRS_SENSORS sensors(mqtt);
#endif

#if defined(esp32) || defined(esp8266)
EthernetClient client;
EthernetServer server(REST_API_PORT);
// PubSubClient mqttClient(client);
// OXRS_MQTT mqtt(mqttClient);
// OXRS_API api(mqtt);
// OXRS_SENSORS sensors(mqtt);
#endif
#endif

#if defined(ethMode) || defined(wifiModes)
#if defined(esp8266) || defined(esp32) || defined(lilyESP)
PubSubClient mqttClient(client);
OXRS_MQTT mqtt(mqttClient);
OXRS_API api(mqtt);
OXRS_SENSORS sensors(mqtt);
#endif
#endif

#if defined(pcaModes)
// PWM LED controllers
PWMDriver pwmDriver[PCA_COUNT];

// LED strip config (allow for a max of all single LED strips)
LEDStrip ledStrips[PCA_COUNT][PCA_CHANNEL_COUNT];
#elif defined(gpioModes)
// PWM LED controller
PWMDriver pwmDriver;

// LED strip config (allow for a max of all single LED strips)
LEDStrip ledStrips[PWM_CHANNEL_COUNT];
#endif

/*--------------------------- Sub routines -----------------*/

#if defined(lilyESP)
void WiFiEvent(WiFiEvent_t event)
{
  // Log the event to serial for debugging
  switch (event)
  {
    case ARDUINO_EVENT_ETH_START:
      Serial.print(F("[ledc] ethernet started: "));
      Serial.println(ETH.macAddress());
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.print(F("[ledc] ethernet connected: "));
      if (ETH.fullDuplex()) { Serial.print(F("full duplex ")); }
      Serial.print(F("@ "));
      Serial.print(ETH.linkSpeed());
      Serial.println(F("mbps"));
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.print(F("[ledc] ip assigned: "));
      Serial.println(ETH.localIP());
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println(F("[ledc] ethernet disconnected"));
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println(F("[ledc] ethernet stopped"));
      break;
    default:
      break;
  }

  // Once our ethernet controller has started continue initialisation
  if (event == ARDUINO_EVENT_ETH_START)
  {
    // Set up MQTT (don't attempt to connect yet)
    // byte mac[6];
    // initialiseMqtt(ETH.macAddress(mac));

    // Set up the REST API once we have an IP address
    // initialiseRestApi();
  }
}
#endif

#if defined(ethMode)
#if defined(lilyESP)
// // Listen for ethernet events
//   WiFi.onEvent(wifiEvent);
  
//   // Reset the Ethernet PHY
//   pinMode(ETH_RST_PIN, OUTPUT);
//   digitalWrite(ETH_RST_PIN, 0);
//   delay(200);
//   digitalWrite(ETH_RST_PIN, 1);
//   delay(200);
//   digitalWrite(ETH_RST_PIN, 0);
//   delay(200);
//   digitalWrite(ETH_RST_PIN, 1);

//   // Start the Ethernet PHY
//   ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLOCK_MODE);
#elif defined(esp32) || defined(esp8266)

void initialiseEthernet(byte * mac)
{
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
}
#endif
#endif


#if defined(esp8266) || defined(esp32) || defined(lilyESP)
/**
  MQTT
*/
void mqttCallback(char * topic, uint8_t * payload, unsigned int length) 
{
  // Pass this message down to our MQTT handler
  mqtt.receive(topic, payload, length);
}
#endif

void setup() {
  
#if defined(ethMode)
#if defined(lilyESP)
// Listen for ethernet events
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
#endif

  



}

void loop() {
  // put your main code here, to run repeatedly:
}