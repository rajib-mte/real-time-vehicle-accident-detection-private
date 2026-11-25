// =====================================================================
//  Standalone data logger (ESP32-S3): MPU6050 + DS18B20 + vibration
//  One CSV line per sample at 100 Hz over USB serial:
//    timestamp_ms,ax,ay,az,gx,gy,gz,temperature,vibration
//  Units: accel g, gyro deg/s, temperature C, vibration 0..1.
//  Lines starting with '#' are status messages (ignored by collect_data.py).
//  BOOT button (GPIO0) toggles pause/resume.
// =====================================================================
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "config.h"

static Adafruit_MPU6050 mpu;
static OneWire oneWire(PIN_DS18B20);
static DallasTemperature ds18(&oneWire);
static bool imuOk = false;
static bool dsOk = false;
static float latestTemp = 25.0f;
static uint32_t tempRequestedAt = 0;
static const float G = 9.80665f;
static const float RAD2DEG = 57.2957795f;

static bool paused = false;
static uint32_t lastBtnChange = 0;
static bool lastBtn = true;

static void sensorsBegin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  imuOk = mpu.begin(MPU6050_ADDR, &Wire);
  if (imuOk) {
    mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
    mpu.setGyroRange(MPU6050_RANGE_2000_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);
  }
  ds18.begin();
  dsOk = ds18.getDeviceCount() > 0;
  if (dsOk) {
    ds18.setResolution(11);              // ~375 ms conversion
    ds18.setWaitForConversion(false);    // async
    ds18.requestTemperatures();
    tempRequestedAt = millis();
  }
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_VIBRATION, ADC_11db);
}

// Non-blocking DS18B20 poll (same approach as the main firmware)
static void updateTemp() {
  if (!dsOk) return;
  if (millis() - tempRequestedAt < 400) return;
  float t = ds18.getTempCByIndex(0);
  if (t != DEVICE_DISCONNECTED_C && t > -55.0f && t < 125.0f) latestTemp = t;
  ds18.requestTemperatures();
  tempRequestedAt = millis();
}

static void checkButton() {
  bool b = digitalRead(PIN_PAUSE_BUTTON);
  if (b != lastBtn && millis() - lastBtnChange > 50) {
    lastBtnChange = millis();
    lastBtn = b;
    if (!b) {  // pressed
      paused = !paused;
      Serial.println(paused ? "# PAUSED" : "# RESUMED");
    }
  }
}

static void sampleAndPrint() {
  float ax = 0, ay = 0, az = 1.0f, gx = 0, gy = 0, gz = 0;
  uint32_t t = millis();
  if (imuOk) {
    sensors_event_t a, g, tt;
    mpu.getEvent(&a, &g, &tt);
    ax = a.acceleration.x / G;  ay = a.acceleration.y / G;  az = a.acceleration.z / G;
    gx = g.gyro.x * RAD2DEG;    gy = g.gyro.y * RAD2DEG;    gz = g.gyro.z * RAD2DEG;
  }
  float vib = analogRead(PIN_VIBRATION) / VIBRATION_ADC_MAX;
  char line[96];
  int n = snprintf(line, sizeof(line), "%lu,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.2f,%.3f\n",
                   (unsigned long)t, ax, ay, az, gx, gy, gz, latestTemp, vib);
  if (n > 0) Serial.write((const uint8_t*)line, n);
}

void setup() {
  Serial.setTxBufferSize(2048);  // absorb bursts so sampling never blocks
  Serial.begin(DATALOG_BAUD);
  delay(500);
  pinMode(PIN_PAUSE_BUTTON, INPUT_PULLUP);
  Serial.println("\n# Data logger: timestamp_ms,ax,ay,az,gx,gy,gz,temperature,vibration");
  sensorsBegin();
  Serial.printf("# MPU6050:%s DS18B20:%s rate=%dHz baud=%d (BOOT = pause/resume)\n",
                imuOk ? "OK" : "MISSING", dsOk ? "OK" : "MISSING", SAMPLE_RATE_HZ, DATALOG_BAUD);
}

void loop() {
  static const uint32_t periodUs = 1000000UL / SAMPLE_RATE_HZ;
  static uint32_t nextUs = micros() + periodUs;
  updateTemp();
  checkButton();
  // wait for the deadline (drift-free: deadline advances by a fixed period)
  while ((int32_t)(micros() - nextUs) < 0) { }
  nextUs += periodUs;
  if ((int32_t)(micros() - nextUs) > (int32_t)(5 * periodUs)) nextUs = micros() + periodUs; // resync after a stall
  if (!paused) sampleAndPrint();
}
