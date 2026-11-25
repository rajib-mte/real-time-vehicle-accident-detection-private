# Real-Time Vehicle Accident Detection Using TinyML and Sensor Fusion — `training/`

এই folder-এ project-এর **machine learning training অংশ** আছে: sensor data তৈরি/সংগ্রহ → feature extraction → model training → **int8 TFLite** conversion → ESP32-S3 (TFLite Micro)-এর জন্য C header export।

> ⚠️ **Disclaimer:** এখানে যে sample data দেওয়া আছে তা **simulated (synthetic)** — আসল গাড়ির crash data **না**। Model ও threshold শুধু pipeline দেখানোর জন্য। বাস্তবে ব্যবহারের আগে **real sensor measurement** দিয়ে অবশ্যই **retrain ও validate** করতে হবে। এই model সরাসরি কোনো life-safety system-এ ব্যবহার করবেন না।

---

## 1. Overview

| Sensor | Signal | Rate |
|---|---|---|
| MPU6050 (I2C) | 3-axis acceleration `ax, ay, az` (g), 3-axis gyro `gx, gy, gz` (deg/s) | 100 Hz |
| DS18B20 (1-Wire) | `temperature` (°C) | continuous (≈1 Hz real, logged at 100 Hz) |
| Vibration sensor (analog) | `vibration` (ADC, normalized 0..1) | 100 Hz |

- **Window:** 1 s (100 samples), 50% overlap → প্রতি 0.5 s-এ একটি inference
- **Features:** 12টি ছোট sensor-fusion feature (MCU-তে সহজে হিসাব করা যায়)
- **Primary model (default): Autoencoder** — শুধু normal driving দিয়ে train; reconstruction error > threshold হলে **anomaly / accident**
- **Optional: Classifier** — `normal / crash / rollover / abnormal_temp`
- **Deployment:** full int8 `.tflite` (~4.5 KB) → `model_data.h/.cc` + `model_config.h`

---

## 2. Project layout

পুরো project folder-এ দুইটি sibling subfolder:

```
9. Real-Time Vehicle Accident Detection Using TinyML and Sensor Fusion/
├── training/          ← এই folder (Python, ML)
└── firmware/          ← ESP32-S3 PlatformIO project (আলাদাভাবে তৈরি; দেখুন ../firmware/README.md)
```

`training/` এর ভেতরে:

```
training/
├── README.md                 এই file
├── requirements.txt          pinned Python packages
├── .gitignore
├── data/
│   ├── raw/                  raw 100 Hz CSV (sample_sessions.csv = simulated sample)
│   └── processed/            features.csv (windowed features)
├── models/
│   ├── autoencoder/          primary model (default)
│   │   ├── model.keras               Keras float model
│   │   ├── scaler.json               feature mean/std/clip (C-তে port করার জন্য)
│   │   ├── meta.json                 threshold, labels, window config
│   │   ├── metrics.json              test metrics (confusion matrix, precision/recall)
│   │   ├── confusion_matrix.png, error_hist.png, training_curve.png
│   │   ├── split.npz                 scaled train/test arrays (conversion-এর জন্য)
│   │   ├── model_int8.tflite         full int8 model
│   │   ├── model_data.h / model_data.cc   C array (TFLite Micro)
│   │   ├── model_config.h            scaler + quant + threshold constants
│   │   └── conversion_report.json    float vs int8 comparison
│   └── classifier/           optional 4-class model (একই রকম files)
├── scripts/
│   ├── collect_data.py       simulate / --serial PORT / --load CSV
│   ├── features.py           shared windowing + feature extraction + scaler
│   ├── train.py              --mode autoencoder (default) | classifier
│   └── convert_tflite.py     int8 PTQ + verify + C header export
└── notebooks/
    └── pipeline.ipynb        end-to-end walkthrough (outputs সহ saved)
```

---

## 3. Setup (Python 3.10 – 3.12)

> TensorFlow 2.19 Windows-এ Python **3.10–3.12** support করে (3.13 না)। Box-এ Python 3.12 দিয়ে test করা হয়েছে।

**Windows (PowerShell / CMD):**
```bat
cd "...\9. Real-Time Vehicle Accident Detection Using TinyML and Sensor Fusion\training"
py -3.12 -m venv .venv
.venv\Scripts\activate
python -m pip install --upgrade pip
pip install -r requirements.txt
```

**Linux / macOS:**
```bash
python3.12 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
```

**VS Code:** Research Workspace-এ এই `training/` folder PlatformIO firmware project-এর পাশাপাশি খোলা যায়। `Ctrl+Shift+P → Python: Select Interpreter` → `training\.venv\Scripts\python.exe` বেছে নিন। Notebook খুললে kernel হিসেবেও একই `.venv` select করুন। PlatformIO নিজের আলাদা Python environment ব্যবহার করে, তাই দুটো conflict করে না।

> Path লম্বা হলে (Windows 260-character limit) venv install-এ সমস্যা হতে পারে; তখন venv ছোট path-এ বানান (যেমন `C:\venvs\accident-ml`) এবং VS Code-এ সেটি select করুন।

---

## 4. Step by step: collect → train → convert → deploy

সব command `training/` folder থেকে চালান।

### Step 1 — Data collect / simulate

**(a) Simulation (default)** — MPU6050-style 100 Hz data, seedable:
```bash
python scripts/collect_data.py --sessions 40 --duration 60 --seed 42 --out data/raw/sim_sessions.csv
```
Scenarios:
- **normal:** `idle`, `cruising`, `braking`, `turning`, `bumpy_road` (pothole ≈ 1–2 g)
- **crash:** 30–150 ms high-g impact spike (4–15 g) + gyro kick + vibration burst
- **rollover:** gravity vector ঘুরে যায় (80–200°), high gyro rate, তারপর sustained tilt
- **abnormal_temp:** engine fire — temperature দ্রুত বেড়ে 95–150 °C

`--mix "none:0.4,crash:0.2,rollover:0.2,abnormal_temp:0.2"` দিয়ে session-এর ভাগ বদলানো যায়।
Repo-তে `data/raw/sample_sessions.csv` (24 sessions × 40 s, `--seed 7`) দেওয়া আছে, তাই notebook সরাসরি চলে।

**(b) Real sensor (serial)** — ESP32 প্রতি sample-এ একটি CSV line print করবে:
`ax,ay,az,gx,gy,gz,temperature,vibration` (8 values) বা আগে `timestamp_ms` সহ 9 values।
```bash
python scripts/collect_data.py --serial COM5 --baud 115200 --label normal --scenario cruising --seconds 300 --out data/raw/real_normal.csv
```
একই `--out` file-এ আবার চালালে নতুন `session_id` হিসেবে append হয়।

**(c) Existing CSV import:**
```bash
python scripts/collect_data.py --load logs/day1.csv logs/day2.csv --out data/raw/merged.csv
```

CSV columns: `session_id, timestamp_ms, ax, ay, az, gx, gy, gz, temperature, vibration, label, scenario`

### Step 2 — Features (`scripts/features.py`)

| # | Feature | Formula |
|---|---|---|
| 0 | `acc_mag_mean` | mean(\|a\|), \|a\| = √(ax²+ay²+az²) |
| 1 | `acc_mag_std` | std(\|a\|) (population) |
| 2 | `acc_mag_max` | max(\|a\|) |
| 3 | `jerk_max` | max\|Δ\|a\|\| × fs (g/s) |
| 4 | `gyro_mag_mean` | mean(\|ω\|) |
| 5 | `gyro_mag_max` | max(\|ω\|) |
| 6 | `tilt_deg` | acos(mean_az / \|mean_a\|) — 0° = upright |
| 7 | `temp_mean` | mean(T) |
| 8 | `temp_slope` | (T_last − T_first) × fs / (N−1) (°C/s) |
| 9 | `vib_mean` | mean(v) |
| 10 | `vib_rms` | √mean((v − v̄)²) |
| 11 | `vib_peak` | max(v) |

Window label: কোনো event window-এর ≥25% হলে সেই label (priority: crash > rollover > abnormal_temp); event সামান্য ছুঁয়ে গেলে `ambiguous` → training/evaluation থেকে বাদ।
Standardization: `z = clip((x − mean) / std, −8, 8)` — mean/std `scaler.json` ও `model_config.h`-এ।

### Step 3 — Train (`scripts/train.py`)

```bash
python scripts/train.py                          # autoencoder (default, primary)
python scripts/train.py --mode classifier        # optional 4-class
python scripts/train.py --data data/raw/real_*.csv --epochs 300 --percentile 99.5
```
- **Autoencoder** (12→16→6→16→12, ~600 params): শুধু normal window দিয়ে train; threshold = held-out normal error-এর `--percentile` (default 99)।
- **Classifier** (12→24→16→4 softmax, ~800 params): class weight দিয়ে imbalance handle।
- Train/test split **session অনুযায়ী** (same session-এর window train ও test দুই দিকে যায় না)।
- Output: `models/<mode>/` — `model.keras`, `scaler.json`, `meta.json`, `metrics.json`, plots, `split.npz`; আর `data/processed/features.csv`।

### Step 4 — Convert to int8 TFLite (`scripts/convert_tflite.py`)

```bash
python scripts/convert_tflite.py                              # models/autoencoder
python scripts/convert_tflite.py --model-dir models/classifier
```
- Full integer post-training quantization (representative dataset = training windows), **int8 input + int8 output**, শুধু `TFLITE_BUILTINS_INT8` ops (`FULLY_CONNECTED`, classifier-এ `SOFTMAX`)
- TFLite interpreter দিয়ে test set চালিয়ে **float vs int8** তুলনা; autoencoder threshold int8 model-এ re-calibrate (`kAnomalyThreshold`)
- Export: `model_int8.tflite`, `model_data.h` / `model_data.cc` (xxd-style, `alignas(16)`), `model_config.h`, `conversion_report.json`
- **Tensor arena:** model খুব ছোট (activations < 100 B)। `kTensorArenaSize = 8 KB` দিয়ে শুরু করুন; `AllocateTensors()`-এর পরে `interpreter.arena_used_bytes()` print করে কমাতে পারেন (এত ছোট model-এ সাধারণত কয়েক KB-ই যথেষ্ট; hardware-এ মেপে নিন)।

### Step 5 — Deploy (high level, firmware লেখা হয়নি)

1. `models/autoencoder/` থেকে **`model_data.h`, `model_data.cc`, `model_config.h`** → **`../firmware/`** এ copy করুন (যেমন headers `firmware/include/`, `.cc` file `firmware/src/`)।
2. Firmware-এ TFLite Micro (ESP32-S3 port) দিয়ে:
   - `tflite::GetModel(g_model_data)`; op resolver-এ শুধু `FullyConnected` (classifier হলে `Softmax`ও)
   - `static uint8_t tensor_arena[kTensorArenaSize];`
3. 100 Hz-এ sensor পড়ে 100-sample ring buffer; প্রতি `kWindowStep` (50) sample পরপর:
   - Step 2-এর **হুবহু একই** 12 feature হিসাব
   - `z = clamp((x − kFeatureMean) / kFeatureStd, ±kFeatureClip)`
   - `q = round(z / kInputScale) + kInputZeroPoint` → int8 input tensor → `Invoke()`
   - Autoencoder: `y = (q_out − kOutputZeroPoint) × kOutputScale`; `score = mean((y − z)²)`; **`score > kAnomalyThreshold` → alert**
   - Classifier: `argmax` of output → `kClassLabels[...]`
4. False alarm কমাতে পরপর 2টি window-এ anomaly হলে alert দিন, আর backup hard rule রাখুন (যেমন `temp > kTempAlarmC`, `|a| > 4 g`)।
5. Alert হলে firmware (SIM800L SMS / MQTT) — এটা `../firmware/` project-এর কাজ।
6. Model retrain করলে শুধু Step 3–4 আবার চালিয়ে নতুন তিনটি file `../firmware/`-এ copy করলেই হবে (feature order বা formula বদলালে firmware code-ও বদলাতে হবে)।

---

## 5. Sensor wiring reference (later, for real data)

| Sensor | Pin | ESP32-S3 |
|---|---|---|
| MPU6050 | SDA | **GPIO8** |
| MPU6050 | SCL | **GPIO9** |
| MPU6050 | AD0 | GND → I2C address **0x68** |
| MPU6050 | VCC / GND | 3.3 V / GND |
| DS18B20 | DATA | **GPIO4** (+ 4.7 kΩ pull-up to 3.3 V) |
| Vibration (analog out) | AO | **GPIO7** (ADC1) |
| SIM800L (firmware-এর জন্য) | TX / RX | GPIO17 / GPIO18 (UART) |

> ⚠️ **ESP32-S3-এ GPIO34-এ কোনো ADC নেই** (classic ESP32-এর মতো না)। ESP32-S3-এ শুধু **GPIO1–20** analog পড়তে পারে। **GPIO1–10 (ADC1)** ব্যবহার করুন, যেমন **GPIO7** — কারণ ADC2 (GPIO11–20) Wi-Fi চালু থাকলে নির্ভরযোগ্য না। Vibration sensor 5 V output দিলে voltage divider দিয়ে ≤3.3 V করুন।
> MPU6050 range: firmware-এ ±16 g ও ±2000 deg/s set করুন (simulation এই range ধরে নিয়েছে), না হলে crash spike clip হয়ে যাবে।

---

## 6. Sample results (simulated data, box-এ verify করা)

Sample dataset (24 sessions, 1888 windows; test = 6 sessions, 394 windows):

| Model | float (Keras) | int8 (TFLite) | `.tflite` size |
|---|---|---|---|
| Autoencoder | acc 0.975, precision 1.00, recall 0.851, F1 0.919 | acc 0.977, precision 1.00, recall 0.866, F1 0.928 | 4,632 B |
| Classifier | acc 0.995 | acc 0.995 | 4,384 B |

Test set ছোট (crash window মাত্র 3টি), তাই এই সংখ্যা শুধু sanity check — real data-য় আলাদা হবে।

---

## 7. Notebook

`notebooks/pipeline.ipynb` — simulate/load → signal plot → features → train (AE + classifier) → evaluate → int8 convert → float vs int8 → header export। Outputs সহ saved। আবার চালাতে:
```bash
jupyter nbconvert --to notebook --execute --inplace notebooks/pipeline.ipynb
```
অথবা VS Code-এ খুলে `.venv` kernel দিয়ে **Run All**। (Notebook চালালে `models/` এর files একই seed দিয়ে আবার তৈরি হয়।)

> ⚠️ আবারও: simulated data ≠ real crash data। Real measurement দিয়ে retrain না করা পর্যন্ত এই model শুধু demo।
