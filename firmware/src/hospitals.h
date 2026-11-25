#pragma once
#include <stddef.h>

struct Hospital {
  char name[48];
  char phone[20];
  double lat, lon;
  float distanceKm;
};

// Query OpenStreetMap Overpass (amenity=hospital around lat/lon) over WiFi.
// Results are cached; on failure the offline table from config.h is used.
void hospitalsRefresh(double lat, double lon);       // blocking HTTP (~1-10 s)
void hospitalsMaybeRefresh(double lat, double lon);  // refresh if stale
size_t hospitalsGet(Hospital* out, size_t maxN, double lat, double lon);
bool hospitalsFromOnline();
