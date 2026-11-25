#include "sms_format.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

namespace {
const int kMaxAlt = 4;
struct Seg { char alt[kMaxAlt][96]; int n; int cur; };   // alt[0] = fullest

void setAlt(Seg& s, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void setAlt(Seg& s, const char* fmt, ...) {
  if (s.n >= kMaxAlt) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s.alt[s.n++], sizeof(s.alt[0]), fmt, ap);
  va_end(ap);
}

// GSM-7 safety: replace non-ASCII / unusual chars with '?'
void sanitize(char* s) {
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c < 32 || c > 126 || c == '[' || c == ']' || c == '{' || c == '}' ||
        c == '\\' || c == '^' || c == '~' || c == '|' || c == '`')
      *s = (c == '|') ? '/' : '?';
  }
}

void upper(char* d, const char* s, size_t n) {
  size_t i = 0;
  for (; s[i] && i + 1 < n; i++) d[i] = (char)toupper((unsigned char)s[i]);
  d[i] = 0;
}
}  // namespace

size_t formatAccidentSms(const SmsInfo& in, char* out, size_t outLen) {
  enum { TYPE, PEAK, AI, TEMP, LOC, PLACE, NSEG };
  static Seg seg[NSEG];
  memset(seg, 0, sizeof(seg));
  char st[16];
  const char* s0 = in.status ? in.status : "accident";
  if (!strcmp(s0, "accident")) s0 = "crash";
  else if (!strcmp(s0, "over_temp")) s0 = "over-temp";
  upper(st, s0, sizeof(st));

  setAlt(seg[TYPE], "ACCIDENT ALERT: %s", st);
  setAlt(seg[TYPE], "ALERT %s", st);
  setAlt(seg[PEAK], "Peak %.1fg", in.peakG);
  if (in.ai >= 0) { setAlt(seg[AI], "AI: %s", in.ai ? "YES" : "NO"); setAlt(seg[AI], "AI:%s", in.ai ? "Y" : "N"); }
  else            { setAlt(seg[AI], "AI: N/A"); }
  if (!isnan(in.tempC)) { setAlt(seg[TEMP], "Temp %.1fC", in.tempC); setAlt(seg[TEMP], "T%.0fC", in.tempC); }
  if (in.hasPos) {
    const char* tagFull = in.posKind == 0 ? "" : in.posKind == 1 ? " (no fix, last known)" : " (no fix, preset)";
    const char* tagShort = in.posKind == 0 ? "" : in.posKind == 1 ? " (last)" : " (preset)";
    setAlt(seg[LOC], "maps.google.com/?q=%.5f,%.5f%s", in.lat, in.lon, tagFull);
    setAlt(seg[LOC], "maps.google.com/?q=%.5f,%.5f%s", in.lat, in.lon, tagShort);
    setAlt(seg[LOC], "Loc %.5f,%.5f%s", in.lat, in.lon, tagShort);
    setAlt(seg[LOC], "%.4f,%.4f", in.lat, in.lon);
  }
  if (in.place && in.place[0]) {
    setAlt(seg[PLACE], "Near: %s %.1fkm", in.place, in.placeKm);
    setAlt(seg[PLACE], "Near %.24s %.1fkm", in.place, in.placeKm);
    setAlt(seg[PLACE], "Near %.14s", in.place);
  }
  for (int i = 0; i < NSEG; i++) sanitize(seg[i].alt[0]), sanitize(seg[i].alt[1]),
                                 sanitize(seg[i].alt[2]), sanitize(seg[i].alt[3]);

  auto build = [&](char* buf, size_t len) -> size_t {
    size_t n = 0;
    buf[0] = 0;
    for (int i = 0; i < NSEG; i++) {
      if (seg[i].n == 0 || seg[i].cur >= seg[i].n) continue;   // empty / dropped
      n += snprintf(buf + n, n < len ? len - n : 0, "%s%s", n ? " | " : "", seg[i].alt[seg[i].cur]);
    }
    return n;
  };

  char tmp[512];
  size_t n = build(tmp, sizeof(tmp));
  // Shorten / drop from the lowest priority upward. TYPE and PEAK never drop.
  for (int i = NSEG - 1; i >= 0 && n > SMS_MAX_LEN; i--) {
    while (n > SMS_MAX_LEN && seg[i].n > 0 && seg[i].cur < seg[i].n) {
      bool last = seg[i].cur == seg[i].n - 1;
      if (last && (i == TYPE || i == PEAK)) break;   // keep the minimal form
      seg[i].cur++;                                  // shorter alt, or drop
      n = build(tmp, sizeof(tmp));
    }
  }
  if (n > SMS_MAX_LEN) { tmp[SMS_MAX_LEN] = 0; n = SMS_MAX_LEN; }   // hard cap
  snprintf(out, outLen, "%s", tmp);
  return strlen(out);
}
