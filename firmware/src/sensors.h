#pragma once
#include "sample.h"

bool sensorsBegin();            // MPU6050 + DS18B20 + ADC
bool sensorsReadImu(Sample& s); // fills accel/gyro/vibration (fast, 100 Hz)
void sensorsUpdateTemp();       // non-blocking DS18B20 poll (call often)
float sensorsLatestTemp();
bool sensorsTempOk();           // DS18B20 present
bool sensorsImuOk();
