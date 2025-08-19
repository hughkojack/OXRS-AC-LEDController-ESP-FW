/*
 * HSG_32_POE.h
 */

#ifndef HSG_32_POE_H
#define HSG_32_POE_H

#include <HSG_MQTT.h>              // For MQTT pub/sub
#include <HSG_API.h>               // For REST API

// Ethernet PHY configuration is now handled in platformio.ini
// We only need to define the DHCP timeouts here.
#define       DHCP_TIMEOUT_MS           15000
#define       DHCP_RESPONSE_TIMEOUT_MS  4000

// I2C - Standard ESP32 pins
#define       I2C_SDA                   13
#define       I2C_SCL                   16

// REST API
#define       REST_API_PORT             80

class HSG_32_POE : public Print
{
  public:
    HSG_32_POE(const uint8_t * fwLogo = NULL);

    void begin(jsonCallback config, jsonCallback command);
    void loop(void);

    // Firmware can define the config/commands it supports - for device discovery and adoption
    void setConfigSchema(JsonVariant json);
    void setCommandSchema(JsonVariant json);

    // Return a pointer to the MQTT library
    HSG_MQTT * getMQTT(void);

    // Return a pointer to the API library
    HSG_API * getAPI(void);
    
    // Helpers for publishing to stat/ and tele/ topics
    bool publishStatus(JsonVariant json);
    bool publishTelemetry(JsonVariant json);

    // Implement Print.h wrapper
    virtual size_t write(uint8_t);
    using Print::write;

  private:
    void _initialiseNetwork(byte * mac);
    void _initialiseMqtt(byte * mac);
    void _initialiseRestApi(void);
    
    bool _isNetworkConnected(void);
};

#endif
