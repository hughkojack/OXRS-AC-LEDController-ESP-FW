/*
 * HSG_API.h
 */

#ifndef HSG_API_H
#define HSG_API_H

#include <HSG_MQTT.h>
#include <ArduinoJson.h>
#include <aWOT.h>
#include <Client.h>
#include <LittleFS.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#if !defined(RASPBERRYPI_PICO)
#include <Update.h>
#endif
#endif

// JSON Schema Version
#define JSON_SCHEMA_VERSION   "http://json-schema.org/draft-07/schema#"

class HSG_API
{
  public:
    HSG_API(HSG_MQTT& mqtt);

    void begin(void);
    void loop(Client * client);

    void get(const char * path, Router::Middleware * middleware);
    void post(const char * path, Router::Middleware * middleware);

    void onAdopt(jsonCallback);
    JsonVariant getAdopt(JsonVariant json);

  private:
    Application _app;
    Router _api;

    void _initialiseRestApi(void);
    void _checkRestart(void);
    void _checkDisconnect(void);
};

#endif
