#include "net.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include <sys/time.h>
#include "config.h"
#include "modem.h"
#include "remote_config.h"

#ifndef NET_MODE
#define NET_MODE NET_GPRS
#endif
#define USE_WIFI (NET_MODE == NET_WIFI || NET_MODE == NET_WIFI_THEN_GPRS)
#define USE_GPRS (NET_MODE == NET_GPRS || NET_MODE == NET_WIFI_THEN_GPRS)

static WiFiClient wifiClient;
static PubSubClient mqtt;
static char clientId[48];
static uint32_t lastWifiTry = 0, lastMqttTry = 0;
static bool ntpStarted = false, modemTimeSynced = false;
static bool paused = false;
enum class Link { None, Wifi, Gprs };
static Link curLink = Link::None;     // transport the MQTT client is bound to

static void onMessage(char* topic, uint8_t* payload, unsigned int len) {
  char ackTopic[96], ack[256];
  if (remoteConfigHandle(topic, payload, len, ackTopic, sizeof(ackTopic), ack, sizeof(ack)) &&
      ack[0]) {
    mqtt.publish(ackTopic, ack);
  }
}

static bool timeValid() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec > 1600000000;
}

void netBegin() {
  snprintf(clientId, sizeof(clientId), "%s-%06llX", MQTT_CLIENT_ID,
           (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFF));
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(2048);         // 10 contacts with Bangla names fit
  mqtt.setKeepAlive(90);           // > worst-case SMS burst
  mqtt.setSocketTimeout(10);
  mqtt.setCallback(onMessage);
#if USE_WIFI
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiTry = millis();
#else
  WiFi.mode(WIFI_OFF);
#endif
  Serial.printf("[net] mode %s, MQTT %s:%d topic '%s' id '%s'\n",
                NET_MODE == NET_GPRS ? "GPRS" : NET_MODE == NET_WIFI ? "WIFI" : "WIFI_THEN_GPRS",
                MQTT_HOST, MQTT_PORT, MQTT_TOPIC, clientId);
}

bool netWifiUp() { return WiFi.status() == WL_CONNECTED; }
bool netGprsUp() { return USE_GPRS && modemGprsUp(); }
bool netMqttUp() {
  if (curLink == Link::Gprs) { ModemLock lk; return lk.ok && mqtt.connected(); }
  return mqtt.connected();
}
bool netUsingGprs() { return curLink == Link::Gprs; }
const char* netLinkName() { return curLink == Link::Wifi ? "wifi" : curLink == Link::Gprs ? "gprs" : "none"; }
void netPause(bool p) { paused = p; }

uint32_t netTelemetryIntervalMs() {
  return curLink == Link::Gprs ? TELEMETRY_INTERVAL_GPRS_MS : TELEMETRY_INTERVAL_MS;
}

// Pick the transport: WiFi if allowed & up, else GPRS if allowed & up.
static Link chooseLink() {
#if USE_WIFI
  if (WiFi.status() == WL_CONNECTED) return Link::Wifi;
#endif
#if USE_GPRS
  if (modemGprsEnsure()) return Link::Gprs;
#endif
  return Link::None;
}

static bool mqttConnect() {
  Link want = chooseLink();
  if (want == Link::None) return false;
  if (want != curLink) {
    if (mqtt.connected()) { ModemLock lk; mqtt.disconnect(); }
    mqtt.setClient(want == Link::Wifi ? (Client&)wifiClient : modemClient());
    curLink = want;
    Serial.printf("[net] MQTT transport -> %s\n", netLinkName());
  }
  ModemLock lk;   // harmless on WiFi; required on GPRS
  if (!mqtt.connect(clientId)) {
    Serial.printf("[net] MQTT connect failed rc=%d (%s)\n", mqtt.state(), netLinkName());
    return false;
  }
  mqtt.subscribe(MQTT_CONTACTS_TOPIC, 1);
  mqtt.subscribe(MQTT_SMS_CFG_TOPIC, 1);
  Serial.printf("[net] MQTT connected via %s\n", netLinkName());
  return true;
}

void netLoop() {
#if USE_WIFI
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 15000) {
    lastWifiTry = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  if (WiFi.status() == WL_CONNECTED && !ntpStarted) {
    configTime(0, 0, NTP_SERVER);  // epoch is UTC
    ntpStarted = true;
    Serial.printf("[net] WiFi up, IP %s\n", WiFi.localIP().toString().c_str());
  }
#endif
  if (paused) return;

  // WIFI_THEN_GPRS: move back to WiFi when it returns
  bool wrongLink = (NET_MODE == NET_WIFI_THEN_GPRS && curLink == Link::Gprs &&
                    WiFi.status() == WL_CONNECTED);
  bool connected;
  {
    ModemLock lk;
    connected = mqtt.connected();
    if (connected && !wrongLink) mqtt.loop();
  }
  if (!connected || wrongLink) {
    if (millis() - lastMqttTry > 5000 || lastMqttTry == 0) {
      lastMqttTry = millis();
      mqttConnect();
    }
  }
  if (curLink == Link::Gprs && !modemTimeSynced && !timeValid() && mqtt.connected()) {
    modemTimeSynced = modemSyncTime() || true;   // try once per boot
  }
}

bool netPublishTo(const char* topic, const char* json, bool retained) {
  ModemLock lk;
  if (!mqtt.connected()) return false;
  return mqtt.publish(topic, json, retained);
}

bool netPublish(const char* json) { return netPublishTo(MQTT_TOPIC, json, false); }

bool netReconnectNow() {
  {
    ModemLock lk;
    if (mqtt.connected()) return true;
  }
  for (int i = 0; i < 2; i++) {
    if (mqttConnect()) return true;
    delay(500);
  }
  return false;
}

uint64_t netNowMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec > 1600000000) return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
  return millis();
}
