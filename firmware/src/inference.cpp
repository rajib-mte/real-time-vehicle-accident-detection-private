#include "inference.h"
#include <Arduino.h>
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/schema/schema_generated.h"
#include "config.h"
#include "feature_extractor.h"
#include "model_data.h"
#include "model_config.h"   // from ../training (or placeholder)

static_assert(kNumFeatures == NUM_FEATURES,
              "model_config.h kNumFeatures != firmware NUM_FEATURES "
              "(feature extraction mismatch with training)");
static_assert(TENSOR_ARENA_SIZE >= kTensorArenaSize,
              "TENSOR_ARENA_SIZE (config.h) smaller than kTensorArenaSize (model_config.h)");
static_assert(SAMPLE_RATE_HZ == kSampleRateHz && WINDOW_SIZE == kWindowSize &&
              WINDOW_HOP == kWindowStep,
              "config.h sampling/window settings differ from training model_config.h");
#ifndef MODEL_IS_PLACEHOLDER
#define MODEL_IS_PLACEHOLDER 0
#endif

namespace {
const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interp = nullptr;
TfLiteTensor* input = nullptr;
TfLiteTensor* output = nullptr;
alignas(16) uint8_t tensorArena[TENSOR_ARENA_SIZE];
bool ready = false;
}  // namespace

bool inferenceBegin() {
  model = tflite::GetModel(g_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.printf("[ml] schema %d != %d\n", (int)model->version(), TFLITE_SCHEMA_VERSION);
    return false;
  }
  // Ops typically used by a Dense autoencoder exported as full-int8.
  static tflite::MicroMutableOpResolver<8> resolver;
  resolver.AddFullyConnected();
  resolver.AddRelu();
  resolver.AddRelu6();
  resolver.AddLogistic();
  resolver.AddTanh();
  resolver.AddReshape();
  resolver.AddQuantize();
  resolver.AddDequantize();

  static tflite::MicroInterpreter staticInterp(model, resolver, tensorArena,
                                               TENSOR_ARENA_SIZE);
  interp = &staticInterp;
  if (interp->AllocateTensors() != kTfLiteOk) {
    Serial.println("[ml] AllocateTensors failed (increase TENSOR_ARENA_SIZE?)");
    return false;
  }
  input = interp->input(0);
  output = interp->output(0);
  int inN = input->dims->data[input->dims->size - 1];
  int outN = output->dims->data[output->dims->size - 1];
  if (input->type != kTfLiteInt8 || output->type != kTfLiteInt8 ||
      inN != NUM_FEATURES || outN != NUM_FEATURES) {
    Serial.printf("[ml] unexpected tensors: in type %d n %d, out type %d n %d\n",
                  input->type, inN, output->type, outN);
    return false;
  }
  Serial.printf("[ml] model %u bytes, arena used %u/%u, threshold %.4f%s\n",
                g_model_data_len, (unsigned)interp->arena_used_bytes(),
                (unsigned)TENSOR_ARENA_SIZE, kAnomalyThreshold,
                MODEL_IS_PLACEHOLDER ? " (PLACEHOLDER)" : "");
  ready = true;
  return true;
}

bool inferenceReady() { return ready; }
float inferenceThreshold() { return kAnomalyThreshold; }
bool inferenceIsPlaceholder() { return MODEL_IS_PLACEHOLDER; }

float inferenceAnomalyScore(const float* f) {
  if (!ready) return -1.0f;
  const float inS = input->params.scale;
  const int inZ = input->params.zero_point;
  const float outS = output->params.scale;
  const int outZ = output->params.zero_point;
  float scaled[NUM_FEATURES];
  for (int i = 0; i < NUM_FEATURES; i++) {
    float sd = kFeatureStd[i] != 0.0f ? kFeatureStd[i] : 1.0f;
    float z = (f[i] - kFeatureMean[i]) / sd;
    scaled[i] = z > kFeatureClip ? kFeatureClip : (z < -kFeatureClip ? -kFeatureClip : z);
    int q = (int)lroundf(scaled[i] / inS) + inZ;
    input->data.int8[i] = (int8_t)constrain(q, -128, 127);
  }
  if (interp->Invoke() != kTfLiteOk) return -1.0f;
  float mse = 0;
  for (int i = 0; i < NUM_FEATURES; i++) {
    float r = (output->data.int8[i] - outZ) * outS;
    float d = r - scaled[i];
    mse += d * d;
  }
  return mse / NUM_FEATURES;
}
