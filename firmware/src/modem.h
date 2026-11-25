#pragma once
#include <Arduino.h>
#include <Client.h>

// Single shared SIM800-family modem (TinyGSM) on Serial1.
// Every AT user (SMS, GPRS/MQTT socket, SIM808 GNSS, time) must hold the
// modem mutex -> use ModemLock. The mutex is recursive.
bool modemBegin();                 // power-up + AT init (non-fatal if absent)
bool modemReady();
bool modemLock(uint32_t timeoutMs = 30000);
void modemUnlock();
struct ModemLock {
  bool ok;
  explicit ModemLock(uint32_t t = 30000) : ok(modemLock(t)) {}
  ~ModemLock() { if (ok) modemUnlock(); }
};

bool modemSendSMS(const char* number, const char* text);   // AT+CMGS
bool modemGprsEnsure();            // attach/connect GPRS (APN) if needed
bool modemGprsUp();
int modemSignal();                 // CSQ 0..31, 99 = unknown
Client& modemClient();             // TinyGsmClient (mux 0) for MQTT
bool modemSyncTime();              // NTP via modem -> sets ESP32 clock (UTC)

// SIM808 GNSS (only when GPS_SOURCE == GPS_SIM808)
bool modemGnssEnable();
bool modemGnssRead(float* lat, float* lon, float* speedKmh, float* alt,
                   int* sats, float* hdop, float* course);
