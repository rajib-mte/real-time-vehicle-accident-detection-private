#include "config_parse.h"
#include <ArduinoJson.h>
#include <string.h>
#include <ctype.h>

bool validPhone(const char* s) {
  if (!s) return false;
  size_t L = strlen(s);
  if (L < 5 || L >= CONTACT_PHONE_LEN) return false;
  for (size_t i = 0; i < L; i++)
    if (!(isdigit((unsigned char)s[i]) || (i == 0 && s[i] == '+'))) return false;
  return true;
}

static void copyTrunc(char* dst, size_t n, const char* src) {
  size_t i = 0;
  for (; src && src[i] && i + 1 < n; i++) dst[i] = src[i];
  dst[i] = 0;
}

// Copy at most CONTACT_NAME_CHARS UTF-8 characters, never splitting a character.
static void copyName(char* dst, const char* src) {
  size_t i = 0, chars = 0;
  while (src && src[i] && chars < CONTACT_NAME_CHARS) {
    unsigned char c = (unsigned char)src[i];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (i + n >= CONTACT_NAME_LEN) break;
    size_t k = 0;
    for (; k < n && src[i + k]; k++) dst[i + k] = src[i + k];
    if (k < n) break;   // truncated sequence in input
    i += n;
    chars++;
  }
  dst[i] = 0;
}

// Common envelope checks. Returns NotForUs / Error / Ok.
static CfgResult envelope(JsonDocument& d, const uint8_t* json, size_t len, const char* type,
                          const char* deviceId, const char* token, uint64_t curVer,
                          uint64_t& ver, const char*& err) {
  ver = 0;
  if (deserializeJson(d, json, len)) { err = "bad_json"; return CfgResult::Error; }
  if (!d.is<JsonObject>()) { err = "bad_json"; return CfgResult::Error; }
  const char* dev = d["device_id"] | "";
  if (strcmp(dev, "*") != 0 && strcmp(dev, deviceId) != 0) return CfgResult::NotForUs;
  ver = d["version"] | (uint64_t)0;
  if (strcmp(d["type"] | "", type) != 0) { err = "bad_type"; return CfgResult::Error; }
  if (strcmp(d["token"] | "", token) != 0) { err = "bad_token"; return CfgResult::Error; }
  if (!d["version"].is<uint64_t>()) { err = "bad_version"; return CfgResult::Error; }
  if (ver < curVer) { err = "stale_version"; return CfgResult::Error; }
  return CfgResult::Ok;
}

CfgResult parseContactsMsg(const uint8_t* json, size_t len, const char* deviceId,
                           const char* token, uint64_t curVer, size_t maxContacts,
                           ContactsMsg& out) {
  memset(&out, 0, sizeof(out));
  if (maxContacts > CONTACTS_MAX_PARSE) maxContacts = CONTACTS_MAX_PARSE;
  JsonDocument d;
  CfgResult r = envelope(d, json, len, "contacts", deviceId, token, curVer, out.version, out.error);
  if (r != CfgResult::Ok) return r;
  if (!d["contacts"].is<JsonArray>()) { out.error = "contacts_not_array"; return CfgResult::Error; }
  JsonArray arr = d["contacts"].as<JsonArray>();
  for (JsonVariant v : arr) {
    const char* phone = nullptr;
    const char* name = "";
    if (v.is<const char*>()) {
      phone = v.as<const char*>();
    } else if (v.is<JsonObject>()) {
      phone = v["phone"] | (const char*)nullptr;
      name = v["name"] | "";
    }
    if (!validPhone(phone) || out.count >= maxContacts) { out.rejected++; continue; }
    copyTrunc(out.contacts[out.count].phone, CONTACT_PHONE_LEN, phone);
    copyName(out.contacts[out.count].name, name);
    out.count++;
  }
  if (arr.size() > 0 && out.count == 0) { out.error = "no_valid_phone"; return CfgResult::Error; }
  return CfgResult::Ok;   // empty list = use config.h fallback numbers
}

CfgResult parseSmsCfgMsg(const uint8_t* json, size_t len, const char* deviceId,
                         const char* token, uint64_t curVer, SmsCfgMsg& out) {
  memset(&out, 0, sizeof(out));
  JsonDocument d;
  CfgResult r = envelope(d, json, len, "sms_config", deviceId, token, curVer, out.version, out.error);
  if (r != CfgResult::Ok) return r;
  if (!d["sms_enabled"].is<bool>()) { out.error = "sms_enabled_not_bool"; return CfgResult::Error; }
  out.smsEnabled = d["sms_enabled"].as<bool>();
  return CfgResult::Ok;
}
