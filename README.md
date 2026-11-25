# 9. Real-Time Vehicle Accident Detection Using TinyML and Sensor Fusion

ESP32-S3-ভিত্তিক একটি **real-time vehicle accident detection** system। MPU6050 (acceleration + gyro), DS18B20 (temperature) আর একটি vibration sensor-এর data **sensor fusion** করে একটি ছোট **TinyML** model (TensorFlow Lite Micro, int8) গাড়িতেই crash, rollover বা abnormal temperature (engine fire) শনাক্ত করে, তারপর alert পাঠায় (SMS / MQTT)।

## Project structure

| Folder | কী আছে | README |
|---|---|---|
| `training/` | Python ML pipeline: data collect/simulate → features → train (autoencoder default, classifier optional) → int8 TFLite → C headers | [training/README.md](training/README.md) |
| `firmware/` | ESP32-S3 PlatformIO project (sensor read, TFLite Micro inference, alerts) | [firmware/README.md](firmware/README.md) |
| `dashboard.html` | Mobile-friendly MQTT dashboard (MQTT.js + Chart.js, HiveMQ WSS default, demo mode) — browser-এ খুলুন | [dashboard.html](dashboard.html) |

## Workflow

1. **training/** — data collect → train → `convert_tflite.py` চালিয়ে `model_data.h`, `model_data.cc`, `model_config.h` তৈরি
2. এই তিনটি file **firmware/**-এ copy (যেমন `firmware/include/`, `firmware/src/`)
3. **firmware/** — PlatformIO দিয়ে build ও ESP32-S3-এ upload
4. Real sensor data log করে (`collect_data.py --serial`) আবার retrain → নতুন header copy

## Hardware (summary)

ESP32-S3 · MPU6050 (SDA GPIO8, SCL GPIO9, 0x68) · DS18B20 (GPIO4) · vibration sensor (ADC pin, যেমন GPIO7 — ESP32-S3-এ GPIO34-এ ADC নেই) · SIM800L (TX GPIO17, RX GPIO18)

> ⚠️ Training-এর sample data simulated — আসল crash data না। Real measurement দিয়ে retrain ও validate না করে এটি কোনো safety system-এ ব্যবহার করবেন না।
