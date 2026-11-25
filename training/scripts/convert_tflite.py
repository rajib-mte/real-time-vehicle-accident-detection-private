"""
convert_tflite.py - full-integer (int8) post-training quantization + C export.

  python scripts/convert_tflite.py                       # models/autoencoder
  python scripts/convert_tflite.py --model-dir models/classifier

Steps:
  1. load model.keras + split.npz written by train.py
  2. convert with a representative dataset (training windows) ->
     int8 weights, int8 activations, int8 input AND output
  3. verify: load the .tflite in the TFLite interpreter, check every
     tensor is integer, run the test set, compare with the float model
  4. export for TFLite Micro:
       model_data.h / model_data.cc   xxd-style byte array (alignas(16))
       model_config.h                 scaler mean/std, clip, quant params,
                                      threshold / labels, window config,
                                      tensor-arena suggestion
  5. write conversion_report.json
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from datetime import datetime
from pathlib import Path

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import features as F  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


# ----------------------------------------------------------------------------
def convert_int8(model, X_rep, tf, n_rep=500):
    import tempfile
    n_in = X_rep.shape[1]
    # Keras 3: export a SavedModel with a fixed batch of 1 (what TFLM runs),
    # then convert it. (from_concrete_functions fails on resource variables.)
    tmp = tempfile.mkdtemp(prefix="accident_sm_")
    model.export(tmp, format="tf_saved_model",
                 input_signature=[tf.TensorSpec([1, n_in], tf.float32)], verbose=False)
    conv = tf.lite.TFLiteConverter.from_saved_model(tmp)
    import shutil
    atexit_dirs.append(tmp)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    rng = np.random.default_rng(0)
    idx = rng.permutation(len(X_rep))[:n_rep]

    def rep():
        for i in idx:
            yield [X_rep[i:i + 1].astype(np.float32)]

    conv.representative_dataset = rep
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    tfl = conv.convert()
    for d in atexit_dirs:
        shutil.rmtree(d, ignore_errors=True)
    return tfl


atexit_dirs: list = []


class Int8Runner:
    """Runs the int8 model on float (already standardized) inputs, exactly
    like the MCU will: quantize -> invoke -> dequantize."""

    def __init__(self, tflite_bytes, tf):
        import warnings
        # Reference kernels only (no XNNPACK delegate) -> closest to TFLite
        # Micro behaviour and lets us inspect every op/tensor.
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")  # tf.lite.Interpreter deprecation note
            self.it = tf.lite.Interpreter(
                model_content=tflite_bytes,
                experimental_op_resolver_type=tf.lite.experimental.OpResolverType
                .BUILTIN_REF)
        self.it.allocate_tensors()
        self.inp = self.it.get_input_details()[0]
        self.out = self.it.get_output_details()[0]
        self.in_scale, self.in_zp = self.inp["quantization"]
        self.out_scale, self.out_zp = self.out["quantization"]

    def quantize(self, X):
        q = np.round(X / self.in_scale + self.in_zp)
        return np.clip(q, -128, 127).astype(np.int8)

    def __call__(self, X):
        res = []
        for row in X:
            self.it.set_tensor(self.inp["index"], self.quantize(row[None, :]))
            self.it.invoke()
            q = self.it.get_tensor(self.out["index"]).astype(np.float32)
            res.append((q - self.out_zp) * self.out_scale)
        return np.concatenate(res, 0)


def inspect_tflite(runner: Int8Runner):
    it = runner.it
    details = it.get_tensor_details()
    dtypes = {}
    for d in details:
        dtypes[np.dtype(d["dtype"]).name] = dtypes.get(np.dtype(d["dtype"]).name, 0) + 1
    try:
        ops = it._get_ops_details()
        op_names = sorted({o["op_name"] for o in ops})
        act_ids = {i for o in ops for i in o["outputs"]} | {runner.inp["index"]}
        act_bytes = sum(int(np.prod(d["shape"])) * np.dtype(d["dtype"]).itemsize
                        for d in details if d["index"] in act_ids)
    except Exception:
        op_names, act_bytes = [], None
    float_tensors = [d["name"] for d in details if np.dtype(d["dtype"]) == np.float32]
    return {"tensor_dtypes": dtypes, "ops": op_names,
            "float32_tensors": float_tensors,
            "input": {"dtype": np.dtype(runner.inp["dtype"]).name,
                      "shape": runner.inp["shape"].tolist(),
                      "scale": float(runner.in_scale), "zero_point": int(runner.in_zp)},
            "output": {"dtype": np.dtype(runner.out["dtype"]).name,
                       "shape": runner.out["shape"].tolist(),
                       "scale": float(runner.out_scale), "zero_point": int(runner.out_zp)},
            "activation_bytes_sum": act_bytes,
            "fully_int8": (np.dtype(runner.inp["dtype"]) == np.int8
                           and np.dtype(runner.out["dtype"]) == np.int8
                           and not float_tensors)}


# ----------------------------------------------------------------------------
# C export
# ----------------------------------------------------------------------------
def _cf(v) -> str:
    """C float literal that always compiles (60 -> 60.0f, 1e-05 -> 1e-05f)."""
    t = f"{float(v):.8g}"
    if not any(c in t for c in ".eEn"):
        t += ".0"
    return t + "f"


def _c_floats(vals):
    return ", ".join(_cf(v) for v in vals)


def write_model_data(tfl: bytes, out: Path, var="g_model_data"):
    (out / "model_data.h").write_text(f"""// Auto-generated by convert_tflite.py - do not edit.
// int8 TFLite model for TFLite Micro (ESP32-S3). {len(tfl)} bytes.
#ifndef ACCIDENT_MODEL_DATA_H_
#define ACCIDENT_MODEL_DATA_H_

extern const unsigned char {var}[];
extern const unsigned int {var}_len;

#endif  // ACCIDENT_MODEL_DATA_H_
""")
    lines = []
    for i in range(0, len(tfl), 12):
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in tfl[i:i + 12]) + ",")
    (out / "model_data.cc").write_text(
        "// Auto-generated by convert_tflite.py - do not edit.\n"
        "// Equivalent to: xxd -i model_int8.tflite (plus 16-byte alignment).\n"
        "#include \"model_data.h\"\n\n"
        "// TFLite Micro needs the flatbuffer aligned; keep it in flash (const).\n"
        f"alignas(16) const unsigned char {var}[] = {{\n" + "\n".join(lines) +
        f"\n}};\nconst unsigned int {var}_len = {len(tfl)};\n")


def write_config(out: Path, meta, scaler, info, threshold_int8, arena):
    mode = meta["mode"]
    names = "\n".join(f"//   [{i:2d}] {n}" for i, n in enumerate(meta["feature_names"]))
    labels = meta["labels"]
    if mode == "autoencoder":
        mode_block = f"""// ---- autoencoder ----
// score = mean over features of (dequant(output[i]) - z[i])^2
// alarm if score > kAnomalyThreshold
#define ACCIDENT_MODEL_AUTOENCODER 1
constexpr float kAnomalyThreshold = {_cf(threshold_int8)};        // calibrated on the int8 model
constexpr float kAnomalyThresholdFloat = {_cf(meta['threshold'])};   // float Keras model (reference)
constexpr float kThresholdPercentile = {_cf(meta['threshold_percentile'])};"""
    else:
        mode_block = f"""// ---- classifier ----
// output = int8 softmax over kNumClasses; class = argmax
#define ACCIDENT_MODEL_CLASSIFIER 1
constexpr int kNumClasses = {len(labels)};
static const char* const kClassLabels[kNumClasses] = {{{", ".join(f'"{l}"' for l in labels)}}};"""
    (out / "model_config.h").write_text(f"""// Auto-generated by convert_tflite.py on {datetime.now():%Y-%m-%d %H:%M} - do not edit.
// Constants that MUST match training (scripts/features.py + scaler.json).
#ifndef ACCIDENT_MODEL_CONFIG_H_
#define ACCIDENT_MODEL_CONFIG_H_

#include <stdint.h>

// ---- sampling / windowing ----
constexpr int kSampleRateHz = {meta['fs_hz']};
constexpr int kWindowSize = {meta['window_size']};   // samples per inference window (1 s)
constexpr int kWindowStep = {meta['window_step']};   // run inference every 0.5 s
constexpr float kTempAlarmC = {_cf(meta['temp_alarm_c'])}; // optional hard rule backup

// ---- features (same order as training) ----
constexpr int kNumFeatures = {meta['n_features']};
{names}

// z[i] = clamp((x[i] - kFeatureMean[i]) / kFeatureStd[i], -kFeatureClip, kFeatureClip)
constexpr float kFeatureClip = {_cf(scaler['clip'])};
static const float kFeatureMean[kNumFeatures] = {{{_c_floats(scaler['mean'])}}};
static const float kFeatureStd[kNumFeatures]  = {{{_c_floats(scaler['std'])}}};

// ---- int8 quantization (input: q = round(z/scale) + zp, output: (q - zp)*scale) ----
constexpr float kInputScale = {_cf(info['input']['scale'])};
constexpr int32_t kInputZeroPoint = {info['input']['zero_point']};
constexpr float kOutputScale = {_cf(info['output']['scale'])};
constexpr int32_t kOutputZeroPoint = {info['output']['zero_point']};

{mode_block}

// ---- TFLite Micro ----
// Suggested arena (measured activations {info['activation_bytes_sum']} B + interpreter overhead).
// After AllocateTensors() print interpreter.arena_used_bytes() and trim.
constexpr int kTensorArenaSize = {arena};

#endif  // ACCIDENT_MODEL_CONFIG_H_
""")


# ----------------------------------------------------------------------------
def run_conversion(model_dir=None, n_rep=500, verbose=True) -> dict:
    import tensorflow as tf
    md = Path(model_dir) if model_dir else ROOT / "models/autoencoder"
    meta = json.loads((md / "meta.json").read_text())
    scaler = json.loads((md / "scaler.json").read_text())
    sp = np.load(md / "split.npz")
    model = tf.keras.models.load_model(md / "model.keras")
    mode = meta["mode"]

    tfl = convert_int8(model, sp["X_rep"], tf, n_rep)
    (md / "model_int8.tflite").write_bytes(tfl)
    runner = Int8Runner(tfl, tf)
    info = inspect_tflite(runner)

    X_te, y_te = sp["X_test"], sp["y_test"]
    f_out = model.predict(X_te, verbose=0)
    q_out = runner(X_te)
    report = {"mode": mode, "tflite_bytes": len(tfl), "tflite": info,
              "n_test_windows": int(len(X_te))}

    if mode == "autoencoder":
        thr = meta["threshold"]
        e_f = np.mean((f_out - X_te) ** 2, 1)
        e_q = np.mean((q_out - X_te) ** 2, 1)
        e_cal_q = np.mean((runner(sp["X_cal_normal"]) - sp["X_cal_normal"]) ** 2, 1)
        thr_q = float(np.percentile(e_cal_q, meta["threshold_percentile"]))
        y_true = (y_te != 0).astype(int)

        def m(err, t):
            from sklearn.metrics import precision_recall_fscore_support
            yh = (err > t).astype(int)
            p, r, f1, _ = precision_recall_fscore_support(y_true, yh, average="binary",
                                                          zero_division=0)
            return {"accuracy": float((yh == y_true).mean()), "precision": float(p),
                    "recall": float(r), "f1": float(f1)}
        report.update({
            "float": m(e_f, thr), "int8": m(e_q, thr_q),
            "int8_with_float_threshold": m(e_q, thr),
            "threshold_float": thr, "threshold_int8": thr_q,
            "decision_agreement": float(((e_f > thr) == (e_q > thr_q)).mean()),
        })
    else:
        thr_q = None
        pf, pq = f_out.argmax(1), q_out.argmax(1)
        report.update({"float": {"accuracy": float((pf == y_te).mean())},
                       "int8": {"accuracy": float((pq == y_te).mean())},
                       "decision_agreement": float((pf == pq).mean())})

    act = info["activation_bytes_sum"] or 1024
    arena = int(max(8192, int(np.ceil((act * 2 + 3072) / 1024.0)) * 1024))
    report["tensor_arena_suggestion_bytes"] = arena
    write_model_data(tfl, md)
    write_config(md, meta, scaler, info, thr_q, arena)
    (md / "conversion_report.json").write_text(json.dumps(report, indent=2))

    if verbose:
        print(f"[tflite] {md / 'model_int8.tflite'}: {len(tfl)} bytes ({len(tfl)/1024:.1f} KB)")
        print(f"[tflite] input {info['input']}  output {info['output']}")
        print(f"[tflite] tensor dtypes {info['tensor_dtypes']}  ops {info['ops']}")
        print(f"[tflite] fully int8 (int8 I/O, no float32 tensors): {info['fully_int8']}")
        print(f"[verify] float: {report['float']}")
        print(f"[verify] int8 : {report['int8']}")
        print(f"[verify] float/int8 decision agreement: {report['decision_agreement']:.3f}")
        if mode == "autoencoder":
            print(f"[verify] threshold float={report['threshold_float']:.4f} "
                  f"int8={report['threshold_int8']:.4f}")
        print(f"[export] model_data.h, model_data.cc, model_config.h -> {md}")
        print(f"[export] tensor arena suggestion: {arena} B "
              f"(activations {info['activation_bytes_sum']} B); tune with arena_used_bytes()")
    return report


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model-dir", type=Path, default=ROOT / "models/autoencoder")
    ap.add_argument("--num-rep", type=int, default=500,
                    help="representative samples for calibration")
    a = ap.parse_args(argv)
    r = run_conversion(a.model_dir, a.num_rep)
    if not r["tflite"]["fully_int8"]:
        sys.exit("ERROR: model is not fully int8")


if __name__ == "__main__":
    main()
