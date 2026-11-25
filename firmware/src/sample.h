#pragma once
#include <stdint.h>

// One fused sensor sample (units used everywhere, incl. training):
// accel in g, gyro in deg/s, temperature in deg C, vibration 0..1
struct Sample {
  uint32_t t_ms;
  float ax, ay, az;
  float gx, gy, gz;
  float temp_c;
  float vibration;
};

#define NUM_CHANNELS 8   // ax ay az gx gy gz temp_c vibration
