#include "sensors.h"
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
static volatile float latestTemp = 25.0f;
static uint32_t tempRequestedAt = 0;
static const float G = 9.80665f;
static const float RAD2DEG = 57.2957795f;

bool sensorsBegin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  imuOk = mpu.begin(MPU6050_ADDR, &Wire);
  if (imuOk) {
    mpu.setAccelerometerRange(MPU6050_RANGE_16_G);   // crashes exceed 8 g
    mpu.setGyroRange(MPU6050_RANGE_2000_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);      // < Nyquist of 100 Hz
  }
  ds18.begin();
  dsOk = ds18.getDeviceCount() > 0;
  if (dsOk) {
    ds18.setResolution(11);                // ~375 ms conversion
    ds18.setWaitForConversion(false);      // async
    ds18.requestTemperatures();
    tempRequestedAt = millis();
  }
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_VIBRATION, ADC_11db);  // 0..~3.1 V
  Serial.printf("[sensors] MPU6050:%s DS18B20:%s\n", imuOk ? "OK" : "MISSING",
                dsOk ? "OK" : "MISSING");
  return imuOk;
}

bool sensorsImuOk() { return imuOk; }

bool sensorsReadImu(Sample& s) {
  s.t_ms = millis();
  if (imuOk) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);
    s.ax = a.acceleration.x / G;
    s.ay = a.acceleration.y / G;
    s.az = a.acceleration.z / G;
    s.gx = g.gyro.x * RAD2DEG;
    s.gy = g.gyro.y * RAD2DEG;
    s.gz = g.gyro.z * RAD2DEG;
  } else {
    s.ax = s.ay = 0; s.az = 1.0f; s.gx = s.gy = s.gz = 0;
  }
  s.vibration = analogRead(PIN_VIBRATION) / VIBRATION_ADC_MAX;
  s.temp_c = latestTemp;
  return imuOk;
}

void sensorsUpdateTemp() {
  if (!dsOk) return;
  if (millis() - tempRequestedAt < 400) return;
  float t = ds18.getTempCByIndex(0);
  if (t != DEVICE_DISCONNECTED_C && t > -55.0f && t < 125.0f) latestTemp = t;
  ds18.requestTemperatures();
  tempRequestedAt = millis();
}

float sensorsLatestTemp() { return latestTemp; }
bool sensorsTempOk() { return dsOk; }
