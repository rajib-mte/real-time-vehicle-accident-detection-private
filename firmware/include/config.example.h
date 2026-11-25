// =====================================================================
//  config.h  -- user configuration (copy of config.example.h)
//  Real-Time Vehicle Accident Detection Using TinyML and Sensor Fusion
//  NOTE: placeholders only. Never commit real passwords / phone numbers.
// =====================================================================
#pragma once

// ---------------- Device ----------------
#define DEVICE_ID            "veh-esp32s3-01"

// ---------------- Internet connection mode ----------------
#define NET_GPRS             1   // SIM800 GPRS only (default)
#define NET_WIFI             2   // WiFi only
#define NET_WIFI_THEN_GPRS   3   // prefer WiFi, fall back to GPRS
#define NET_MODE             NET_GPRS

// ---------------- GPRS (SIM800 family) ----------------
// Bangladesh APNs (verify with your operator, they can change):
//   Grameenphone "internet" | Robi "internet" | Airtel BD "internet"
//   Banglalink "blweb"      | Teletalk "wap" (some plans "internet")
#define GPRS_APN             "internet"
#define GPRS_USER            ""
#define GPRS_PASS            ""
#define GPRS_RETRY_MS        30000   // reconnect attempt interval

// ---------------- WiFi ----------------
#define WIFI_SSID            "YOUR_WIFI_SSID"
#define WIFI_PASSWORD        "YOUR_WIFI_PASSWORD"

// ---------------- MQTT (HiveMQ public broker, no TLS / no auth) --------
#define MQTT_HOST            "broker.hivemq.com"
#define MQTT_PORT            1883
#define MQTT_TOPIC           "vehicle/accident"   // telemetry + alerts (same topic)
#define MQTT_CLIENT_ID       "accident-esp32s3"   // chip-id suffix is appended
#define TELEMETRY_INTERVAL_MS 1000                 // telemetry publish period
// Over GPRS 1 s telemetry is ~40 MB/day of mobile data; raise if needed.
#define TELEMETRY_INTERVAL_GPRS_MS TELEMETRY_INTERVAL_MS
// Remote config from the dashboard (RETAINED, QoS1). Both messages carry
// "token" which must equal CONTACTS_TOKEN. NOTE: on the public broker anyone
// can read the token -> this is a guard against mistakes, not real security.
#define CONTACTS_TOKEN       "CHANGE_ME_SHARED_SECRET"
// {"type":"contacts","device_id":"<id|*>","version":<ms>,"token":"..","contacts":["+880.."]}
#define MQTT_CONTACTS_TOPIC  "vehicle/accident/config/contacts"   // ack: <topic>/ack
// {"type":"sms_config","device_id":"<id|*>","version":<ms>,"token":"..","sms_enabled":true}
#define MQTT_SMS_CFG_TOPIC   "vehicle/accident/config/sms"        // ack: <topic>/ack
#define MAX_CONTACTS         10      // dashboard allows 10

// ---------------- NTP (epoch-ms timestamps; falls back to millis) -----
#define NTP_SERVER           "pool.ntp.org"
#define GMT_OFFSET_SEC       (6 * 3600)            // Asia/Dhaka
#define DST_OFFSET_SEC       0

// ---------------- SMS (sent by the SIM800 modem itself: AT+CMGS) --------
#define SMS_ENABLED          1       // compile-time master switch (0 = no modem SMS code path)
#define SMS_ENABLED_DEFAULT  true    // runtime default until dashboard sets sms_config
#define SMS_RETRY_ONCE       1       // retry each failed number once
#define SMS_NUM_RECIPIENTS   2       // fallback list (used if no synced list)
static const char* const SMS_RECIPIENTS[SMS_NUM_RECIPIENTS] = {
  "+8801XXXXXXXXX",     // placeholder - replace
  "+8801YYYYYYYYY",     // placeholder - replace
};
#define SIM_PIN_CODE         ""      // SIM PIN, "" if none
#define SIM_BAUD             9600

// ---------------- Location / GPS ----------------
#define GPS_NONE             0   // no GPS: fixed location only
#define GPS_UART             1   // NEO-6M / NEO-M8N NMEA on Serial2 (default)
#define GPS_SIM808           2   // SIM808 built-in GNSS (AT+CGNSPWR/CGNSINF)
#define GPS_SOURCE           GPS_UART
#define GPS_BAUD             9600
#define GPS_FIX_MAX_AGE_MS   3000    // older fix -> gps_fix=false
#define GPS_SIM808_POLL_MS   2000
// Fallback when no fix and no last-known fix:
#define FIXED_LAT            23.810300   // Dhaka (example)
#define FIXED_LON            90.412500

// ---------------- Hospital lookup ----------------
#define HOSPITAL_LOOKUP_ENABLED   1
#define HOSPITAL_SEARCH_RADIUS_M  5000
#define HOSPITAL_MAX_RESULTS      3
#define HOSPITAL_REFRESH_MS       (30UL * 60UL * 1000UL)   // re-query every 30 min
#define HOSPITAL_REQUERY_KM       2.0f    // ...or when moved more than this
// Overpass is HTTPS: queried over WiFi only (SIM800 TLS is too old).
// Over GPRS-only the offline table (+ last online result) is used.
#define OVERPASS_URL              "https://overpass-api.de/api/interpreter"

// Offline fallback table (used when WiFi/Overpass is unavailable).
// Coordinates are approximate; fill in verified phone numbers yourself.
struct FallbackHospital { const char* name; double lat; double lon; const char* phone; };
static const FallbackHospital FALLBACK_HOSPITALS[] = {
  {"Dhaka Medical College Hospital", 23.7257, 90.3976, ""},
  {"Square Hospital",                23.7527, 90.3816, ""},
  {"Evercare Hospital Dhaka",        23.8103, 90.4316, ""},
  {"NITOR (Pongu Hospital)",         23.7766, 90.3700, ""},
};
#define EMERGENCY_NUMBER          "999"   // Bangladesh national emergency

// ---------------- Pins (ESP32-S3) ----------------
#define PIN_I2C_SDA          8
#define PIN_I2C_SCL          9
#define MPU6050_ADDR         0x68
#define PIN_DS18B20          4      // needs 4.7k pull-up to 3V3
#define PIN_VIBRATION        7      // ADC1_CH6 (GPIO34 has no ADC on S3!)
#define PIN_SIM_TX           17     // ESP32 TX -> SIM800L RXD
#define PIN_SIM_RX           18     // ESP32 RX <- SIM800L TXD
#define PIN_SIM_RST          -1     // optional SIM800L RST, -1 = unused
#define GPS_RX_PIN           15     // ESP32 RX <- GPS TX  (Serial2)
#define GPS_TX_PIN           16     // ESP32 TX -> GPS RX  (optional)
#define PIN_CANCEL_BUTTON    0      // BOOT button (active LOW) cancels alert
#define PIN_BUZZER           -1     // optional buzzer/LED, -1 = unused

// ---------------- Sampling / windowing ----------------
#define SAMPLE_RATE_HZ       100
#define WINDOW_SIZE          100    // 1 s window @ 100 Hz (= kWindowSize)
#define WINDOW_HOP           50     // every 0.5 s, 50% overlap (= kWindowStep)

// ---------------- TinyML ----------------
#define TENSOR_ARENA_SIZE    (8 * 1024)    // >= kTensorArenaSize in model_config.h
#define ML_CONSECUTIVE_WINDOWS 2    // anomaly must persist N windows
// With the PLACEHOLDER model the ML score is reported but does not raise
// alerts by itself (rules still do). Set 1 to let it trigger anyway.
#define ML_TRIGGER_WITH_PLACEHOLDER 0

// ---------------- Rule-based fallback thresholds ----------------
#define IMPACT_G             4.0f   // |a| peak (g) -> accident
#define ROLLOVER_TILT_DEG    60.0f  // tilt from vertical (deg)
#define ROLLOVER_HOLD_MS     2000   // tilt must be sustained
#define OVER_TEMP_C          60.0f  // DS18B20 over-temp (= training kTempAlarmC)
#define VIBRATION_ADC_MAX    4095.0f

// ---------------- Alert state machine ----------------
#define CANCEL_WINDOW_MS     10000  // driver can cancel with button; 0 = send immediately
#define ALERT_COOLDOWN_MS    60000  // ignore new triggers after an alert
