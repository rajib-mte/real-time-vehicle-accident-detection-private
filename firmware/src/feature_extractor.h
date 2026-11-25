#pragma once
#include <stddef.h>
#include "sample.h"

// ---------------------------------------------------------------------
// Feature extraction -- 1:1 port of ../training/scripts/features.py
// Window: 1 s = 100 samples @ 100 Hz, hop 50 (50 % overlap).
// Order (MUST match training / model_config.h):
//   0 acc_mag_mean  mean(|a|)                     [g]
//   1 acc_mag_std   std(|a|) population           [g]
//   2 acc_mag_max   max(|a|)                      [g]
//   3 jerk_max      max|diff(|a|)| * fs           [g/s]
//   4 gyro_mag_mean mean(|w|)                     [deg/s]
//   5 gyro_mag_max  max(|w|)                      [deg/s]
//   6 tilt_deg      acos(mean_az/|mean_a|)        [deg] 0 = upright
//   7 temp_mean     mean(T)                       [C]
//   8 temp_slope    (T[last]-T[first])*fs/(N-1)   [C/s]
//   9 vib_mean      mean(v)                       [0..1]
//  10 vib_rms       sqrt(mean((v-mean v)^2))      [0..1]
//  11 vib_peak      max(v)                        [0..1]
// ---------------------------------------------------------------------
#define NUM_FEATURES 12

enum FeatureIndex {
  F_ACC_MAG_MEAN = 0, F_ACC_MAG_STD, F_ACC_MAG_MAX, F_JERK_MAX,
  F_GYRO_MAG_MEAN, F_GYRO_MAG_MAX, F_TILT_DEG,
  F_TEMP_MEAN, F_TEMP_SLOPE, F_VIB_MEAN, F_VIB_RMS, F_VIB_PEAK
};

void extractFeatures(const Sample* win, size_t n, float fs, float* out /*NUM_FEATURES*/);
