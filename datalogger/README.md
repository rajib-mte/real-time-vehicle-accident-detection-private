# Accident Datalogger (ESP32-S3)

Standalone PlatformIO project for collecting **real driving data** to retrain the
autoencoder. It reads the same sensors as the alert firmware (`../firmware/`), with
the same pins, ranges, filter and units, and streams one CSV line per sample over USB
serial. No network, modem, GPS, MQTT, SMS or ML. Self-contained: it includes nothing
from `firmware/`.

## Wiring (same as the main firmware)

| Sensor | Pin |
|---|---|
| MPU6050 SDA / SCL (addr 0x68) | GPIO 8 / GPIO 9 |
| DS18B20 data (4.7 k pull-up to 3V3) | GPIO 4 |
| Vibration sensor (analog out) | GPIO 7 (ADC1_CH6, 12-bit, 11 dB) |
| BOOT button | GPIO 0: pause / resume logging |

## Flash

```
pio run -t upload
```

## Line format

100 lines per second, 115200 baud, exactly one line per sample, 9 comma-separated
numbers, no spaces, no units:

```
timestamp_ms,ax,ay,az,gx,gy,gz,temperature,vibration
1234,0.0123,-0.0456,0.9987,-0.12,0.34,0.05,27.56,0.012
```

Units: accel in g (4 decimals), gyro in deg/s (2), temperature in C (2), vibration
0..1 (3). Lines starting with `#` are boot/status messages (`# PAUSED`, `# RESUMED`);
`collect_data.py` skips them. Bandwidth: a line is at most about 66 characters, so
100 lines/s is at most 6.6 kB/s against about 11.5 kB/s available at 115200 baud (57 %).

## Save to CSV from PlatformIO

`platformio.ini` enables a custom monitor filter (`monitor_filters = csvlog`, in
`monitor/filter_csvlog.py`). Open the PlatformIO Monitor (VS Code button) or run
`pio device monitor`, and every sample line is saved straight to a CSV in
`datalogger/data/datalog_YYYYMMDD_HHMMSS.csv`. The terminal still shows the raw text;
`#` and other non-numeric lines are not saved. The columns match the training raw
format: `session_id,timestamp_ms,ax,ay,az,gx,gy,gz,temperature,vibration,label,scenario`
(session_id is 0). Set the label and scenario first (default `normal` / `driving`):

```
$env:DATALOG_LABEL='normal'; $env:DATALOG_SCENARIO='cruising'; pio device monitor
```

(bash: `DATALOG_LABEL=normal DATALOG_SCENARIO=cruising pio device monitor`). The path is
printed when the monitor starts; the file is flushed every 100 rows and closed on exit.
Then copy the files to `training/data/raw/` (name them `real_*.csv`) and train:

```
python scripts/train.py --data data/raw/real_*.csv
```

The `esp32_exception_decoder` filter was removed: it buffers and rewrites the serial
stream, and this firmware prints no crash traces worth decoding during logging.

## Alternative: collect_data.py

### Capture on the PC

Run from `training/`:

```
python scripts/collect_data.py --serial COM5 --baud 115200 --label normal --scenario cruising --seconds 300 --out data/raw/real_normal.csv
```

The output is saved as CSV (not Excel); re-running with the same `--out` appends a
new session. Capture normal driving only for the autoencoder, then retrain,
convert to int8 TFLite and recalibrate the threshold.

## After logging

Reflash the main `firmware/` project (`pio run -e esp32s3 -t upload`) to go back to
alert mode.
