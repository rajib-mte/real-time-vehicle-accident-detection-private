#include "rules.h"
#include <math.h>
#include "config.h"

static uint32_t tiltSince = 0;

RuleResult rulesEvaluate(const Sample* win, size_t n, uint32_t nowMs) {
  RuleResult r{};
  if (n == 0) return r;
  float sx = 0, sy = 0, sz = 0;
  for (size_t i = 0; i < n; i++) {
    const Sample& s = win[i];
    float g = sqrtf(s.ax * s.ax + s.ay * s.ay + s.az * s.az);
    if (g > r.peakG) r.peakG = g;
    sx += s.ax; sy += s.ay; sz += s.az;
  }
  // Tilt = angle between mean gravity vector and the board Z axis
  // (mount the board flat, Z up). Mean over the window filters vibration.
  float mag = sqrtf(sx * sx + sy * sy + sz * sz);
  r.tiltDeg = mag > 1e-3f ? acosf(fmaxf(-1.f, fminf(1.f, sz / mag))) * 57.29578f : 0;

  r.impact = r.peakG > IMPACT_G;
  if (r.tiltDeg > ROLLOVER_TILT_DEG) {
    if (tiltSince == 0) tiltSince = nowMs ? nowMs : 1;
    r.rollover = (nowMs - tiltSince) >= ROLLOVER_HOLD_MS;
  } else {
    tiltSince = 0;
  }
  r.overTemp = win[n - 1].temp_c > OVER_TEMP_C;
  return r;
}
