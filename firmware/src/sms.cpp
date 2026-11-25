#include "sms.h"
#include <Arduino.h>
#include "config.h"
#include "modem.h"
#include "net.h"
#include "remote_config.h"

const char* smsMask(const char* n, char* buf, size_t len) {
  size_t L = strlen(n);
  if (L <= 7) { strlcpy(buf, n, len); return buf; }
  snprintf(buf, len, "%.*s****%s", (int)(L - 7), n, n + L - 3);
  return buf;
}

SmsReport smsSendAll(const char* text) {
  SmsReport r{0, 0, 0, false};
  Contact ct[MAX_CONTACTS];
  bool synced = false;
  r.recipients = (int)contactsGet(ct, MAX_CONTACTS, &synced);
  if (!SMS_ENABLED || !smsEnabled()) {
    r.skipped = true;
    Serial.println("[sms] SMS disabled (remote switch) -> skipped");
    return r;
  }
  if (!modemReady()) {
    r.failed = r.recipients;
    Serial.println("[sms] modem not ready -> all failed");
    return r;
  }
  Serial.printf("[sms] sending to %d %s contacts: %s\n", r.recipients,
                synced ? "synced" : "config.h", text);
  netPause(true);              // MQTT loop paused...
  {
    ModemLock lk(60000);       // ...and modem owned by SMS for the burst
    bool ok[MAX_CONTACTS] = {false};
    for (int pass = 0; pass < (SMS_RETRY_ONCE ? 2 : 1); pass++) {
      for (int i = 0; i < r.recipients; i++) {
        if (ok[i]) continue;
        ok[i] = modemSendSMS(ct[i].phone, text);
        char m[24];
        Serial.printf("[sms] %s -> %s %s : %s\n", pass ? "retry" : "send",
                      ct[i].name[0] ? ct[i].name : "(no name)",
                      smsMask(ct[i].phone, m, sizeof(m)), ok[i] ? "OK" : "FAIL");
        delay(ok[i] ? 500 : 1500);
      }
    }
    for (int i = 0; i < r.recipients; i++) ok[i] ? r.sent++ : r.failed++;
  }
  netPause(false);
  Serial.printf("[sms] done: sent %d, failed %d\n", r.sent, r.failed);
  return r;
}
