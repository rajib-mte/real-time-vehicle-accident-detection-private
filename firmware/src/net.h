#pragma once
#include <stdint.h>
#include <stddef.h>

// Internet: WiFi and/or SIM800 GPRS (NET_MODE in config.h), MQTT on top.
void netBegin();                 // call after modemBegin()
void netLoop();                  // keep links + MQTT alive, call every loop
bool netWifiUp();
bool netGprsUp();
bool netMqttUp();
bool netUsingGprs();             // current MQTT transport is GPRS
const char* netLinkName();       // "wifi" | "gprs" | "none"
bool netPublish(const char* json);                    // main topic
bool netPublishTo(const char* topic, const char* json, bool retained = false);
bool netReconnectNow();          // blocking MQTT reconnect attempt (for alerts)
void netPause(bool paused);      // pause MQTT loop (e.g. while sending SMS)
uint64_t netNowMs();             // epoch ms if time synced, else millis()
uint32_t netTelemetryIntervalMs();
