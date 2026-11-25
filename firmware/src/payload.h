#pragma once
#include <stddef.h>
#include <stdint.h>
#include "sample.h"
#include "location.h"
#include "hospitals.h"

enum class Status { Normal, Accident, Rollover, OverTemp };
const char* statusStr(Status s);

// Alert-only extras
struct AlertExtra {
  float peakG;          // peak |a| over the detection window [g]
  bool aiDetected;      // ML model flagged the event (score > threshold)
  int smsSent, smsFailed;
  bool smsSkipped;      // SMS switched off remotely
  bool smsPending;      // SMS will be sent after this publish -> sms_report follows
};

// Shared MQTT payload contract (README "MQTT payload"). Base fields unchanged:
// {"type","device_id","ts","ax","ay","az","gx","gy","gz","temp_c","vibration",
//  "anomaly_score","threshold","status","lat","lon", "hospitals"(alerts only)}
// Extra (additive) fields: gps_fix, sats, speed_kmh, heading_deg, alt_m, hdop,
// sms_enabled; alerts also: peak_g, ai_detected, sms_sent, sms_failed, sms_skipped, sms_pending.
size_t buildPayload(char* buf, size_t len, bool isAlert, const Sample& s,
                    float score, float threshold, Status st, const GeoPoint& g,
                    const Hospital* hosp, size_t nHosp, const AlertExtra* ex,
                    uint64_t tsMs);

// {"type":"sms_report","device_id","ts","alert_ts","status","sms_enabled",
//  "sms_sent","sms_failed","recipients"}
size_t buildSmsReport(char* buf, size_t len, uint64_t alertTs, Status st,
                      int recipients, int sent, int failed);
