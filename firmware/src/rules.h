#pragma once
#include <stddef.h>
#include <stdint.h>
#include "sample.h"

// Rule-based fallback detector (works even without / beside the ML model).
struct RuleResult {
  bool impact;      // |a| peak > IMPACT_G
  bool rollover;    // tilt > ROLLOVER_TILT_DEG sustained ROLLOVER_HOLD_MS
  bool overTemp;    // temp > OVER_TEMP_C
  float peakG;
  float tiltDeg;
};

RuleResult rulesEvaluate(const Sample* win, size_t n, uint32_t nowMs);
