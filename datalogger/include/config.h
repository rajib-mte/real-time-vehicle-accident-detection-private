// Pins, rates and scaling: identical to the main firmware (include/config.h,
// src/sensors.cpp) so logged data matches what the alert firmware sees.
#pragma once

#define PIN_I2C_SDA          8
#define PIN_I2C_SCL          9
#define MPU6050_ADDR         0x68
#define PIN_DS18B20          4      // needs 4.7k pull-up to 3V3
#define PIN_VIBRATION        7      // ADC1_CH6
#define PIN_PAUSE_BUTTON     0      // BOOT button (active LOW): pause/resume

#define SAMPLE_RATE_HZ       100
#define VIBRATION_ADC_MAX    4095.0f
#define DATALOG_BAUD         115200
