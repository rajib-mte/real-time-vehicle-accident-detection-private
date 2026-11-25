#pragma once
#include <stdbool.h>

bool inferenceBegin();                       // load model_data, allocate arena
bool inferenceReady();
// features: raw (unscaled) NUM_FEATURES vector. Returns reconstruction MSE
// in scaled space, or -1 on error.
float inferenceAnomalyScore(const float* features);
float inferenceThreshold();
bool inferenceIsPlaceholder();
