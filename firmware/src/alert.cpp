#include "alert.h"
#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "net.h"
#include "sms.h"
#include "sms_format.h"
#include "location.h"
#include "hospitals.h"
#include "remote_config.h"
#include "sensors.h"

static AlertState state = AlertState::Idle;
static Status reason = Status::Normal;
static uint32_t stateSince = 0;
static Sample alertSample{};
static float alertScore = 0, alertThr = 0, alertPeak = 0;
static int alertMl = -1;
static volatile bool cancelPressed = false;

static void IRAM_ATTR onCancel() { cancelPressed = true; }

static void buzzer(bool on) {
  if (PIN_BUZZER >= 0) digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
}

void alertBegin() {
  if (PIN_CANCEL_BUTTON >= 0) {
    pinMode(PIN_CANCEL_BUTTON, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_CANCEL_BUTTON), onCancel, FALLING);
  }
  if (PIN_BUZZER >= 0) { pinMode(PIN_BUZZER, OUTPUT); buzzer(false); }
}

static void dispatch() {
  GeoPoint g = locationGet();
  Hospital h[HOSPITAL_MAX_RESULTS];
  size_t nh = hospitalsGet(h, HOSPITAL_MAX_RESULTS, g.lat, g.lon);
  const uint64_t alertTs = netNowMs();
  const bool willSms = SMS_ENABLED && smsEnabled();

  // 1) MQTT alert first (dashboard sees it immediately)
  static char json[1024];
  AlertExtra ex{alertPeak, alertMl == 1, 0, 0, !willSms, willSms};
  netReconnectNow();
  buildPayload(json, sizeof(json), true, alertSample, alertScore, alertThr, reason, g,
               h, nh, &ex, alertTs);
  bool pub = netPublish(json);
  Serial.printf("[alert] MQTT alert %s: %s\n", pub ? "published" : "NOT published", json);
  if (!willSms) return;

  // 2) SMS via SIM800 (AT+CMGS) to every contact; MQTT paused meanwhile
  SmsInfo si{statusStr(reason), alertPeak, alertMl, sensorsTempOk() ? alertSample.temp_c : NAN, true,
             g.lat, g.lon, g.source == LocSource::Gps ? 0 : g.source == LocSource::LastKnown ? 1 : 2,
             nh ? h[0].name : nullptr, nh ? h[0].distanceKm : 0.0f};
  char sms[SMS_MAX_LEN + 1];
  size_t n = formatAccidentSms(si, sms, sizeof(sms));
  Serial.printf("[alert] SMS (%u chars): %s\n", (unsigned)n, sms);
  SmsReport r = smsSendAll(sms);

  // 3) Follow-up report (or the full alert again if the first publish failed)
  netReconnectNow();
  if (pub) {
    buildSmsReport(json, sizeof(json), alertTs, reason, r.recipients, r.sent, r.failed);
  } else {
    AlertExtra ex2{alertPeak, alertMl == 1, r.sent, r.failed, r.skipped, false};
    buildPayload(json, sizeof(json), true, alertSample, alertScore, alertThr, reason, g,
                 h, nh, &ex2, alertTs);
  }
  bool pub2 = netPublish(json);
  Serial.printf("[alert] MQTT %s %s\n", pub ? "sms_report" : "alert(retry)",
                pub2 ? "published" : "NOT published");
}

void alertUpdate(Status detected, const Sample& latest, float score, float threshold,
                 float peakG, int ml) {
  uint32_t now = millis();
  switch (state) {
    case AlertState::Idle:
      if (detected != Status::Normal) {
        reason = detected;
        alertSample = latest; alertScore = score; alertThr = threshold;
        alertPeak = peakG; alertMl = ml;
        state = AlertState::Pending;
        stateSince = now;
        cancelPressed = false;
        buzzer(true);
        Serial.printf("[alert] %s detected (peak %.1fg)! Press BOOT within %d s to cancel\n",
                      statusStr(reason), peakG, CANCEL_WINDOW_MS / 1000);
      }
      break;
    case AlertState::Pending:
      if (peakG > alertPeak) alertPeak = peakG;      // keep the crash peak
      if (ml == 1) alertMl = 1;
      // Escalate reason if something more severe appears (rollover > accident > over_temp)
      if (detected == Status::Rollover ||
          (detected == Status::Accident && reason == Status::OverTemp)) {
        reason = detected; alertSample = latest; alertScore = score;
      }
      if (cancelPressed) {
        Serial.println("[alert] cancelled by driver");
        buzzer(false);
        state = AlertState::Cooldown;   // brief cooldown avoids instant re-trigger
        stateSince = now - (ALERT_COOLDOWN_MS > 10000 ? ALERT_COOLDOWN_MS - 10000 : 0);
        reason = Status::Normal;
      } else if (now - stateSince >= (uint32_t)CANCEL_WINDOW_MS) {
        buzzer(false);
        dispatch();
        state = AlertState::Cooldown;
        stateSince = millis();
      }
      break;
    case AlertState::Cooldown:
      if (millis() - stateSince >= (uint32_t)ALERT_COOLDOWN_MS) {
        state = AlertState::Idle;
        reason = Status::Normal;
      }
      break;
  }
}

AlertState alertState() { return state; }
Status alertActiveStatus() { return state == AlertState::Idle ? Status::Normal : reason; }
