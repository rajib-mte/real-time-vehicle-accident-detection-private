#include "modem.h"
#include <sys/time.h>
#include "config.h"

// The one and only TinyGSM include point -> modem model chosen here.
#if GPS_SOURCE == GPS_SIM808
#define TINY_GSM_MODEM_SIM808
#else
#define TINY_GSM_MODEM_SIM800
#endif
#include <TinyGsmClient.h>

static TinyGsm modem(Serial1);
static TinyGsmClient gsmClient(modem, 0);
static SemaphoreHandle_t mtx = nullptr;
static bool ready = false;
static uint32_t lastGprsTry = 0;

bool modemLock(uint32_t t) {
  if (!mtx) return true;
  return xSemaphoreTakeRecursive(mtx, pdMS_TO_TICKS(t)) == pdTRUE;
}
void modemUnlock() { if (mtx) xSemaphoreGiveRecursive(mtx); }

bool modemBegin() {
  if (!mtx) mtx = xSemaphoreCreateRecursiveMutex();
  ModemLock lk;
  Serial1.begin(SIM_BAUD, SERIAL_8N1, PIN_SIM_RX, PIN_SIM_TX);
  if (PIN_SIM_RST >= 0) {
    pinMode(PIN_SIM_RST, OUTPUT);
    digitalWrite(PIN_SIM_RST, LOW);  delay(200);
    digitalWrite(PIN_SIM_RST, HIGH); delay(3000);
  }
  if (!modem.init()) {
    Serial.println("[modem] SIM800 not responding (SMS/GPRS disabled)");
    return false;
  }
  if (strlen(SIM_PIN_CODE) && modem.getSimStatus() != 3) modem.simUnlock(SIM_PIN_CODE);
  Serial.printf("[modem] %s\n", modem.getModemInfo().c_str());
  modem.sendAT(GF("+CMGF=1"));            // SMS text mode
  modem.waitResponse();
  if (!modem.waitForNetwork(60000L)) Serial.println("[modem] no GSM network yet");
  Serial.printf("[modem] operator '%s', CSQ %d\n", modem.getOperator().c_str(),
                modem.getSignalQuality());
  ready = true;
  return true;
}

bool modemReady() { return ready; }

bool modemSendSMS(const char* number, const char* text) {
  if (!ready) return false;
  ModemLock lk;
  if (!lk.ok) return false;
  return modem.sendSMS(number, text);     // AT+CMGS (works with GPRS socket open)
}

bool modemGprsUp() {
  if (!ready) return false;
  ModemLock lk;
  return lk.ok && modem.isGprsConnected();
}

bool modemGprsEnsure() {
  if (!ready) return false;
  ModemLock lk;
  if (!lk.ok) return false;
  if (modem.isGprsConnected()) return true;
  if (lastGprsTry && millis() - lastGprsTry < GPRS_RETRY_MS) return false;
  lastGprsTry = millis();
  if (!modem.isNetworkConnected() && !modem.waitForNetwork(10000L)) {
    Serial.println("[modem] GPRS: no network");
    return false;
  }
  Serial.printf("[modem] GPRS connect APN '%s'...\n", GPRS_APN);
  bool ok = modem.gprsConnect(GPRS_APN, GPRS_USER, GPRS_PASS);
  Serial.printf("[modem] GPRS %s, IP %s\n", ok ? "up" : "FAILED",
                ok ? modem.getLocalIP().c_str() : "-");
  return ok;
}

int modemSignal() {
  if (!ready) return 99;
  ModemLock lk;
  return lk.ok ? modem.getSignalQuality() : 99;
}

Client& modemClient() { return gsmClient; }

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

bool modemSyncTime() {
  if (!ready) return false;
  ModemLock lk;
  if (!lk.ok) return false;
  modem.NTPServerSync(NTP_SERVER, 0);     // modem RTC = UTC
  int y, mo, d, h, mi, s; float tz;
  if (!modem.getNetworkTime(&y, &mo, &d, &h, &mi, &s, &tz) || y < 2024) return false;
  int64_t epoch = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s -
                  (int64_t)(tz * 3600);
  struct timeval tv = {(time_t)epoch, 0};
  settimeofday(&tv, nullptr);
  Serial.printf("[modem] time synced %04d-%02d-%02d %02d:%02d:%02d (tz %.2f)\n",
                y, mo, d, h, mi, s, tz);
  return true;
}

bool modemGnssEnable() {
#if GPS_SOURCE == GPS_SIM808
  if (!ready) return false;
  ModemLock lk;
  return lk.ok && modem.enableGPS();      // AT+CGNSPWR=1
#else
  return false;
#endif
}

bool modemGnssRead(float* lat, float* lon, float* speedKmh, float* alt,
                   int* sats, float* hdop, float* course) {
#if GPS_SOURCE == GPS_SIM808
  if (!ready) return false;
  ModemLock lk;
  if (!lk.ok) return false;
  // +CGNSINF: run,fix,utc,lat,lon,alt_m,speed_kmh,course,fixmode,res,HDOP,PDOP,
  //           VDOP,res,GPS_in_view,GNSS_used,GLONASS_in_view,...
  modem.sendAT(GF("+CGNSINF"));
  if (modem.waitResponse(2000L, GF("+CGNSINF: ")) != 1) return false;
  String line = modem.stream.readStringUntil('\n');
  modem.waitResponse();
  String f[17];
  int idx = 0, from = 0;
  for (int i = 0; i <= (int)line.length() && idx < 17; i++) {
    if (i == (int)line.length() || line[i] == ',') { f[idx++] = line.substring(from, i); from = i + 1; }
  }
  if (idx < 16 || f[1].toInt() != 1) return false;   // no fix
  *lat = f[3].toFloat();  *lon = f[4].toFloat();
  *alt = f[5].toFloat();  *speedKmh = f[6].toFloat();
  *course = f[7].length() ? f[7].toFloat() : NAN;
  *hdop = f[10].length() ? f[10].toFloat() : NAN;
  *sats = f[15].length() ? f[15].toInt() : f[14].toInt();
  return true;
#else
  return false;
#endif
}
