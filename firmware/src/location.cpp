#include "location.h"
#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "modem.h"
#if GPS_SOURCE == GPS_UART
#include <TinyGPSPlus.h>
static TinyGPSPlus gps;
#endif

static GeoPoint last = {FIXED_LAT, FIXED_LON, false, 0, 0, 0, 0, 99.9f, LocSource::Fixed};
static bool haveLast = false;
static uint32_t lastFixMs = 0;
static int satsNow = 0;
static uint32_t lastPoll = 0;

void locationBegin() {
#if GPS_SOURCE == GPS_UART
  Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.printf("[gps] UART NMEA on Serial2 RX=%d TX=%d @%d\n", GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);
#elif GPS_SOURCE == GPS_SIM808
  Serial.printf("[gps] SIM808 GNSS %s\n", modemGnssEnable() ? "enabled" : "NOT available");
#else
  Serial.println("[gps] none -> fixed location");
#endif
}

static void storeFix(double lat, double lon, int sats, float spd, float hdg,
                     float alt, float hdop) {
  last.lat = lat; last.lon = lon; last.sats = sats;
  if (!isnan(spd)) last.speedKmh = spd;
  if (!isnan(hdg)) last.headingDeg = hdg;
  if (!isnan(alt)) last.altM = alt;
  if (!isnan(hdop)) last.hdop = hdop;
  haveLast = true;
  lastFixMs = millis();
}

void locationPoll() {
#if GPS_SOURCE == GPS_UART
  while (Serial2.available()) gps.encode(Serial2.read());
  if (gps.satellites.isValid()) satsNow = gps.satellites.value();
  if (gps.location.isUpdated() && gps.location.isValid() && gps.location.age() < GPS_FIX_MAX_AGE_MS) {
    storeFix(gps.location.lat(), gps.location.lng(), satsNow,
             gps.speed.isValid() ? gps.speed.kmph() : NAN,
             gps.course.isValid() ? gps.course.deg() : NAN,
             gps.altitude.isValid() ? gps.altitude.meters() : NAN,
             gps.hdop.isValid() ? gps.hdop.hdop() : NAN);
  }
#elif GPS_SOURCE == GPS_SIM808
  if (millis() - lastPoll < GPS_SIM808_POLL_MS) return;
  lastPoll = millis();
  float la, lo, spd, alt, hdop, crs; int sats = 0;
  if (modemGnssRead(&la, &lo, &spd, &alt, &sats, &hdop, &crs)) {
    satsNow = sats;
    storeFix(la, lo, sats, spd, crs, alt, hdop);
  }
#endif
  (void)lastPoll;
}

GeoPoint locationGet() {
  GeoPoint g = last;
  bool fresh = haveLast && (millis() - lastFixMs) < GPS_FIX_MAX_AGE_MS;
  g.fix = fresh;
  g.sats = satsNow;
  g.source = fresh ? LocSource::Gps : (haveLast ? LocSource::LastKnown : LocSource::Fixed);
  if (!fresh) g.speedKmh = haveLast ? g.speedKmh : 0;  // last known value kept
  return g;
}
