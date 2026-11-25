#pragma once
#include <stddef.h>

struct SmsReport {
  int recipients;
  int sent;
  int failed;
  bool skipped;      // SMS disabled (remote switch / no modem path)
};

// Send `text` with the SIM800 modem itself (AT+CMGS via TinyGSM) to EVERY
// contact (synced list from dashboard, else config.h SMS_RECIPIENTS).
// Pauses MQTT and holds the modem mutex for the whole burst; each failed
// number is retried once. Logs a per-number result on Serial.
SmsReport smsSendAll(const char* text);
const char* smsMask(const char* number, char* buf, size_t len);  // +88017****123
