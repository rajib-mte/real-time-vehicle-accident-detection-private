#pragma once
#include <stddef.h>

// Pure (host-testable) accident SMS formatter. Output always <= 160 chars
// (one GSM-7 SMS). Field priority (lowest is shortened/dropped first):
//   1 alert type  2 peak accel  3 AI result  4 temperature
//   5 GPS coords + Google Maps link (short link -> plain coords -> drop)
//   6 place name (nearest hospital)  <- dropped first
struct SmsInfo {
  const char* status;     // "accident" | "rollover" | "over_temp"
  float peakG;            // peak |a| over the detection window [g]
  int ai;                 // 1 = anomaly (YES), 0 = NO, -1 = model unavailable
  float tempC;            // NAN if unknown
  bool hasPos;            // any position (GPS / last known / fixed)
  double lat, lon;
  int posKind;            // 0 = live GPS fix, 1 = last known, 2 = fixed/config
  const char* place;      // nearest hospital name or nullptr
  float placeKm;
};

#define SMS_MAX_LEN 160
size_t formatAccidentSms(const SmsInfo& in, char* out, size_t outLen);
