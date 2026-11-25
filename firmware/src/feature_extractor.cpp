#include "feature_extractor.h"
#include <math.h>

void extractFeatures(const Sample* w, size_t n, float fs, float* out) {
  for (int i = 0; i < NUM_FEATURES; i++) out[i] = 0;
  if (n < 2) return;
  double aSum = 0, aSum2 = 0, aMax = 0, jerkMax = 0, prevA = 0;
  double gSum = 0, gMax = 0;
  double mx = 0, my = 0, mz = 0;
  double tSum = 0, vSum = 0, vMax = -1e30;
  for (size_t i = 0; i < n; i++) {
    const Sample& s = w[i];
    double a = sqrt((double)s.ax * s.ax + (double)s.ay * s.ay + (double)s.az * s.az);
    double g = sqrt((double)s.gx * s.gx + (double)s.gy * s.gy + (double)s.gz * s.gz);
    aSum += a; aSum2 += a * a;
    if (a > aMax) aMax = a;
    if (i > 0) { double j = fabs(a - prevA); if (j > jerkMax) jerkMax = j; }
    prevA = a;
    gSum += g; if (g > gMax) gMax = g;
    mx += s.ax; my += s.ay; mz += s.az;
    tSum += s.temp_c;
    vSum += s.vibration; if (s.vibration > vMax) vMax = s.vibration;
  }
  double aMean = aSum / n;
  double aVar = aSum2 / n - aMean * aMean;
  mx /= n; my /= n; mz /= n;
  double norm = sqrt(mx * mx + my * my + mz * mz) + 1e-9;
  double c = mz / norm; if (c > 1) c = 1; if (c < -1) c = -1;
  double vMean = vSum / n, vVar = 0;
  for (size_t i = 0; i < n; i++) { double d = w[i].vibration - vMean; vVar += d * d; }

  out[F_ACC_MAG_MEAN]  = (float)aMean;
  out[F_ACC_MAG_STD]   = (float)sqrt(aVar > 0 ? aVar : 0);
  out[F_ACC_MAG_MAX]   = (float)aMax;
  out[F_JERK_MAX]      = (float)(jerkMax * fs);
  out[F_GYRO_MAG_MEAN] = (float)(gSum / n);
  out[F_GYRO_MAG_MAX]  = (float)gMax;
  out[F_TILT_DEG]      = (float)(acos(c) * 57.29577951308232);
  out[F_TEMP_MEAN]     = (float)(tSum / n);
  out[F_TEMP_SLOPE]    = (float)((w[n - 1].temp_c - w[0].temp_c) * fs / (n - 1));
  out[F_VIB_MEAN]      = (float)vMean;
  out[F_VIB_RMS]       = (float)sqrt(vVar / n);
  out[F_VIB_PEAK]      = (float)vMax;
}
