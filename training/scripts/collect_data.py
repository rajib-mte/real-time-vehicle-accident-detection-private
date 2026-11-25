"""
collect_data.py - build a raw sensor dataset (100 Hz, MPU6050-style).

Three modes:
  simulate (default)  synthetic, labeled sessions (seedable)
  --serial PORT       log real CSV lines from a device over USB serial
  --load CSV [CSV..]  import / validate / merge existing CSV logs

Output CSV columns:
  session_id, timestamp_ms, ax, ay, az [g], gx, gy, gz [deg/s],
  temperature [C], vibration [0..1 normalized ADC], label, scenario

Labels: normal | crash | rollover | abnormal_temp
Normal scenarios: idle, cruising, braking, turning, bumpy_road

Examples:
  python scripts/collect_data.py --sessions 40 --duration 60 --seed 42
  python scripts/collect_data.py --serial COM5 --baud 115200 --label normal \
         --scenario cruising --seconds 120 --out data/raw/real_cruise.csv
  python scripts/collect_data.py --load logs/a.csv logs/b.csv --out data/raw/merged.csv

NOTE: simulated data is NOT real crash data. Retrain on real measurements.
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent))
from features import CSV_COLS, FS, SENSOR_COLS, TEMP_ALARM_C, load_csvs  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
NORMAL_SCENARIOS = ["idle", "cruising", "braking", "turning", "bumpy_road"]
NORMAL_P = [0.15, 0.35, 0.15, 0.2, 0.15]
EVENTS = ["none", "crash", "rollover", "abnormal_temp"]

ACC_RANGE_G = 16.0      # MPU6050 +/-16 g
GYRO_RANGE_DPS = 2000.0 # MPU6050 +/-2000 dps


# ----------------------------------------------------------------------------
# helpers
# ----------------------------------------------------------------------------
def _lowpass_noise(rng, n, sigma, alpha=0.02):
    """first-order low-passed gaussian noise, unit-ish std scaled by sigma."""
    w = rng.normal(0, 1, n)
    y = np.empty(n)
    acc = 0.0
    for i in range(n):
        acc += alpha * (w[i] - acc)
        y[i] = acc
    s = y.std() + 1e-9
    return y / s * sigma


def _half_sine(n):
    return np.sin(np.linspace(0, np.pi, n)) if n > 1 else np.ones(n)


def _smoothstep(n):
    x = np.linspace(0, 1, n)
    return x * x * (3 - 2 * x)


class _Session:
    """Holds the per-sample component arrays of one session."""

    def __init__(self, rng, n):
        self.rng = rng
        self.n = n
        self.lin = np.zeros((n, 3))        # linear accel (vehicle) [g]
        self.gyro = np.zeros((n, 3))       # [deg/s]
        self.roll = np.full(n, rng.normal(0, 2.0))   # mounting offset [deg]
        self.pitch = np.full(n, rng.normal(0, 2.0))
        self.vib_level = np.zeros(n)
        self.vib_extra = np.zeros(n)
        self.label = np.array(["normal"] * n, dtype=object)
        self.scenario = np.array([""] * n, dtype=object)

    # ---- normal driving segments ------------------------------------------
    def segment(self, sc, a, b):
        rng, L = self.rng, b - a
        if L <= 0:
            return
        sl = slice(a, b)
        self.scenario[sl] = sc
        t = np.arange(L) / FS
        if sc == "idle":
            f = rng.uniform(20, 30)                      # engine hum
            self.lin[sl, 2] += 0.015 * np.sin(2 * np.pi * f * t)
            self.vib_level[sl] = rng.uniform(0.05, 0.10)
        elif sc == "cruising":
            self.lin[sl, 0] += _lowpass_noise(rng, L, 0.05)
            self.lin[sl, 1] += _lowpass_noise(rng, L, 0.03)
            self.lin[sl, 2] += rng.normal(0, 0.03, L)    # road texture
            self.gyro[sl, 2] += _lowpass_noise(rng, L, 1.5)
            self.vib_level[sl] = rng.uniform(0.12, 0.20)
        elif sc == "braking":
            A = rng.uniform(0.3, 0.8)
            shape = np.sin(np.linspace(0, np.pi, L)) ** 2
            self.lin[sl, 0] += -A * shape
            self.lin[sl, 2] += rng.normal(0, 0.03, L)
            self.pitch[sl] += -2.5 * A * shape           # nose dive
            self.gyro[sl, 1] += np.gradient(-2.5 * A * shape) * FS
            self.vib_level[sl] = rng.uniform(0.12, 0.20)
        elif sc == "turning":
            A = rng.uniform(0.15, 0.5) * rng.choice([-1, 1])
            shape = _half_sine(L)
            self.lin[sl, 1] += A * shape
            self.gyro[sl, 2] += np.sign(A) * rng.uniform(10, 35) * shape
            self.roll[sl] += 3.0 * A * shape             # body roll
            self.lin[sl, 2] += rng.normal(0, 0.03, L)
            self.vib_level[sl] = rng.uniform(0.12, 0.20)
        elif sc == "bumpy_road":
            self.lin[sl, 2] += rng.normal(0, 0.12, L)
            self.lin[sl, 0] += _lowpass_noise(rng, L, 0.06)
            self.gyro[sl, 0] += rng.normal(0, 6, L)
            self.gyro[sl, 1] += rng.normal(0, 6, L)
            for _ in range(rng.integers(1, 2 + L // FS)):   # potholes
                d = int(rng.uniform(0.02, 0.06) * FS) + 1
                s0 = rng.integers(0, max(1, L - d))
                amp = rng.uniform(0.4, 1.4) * rng.choice([-1, 1])
                self.lin[a + s0:a + s0 + d, 2] += amp * _half_sine(d)[: L - s0]
                self.gyro[a + s0:a + s0 + d, 0] += amp * 25 * _half_sine(d)[: L - s0]
                self.vib_extra[a + s0:a + min(L, s0 + 3 * d)] += 0.25
            self.vib_level[sl] = rng.uniform(0.30, 0.55)
        else:
            raise ValueError(sc)

    def fill_normal(self, a, b, first="idle"):
        i, sc = a, first
        while i < b:
            L = int(self.rng.uniform(3, 10) * FS)
            L = min(L, b - i)
            self.segment(sc, i, i + L)
            i += L
            sc = self.rng.choice(NORMAL_SCENARIOS, p=NORMAL_P)

    # ---- events ------------------------------------------------------------
    def crash(self, a):
        rng, n = self.rng, self.n
        self.fill_normal(max(0, a - 3 * FS), a, first="cruising")
        d = int(rng.uniform(0.03, 0.15) * FS) + 2       # impact duration
        A = rng.uniform(4.0, 15.0)                      # peak g
        ang = rng.normal(np.pi, 0.7)                    # mostly frontal
        p = _half_sine(d)
        e = min(n, a + d)
        p = p[: e - a]
        self.lin[a:e, 0] += A * np.cos(ang) * p
        self.lin[a:e, 1] += A * np.sin(ang) * p
        self.lin[a:e, 2] += rng.uniform(0.5, 2.5) * rng.choice([-1, 1]) * p
        # ringing + angular kick, decaying
        L = min(n - a, int(1.0 * FS))
        tt = np.arange(L) / FS
        decay = np.exp(-tt / rng.uniform(0.15, 0.35))
        self.lin[a:a + L, 0] += 0.4 * decay * np.sin(2 * np.pi * rng.uniform(8, 15) * tt)
        self.gyro[a:a + L, 2] += rng.uniform(80, 400) * rng.choice([-1, 1]) * decay
        self.gyro[a:a + L, 0] += rng.uniform(30, 200) * rng.choice([-1, 1]) * decay
        self.gyro[a:a + L, 1] += rng.uniform(30, 200) * rng.choice([-1, 1]) * decay
        self.vib_extra[a:a + L] += 0.9 * decay
        self.label[a:a + L] = "crash"
        self.scenario[a:a + L] = "crash_impact"
        # aftermath: vehicle stopped, small final tilt
        if a + L < n:
            self.segment("idle", a + L, n)
            self.scenario[a + L:] = "post_crash_idle"
            off = rng.normal(0, 4)
            self.roll[a + L:] += off
            self.vib_level[a + L:] = rng.uniform(0.0, 0.06)

    def rollover(self, a):
        rng, n = self.rng, self.n
        Tr = int(rng.uniform(1.0, 3.0) * FS)
        e = min(n, a + Tr)
        theta = rng.uniform(80, 200) * rng.choice([-1, 1])
        axis_roll = rng.random() < 0.75                 # roll vs pitch-over
        prof = theta * _smoothstep(Tr)[: e - a]
        tgt = self.roll if axis_roll else self.pitch
        base = tgt[a]
        tgt[a:e] = base + prof
        tgt[e:] = base + theta + rng.normal(0, 1.0, n - e)
        gi = 0 if axis_roll else 1
        self.gyro[a:e, gi] += np.gradient(prof) * FS
        self.gyro[a:e, :] += rng.normal(0, 15, (e - a, 3))
        for _ in range(rng.integers(2, 6)):             # body impacts
            d = int(rng.uniform(0.03, 0.08) * FS) + 1
            s0 = rng.integers(a, max(a + 1, e - d))
            amp = rng.uniform(2.0, 6.0)
            v = rng.normal(0, 1, 3)
            v /= np.linalg.norm(v)
            seg = _half_sine(d)[: max(0, min(n, s0 + d) - s0)]
            self.lin[s0:s0 + len(seg)] += np.outer(seg, v * amp)
        self.vib_level[a:e] = rng.uniform(0.5, 0.8)
        self.vib_level[e:] = rng.uniform(0.0, 0.08)
        self.label[a:] = "rollover"
        self.scenario[a:e] = "rollover_rotating"
        self.scenario[e:] = "rollover_resting"
        self.lin[e:] = 0.0

    def abnormal_temp(self, a, temp):
        rng, n = self.rng, self.n
        self.fill_normal(a, n, first=rng.choice(["idle", "cruising"]))
        T0 = temp[a]
        Tmax = rng.uniform(95, 150)
        tau = rng.uniform(6, 20)
        tt = np.arange(n - a) / FS
        temp[a:] = T0 + (Tmax - T0) * (1 - np.exp(-tt / tau))
        # labelled from the start of the fire: the fast rise (several C/s)
        # is already abnormal even before the absolute alarm level is hit
        self.label[a:] = "abnormal_temp"
        self.scenario[a:] = np.where(temp[a:] >= TEMP_ALARM_C, "engine_fire",
                                     "engine_fire_rising")

    # ---- compose -------------------------------------------------------------
    def render(self, temp):
        rng, n = self.rng, self.n
        r, p = np.radians(self.roll), np.radians(self.pitch)
        g = np.stack([-np.sin(p), np.sin(r) * np.cos(p), np.cos(r) * np.cos(p)], 1)
        acc = g + self.lin + rng.normal(0, 0.01, (n, 3))         # sensor noise
        gyro = self.gyro + rng.normal(0, 0.25, (n, 3)) + rng.normal(0, 0.5, 3)  # + bias
        vib = (self.vib_level * (1 + 0.3 * rng.normal(0, 1, n))
               + self.vib_extra + np.abs(rng.normal(0, 0.01, n)))
        temp_q = np.round((temp + rng.normal(0, 0.05, n)) / 0.0625) * 0.0625  # DS18B20 LSB
        return (np.clip(acc, -ACC_RANGE_G, ACC_RANGE_G),
                np.clip(gyro, -GYRO_RANGE_DPS, GYRO_RANGE_DPS),
                temp_q, np.clip(vib, 0.0, 1.0))


def simulate_session(rng, duration_s: float, event: str) -> pd.DataFrame:
    n = int(duration_s * FS)
    s = _Session(rng, n)
    temp = rng.uniform(22, 42) + _lowpass_noise(rng, n, 0.5, alpha=0.001)
    if event == "none":
        s.fill_normal(0, n, first=rng.choice(NORMAL_SCENARIOS))
    else:
        a = int(rng.uniform(0.5, 0.75) * n)
        s.fill_normal(0, a, first=rng.choice(NORMAL_SCENARIOS))
        if event == "crash":
            s.crash(a)
        elif event == "rollover":
            s.fill_normal(max(0, a - 2 * FS), a, first="turning")
            s.rollover(a)
        elif event == "abnormal_temp":
            s.abnormal_temp(a, temp)
        else:
            raise ValueError(event)
    acc, gyro, temp_q, vib = s.render(temp)
    return pd.DataFrame({
        "timestamp_ms": (np.arange(n) * 1000 // FS).astype(np.int64),
        "ax": acc[:, 0], "ay": acc[:, 1], "az": acc[:, 2],
        "gx": gyro[:, 0], "gy": gyro[:, 1], "gz": gyro[:, 2],
        "temperature": temp_q, "vibration": vib,
        "label": s.label, "scenario": s.scenario,
    })


def parse_mix(text: str) -> dict:
    mix = {}
    for part in text.split(","):
        k, v = part.split(":")
        k = k.strip()
        if k not in EVENTS:
            raise ValueError(f"unknown event '{k}', choose from {EVENTS}")
        mix[k] = float(v)
    tot = sum(mix.values())
    return {k: v / tot for k, v in mix.items()}


def simulate_dataset(sessions: int = 40, duration: float = 60.0, seed: int = 42,
                     mix: str = "none:0.4,crash:0.2,rollover:0.2,abnormal_temp:0.2"
                     ) -> pd.DataFrame:
    """Deterministic for a given seed. Event counts follow the mix exactly
    (largest-remainder rounding), then the order is shuffled."""
    if duration < 10:
        raise ValueError("duration must be >= 10 s")
    rng = np.random.default_rng(seed)
    m = parse_mix(mix)
    raw = {k: v * sessions for k, v in m.items()}
    cnt = {k: int(np.floor(v)) for k, v in raw.items()}
    for k in sorted(raw, key=lambda k: raw[k] - cnt[k], reverse=True):
        if sum(cnt.values()) >= sessions:
            break
        cnt[k] += 1
    events = [k for k, c in cnt.items() for _ in range(c)]
    rng.shuffle(events)
    frames = []
    for sid, ev in enumerate(events):
        d = simulate_session(rng, duration, ev)
        d.insert(0, "session_id", sid)
        frames.append(d)
    return pd.concat(frames, ignore_index=True)[CSV_COLS]


def save_csv(df: pd.DataFrame, out: Path):
    out.parent.mkdir(parents=True, exist_ok=True)
    df.to_csv(out, index=False, float_format="%.4f")
    print(f"saved {len(df):,} rows, {df['session_id'].nunique()} sessions -> {out}")
    print(df["label"].value_counts().to_string())


# ----------------------------------------------------------------------------
# serial logging (real hardware)
# ----------------------------------------------------------------------------
def log_serial(port, baud, seconds, label, scenario, session_id, out: Path):
    """Device prints one line per sample, comma separated, either
       ax,ay,az,gx,gy,gz,temperature,vibration            (8 values) or
       timestamp_ms,ax,ay,az,gx,gy,gz,temperature,vibration (9 values).
    Lines that don't parse (boot messages, headers) are skipped."""
    import serial  # pyserial

    rows, t0 = [], time.time()
    with serial.Serial(port, baud, timeout=1) as ser:
        print(f"logging {port}@{baud} for {seconds}s  (Ctrl+C to stop early)")
        try:
            while time.time() - t0 < seconds:
                line = ser.readline().decode("ascii", "ignore").strip()
                parts = line.split(",")
                try:
                    vals = [float(x) for x in parts]
                except ValueError:
                    continue
                if len(vals) == 8:
                    ts = int((time.time() - t0) * 1000)
                elif len(vals) == 9:
                    ts, vals = int(vals[0]), vals[1:]
                else:
                    continue
                rows.append([session_id, ts, *vals, label, scenario])
                if len(rows) % 500 == 0:
                    print(f"  {len(rows)} samples")
        except KeyboardInterrupt:
            print("stopped by user")
    df = pd.DataFrame(rows, columns=CSV_COLS)
    if out.exists():  # append as a new session
        old = pd.read_csv(out)
        df["session_id"] = int(old["session_id"].max()) + 1
        df = pd.concat([old, df], ignore_index=True)
    save_csv(df, out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=ROOT / "data/raw/sim_sessions.csv")
    g = ap.add_argument_group("simulation")
    g.add_argument("--sessions", type=int, default=40)
    g.add_argument("--duration", type=float, default=60.0, help="seconds per session")
    g.add_argument("--seed", type=int, default=42)
    g.add_argument("--mix", default="none:0.4,crash:0.2,rollover:0.2,abnormal_temp:0.2",
                   help="event share per session type")
    s = ap.add_argument_group("serial logging")
    s.add_argument("--serial", metavar="PORT", help="e.g. COM5 or /dev/ttyUSB0")
    s.add_argument("--baud", type=int, default=115200)
    s.add_argument("--seconds", type=float, default=60)
    s.add_argument("--label", default="normal", choices=["normal", "crash", "rollover", "abnormal_temp"])
    s.add_argument("--scenario", default="real")
    s.add_argument("--session-id", type=int, default=0)
    ap.add_argument("--load", nargs="+", type=Path, metavar="CSV",
                    help="import existing CSV(s) and merge into --out")
    a = ap.parse_args(argv)

    if a.serial:
        log_serial(a.serial, a.baud, a.seconds, a.label, a.scenario, a.session_id, a.out)
    elif a.load:
        df = load_csvs(a.load)
        for c in ("label", "scenario"):
            if c not in df.columns:
                df[c] = "normal" if c == "label" else "imported"
        save_csv(df[CSV_COLS], a.out)
    else:
        t = time.time()
        df = simulate_dataset(a.sessions, a.duration, a.seed, a.mix)
        save_csv(df, a.out)
        print(f"simulated in {time.time() - t:.1f}s")


if __name__ == "__main__":
    main()
