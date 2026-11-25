// =====================================================================
//  Real-Time Vehicle Accident Detection Using TinyML and Sensor Fusion
//  ESP32-S3 firmware: MPU6050 + DS18B20 + vibration -> features ->
//  TFLite Micro autoencoder + rule fallback -> alert (SMS + MQTT)
// =====================================================================
#include <Arduino.h>
#include "config.h"
#include "sample.h"
#include "ring_buffer.h"
#include "sensors.h"
#include "feature_extractor.h"
#include "inference.h"
#include "rules.h"
#include "alert.h"
#include "net.h"
#include "modem.h"
#include "remote_config.h"
#include "location.h"
#include "hospitals.h"
#include "payload.h"

static SampleRing<256> ring;            // 2.56 s of history @ 100 Hz
static Sample window[WINDOW_SIZE];
static uint32_t lastDetectTotal = 0;
static uint32_t lastTelemetry = 0;
static float lastScore = 0;
static int mlConsec = 0;
static Status lastDetected = Status::Normal;
static float lastPeakG = 0;
static int lastMl = -1;

// 100 Hz sampler task (higher priority than loop; network can block loop)
static void samplerTask(void*) {
  const TickType_t period = pdMS_TO_TICKS(1000 / SAMPLE_RATE_HZ);
  TickType_t wake = xTaskGetTickCount();
  Sample s;
  for (;;) {
    sensorsReadImu(s);
    ring.push(s);
    vTaskDelayUntil(&wake, period);
  }
}

static Status detect() {
  size_t n = ring.latest(window, WINDOW_SIZE);
  if (n < WINDOW_SIZE) return Status::Normal;

  float feat[NUM_FEATURES];
  extractFeatures(window, n, (float)SAMPLE_RATE_HZ, feat);
  lastScore = inferenceAnomalyScore(feat);
  bool mlAnomaly = lastScore >= 0 && lastScore > inferenceThreshold();
  lastMl = lastScore < 0 ? -1 : (mlAnomaly ? 1 : 0);
  mlConsec = mlAnomaly ? mlConsec + 1 : 0;
  bool mlTrigger = mlConsec >= ML_CONSECUTIVE_WINDOWS &&
                   (!inferenceIsPlaceholder() || ML_TRIGGER_WITH_PLACEHOLDER);

  RuleResult r = rulesEvaluate(window, n, millis());
  lastPeakG = r.peakG;
  Status st = Status::Normal;
  if (r.rollover) st = Status::Rollover;
  else if (r.impact || mlTrigger) st = Status::Accident;
  else if (r.overTemp) st = Status::OverTemp;

  if (st != Status::Normal || mlAnomaly) {
    Serial.printf("[detect] status=%s score=%.3f thr=%.3f peak=%.2fg tilt=%.0fdeg T=%.1fC\n",
                  statusStr(st), lastScore, inferenceThreshold(), r.peakG, r.tiltDeg,
                  window[n - 1].temp_c);
  }
  return st;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Vehicle Accident Detection (ESP32-S3, TinyML) ===");
  sensorsBegin();
  if (!inferenceBegin()) Serial.println("[ml] disabled -> rule-based detection only");
  alertBegin();
  remoteConfigBegin();              // NVS: sms on/off + synced contacts
#if SMS_ENABLED || NET_MODE != NET_WIFI || GPS_SOURCE == GPS_SIM808
  modemBegin();                     // one shared SIM800 instance (SMS + GPRS + GNSS)
#endif
  locationBegin();
  netBegin();
  xTaskCreatePinnedToCore(samplerTask, "sampler", 4096, nullptr, 5, nullptr, 1);
}

void loop() {
  sensorsUpdateTemp();
  locationPoll();
  netLoop();

  uint32_t total = ring.total();
  if (total - lastDetectTotal >= WINDOW_HOP) {
    lastDetectTotal = total;
    lastDetected = detect();
    Sample latest[1];
    ring.latest(latest, 1);
    alertUpdate(lastDetected, latest[0], lastScore, inferenceThreshold(), lastPeakG, lastMl);
  }

  if (millis() - lastTelemetry >= netTelemetryIntervalMs()) {
    lastTelemetry = millis();
    Sample latest[1];
    if (ring.latest(latest, 1) == 1) {
      Status st = alertActiveStatus();
      if (st == Status::Normal) st = lastDetected;
      static char json[640];
      buildPayload(json, sizeof(json), false, latest[0], lastScore,
                   inferenceThreshold(), st, locationGet(), nullptr, 0, nullptr,
                   netNowMs());
      netPublish(json);
    }
  }

  // Refresh hospital cache only when no alert is in progress (blocking HTTP)
  if (alertState() == AlertState::Idle) {
    GeoPoint g = locationGet();
    hospitalsMaybeRefresh(g.lat, g.lon);
  }
  delay(5);
}
