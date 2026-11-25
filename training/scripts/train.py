"""
train.py - train the TinyML model on windowed sensor-fusion features.

Modes:
  --mode autoencoder (DEFAULT, primary)
      Trained on NORMAL windows only. Anomaly score = mean squared
      reconstruction error; threshold = Nth percentile (default 99) of the
      error on held-out normal windows. Detects "anything unusual"
      (crash, rollover, fire, ...) without needing many crash recordings.
  --mode classifier (optional)
      Small dense softmax net: normal / crash / rollover / abnormal_temp.

Outputs in models/<mode>/:
  model.keras          Keras model
  scaler.json          feature mean/std/clip (port to C)
  meta.json            labels, threshold (AE), feature names, window config
  metrics.json         test-set metrics (confusion matrix, precision/recall)
  confusion_matrix.png, training_curve.png (+ error_hist.png for AE)
  split.npz            scaled train/test/calibration arrays for convert_tflite.py
Also writes data/processed/features.csv.

Example:
  python scripts/train.py                         # autoencoder, data/raw/*.csv
  python scripts/train.py --mode classifier --epochs 150
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys
import time
from pathlib import Path

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

import numpy as np
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent))
import features as F  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


# ----------------------------------------------------------------------------
# data
# ----------------------------------------------------------------------------
def build_features(data_paths, processed_out: Path | None = None) -> pd.DataFrame:
    df = F.load_csvs(data_paths)
    fx = F.extract_features(df)
    if processed_out is not None:
        processed_out.parent.mkdir(parents=True, exist_ok=True)
        fx.to_csv(processed_out, index=False, float_format="%.5f")
    return fx


def session_split(fx: pd.DataFrame, test_frac=0.25, seed=42):
    """Split by SESSION (no window leakage), stratified by the session's
    event type so every event type lands in train and test when possible."""
    rng = np.random.default_rng(seed)
    sev = {"normal": 0, "abnormal_temp": 1, "rollover": 2, "crash": 3}
    stype = (fx[fx.label != F.AMBIGUOUS].groupby("session_id")["label"]
             .agg(lambda s: max(s, key=lambda x: sev.get(x, 0))))
    test_ids = []
    for _, ids in stype.groupby(stype):
        ids = ids.index.to_numpy().copy()
        rng.shuffle(ids)
        k = int(round(len(ids) * test_frac))
        if len(ids) >= 2:
            k = max(1, min(k, len(ids) - 1))
        test_ids.extend(ids[:k].tolist())
    is_test = fx.session_id.isin(test_ids).to_numpy()
    return ~is_test, is_test


# ----------------------------------------------------------------------------
# models (tiny on purpose: a few KB as int8)
# ----------------------------------------------------------------------------
def make_autoencoder(n_in, tf):
    k = tf.keras
    inp = k.Input((n_in,), name="features")
    x = k.layers.Dense(16, activation="relu")(inp)
    x = k.layers.Dense(6, activation="relu", name="bottleneck")(x)
    x = k.layers.Dense(16, activation="relu")(x)
    out = k.layers.Dense(n_in, activation="linear", name="reconstruction")(x)
    m = k.Model(inp, out, name="accident_autoencoder")
    m.compile(optimizer=k.optimizers.Adam(2e-3), loss="mse")
    return m


def make_classifier(n_in, n_cls, tf):
    k = tf.keras
    inp = k.Input((n_in,), name="features")
    x = k.layers.Dense(24, activation="relu")(inp)
    x = k.layers.Dropout(0.1)(x)
    x = k.layers.Dense(16, activation="relu")(x)
    out = k.layers.Dense(n_cls, activation="softmax", name="probs")(x)
    m = k.Model(inp, out, name="accident_classifier")
    m.compile(optimizer=k.optimizers.Adam(2e-3),
              loss="sparse_categorical_crossentropy", metrics=["accuracy"])
    return m


def recon_error(model_or_fn, X):
    R = model_or_fn(X)
    return np.mean((np.asarray(R, np.float32) - X) ** 2, axis=1)


# ----------------------------------------------------------------------------
# metrics / plots
# ----------------------------------------------------------------------------
def binary_metrics(y_true_anom, y_pred_anom):
    from sklearn.metrics import confusion_matrix, precision_recall_fscore_support
    cm = confusion_matrix(y_true_anom, y_pred_anom, labels=[0, 1])
    p, r, f, _ = precision_recall_fscore_support(
        y_true_anom, y_pred_anom, average="binary", zero_division=0)
    acc = float((y_true_anom == y_pred_anom).mean())
    return {"accuracy": acc, "precision": float(p), "recall": float(r),
            "f1": float(f), "confusion_matrix": cm.tolist(),
            "confusion_labels": ["normal", "anomaly"]}


def multiclass_metrics(y_true, y_pred, labels):
    from sklearn.metrics import classification_report, confusion_matrix
    idx = list(range(len(labels)))
    cm = confusion_matrix(y_true, y_pred, labels=idx)
    rep = classification_report(y_true, y_pred, labels=idx, target_names=labels,
                                output_dict=True, zero_division=0)
    return {"accuracy": float((y_true == y_pred).mean()),
            "macro_f1": float(rep["macro avg"]["f1-score"]),
            "per_class": {l: {k: float(rep[l][k]) for k in
                              ("precision", "recall", "f1-score", "support")}
                          for l in labels},
            "confusion_matrix": cm.tolist(), "confusion_labels": labels}


def plot_cm(cm, labels, path, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    cm = np.asarray(cm)
    fig, ax = plt.subplots(figsize=(1.3 * len(labels) + 2, 1.1 * len(labels) + 1.5))
    ax.imshow(cm, cmap="Blues")
    ax.set_xticks(range(len(labels)), labels, rotation=30, ha="right")
    ax.set_yticks(range(len(labels)), labels)
    for i in range(cm.shape[0]):
        for j in range(cm.shape[1]):
            ax.text(j, i, int(cm[i, j]), ha="center", va="center",
                    color="white" if cm[i, j] > cm.max() / 2 else "black")
    ax.set_xlabel("predicted"); ax.set_ylabel("true"); ax.set_title(title)
    fig.tight_layout(); fig.savefig(path, dpi=110); plt.close(fig)


def plot_history(hist, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(5, 3))
    ax.plot(hist.history["loss"], label="train")
    if "val_loss" in hist.history:
        ax.plot(hist.history["val_loss"], label="val")
    ax.set_xlabel("epoch"); ax.set_ylabel("loss"); ax.set_yscale("log"); ax.legend()
    fig.tight_layout(); fig.savefig(path, dpi=110); plt.close(fig)


# ----------------------------------------------------------------------------
# main training routine (also called from the notebook)
# ----------------------------------------------------------------------------
def run_training(data=None, mode="autoencoder", out_dir=None, epochs=None,
                 percentile=99.0, seed=42, batch_size=64, verbose=1,
                 features_df: pd.DataFrame | None = None) -> dict:
    import tensorflow as tf
    tf.keras.utils.set_random_seed(seed)
    try:
        tf.config.experimental.enable_op_determinism()
    except Exception:
        pass

    out = Path(out_dir) if out_dir else ROOT / "models" / mode
    out.mkdir(parents=True, exist_ok=True)

    if features_df is None:
        paths = data or sorted(glob.glob(str(ROOT / "data/raw/*.csv")))
        if isinstance(paths, (str, Path)):
            paths = sorted(glob.glob(str(paths))) or [paths]
        print(f"[data] {len(paths)} file(s): {[Path(p).name for p in paths]}")
        fx = build_features(paths, ROOT / "data/processed/features.csv")
    else:
        fx = features_df
    n_amb = int((fx.label == F.AMBIGUOUS).sum())
    fx = fx[fx.label != F.AMBIGUOUS].reset_index(drop=True)
    print(f"[data] {len(fx)} windows ({n_amb} ambiguous dropped):",
          fx.label.value_counts().to_dict())

    X_all = fx[F.FEATURE_NAMES].to_numpy(np.float32)
    y_all = fx.label.map({l: i for i, l in enumerate(F.LABELS)}).to_numpy(np.int64)
    tr, te = session_split(fx, seed=seed)
    rng = np.random.default_rng(seed)
    t0 = time.time()

    if mode == "autoencoder":
        epochs = epochs or 200
        norm_tr = np.where(tr & (y_all == 0))[0]
        rng.shuffle(norm_tr)
        n_cal = max(20, int(0.2 * len(norm_tr)))
        cal_idx, fit_idx = norm_tr[:n_cal], norm_tr[n_cal:]
        scaler = F.StandardScalerLite().fit(X_all[fit_idx])
        Xf, Xc = scaler.transform(X_all[fit_idx]), scaler.transform(X_all[cal_idx])
        model = make_autoencoder(F.N_FEATURES, tf)
        hist = model.fit(Xf, Xf, validation_data=(Xc, Xc), epochs=epochs,
                         batch_size=batch_size, verbose=verbose if verbose > 1 else 0,
                         callbacks=[tf.keras.callbacks.EarlyStopping(
                             patience=20, restore_best_weights=True)])
        pred = lambda X: model.predict(X, verbose=0)
        err_cal = recon_error(pred, Xc)
        threshold = float(np.percentile(err_cal, percentile))

        X_te, y_te = scaler.transform(X_all[te]), y_all[te]
        err_te = recon_error(pred, X_te)
        y_hat = (err_te > threshold).astype(int)
        metrics = binary_metrics((y_te != 0).astype(int), y_hat)
        metrics["detection_rate_per_event"] = {
            l: float(y_hat[y_te == i].mean()) if (y_te == i).any() else None
            for i, l in enumerate(F.LABELS) if i > 0}
        metrics["false_alarm_rate_normal"] = float(y_hat[y_te == 0].mean())
        meta_extra = {"threshold": threshold, "threshold_percentile": percentile,
                      "score": "mean((reconstruction - z)^2) over features",
                      "output": "reconstruction of the 12 standardized features"}
        plot_cm(metrics["confusion_matrix"], ["normal", "anomaly"],
                out / "confusion_matrix.png", "Autoencoder (test)")
        # error histogram
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(figsize=(6, 3))
        bins = np.logspace(np.log10(max(err_te.min(), 1e-4)), np.log10(err_te.max() + 1e-3), 50)
        for i, l in enumerate(F.LABELS):
            if (y_te == i).any():
                ax.hist(err_te[y_te == i], bins=bins, alpha=0.6, label=l)
        ax.axvline(threshold, color="k", ls="--", label=f"threshold={threshold:.3f}")
        ax.set_xscale("log"); ax.set_xlabel("reconstruction MSE"); ax.legend(fontsize=7)
        fig.tight_layout(); fig.savefig(out / "error_hist.png", dpi=110); plt.close(fig)
        X_rep = scaler.transform(X_all[tr])        # representative data (all train windows)
        X_cal = Xc                                  # normal windows for int8 threshold
        np.savez_compressed(out / "split.npz", X_rep=X_rep, X_test=X_te, y_test=y_te,
                            X_cal_normal=X_cal)
    elif mode == "classifier":
        epochs = epochs or 300
        idx_tr = np.where(tr)[0]
        rng.shuffle(idx_tr)
        n_val = max(20, int(0.15 * len(idx_tr)))
        val_idx, fit_idx = idx_tr[:n_val], idx_tr[n_val:]
        scaler = F.StandardScalerLite().fit(X_all[fit_idx])
        Xf, yf = scaler.transform(X_all[fit_idx]), y_all[fit_idx]
        Xv, yv = scaler.transform(X_all[val_idx]), y_all[val_idx]
        counts = np.bincount(yf, minlength=len(F.LABELS)).astype(float)
        cw = {i: float(len(yf) / (len(F.LABELS) * c)) if c > 0 else 1.0
              for i, c in enumerate(counts)}
        model = make_classifier(F.N_FEATURES, len(F.LABELS), tf)
        hist = model.fit(Xf, yf, validation_data=(Xv, yv), epochs=epochs,
                         batch_size=batch_size, class_weight=cw,
                         verbose=verbose if verbose > 1 else 0,
                         callbacks=[tf.keras.callbacks.EarlyStopping(
                             patience=30, restore_best_weights=True)])
        X_te, y_te = scaler.transform(X_all[te]), y_all[te]
        y_hat = model.predict(X_te, verbose=0).argmax(1)
        metrics = multiclass_metrics(y_te, y_hat, F.LABELS)
        meta_extra = {"class_weight": cw, "output": "softmax probabilities, order = labels"}
        plot_cm(metrics["confusion_matrix"], F.LABELS, out / "confusion_matrix.png",
                "Classifier (test)")
        np.savez_compressed(out / "split.npz", X_rep=scaler.transform(X_all[tr]),
                            X_test=X_te, y_test=y_te)
    else:
        raise ValueError("mode must be 'autoencoder' or 'classifier'")

    plot_history(hist, out / "training_curve.png")
    model.save(out / "model.keras")
    scaler.save(out / "scaler.json")
    meta = {"mode": mode, "labels": F.LABELS, "feature_names": F.FEATURE_NAMES,
            "n_features": F.N_FEATURES, "fs_hz": F.FS, "window_size": F.WINDOW_SIZE,
            "window_step": F.WINDOW_STEP, "label_min_fraction": F.LABEL_MIN_FRACTION,
            "feature_clip": F.FEATURE_CLIP, "temp_alarm_c": F.TEMP_ALARM_C,
            "epochs_run": len(hist.history["loss"]), "seed": seed,
            "n_params": int(model.count_params()),
            "n_train_windows": int(tr.sum()), "n_test_windows": int(te.sum()),
            "tensorflow": tf.__version__, **meta_extra}
    (out / "meta.json").write_text(json.dumps(meta, indent=2))
    metrics["mode"] = mode
    metrics["n_test_windows"] = int(te.sum())
    (out / "metrics.json").write_text(json.dumps(metrics, indent=2))

    print(f"[train] {mode}: {meta['epochs_run']} epochs, {meta['n_params']} params, "
          f"{time.time() - t0:.1f}s -> {out}")
    if mode == "autoencoder":
        print(f"[train] threshold (p{percentile:g} normal val error) = {threshold:.4f}")
        print(f"[test ] acc={metrics['accuracy']:.3f} P={metrics['precision']:.3f} "
              f"R={metrics['recall']:.3f} F1={metrics['f1']:.3f} "
              f"false-alarm={metrics['false_alarm_rate_normal']:.3f}")
        print(f"[test ] detection per event: {metrics['detection_rate_per_event']}")
    else:
        print(f"[test ] acc={metrics['accuracy']:.3f} macro-F1={metrics['macro_f1']:.3f}")
    print(f"[test ] confusion matrix {metrics['confusion_labels']}:\n"
          f"{np.array(metrics['confusion_matrix'])}")
    return {"model": model, "scaler": scaler, "meta": meta, "metrics": metrics,
            "out_dir": out}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", nargs="+", default=None,
                    help="raw CSV file(s) or glob (default: data/raw/*.csv)")
    ap.add_argument("--mode", choices=["autoencoder", "classifier"], default="autoencoder")
    ap.add_argument("--out", type=Path, default=None, help="default: models/<mode>")
    ap.add_argument("--epochs", type=int, default=None)
    ap.add_argument("--batch-size", type=int, default=64)
    ap.add_argument("--percentile", type=float, default=99.0,
                    help="AE threshold percentile of normal validation error")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--verbose", type=int, default=1)
    a = ap.parse_args(argv)
    data = None
    if a.data:
        data = [p for pat in a.data for p in (sorted(glob.glob(pat)) or [pat])]
    run_training(data, a.mode, a.out, a.epochs, a.percentile, a.seed,
                 a.batch_size, a.verbose)


if __name__ == "__main__":
    main()
