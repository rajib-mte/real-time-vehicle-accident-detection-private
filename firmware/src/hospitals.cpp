#include "hospitals.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include "config.h"

static Hospital cache[HOSPITAL_MAX_RESULTS];
static size_t cacheN = 0;
static bool cacheOnline = false;
static uint32_t lastRefresh = 0;
static bool everRefreshed = false;
static double queryLat = 0, queryLon = 0;
static bool lastQueryOk = false;

static float haversineKm(double la1, double lo1, double la2, double lo2) {
  const double R = 6371.0, d2r = M_PI / 180.0;
  double dla = (la2 - la1) * d2r, dlo = (lo2 - lo1) * d2r;
  double a = sin(dla / 2) * sin(dla / 2) +
             cos(la1 * d2r) * cos(la2 * d2r) * sin(dlo / 2) * sin(dlo / 2);
  return (float)(2 * R * atan2(sqrt(a), sqrt(1 - a)));
}

static void insertSorted(Hospital* arr, size_t& n, size_t cap, const Hospital& h) {
  size_t pos = n;
  while (pos > 0 && arr[pos - 1].distanceKm > h.distanceKm) pos--;
  if (pos >= cap) return;
  size_t last = n < cap ? n : cap - 1;
  for (size_t i = last; i > pos; i--) arr[i] = arr[i - 1];
  arr[pos] = h;
  if (n < cap) n++;
}

static size_t fallback(Hospital* out, size_t maxN, double lat, double lon) {
  size_t n = 0;
  for (const auto& f : FALLBACK_HOSPITALS) {
    Hospital h{};
    strlcpy(h.name, f.name, sizeof(h.name));
    strlcpy(h.phone, f.phone, sizeof(h.phone));
    h.lat = f.lat; h.lon = f.lon;
    h.distanceKm = haversineKm(lat, lon, f.lat, f.lon);
    insertSorted(out, n, maxN, h);
  }
  return n;
}

void hospitalsRefresh(double lat, double lon) {
  lastRefresh = millis();
  everRefreshed = true;
  lastQueryOk = false;
#if HOSPITAL_LOOKUP_ENABLED
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();   // public data, no cert pinning (prototype)
  HTTPClient http;
  http.useHTTP10(true);   // no chunked encoding -> stream parsing works
  http.setTimeout(12000);
  if (!http.begin(client, OVERPASS_URL)) return;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.setUserAgent("AccidentDetector-ESP32S3/1.0");
  char q[256];
  snprintf(q, sizeof(q),
           "data=[out:json][timeout:10];nwr[\"amenity\"=\"hospital\"](around:%d,%.6f,%.6f);out center 25;",
           HOSPITAL_SEARCH_RADIUS_M, lat, lon);
  int code = http.POST((uint8_t*)q, strlen(q));
  if (code != 200) {
    Serial.printf("[hosp] Overpass HTTP %d\n", code);
    http.end();
    return;
  }
  JsonDocument filter;
  filter["elements"][0]["lat"] = true;
  filter["elements"][0]["lon"] = true;
  filter["elements"][0]["center"] = true;
  filter["elements"][0]["tags"]["name"] = true;
  filter["elements"][0]["tags"]["name:en"] = true;
  filter["elements"][0]["tags"]["phone"] = true;
  filter["elements"][0]["tags"]["contact:phone"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(),
                                             DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("[hosp] JSON error: %s\n", err.c_str());
    return;
  }
  Hospital tmp[HOSPITAL_MAX_RESULTS];
  size_t n = 0;
  for (JsonObject e : doc["elements"].as<JsonArray>()) {
    double hl = e["lat"] | (double)(e["center"]["lat"] | 0.0);
    double ho = e["lon"] | (double)(e["center"]["lon"] | 0.0);
    const char* name = e["tags"]["name:en"] | (const char*)(e["tags"]["name"] | "");
    if (!name[0] || (hl == 0 && ho == 0)) continue;
    Hospital h{};
    strlcpy(h.name, name, sizeof(h.name));
    const char* ph = e["tags"]["phone"] | (const char*)(e["tags"]["contact:phone"] | "");
    strlcpy(h.phone, ph, sizeof(h.phone));
    h.lat = hl; h.lon = ho;
    h.distanceKm = haversineKm(lat, lon, hl, ho);
    insertSorted(tmp, n, HOSPITAL_MAX_RESULTS, h);
  }
  if (n > 0) {
    memcpy(cache, tmp, sizeof(Hospital) * n);
    cacheN = n;
    cacheOnline = true;
    lastQueryOk = true;
    queryLat = lat; queryLon = lon;
    Serial.printf("[hosp] %u hospitals from Overpass, nearest: %s (%.2f km)\n",
                  (unsigned)n, cache[0].name, cache[0].distanceKm);
  }
#endif
}

void hospitalsMaybeRefresh(double lat, double lon) {
  // Overpass needs HTTPS -> WiFi only (SIM800 TLS is not usable).
  if (WiFi.status() != WL_CONNECTED) return;
  bool moved = lastQueryOk &&
               haversineKm(queryLat, queryLon, lat, lon) > HOSPITAL_REQUERY_KM;
  if (!everRefreshed || moved || (!lastQueryOk && millis() - lastRefresh > 60000UL) ||
      millis() - lastRefresh > HOSPITAL_REFRESH_MS) {
    if (moved) Serial.println("[hosp] moved > requery distance -> new Overpass query");
    hospitalsRefresh(lat, lon);
  }
}

// Nearest N from (last online result  U  offline table), distances
// recomputed from the CURRENT position (so it stays right while moving).
size_t hospitalsGet(Hospital* out, size_t maxN, double lat, double lon) {
  size_t n = 0;
  auto consider = [&](const Hospital& h0) {
    for (size_t i = 0; i < n; i++) if (strcmp(out[i].name, h0.name) == 0) return;
    Hospital h = h0;
    h.distanceKm = haversineKm(lat, lon, h.lat, h.lon);
    insertSorted(out, n, maxN, h);
  };
  if (cacheOnline) for (size_t i = 0; i < cacheN; i++) consider(cache[i]);
  Hospital fb[HOSPITAL_MAX_RESULTS + 8];
  size_t fn = fallback(fb, sizeof(fb) / sizeof(fb[0]), lat, lon);
  for (size_t i = 0; i < fn; i++) consider(fb[i]);
  return n;
}

bool hospitalsFromOnline() { return cacheOnline; }
