# Real-Time Vehicle Accident Detection Using TinyML and Sensor Fusion — ESP32-S3 Firmware

> ⚠️ **Safety disclaimer:** এটি একটি **research prototype**। এটি কোনো certified (যেমন eCall / ISO 26262) safety device নয়। জীবন রক্ষাকারী emergency system হিসেবে এর উপর নির্ভর করবেন না। Real vehicle-এ test করার সময় সবসময় নিরাপদ পরিবেশ ও নিয়ম মেনে চলুন।

---

## 1. Overview (সংক্ষেপে)

এই **PlatformIO** project টি **ESP32-S3** board-এ চলে এবং গাড়ির দুর্ঘটনা real-time-এ detect করে:

```
MPU6050 (accel+gyro) ┐
DS18B20 (temperature)├─► 100 Hz sampling ─► ring buffer ─► 1 s window (50% overlap)
Vibration (analog)   ┘                                      │
                                     ┌──────────────────────┴───────────────┐
                                     ▼                                      ▼
                       12 features ─► int8 TFLite Micro           Rule-based fallback
                       (autoencoder, anomaly = MSE > threshold)   (high-g impact, rollover tilt,
                                     │                             over-temperature)
                                     └──────────────┬───────────────────────┘
GPS (NEO-6M/M8N, Serial2) ─► lat/lon ──────────────►▼
                         Alert state machine (cancel window ─► dispatch ─► cooldown)
                                                    ▼
      MQTT alert JSON (HiveMQ, GPRS বা WiFi)  ─►  SIM800 SMS সব contact-এ (AT+CMGS)  ─►  MQTT sms_report
```

- **TinyML:** training project (`../training`) থেকে আসা int8 autoencoder; reconstruction MSE `kAnomalyThreshold`-এর বেশি হলে anomaly।
- **Rule-based fallback:** model ছাড়াও কাজ করে — `|a| > 4 g` (impact), sustained tilt > 60° (rollover), temperature > 60 °C।
- **GPS:** NEO-6M / NEO-M8N (UART NMEA, TinyGPSPlus) — অথবা SIM808 হলে তার built-in GNSS। Fix না থাকলে last known fix, তাও না থাকলে config-এর fixed location।
- **Internet:** default **SIM800 GPRS** (একই modem SMS-ও পাঠায়); চাইলে WiFi বা "WiFi, না পেলে GPRS"।
- **SMS:** সবসময় **SIM800 modem নিজেই** (AT+CMGS) পাঠায় — কোনো web SMS gateway নয়। Dashboard থেকে contact list sync ও SMS on/off করা যায়।
- **Telemetry:** প্রতি ~1 s-এ `vehicle/accident` topic-এ telemetry JSON (web dashboard-এর জন্য)।

---

## 2. Project layout

```
firmware/
├── platformio.ini            board, libraries, build flags
├── README.md                 এই file
├── include/
│   ├── config.h              আপনার settings (NET_MODE, APN, WiFi, GPS, phone, MQTT, pins, thresholds)
│   ├── config.example.h      template
│   ├── model_data.h          model symbols (placeholder; training থেকে replace করুন)
│   └── model_config.h        scaler, quant params, threshold (placeholder)
├── src/
│   ├── main.cpp              setup/loop, 100 Hz sampler task, detection
│   ├── sample.h, ring_buffer.h
│   ├── sensors.cpp/.h        MPU6050, DS18B20 (async), vibration ADC
│   ├── feature_extractor.cpp/.h   12 features (training-এর features.py-এর হুবহু port)
│   ├── inference.cpp/.h      TFLite Micro (static arena, op resolver, int8 quant)
│   ├── rules.cpp/.h          impact / rollover / over-temp rules
│   ├── alert.cpp/.h          alert state machine + dispatch (MQTT → SMS → sms_report)
│   ├── modem.cpp/.h          একটাই TinyGSM SIM800 instance + recursive mutex (SMS, GPRS, GNSS)
│   ├── sms.cpp/.h            সব contact-এ SMS, retry, per-number log
│   ├── sms_format.cpp/.h     160-char SMS formatter (priority trimming, host-tested)
│   ├── remote_config.cpp/.h  dashboard contacts (name+phone) + SMS on/off (NVS-এ save), acks
│   ├── config_parse.cpp/.h   config message JSON parser (host-tested)
│   ├── net.cpp/.h            NET_MODE: GPRS / WiFi / WiFi→GPRS, MQTT, NTP
│   ├── payload.cpp/.h        MQTT JSON (telemetry, alert, sms_report)
│   ├── hospitals.cpp/.h      Overpass lookup (WiFi) + offline table, 2 km re-query
│   ├── location.cpp/.h       GPS reader (TinyGPSPlus / SIM808 CGNSINF) + fallback
│   └── model_data.cc         model byte array (placeholder)
├── models/placeholder_model.tflite
└── tools/
    ├── make_placeholder_model.py
    ├── tflite_to_c.py
    ├── sms_format_test.cpp   SMS formatter host test
    └── config_parse_test.cpp contacts / sms_config parser host test
```

---

## 3. Hardware ও Pin table

| Part | Signal | ESP32-S3 pin | Notes |
|---|---|---|---|
| MPU6050 | SDA / SCL | **GPIO8 / GPIO9** | I2C 400 kHz, address **0x68** |
| DS18B20 | DQ | **GPIO4** | **4.7 kΩ pull-up** DQ → 3V3 |
| Vibration sensor (analog) | AO | **GPIO7** | ADC1_CH6. **GPIO34-এ S3-তে ADC নেই!** |
| SIM800 ("SIM 8000") | RXD | **GPIO17** (ESP TX) | Serial1, 9600 baud |
| SIM800 | TXD | **GPIO18** (ESP RX) | |
| SIM800 | VCC / GND | আলাদা **3.7–4.2 V, ≥2 A** / **common GND** | ESP32-এর 3V3/5V pin থেকে **নয়** |
| GPS NEO-6M / NEO-M8N | TX | **GPIO15** (ESP RX, `GPS_RX_PIN`) | Serial2, 9600 baud NMEA |
| GPS | RX | **GPIO16** (ESP TX, `GPS_TX_PIN`) | optional (config পাঠাতে চাইলে) |
| GPS | VCC / GND | 3V3 (বা module-এর regulator থাকলে 5V) / GND | |
| Cancel button | — | **GPIO0** (BOOT) | active LOW |
| Buzzer/LED (optional) | — | `PIN_BUZZER` (default -1) | |

**Pin check (GPS 15/16):** কোনো existing pin-এর সাথে clash নেই (I2C 8/9, DS18B20 4, vibration 7, SIM800 17/18, BOOT 0)। GPIO15/16 ESP32-S3-এর **strapping pin নয়** (strapping: 0, 3, 45, 46), **flash/PSRAM pin নয়** (SPI flash 26–32, octal PSRAM 33–37), **USB নয়** (19/20), **UART0 console নয়** (43/44)। এগুলো শুধু optional 32 kHz crystal (XTAL_32K) pin — DevKitC-1-এ crystal নেই, তাই free।

### Wiring diagram (ASCII)

```
                        +-----------------------------+
   MPU6050              |        ESP32-S3 DevKitC-1   |
   VCC ---------------- | 3V3                         |
   GND ---------------- | GND ----------+ (common GND)|
   SDA ---------------- | GPIO8         |             |
   SCL ---------------- | GPIO9         |             |
                        |               |             |
   DS18B20   4.7k to 3V3|               |             |
   VDD ---------------- | 3V3           |             |
   DQ  ------+--------- | GPIO4         |             |
             +-[4.7k]-- | 3V3           |             |
   GND ---------------- | GND           |             |
                        |               |             |
   Vibration AO ------- | GPIO7 (ADC1)  |             |
                        |               |             |
   GPS NEO-6M/M8N       |               |             |
   VCC ---------------- | 3V3           |             |
   TX  ---------------->| GPIO15 (RX2)  |             |
   RX  <----------------| GPIO16 (TX2)  |             |
   GND ---------------- | GND           |             |
                        |               |             |
                        | GPIO17 (TX1) -+---[1k]--+---> SIM800 RXD
                        | GPIO18 (RX1) <----------|---- SIM800 TXD
                        | GPIO0 = BOOT (cancel)   |
                        +-------------------------|---+
                                                [2k]   (optional divider)
                                                  |
   SIM800 VCC <-- Li-ion 3.7-4.2 V / 4.0 V buck >=2 A (+1000 uF)
   SIM800 GND ---- supply GND ---- ESP32 GND  (common)
```

### Power notes (গুরুত্বপূর্ণ)

- **SIM800** transmit burst-এ **~2 A** টানে — বিশেষ করে **GPRS + SMS** একসাথে চললে। ESP32 board-এর pin থেকে দিলে modem reset হবে। **Li-ion cell (3.7–4.2 V)** বা **4.0 V buck (≥2 A)** + VCC-এর কাছে **470–1000 µF capacitor** দিন।
- **GPS module** ~30–50 mA; 3V3 থেকে চলে। প্রথম fix (cold start) খোলা আকাশে **30 s – কয়েক মিনিট** লাগতে পারে; antenna গাড়ির ছাদ/ড্যাশবোর্ডে আকাশমুখী রাখুন।
- সব module-এর **GND common** রাখুন।
- **Logic level:** SIM800 UART ~2.8 V; ESP TX → SIM800 RXD লাইনে **1k/2k divider** নিরাপদ। GPS module 3.3 V logic।
- গাড়িতে 12 V থেকে চালালে **automotive buck converter** (load-dump protection সহ) ব্যবহার করুন।
- MPU6050 board chassis-এ শক্তভাবে, **flat (Z axis উপরে)** লাগান।

---

## 4. Setup (VS Code + PlatformIO)

1. **VS Code** → Extensions → **PlatformIO IDE** install।
2. **File → Open Folder** দিয়ে এই `firmware` folder খুলুন।
3. প্রথম build-এ platform, toolchain ও library নিজে download হবে (internet লাগবে)।

### Libraries (platformio.ini-তে `lib_deps`)

| Library | কাজ |
|---|---|
| `adafruit/Adafruit MPU6050` (+ Unified Sensor) | MPU6050 accel/gyro |
| `paulstoffregen/OneWire`, `milesburton/DallasTemperature` | DS18B20 |
| `knolleary/PubSubClient` | MQTT |
| `bblanchon/ArduinoJson` (v7) | JSON |
| `vshymanskyy/TinyGSM` | SIM800 SMS + GPRS (SIM808 GNSS) |
| `mikalhart/TinyGPSPlus` | NEO-6M/M8N NMEA parse |
| `sebastianregelmann/tflite-micro@1.26.46` | official **TensorFlow Lite Micro** (C++17), per-channel int8 FULLY_CONNECTED |

> কেন `tanakamasayuki/TensorFlowLite_ESP32` নয়? ওই library-র TFLM পুরনো; নতুন TensorFlow converter-এর **per-channel int8 dense weights** ঠিকমতো চালায় না। Host test: পুরনো library-তে alarm decision 241/271 মিলেছে, নতুন library-তে 271/271।

---

## 5. Configuration (`include/config.h`)

| Setting | মানে |
|---|---|
| `NET_MODE` | `NET_GPRS` (default) / `NET_WIFI` / `NET_WIFI_THEN_GPRS` |
| `GPRS_APN`, `GPRS_USER`, `GPRS_PASS` | SIM-এর APN (নিচে দেখুন) |
| `WIFI_SSID`, `WIFI_PASSWORD` | WiFi (NET_WIFI* mode, hospital online lookup) |
| `GPS_SOURCE` | `GPS_UART` (default) / `GPS_SIM808` / `GPS_NONE` |
| `GPS_RX_PIN`, `GPS_TX_PIN`, `GPS_BAUD` | 15 / 16 / 9600 |
| `FIXED_LAT`, `FIXED_LON` | GPS fix কখনো না পেলে এই location |
| `SMS_RECIPIENTS[]`, `SMS_NUM_RECIPIENTS` | fallback contact list (synced list না থাকলে) |
| `SMS_ENABLED_DEFAULT` | dashboard কিছু না বলা পর্যন্ত SMS on (true) |
| `CONTACTS_TOKEN` | dashboard config message-এর shared token |
| `DEVICE_ID`, `MQTT_TOPIC` | default `vehicle/accident` (বদলাবেন না, dashboard এটাই শোনে) |
| `TELEMETRY_INTERVAL_MS`, `TELEMETRY_INTERVAL_GPRS_MS` | default 1000 ms দুটোই |
| `FALLBACK_HOSPITALS[]` | offline hospital table (approx. coordinates; phone নিজে verify করুন) |
| `HOSPITAL_REQUERY_KM` | এত km সরে গেলে নতুন hospital query (default 2) |
| `IMPACT_G`, `ROLLOVER_TILT_DEG`, `ROLLOVER_HOLD_MS`, `OVER_TEMP_C` | rule thresholds |
| `CANCEL_WINDOW_MS`, `ALERT_COOLDOWN_MS` | alert cancel সময় ও cooldown |

### NET_MODE ও APN (Bangladesh)

| Operator | APN (সাধারণত) |
|---|---|
| Grameenphone | `internet` |
| Robi / Airtel | `internet` |
| Banglalink | `blweb` |
| Teletalk | `wap` (কিছু package-এ `internet`) |

> ⚠️ APN operator বদলাতে পারে — SIM-এর customer care বা phone-এর APN setting দেখে নিশ্চিত হন। SIM-এ **data package** থাকতে হবে।
> SIM800 **শুধু 2G (GSM/GPRS)**। Operator আপনার এলাকায় 2G বন্ধ করলে GPRS ও SMS কোনোটাই চলবে না (তখন 4G module যেমন A7670/SIM7600 লাগবে)।
> **Data usage:** GPRS-এ 1 s telemetry ≈ **40 MB/দিন** (গাড়ি সারাদিন চালু থাকলে)। কমাতে `TELEMETRY_INTERVAL_GPRS_MS` বাড়ান (যেমন 5000)।

**একই modem, SMS + GPRS:** firmware-এ একটাই TinyGSM instance (`src/modem.cpp`) ও একটি **recursive mutex** আছে। SMS পাঠানোর সময় MQTT loop **pause** হয়, modem mutex SMS নিয়ে নেয়, সব SMS শেষে MQTT আবার চলে (keepalive 90 s, দরকার হলে reconnect)। SIM800 GPRS socket খোলা অবস্থাতেই AT+CMGS দিয়ে SMS পাঠাতে পারে।

> 🔒 আসল password/phone/token দিয়ে `config.h` public share করবেন না।

---

## 6. TinyML model — placeholder ও আসল model বসানো

### Placeholder

Firmware-এ একটি **ছোট কিন্তু valid int8 TFLite autoencoder** (3.8 KB, 12→8→4→8→12, শুধু FULLY_CONNECTED) দেওয়া আছে, যাতে আসল model ছাড়াই firmware build ও run হয়। এটি শুধু simulated normal data-য় train করা — **detection-এর জন্য নয়**। `model_config.h`-এ `MODEL_IS_PLACEHOLDER 1` থাকে, তাই placeholder থাকলে ML score শুধু telemetry-তে দেখায়, alert দেয় না (rules ঠিকই কাজ করে)। আবার তৈরি করতে: `python tools/make_placeholder_model.py` (tensorflow লাগবে)।

### আসল model বসানো (drop-in)

Training project (`../training`) `convert_tflite.py` চালালে `models/autoencoder/`-এ তিনটি file তৈরি হয়। এগুলো copy করুন (overwrite):

| `../training/models/autoencoder/` থেকে | → firmware-এ |
|---|---|
| `model_data.h` | `include/model_data.h` |
| `model_config.h` | `include/model_config.h` |
| `model_data.cc` | `src/model_data.cc` |

Windows PowerShell (firmware folder থেকে):
```powershell
Copy-Item ..\training\models\autoencoder\model_data.h   include\ -Force
Copy-Item ..\training\models\autoencoder\model_config.h include\ -Force
Copy-Item ..\training\models\autoencoder\model_data.cc  src\     -Force
```
তারপর আবার build/upload করুন। Code বদলাতে হবে না।

**Expected symbols (contract):**

- `model_data.h/.cc`: `extern const unsigned char g_model_data[];` (16-byte aligned), `extern const unsigned int g_model_data_len;`
- `model_config.h`: `kNumFeatures` (=12), `kFeatureMean[12]`, `kFeatureStd[12]`, `kFeatureClip`, `kAnomalyThreshold`, `kTensorArenaSize`, `kSampleRateHz` (100), `kWindowSize` (100), `kWindowStep` (50), `kTempAlarmC`, `kInputScale/kInputZeroPoint`, `kOutputScale/kOutputZeroPoint`
- Model: input int8 `[1,12]`, output int8 `[1,12]` (autoencoder)। Op resolver-এ আছে: FullyConnected, Relu, Relu6, Logistic, Tanh, Reshape, Quantize, Dequantize।
- Compile-time check: `kNumFeatures`, window/sample rate config না মিললে build error দেবে।
- `TENSOR_ARENA_SIZE` (config.h, default 8 KB) ≥ `kTensorArenaSize` হতে হবে। Boot log-এ `arena used` দেখায় (training model-এ ~1.9 KB)।

**Inference math (training-এর সাথে হুবহু):**
```
z[i]  = clamp((x[i] - kFeatureMean[i]) / kFeatureStd[i], -kFeatureClip, +kFeatureClip)
q[i]  = round(z[i] / input_scale) + input_zero_point      (int8)
y[i]  = (out_q[i] - output_zero_point) * output_scale
score = mean_i (y[i] - z[i])^2        anomaly  if  score > kAnomalyThreshold
```
Alert-এর জন্য পরপর `ML_CONSECUTIVE_WINDOWS` (2) window-এ anomaly লাগে।

### 12 features (order গুরুত্বপূর্ণ — `src/feature_extractor.cpp`)

1 s window = 100 samples @ 100 Hz, hop 50। Units: accel **g**, gyro **deg/s**, temperature **°C**, vibration **0–1**। `|a| = sqrt(ax²+ay²+az²)`, `|w| = sqrt(gx²+gy²+gz²)`।

| # | Name | Formula |
|---|---|---|
| 0 | acc_mag_mean | mean(\|a\|) |
| 1 | acc_mag_std | std(\|a\|) (population) |
| 2 | acc_mag_max | max(\|a\|) |
| 3 | jerk_max | max(abs(diff(\|a\|))) × fs |
| 4 | gyro_mag_mean | mean(\|w\|) |
| 5 | gyro_mag_max | max(\|w\|) |
| 6 | tilt_deg | acos(mean_az / \|mean_a\|) (0 = upright) |
| 7 | temp_mean | mean(T) |
| 8 | temp_slope | (T[last] − T[first]) × fs / (N−1) |
| 9 | vib_mean | mean(v) |
| 10 | vib_rms | sqrt(mean((v − mean v)²)) |
| 11 | vib_peak | max(v) |

C++ feature code training-এর `scripts/features.py`-এর সাথে host-এ মিলিয়ে দেখা হয়েছে (max relative error ~2e-6)। Feature formula বা order বদলালে দুই জায়গাতেই বদলাতে হবে।

---

## 7. Build / Upload / Monitor

```bash
pio run -e esp32s3                 # build
pio run -e esp32s3 -t upload       # upload (USB)
pio device monitor -b 115200       # serial monitor
pio run -e esp32s3 -t clean        # clean
```

Upload না হলে: **BOOT** চেপে ধরে **RST** চাপুন, তারপর BOOT ছাড়ুন।

Boot log উদাহরণ:
```
=== Vehicle Accident Detection (ESP32-S3, TinyML) ===
[sensors] MPU6050:OK DS18B20:OK
[ml] model 3824 bytes, arena used .../8192, threshold 0.9519 (PLACEHOLDER)
[cfg] sms_enabled=true (v0), synced contacts=0 (v0)
[modem] SIM800 R14.18
[modem] operator 'Grameenphone', CSQ 18
[gps] UART NMEA on Serial2 RX=15 TX=16 @9600
[net] mode GPRS, MQTT broker.hivemq.com:1883 topic 'vehicle/accident' id '...'
[modem] GPRS up, IP 10.x.x.x
[net] MQTT connected via gprs
```

Build result (verified, default config NET_GPRS + GPS_UART): RAM **23.6 %** (77,380 / 327,680 B), Flash **33.4 %** (1,050,205 / 3,145,728 B)। NET_WIFI, NET_WIFI_THEN_GPRS, GPS_SIM808, GPS_NONE — সব variant-ও build হয়েছে।

---

## 8. MQTT payload (web dashboard contract)

- Broker: `broker.hivemq.com:1883` (public, **no TLS, no auth**)
- Topic: `vehicle/accident` — **telemetry** (প্রতি ~1 s), **alert**, **sms_report** সব এখানে।

### Telemetry / alert (আগের field অপরিবর্তিত; নতুন field শুধু যোগ হয়েছে)

```json
{
  "type": "telemetry",            // "telemetry" | "alert"
  "device_id": "veh-esp32s3-01",
  "ts": 1795246160123,            // epoch ms (NTP/modem time sync হলে), নাহলে millis()
  "ax": 0.012, "ay": -0.034, "az": 1.002,     // g
  "gx": 0.5, "gy": -1.2, "gz": 3.4,           // deg/s
  "temp_c": 32.5,
  "vibration": 0.18,              // 0..1
  "anomaly_score": 0.21,
  "threshold": 0.444,
  "status": "normal",             // "normal" | "accident" | "rollover" | "over_temp"
  "lat": 23.810312, "lon": 90.412518,

  "gps_fix": true,                // NEW: fresh GPS fix (< 3 s)
  "sats": 9,                      // NEW: satellites used
  "speed_kmh": 42.3,              // NEW (last known if no fix)
  "heading_deg": 87.5,            // NEW (last known)
  "alt_m": 12.4,                  // NEW (last known)
  "hdop": 0.9,                    // NEW (99.9 = unknown)
  "sms_enabled": true             // NEW: remote SMS switch state
}
```

`gps_fix:false` হলে `lat/lon` = last known fix, আর কখনো fix না পেলে `config.h`-এর `FIXED_LAT/LON`। তখন speed/heading/alt last known value (কখনো fix না পেলে 0)।

**Alert** (`"type":"alert"`) — উপরের সব field + :
```json
{
  "hospitals": [{"name": "Evercare Hospital Dhaka", "distance_km": 2.05, "phone": ""}],
  "peak_g": 6.8,                  // NEW: detection window-এর peak |a| (g)
  "ai_detected": true,            // NEW: ML model anomaly দেখেছে কি না (bool)
  "sms_enabled": true,
  "sms_sent": 0, "sms_failed": 0, // NEW (SMS পরে যায়, তাই alert-এ 0)
  "sms_skipped": false,           // NEW: true = SMS remote switch off
  "sms_pending": true             // NEW: SMS এখন যাবে -> পরে sms_report আসবে
}
```
SMS off থাকলে: `"sms_enabled": false, "sms_sent": 0, "sms_failed": 0, "sms_skipped": true, "sms_pending": false` এবং কোনো sms_report আসে না।

**SMS report** (SMS শেষ হলে, একই topic-এ; `alert_ts` = ওই alert-এর `ts` হুবহু):
```json
{"type":"sms_report","device_id":"veh-esp32s3-01","ts":1795246175000,
 "alert_ts":1795246160123,"status":"accident","sms_enabled":true,
 "recipients":3,"sms_sent":3,"sms_failed":0}
```
(alert publish ব্যর্থ হলে SMS-এর পরে পুরো alert আবার পাঠানো হয়, তাতে আসল `sms_sent/sms_failed` থাকে।)

### Dashboard → device config (RETAINED, QoS1)

| Topic | Payload | Ack topic / payload |
|---|---|---|
| `vehicle/accident/config/sms` | `{"type":"sms_config","device_id":"<id or *>","version":<epoch ms>,"token":"<CONTACTS_TOKEN>","sms_enabled":true}` | `vehicle/accident/config/sms/ack` → `{"type":"sms_ack","device_id","version","sms_enabled","ok"}` (+ `"error"` যখন `ok:false`) |
| `vehicle/accident/config/contacts` | `{"type":"contacts","device_id":"<id or *>","version":<epoch ms>,"token":"<CONTACTS_TOKEN>","contacts":[{"name":"Abbu","phone":"+8801..."}]}` — plain string `"+8801..."` entry-ও চলে | `vehicle/accident/config/contacts/ack` → `{"type":"contacts_ack","device_id","version","count","ok","error"?}` |

- `error` মান: `bad_json`, `bad_type`, `bad_token`, `bad_version`, `stale_version` (version পুরনো), `contacts_not_array`, `no_valid_phone`, `sms_enabled_not_bool`। Error হলে কিছু বদলায় না।
- অন্য `device_id`-এর message ignore হয় (ack দেয় না); `"*"` = সব device।
- Contact name 24 character-এ কাটা হয় (UTF-8/বাংলা নাম-ও ঠিক থাকে); phone `+` ও digit (5–19 char) হতে হবে, ভুল entry বাদ যায় (বাকিগুলো accept, `count` = গৃহীত সংখ্যা)। সর্বোচ্চ **10টি** (`MAX_CONTACTS`, dashboard-এর limit-এর সমান); বেশি হলে বাকিগুলো বাদ।
- Accept হলে NVS (flash)-এ name+phone save হয়, reboot-এর পরেও থাকে। `contacts: []` দিলে আবার `config.h`-এর numbers ব্যবহার হবে। Serial log-এ name ও masked number দেখায় (যেমন `Abbu +880171****111`)।
- ⚠️ Public broker-এ token যে কেউ পড়তে পারে — এটা ভুল-বার্তা ঠেকানোর guard, আসল security নয়। Production-এ private TLS broker ব্যবহার করুন।
- Parser host test: `tools/config_parse_test.cpp` (20+ case, সব pass)।

### MQTT test

```bash
mosquitto_sub -h broker.hivemq.com -p 1883 -t "vehicle/accident/#" -v
# SMS off (retained, QoS1)
mosquitto_pub -h broker.hivemq.com -t "vehicle/accident/config/sms" -r -q 1 \
  -m '{"type":"sms_config","device_id":"*","version":1795246160000,"token":"CHANGE_ME_SHARED_SECRET","sms_enabled":false}'
# contacts
mosquitto_pub -h broker.hivemq.com -t "vehicle/accident/config/contacts" -r -q 1 \
  -m '{"type":"contacts","device_id":"veh-esp32s3-01","version":1795246160000,"token":"CHANGE_ME_SHARED_SECRET","contacts":[{"name":"Abbu","phone":"+8801XXXXXXXXX"}]}'
```
Browser-এ **HiveMQ WebSocket client** বা **MQTT Explorer**-ও ব্যবহার করা যায়।

---

## 9. Alert flow ও SMS

1. Trigger (ML/rule) → **PENDING**: `CANCEL_WINDOW_MS` (10 s)-এর মধ্যে **BOOT button** চাপলে cancel। এই সময় telemetry-তে `status` আগেই accident/rollover দেখায়।
2. **DISPATCH:**
   a. MQTT `alert` publish (দরকার হলে আগে reconnect)।
   b. SMS on থাকলে: MQTT loop pause → modem mutex → **প্রতিটি contact-এ SIM800 দিয়ে SMS (AT+CMGS)** → ব্যর্থ number-এ **একবার retry** → serial-এ per-number result (name + masked number: `Abbu +880171****678 : OK`)।
   c. MQTT resume → `sms_report` publish।
3. **COOLDOWN** (60 s)।

Contact list: dashboard-এর synced list (NVS), না থাকলে `config.h`-এর `SMS_RECIPIENTS`। SMS কখনো MQTT বা কোনো web gateway দিয়ে যায় না।

### SMS format (সর্বোচ্চ 160 character, একটি SMS)

Priority (কম গুরুত্বপূর্ণটা আগে ছোট/বাদ হয়): **1 alert type → 2 peak accel → 3 AI YES/NO → 4 temperature → 5 GPS + Google Maps link** (link না আঁটলে plain coords) **→ 6 place (nearest hospital, সবার আগে বাদ)**। Host test-এ (`tools/sms_format_test.cpp`) 20,000 random case-এ কখনো 160 ছাড়ায়নি।

```
ACCIDENT ALERT: CRASH | Peak 6.8g | AI: YES | Temp 34.5C | maps.google.com/?q=23.81031,90.41252 | Near: Evercare Hospital Dhaka 2.0km     (133)
ACCIDENT ALERT: CRASH | Peak 4.4g | AI: YES | Temp 29.0C | maps.google.com/?q=23.81030,90.41250 (no fix, preset) | Near: Dhaka Medical College Hospital 9.6km   (157)
```
`(no fix, last known)` = GPS fix নেই, শেষ জানা location; `(no fix, preset)` = কখনো fix পায়নি, config location। `AI: N/A` = model চলছে না। Temperature sensor না থাকলে temp field বাদ।

SMS notes: SIM-এ balance লাগবে; SIM800 শুধু 2G; hospital phone না থাকলে `EMERGENCY_NUMBER` (999) config-এ আছে।

### Nearby hospital lookup

- Overpass API **HTTPS** — SIM800-এর পুরনো TLS দিয়ে সম্ভব নয়, তাই online query **শুধু WiFi থাকলে** (NET_WIFI / NET_WIFI_THEN_GPRS mode-এ, বা WiFi connected থাকলে)।
- Boot-এ, প্রতি 30 min-এ, এবং **2 km-এর বেশি সরে গেলে** নতুন query।
- Alert-এর সময় (last online result + offline table) থেকে **বর্তমান GPS position** অনুযায়ী distance আবার হিসাব করে কাছের 3টি দেয়। GPRS-only mode-এ শুধু offline table (`FALLBACK_HOSPITALS`) — নিজের এলাকার hospital যোগ করুন।

---

## 10. GPS

- **GPS_UART (default):** NEO-6M / NEO-M8N, 9600 baud NMEA, Serial2 (RX=GPIO15, TX=GPIO16), TinyGPSPlus দিয়ে parse।
- **GPS_SIM808:** আপনার "SIM 8000" module যদি আসলে **SIM808** (built-in GNSS) হয়, `GPS_SOURCE GPS_SIM808` দিন — `AT+CGNSPWR=1` ও `AT+CGNSINF` (modem mutex-এর মধ্যে, প্রতি 2 s)। GNSS antenna module-এর GPS connector-এ লাগাতে হবে। সাধারণ SIM800L-এ GNSS নেই।
- **GPS_NONE:** (optional) GPS module না লাগালে — তখন শুধু `FIXED_LAT/LON`। Default `GPS_UART`, অর্থাৎ GPS module চালু।
- Fix logic: fresh fix (< `GPS_FIX_MAX_AGE_MS` = 3 s) → `gps_fix:true`; নাহলে last known fix, তাও না থাকলে `FIXED_LAT/LON` (`gps_fix:false`)।
- SMS-এর Google Maps link ও hospital lookup সবসময় এই position ব্যবহার করে।

---

## 11. Known limitations / caveats

- Placeholder model detection-এর জন্য নয়; আসল model বসান।
- Training data simulated — real data দিয়ে retrain/threshold calibrate করুন।
- Hardware-এ end-to-end test (sensor, GPS, SIM800 GPRS+SMS, WiFi) আপনাকে করতে হবে; firmware compile ও host-side (model, features, SMS formatter) test করা হয়েছে।
- SIM800 2G-only; GPRS ধীর (MQTT reconnect কয়েক second লাগতে পারে); SMS চলাকালীন MQTT কয়েক second pause থাকে।
- HiveMQ public broker-এ privacy/security নেই; config token-ও public।
- Overpass HTTPS শুধু WiFi-তে, certificate verify করা হয় না (`setInsecure`)।
- SIM808 GNSS path (`GPS_SIM808`) শুধু compile-tested।
