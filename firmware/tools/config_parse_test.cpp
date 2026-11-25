// Host test for src/config_parse.cpp (needs ArduinoJson headers, e.g. from .pio):
//   g++ -std=c++17 -Isrc -I.pio/libdeps/esp32s3/ArduinoJson/src tools/config_parse_test.cpp src/config_parse.cpp -o cfgtest && ./cfgtest
#include <cstdio>
#include <cstring>
#include "config_parse.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static const char* DEV = "veh-esp32s3-01";
static const char* TOK = "secret";

static CfgResult ct(const char* j, ContactsMsg& m, uint64_t cur = 0, size_t maxN = 10) {  // MAX_CONTACTS = 10
  return parseContactsMsg((const uint8_t*)j, strlen(j), DEV, TOK, cur, maxN, m);
}
static CfgResult sc(const char* j, SmsCfgMsg& m, uint64_t cur = 0) {
  return parseSmsCfgMsg((const uint8_t*)j, strlen(j), DEV, TOK, cur, m);
}

int main() {
  static ContactsMsg m; SmsCfgMsg s;
  puts("contacts: dashboard object format");
  CHECK(ct(R"({"type":"contacts","device_id":"veh-esp32s3-01","version":1795246160000,"token":"secret",
    "contacts":[{"name":"Abbu","phone":"+8801711111111"},{"name":"A very long contact name that exceeds","phone":"+8801822222222"}]})", m) == CfgResult::Ok);
  CHECK(m.count == 2 && m.version == 1795246160000ULL);
  CHECK(!strcmp(m.contacts[0].name, "Abbu") && !strcmp(m.contacts[0].phone, "+8801711111111"));
  CHECK(strlen(m.contacts[1].name) == 24);
  printf("  name[1] truncated -> '%s'\n", m.contacts[1].name);
  puts("contacts: plain strings + mixed + wildcard device");
  CHECK(ct(R"({"type":"contacts","device_id":"*","version":5,"token":"secret","contacts":["+8801711111111",{"phone":"01933333333"},{"name":"bad","phone":"12ab"},42]})", m) == CfgResult::Ok);
  CHECK(m.count == 2 && m.rejected == 2 && m.contacts[1].name[0] == 0);
  puts("contacts: errors");
  CHECK(ct(R"({"type":"contacts","device_id":"*","version":5,"token":"WRONG","contacts":[]})", m) == CfgResult::Error && !strcmp(m.error, "bad_token"));
  CHECK(ct(R"({"type":"contacts","device_id":"*","version":4,"token":"secret","contacts":[]})", m, 5) == CfgResult::Error && !strcmp(m.error, "stale_version"));
  CHECK(ct(R"({"type":"contacts","device_id":"*","version":5,"token":"secret","contacts":["x"]})", m) == CfgResult::Error && !strcmp(m.error, "no_valid_phone"));
  CHECK(ct(R"({"type":"contacts","device_id":"*","version":5,"token":"secret","contacts":{}})", m) == CfgResult::Error && !strcmp(m.error, "contacts_not_array"));
  CHECK(ct(R"({"type":"sms_config","device_id":"*","version":5,"token":"secret","contacts":[]})", m) == CfgResult::Error && !strcmp(m.error, "bad_type"));
  CHECK(ct(R"({not json)", m) == CfgResult::Error && !strcmp(m.error, "bad_json"));
  CHECK(ct(R"({"type":"contacts","device_id":"other-car","version":5,"token":"secret","contacts":[]})", m) == CfgResult::NotForUs);
  puts("contacts: empty list ok (fallback to config.h), cap at maxN");
  CHECK(ct(R"({"type":"contacts","device_id":"*","version":6,"token":"secret","contacts":[]})", m) == CfgResult::Ok && m.count == 0);
  CHECK(ct(R"({"type":"contacts","device_id":"*","version":6,"token":"secret","contacts":["+8801","+88012","+880123","+8801234"]})", m, 0, 2) == CfgResult::Ok && m.count == 2 && m.rejected == 2);
  puts("contacts: cap = 10 (dashboard limit), 11th rejected");
  CHECK(ct(R"({"type":"contacts","device_id":"*","version":9,"token":"secret","contacts":[
    {"name":"c1","phone":"+8801700000001"},{"name":"c2","phone":"+8801700000002"},{"name":"c3","phone":"+8801700000003"},
    {"name":"c4","phone":"+8801700000004"},{"name":"c5","phone":"+8801700000005"},{"name":"c6","phone":"+8801700000006"},
    {"name":"c7","phone":"+8801700000007"},{"name":"c8","phone":"+8801700000008"},{"name":"c9","phone":"+8801700000009"},
    {"name":"c10","phone":"+8801700000010"},{"name":"c11","phone":"+8801700000011"}]})", m) == CfgResult::Ok);
  CHECK(m.count == 10 && m.rejected == 1 && !strcmp(m.contacts[9].name, "c10"));
  puts("contacts: Bangla name truncated to 24 characters without splitting UTF-8");
  CHECK(ct(R"({"type":"contacts","device_id":"*","version":9,"token":"secret","contacts":[{"name":"আব্দুল করিম মোহাম্মদ রহমান চৌধুরী","phone":"+8801711111111"}]})", m) == CfgResult::Ok);
  { size_t chars = 0; for (const char* p = m.contacts[0].name; *p; p++) if ((*p & 0xC0) != 0x80) chars++;
    CHECK(chars == 24); printf("  -> '%s' (%zu chars, %zu bytes)\n", m.contacts[0].name, chars, strlen(m.contacts[0].name)); }
  puts("sms_config");
  CHECK(sc(R"({"type":"sms_config","device_id":"veh-esp32s3-01","version":1795246160000,"token":"secret","sms_enabled":false})", s) == CfgResult::Ok && !s.smsEnabled);
  CHECK(sc(R"({"type":"sms_config","device_id":"*","version":7,"token":"secret","sms_enabled":true})", s) == CfgResult::Ok && s.smsEnabled);
  CHECK(sc(R"({"type":"sms_config","device_id":"*","version":7,"token":"secret","sms_enabled":"no"})", s) == CfgResult::Error && !strcmp(s.error, "sms_enabled_not_bool"));
  CHECK(sc(R"({"type":"sms_config","device_id":"*","version":6,"token":"secret","sms_enabled":true})", s, 7) == CfgResult::Error && !strcmp(s.error, "stale_version"));
  CHECK(sc(R"({"type":"sms_config","device_id":"*","version":8,"token":"nope","sms_enabled":true})", s) == CfgResult::Error && !strcmp(s.error, "bad_token"));
  CHECK(sc(R"({"type":"sms_config","device_id":"car-2","version":8,"token":"secret","sms_enabled":true})", s) == CfgResult::NotForUs);
  printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
  return fails != 0;
}
