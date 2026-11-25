#pragma once
#include <Arduino.h>
#include "sample.h"

// Single-producer (sampler task) / single-consumer (loop) ring buffer.
template <size_t N>
class SampleRing {
 public:
  void push(const Sample& s) {
    portENTER_CRITICAL(&mux_);
    buf_[head_] = s;
    head_ = (head_ + 1) % N;
    total_++;
    portEXIT_CRITICAL(&mux_);
  }
  // Copy the most recent `count` samples (oldest first). Returns copied count.
  size_t latest(Sample* out, size_t count) {
    portENTER_CRITICAL(&mux_);
    size_t avail = total_ < N ? total_ : N;
    if (count > avail) count = avail;
    size_t start = (head_ + N - count) % N;
    for (size_t i = 0; i < count; i++) out[i] = buf_[(start + i) % N];
    portEXIT_CRITICAL(&mux_);
    return count;
  }
  uint32_t total() {
    portENTER_CRITICAL(&mux_);
    uint32_t t = total_;
    portEXIT_CRITICAL(&mux_);
    return t;
  }

 private:
  Sample buf_[N];
  size_t head_ = 0;
  uint32_t total_ = 0;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
