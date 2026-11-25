#pragma once
#include <stdint.h>

// Position from GPS (NEO-6M/NEO-M8N on Serial2 via TinyGPSPlus, or SIM808
// GNSS). No current fix -> last known fix -> FIXED_LAT/LON (fix=false).
enum class LocSource : uint8_t { Gps, LastKnown, Fixed };

struct GeoPoint {
  double lat, lon;
  bool fix;           // true only if a fresh GPS fix (< GPS_FIX_MAX_AGE_MS)
  int sats;           // satellites used (0 if unknown)
  float speedKmh;     // last known (0 if never)
  float headingDeg;   // course over ground (last known)
  float altM;         // altitude MSL (last known)
  float hdop;         // horizontal dilution (99.9 if unknown)
  LocSource source;
};

void locationBegin();
void locationPoll();          // call every loop (feeds NMEA / polls SIM808)
GeoPoint locationGet();
