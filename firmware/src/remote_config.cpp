#include "remote_config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "config.h"
#include "sms.h"

static_assert(MAX_CONTACTS <= CONTACTS_MAX_PARSE, "MAX_CONTACTS too large");

static Preferences prefs;
static bool smsOn = SMS_ENABLED_DEFAULT;
static uint64_t smsVer = 0, contactsVer = 0;
static Contact synced[MAX_CONTACTS];
static size_t syncedN = 0;

static void logContacts(const char* tag) {
  for (size_t i = 0; i < syncedN; i++) {
    char m[24];
    Serial.printf("[cfg] %s %u: %s %s\n", tag, (unsigned)i + 1,
                  synced[i].name[0] ? synced[i].name : "(no name)",
                  smsMask(synced[i].phone, m, sizeof(m)));
  }
}

void remoteConfigBegin() {
  prefs.begin("accident", false);
  smsOn = prefs.getBool("sms_on", SMS_ENABLED_DEFAULT);
  smsVer = prefs.getULong64("sms_ver", 0);
  contactsVer = prefs.getULong64("ct_ver", 0);
  syncedN = prefs.getUChar("ct_n", 0);
  if (syncedN > MAX_CONTACTS) syncedN = 0;
  for (size_t i = 0; i < syncedN; i++) {
    char key[8];
    snprintf(key, sizeof(key), "ct%u", (unsigned)i);
    prefs.getString(key, synced[i].phone, sizeof(synced[i].phone));
    snprintf(key, sizeof(key), "cn%u", (unsigned)i);
    prefs.getString(key, synced[i].name, sizeof(synced[i].name));
  }
  Serial.printf("[cfg] sms_enabled=%s (v%llu), synced contacts=%u (v%llu)\n",
                smsOn ? "true" : "false", (unsigned long long)smsVer,
                (unsigned)syncedN, (unsigned long long)contactsVer);
  logContacts("contact");
}

bool smsEnabled() { return smsOn; }

size_t contactsGet(Contact* out, size_t maxN, bool* fromSync) {
  size_t n = 0;
  if (syncedN > 0) {
    for (size_t i = 0; i < syncedN && n < maxN; i++) out[n++] = synced[i];
    if (fromSync) *fromSync = true;
  } else {
    for (size_t i = 0; i < SMS_NUM_RECIPIENTS && n < maxN; i++) {
      strlcpy(out[n].phone, SMS_RECIPIENTS[i], sizeof(out[n].phone));
      out[n].name[0] = 0;
      n++;
    }
    if (fromSync) *fromSync = false;
  }
  return n;
}

bool remoteConfigHandle(const char* topic, const uint8_t* payload, size_t len,
                        char* ackTopic, size_t ackTopicLen, char* ackJson, size_t ackLen) {
  bool isSms = strcmp(topic, MQTT_SMS_CFG_TOPIC) == 0;
  bool isCt = strcmp(topic, MQTT_CONTACTS_TOPIC) == 0;
  if (!isSms && !isCt) return false;
  ackTopic[0] = ackJson[0] = 0;
  if (len == 0) return true;   // retained message cleared

  JsonDocument a;
  if (isSms) {
    SmsCfgMsg m;
    CfgResult r = parseSmsCfgMsg(payload, len, DEVICE_ID, CONTACTS_TOKEN, smsVer, m);
    if (r == CfgResult::NotForUs) return true;
    if (r == CfgResult::Ok) {
      bool changed = m.version != smsVer || smsOn != m.smsEnabled;
      smsOn = m.smsEnabled;
      if (changed) {
        smsVer = m.version;
        prefs.putBool("sms_on", smsOn);
        prefs.putULong64("sms_ver", smsVer);
      }
    }
    Serial.printf("[cfg] sms_config v%llu -> %s, sms_enabled=%s\n", (unsigned long long)m.version,
                  r == CfgResult::Ok ? "applied" : m.error, smsOn ? "true" : "false");
    a["type"] = "sms_ack";
    a["device_id"] = DEVICE_ID;
    a["version"] = m.version;
    a["sms_enabled"] = smsOn;
    a["ok"] = r == CfgResult::Ok;
    if (r != CfgResult::Ok) a["error"] = m.error;
  } else {
    static ContactsMsg m;   // ~750 B, keep off the stack
    CfgResult r = parseContactsMsg(payload, len, DEVICE_ID, CONTACTS_TOKEN, contactsVer,
                                   MAX_CONTACTS, m);
    if (r == CfgResult::NotForUs) return true;
    if (r == CfgResult::Ok) {
      bool changed = m.version != contactsVer || m.count != syncedN;
      for (size_t i = 0; !changed && i < m.count; i++)
        changed = strcmp(m.contacts[i].phone, synced[i].phone) || strcmp(m.contacts[i].name, synced[i].name);
      memcpy(synced, m.contacts, sizeof(Contact) * m.count);
      syncedN = m.count;
      if (changed) {
        contactsVer = m.version;
        prefs.putUChar("ct_n", (uint8_t)syncedN);
        for (size_t i = 0; i < syncedN; i++) {
          char key[8];
          snprintf(key, sizeof(key), "ct%u", (unsigned)i);
          prefs.putString(key, synced[i].phone);
          snprintf(key, sizeof(key), "cn%u", (unsigned)i);
          prefs.putString(key, synced[i].name);
        }
        prefs.putULong64("ct_ver", contactsVer);
      }
      Serial.printf("[cfg] contacts v%llu applied: %u numbers (%u rejected)%s\n",
                    (unsigned long long)m.version, (unsigned)syncedN, (unsigned)m.rejected,
                    syncedN ? "" : " -> using config.h numbers");
      logContacts("contact");
    } else {
      Serial.printf("[cfg] contacts v%llu REJECTED: %s\n", (unsigned long long)m.version, m.error);
    }
    a["type"] = "contacts_ack";
    a["device_id"] = DEVICE_ID;
    a["version"] = m.version;
    a["count"] = (int)syncedN;
    a["ok"] = r == CfgResult::Ok;
    if (r != CfgResult::Ok) a["error"] = m.error;
  }
  snprintf(ackTopic, ackTopicLen, "%s/ack", topic);
  serializeJson(a, ackJson, ackLen);
  return true;
}
