"""
features.py - shared windowing + feature extraction (sensor fusion).

Used by train.py, convert_tflite.py and the notebook, so the exact same
features are produced everywhere. The formulas are intentionally simple so
they can be re-implemented 1:1 in C/C++ on the ESP32-S3 later.

Window: 1 s (100 samples @ 100 Hz), 50 % overlap (step 50 samples).

Features (12, order matters - the MCU must use the same order):
  0  acc_mag_mean   mean(|a|)                       [g]
  1  acc_mag_std    std(|a|)  (population std)      [g]
  2  acc_mag_max    max(|a|)                        [g]
  3  jerk_max       max(abs(diff(|a|))) * fs        [g/s]
  4  gyro_mag_mean  mean(|w|)                       [deg/s]
  5  gyro_mag_max   max(|w|)                        [deg/s]
  6  tilt_deg       acos(mean_az / |mean_a|)        [deg]  (0 = upright)
  7  temp_mean      mean(T)                         [C]
  8  temp_slope     (T[last]-T[first]) * fs/(N-1)   [C/s]
  9  vib_mean       mean(v)                         [0..1]
  10 vib_rms        sqrt(mean((v-mean(v))^2))       [0..1]
  11 vib_peak       max(v)                          [0..1]

|a| = sqrt(ax^2+ay^2+az^2), |w| = sqrt(gx^2+gy^2+gz^2)
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pandas as pd

FS = 100                 # sample rate [Hz]
WINDOW_SIZE = 100        # samples per window (1 s)
WINDOW_STEP = 50         # hop (50 % overlap)
LABEL_MIN_FRACTION = 0.25  # an event label needs >= 25 % of the window
FEATURE_CLIP = 8.0       # standardized features are clipped to [-8, 8]
TEMP_ALARM_C = 60.0      # absolute over-temperature alarm level [C]

SENSOR_COLS = ["ax", "ay", "az", "gx", "gy", "gz", "temperature", "vibration"]
CSV_COLS = ["session_id", "timestamp_ms"] + SENSOR_COLS + ["label", "scenario"]

# class order (index = class id in the classifier)
LABELS = ["normal", "crash", "rollover", "abnormal_temp"]
# when several events fall into one window, the most severe wins
EVENT_PRIORITY = ["crash", "rollover", "abnormal_temp"]
AMBIGUOUS = "ambiguous"   # window label for partial-event windows (dropped)

FEATURE_NAMES = [
    "acc_mag_mean", "acc_mag_std", "acc_mag_max", "jerk_max",
    "gyro_mag_mean", "gyro_mag_max", "tilt_deg",
    "temp_mean", "temp_slope",
    "vib_mean", "vib_rms", "vib_peak",
]
N_FEATURES = len(FEATURE_NAMES)


# ----------------------------------------------------------------------------
# core feature math (works on a batch of windows: shape [n_win, N])
# ----------------------------------------------------------------------------
def window_features(acc: np.ndarray, gyro: np.ndarray, temp: np.ndarray,
                    vib: np.ndarray, fs: int = FS) -> np.ndarray:
    """acc/gyro: [n_win, N, 3]; temp/vib: [n_win, N]. Returns [n_win, 12]."""
    acc = np.asarray(acc, dtype=np.float64)
    gyro = np.asarray(gyro, dtype=np.float64)
    temp = np.asarray(temp, dtype=np.float64)
    vib = np.asarray(vib, dtype=np.float64)
    n = acc.shape[1]

    a_mag = np.sqrt((acc ** 2).sum(axis=2))
    w_mag = np.sqrt((gyro ** 2).sum(axis=2))

    mean_a = acc.mean(axis=1)                       # [n_win, 3]
    mean_a_norm = np.linalg.norm(mean_a, axis=1) + 1e-9
    tilt = np.degrees(np.arccos(np.clip(mean_a[:, 2] / mean_a_norm, -1.0, 1.0)))

    vib_mean = vib.mean(axis=1)
    feats = np.stack([
        a_mag.mean(axis=1),
        a_mag.std(axis=1),
        a_mag.max(axis=1),
        np.abs(np.diff(a_mag, axis=1)).max(axis=1) * fs,
        w_mag.mean(axis=1),
        w_mag.max(axis=1),
        tilt,
        temp.mean(axis=1),
        (temp[:, -1] - temp[:, 0]) * fs / (n - 1),
        vib_mean,
        np.sqrt(((vib - vib_mean[:, None]) ** 2).mean(axis=1)),
        vib.max(axis=1),
    ], axis=1)
    return feats.astype(np.float32)


def window_label(labels_in_window: np.ndarray,
                 min_fraction: float = LABEL_MIN_FRACTION) -> str:
    """Event label if it covers >= min_fraction of the window (most severe
    first). Windows that touch an event only a little (0 < share < min) are
    'ambiguous' and are dropped from training/evaluation."""
    n = len(labels_in_window)
    touched = False
    for ev in EVENT_PRIORITY:
        c = np.count_nonzero(labels_in_window == ev)
        if c >= min_fraction * n:
            return ev
        touched |= c > 0
    return AMBIGUOUS if touched else "normal"


def extract_features(df: pd.DataFrame, window: int = WINDOW_SIZE,
                     step: int = WINDOW_STEP, fs: int = FS) -> pd.DataFrame:
    """Slice every session into windows and compute features.

    Returns a DataFrame with columns:
      session_id, t_start_ms, <12 features>, label, scenario
    'label'/'scenario' are optional in the input (unlabeled logs -> 'normal').
    """
    from numpy.lib.stride_tricks import sliding_window_view

    if "session_id" not in df.columns:
        df = df.assign(session_id=0)
    if "label" not in df.columns:
        df = df.assign(label="normal")
    if "scenario" not in df.columns:
        df = df.assign(scenario="unknown")

    rows = []
    for sid, s in df.groupby("session_id", sort=True):
        s = s.sort_values("timestamp_ms")
        if len(s) < window:
            continue
        acc = s[["ax", "ay", "az"]].to_numpy(np.float64)
        gyr = s[["gx", "gy", "gz"]].to_numpy(np.float64)
        tmp = s["temperature"].to_numpy(np.float64)
        vib = s["vibration"].to_numpy(np.float64)
        lab = s["label"].astype(str).to_numpy()
        scn = s["scenario"].astype(str).to_numpy()
        ts = s["timestamp_ms"].to_numpy(np.int64)

        acc_w = sliding_window_view(acc, window, axis=0)[::step].transpose(0, 2, 1)
        gyr_w = sliding_window_view(gyr, window, axis=0)[::step].transpose(0, 2, 1)
        tmp_w = sliding_window_view(tmp, window)[::step]
        vib_w = sliding_window_view(vib, window)[::step]
        feats = window_features(acc_w, gyr_w, tmp_w, vib_w, fs)

        starts = np.arange(0, len(s) - window + 1, step)
        for k, st in enumerate(starts):
            vals, cnt = np.unique(scn[st:st + window], return_counts=True)
            rows.append((sid, int(ts[st]), *feats[k],
                         window_label(lab[st:st + window]), vals[np.argmax(cnt)]))
    cols = ["session_id", "t_start_ms"] + FEATURE_NAMES + ["label", "scenario"]
    return pd.DataFrame(rows, columns=cols)


# ----------------------------------------------------------------------------
# scaler (mean/std as plain JSON so it can be ported to C)
# ----------------------------------------------------------------------------
class StandardScalerLite:
    def __init__(self, mean=None, std=None, clip: float = FEATURE_CLIP):
        self.mean = None if mean is None else np.asarray(mean, np.float32)
        self.std = None if std is None else np.asarray(std, np.float32)
        self.clip = float(clip)

    def fit(self, X: np.ndarray):
        X = np.asarray(X, np.float64)
        self.mean = X.mean(axis=0).astype(np.float32)
        std = X.std(axis=0)
        std[std < 1e-6] = 1.0          # avoid divide-by-zero
        self.std = std.astype(np.float32)
        return self

    def transform(self, X: np.ndarray) -> np.ndarray:
        Z = (np.asarray(X, np.float32) - self.mean) / self.std
        return np.clip(Z, -self.clip, self.clip).astype(np.float32)

    def fit_transform(self, X):
        return self.fit(X).transform(X)

    def to_dict(self):
        return {"feature_names": FEATURE_NAMES, "mean": self.mean.tolist(),
                "std": self.std.tolist(), "clip": self.clip,
                "formula": "z = clip((x - mean) / std, -clip, clip)"}

    def save(self, path):
        Path(path).write_text(json.dumps(self.to_dict(), indent=2))

    @classmethod
    def load(cls, path):
        d = json.loads(Path(path).read_text())
        return cls(d["mean"], d["std"], d.get("clip", FEATURE_CLIP))


def load_csvs(paths) -> pd.DataFrame:
    """Load one or more raw CSVs; session ids are made unique across files."""
    frames, offset = [], 0
    for p in paths:
        d = pd.read_csv(p)
        missing = [c for c in ["timestamp_ms"] + SENSOR_COLS if c not in d.columns]
        if missing:
            raise ValueError(f"{p}: missing columns {missing}")
        if "session_id" not in d.columns:
            d["session_id"] = 0
        d["session_id"] = d["session_id"].astype(int) + offset
        offset = int(d["session_id"].max()) + 1
        frames.append(d)
    if not frames:
        raise FileNotFoundError("no CSV files given")
    return pd.concat(frames, ignore_index=True)
