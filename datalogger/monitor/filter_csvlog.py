"""PlatformIO device monitor filter: save logged samples to CSV.

Enable with `monitor_filters = csvlog` in platformio.ini. Text is passed to the
terminal unchanged. Every complete line that is exactly 9 comma-separated numbers
(timestamp_ms,ax,ay,az,gx,gy,gz,temperature,vibration; same rule as
collect_data.py) is appended to <project>/data/datalog_YYYYMMDD_HHMMSS.csv in the
training raw format. '#' and other non-numeric lines are skipped.

Env vars: DATALOG_LABEL (default 'normal'), DATALOG_SCENARIO (default 'driving').
"""
import atexit
import os
from datetime import datetime

from platformio.device.monitor.filters.base import DeviceMonitorFilterBase

HEADER = ("session_id,timestamp_ms,ax,ay,az,gx,gy,gz,temperature,vibration,"
          "label,scenario\n")
FLUSH_EVERY = 100


class CsvLog(DeviceMonitorFilterBase):
    NAME = "csvlog"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._fp = None
        self._buf = ""
        self._rows = 0
        self.path = None
        self.label = os.environ.get("DATALOG_LABEL", "normal")
        self.scenario = os.environ.get("DATALOG_SCENARIO", "driving")

    def __call__(self):
        self._open()
        return self

    def _open(self):
        if self._fp:
            return
        base = self.project_dir or os.getcwd()
        out_dir = os.path.join(base, "data")
        os.makedirs(out_dir, exist_ok=True)
        self.path = os.path.join(
            out_dir, "datalog_%s.csv" % datetime.now().strftime("%Y%m%d_%H%M%S"))
        self._fp = open(self.path, "w", encoding="utf-8", newline="")
        self._fp.write(HEADER)
        self._fp.flush()
        atexit.register(self.close)
        print("--- Saving samples to %s (label=%s, scenario=%s)"
              % (os.path.abspath(self.path), self.label, self.scenario))

    def close(self):
        if self._fp:
            try:
                self._fp.flush()
                self._fp.close()
            finally:
                self._fp = None

    def __del__(self):
        self.close()

    def _handle_line(self, line):
        line = line.strip()
        if not line or line.startswith("#"):
            return
        parts = line.split(",")
        if len(parts) != 9:
            return
        try:
            vals = [float(x) for x in parts]
        except ValueError:
            return
        if self._fp is None:
            self._open()
        ts = int(vals[0])
        self._fp.write("0,%d,%s,%s,%s\n" % (
            ts, ",".join(p.strip() for p in parts[1:]), self.label, self.scenario))
        self._rows += 1
        if self._rows % FLUSH_EVERY == 0:
            self._fp.flush()

    def rx(self, text):
        self._buf += text
        *lines, self._buf = self._buf.split("\n")
        for ln in lines:
            self._handle_line(ln)
        if len(self._buf) > 4096:   # runaway partial line: drop it
            self._buf = ""
        return text
