#include "payload.h"
#include <ArduinoJson.h>
#include <math.h>
#include "config.h"
#include "net.h"
#include "remote_config.h"

const char* statusStr(Status s) {
  switch (s) {
    case Status::Accident: return "accident";
    case Status::Rollover: return "rollover";
    case Status::OverTemp: return "over_temp";
    default: return "normal";
  }
}

static float r3(float v) { return isnan(v) ? 0.0f : roundf(v * 1000.0f) / 1000.0f; }
static float r1(float v) { return isnan(v) ? 0.0f : roundf(v * 10.0f) / 10.0f; }

size_t buildPayload(char* buf, size_t len, bool isAlert, const Sample& s,
                    float score, float threshold, Status st, const GeoPoint& g,
                    const Hospital* hosp, size_t nHosp, const AlertExtra* ex,
                    uint64_t tsMs) {
  JsonDocument d;
  d["type"] = isAlert ? "alert" : "telemetry";
  d["device_id"] = DEVICE_ID;
  d["ts"] = tsMs;
  d["ax"] = r3(s.ax);  d["ay"] = r3(s.ay);  d["az"] = r3(s.az);
  d["gx"] = r3(s.gx);  d["gy"] = r3(s.gy);  d["gz"] = r3(s.gz);
  d["temp_c"] = r3(s.temp_c);
  d["vibration"] = r3(s.vibration);
  d["anomaly_score"] = score < 0 ? 0.0f : r3(score);
  d["threshold"] = r3(threshold);
  d["status"] = statusStr(st);
  d["lat"] = serialized(String(g.lat, 6));
  d["lon"] = serialized(String(g.lon, 6));
  // ---- additive GPS fields ----
  d["gps_fix"] = g.fix;
  d["sats"] = g.sats;
  d["speed_kmh"] = r1(g.speedKmh);
  d["heading_deg"] = r1(g.headingDeg);
  d["alt_m"] = r1(g.altM);
  d["hdop"] = r1(g.hdop);
  d["sms_enabled"] = smsEnabled();
  if (isAlert) {
    JsonArray arr = d["hospitals"].to<JsonArray>();
    for (size_t i = 0; i < nHosp; i++) {
      JsonObject h = arr.add<JsonObject>();
      h["name"] = hosp[i].name;
      h["distance_km"] = roundf(hosp[i].distanceKm * 100.0f) / 100.0f;
      h["phone"] = hosp[i].phone;
    }
    if (ex) {
      d["peak_g"] = r1(ex->peakG);
      d["ai_detected"] = ex->aiDetected;
      d["sms_sent"] = ex->smsSent;
      d["sms_failed"] = ex->smsFailed;
      d["sms_skipped"] = ex->smsSkipped;
      d["sms_pending"] = ex->smsPending;
    }
  }
  return serializeJson(d, buf, len);
}

size_t buildSmsReport(char* buf, size_t len, uint64_t alertTs, Status st,
                      int recipients, int sent, int failed) {
  JsonDocument d;
  d["type"] = "sms_report";
  d["device_id"] = DEVICE_ID;
  d["ts"] = netNowMs();
  d["alert_ts"] = alertTs;
  d["status"] = statusStr(st);
  d["sms_enabled"] = smsEnabled();
  d["recipients"] = recipients;
  d["sms_sent"] = sent;
  d["sms_failed"] = failed;
  return serializeJson(d, buf, len);
}
