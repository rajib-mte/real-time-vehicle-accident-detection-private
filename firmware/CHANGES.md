# accident-firmware v2 change list (GPS, GPRS, SMS switch/contacts, SMS formatter)
ADDED
- src/modem.h/.cpp: one shared TinyGSM instance on Serial1 with a recursive FreeRTOS mutex (ModemLock), shared by SMS, GPRS/MQTT, SIM808 GNSS and time sync. Picks SIM800 or SIM808 from GPS_SOURCE; handles the APN connection, AT+CMGS SMS, NTP time via the modem, and raw AT+CGNSINF parsing.
- src/remote_config.h/.cpp: settings the dashboard can change, saved in flash (NVS). An SMS on/off switch (vehicle/accident/config/sms, sms_config with token and version, ack on /ack) and a synced contact list (vehicle/accident/config/contacts, ack on /ack). Falls back to the config.h numbers.
- src/sms_format.h/.cpp: a 160-character SMS formatter with no Arduino dependencies, so it can be tested on a PC. Field priority: type > peak g > AI YES/NO > temperature > maps link/coordinates > nearest hospital. Shortens or drops the lowest field first and keeps the text GSM-7 safe.
- tools/sms_format_test.cpp: host test that prints the sample SMS texts and runs a 20,000-case fuzz check that no text exceeds 160 characters.
CHANGED
- platformio.ini: added mikalhart/TinyGPSPlus. Removed the -DTINY_GSM_MODEM_SIM800 flag (the modem model is now chosen in modem.cpp).
- include/config.h + config.example.h: new settings for NET_MODE (GPRS/WIFI/WIFI_THEN_GPRS), GPRS_APN/USER/PASS with notes on Bangladesh APNs, GPS_SOURCE (UART/SIM808/NONE), GPS_RX_PIN 15 / GPS_TX_PIN 16 / GPS_BAUD, GPS fix age, TELEMETRY_INTERVAL_GPRS_MS, CONTACTS_TOKEN, the config topics, MAX_CONTACTS, SMS_ENABLED_DEFAULT, SMS_RETRY_ONCE and HOSPITAL_REQUERY_KM.
- src/location.h/.cpp: now a real GPS reader. Reads NMEA from a NEO-6M/M8N on Serial2 through TinyGPSPlus, or polls the SIM808 GNSS. Falls back to the last known fix, then to the fixed location. Provides fix, sats, speed, heading, altitude and hdop.
- src/net.h/.cpp: the NET_MODE transport switch. MQTT runs over a TinyGsmClient (GPRS) or WiFi and returns to WiFi when it comes back. Subscribes to the config topics (QoS1) and publishes acks. MQTT is paused during SMS, with a modem lock on every MQTT call. Adds a blocking reconnect for alerts, keepalive 90 s, time sync over GPRS, and a per-transport telemetry interval.
- src/sms.h/.cpp: SMS goes only through the SIM800 modem (AT+CMGS) to EVERY contact, synced list first, config.h list as fallback. MQTT is paused and the modem mutex held for the whole burst. Each failed number is retried once. Per-number results are logged with the number masked. Returns recipients/sent/failed/skipped and respects the remote SMS switch.
- src/alert.h/.cpp: the dispatch order is now MQTT alert (with peak_g, sms_enabled, sms_sent/failed, sms_pending or sms_skipped), then SMS to all contacts, then an sms_report follow-up on vehicle/accident. If the first alert publish failed, the alert is re-sent with the final counts instead. Records the peak g and AI result over the pending window.
- src/payload.h/.cpp: additive fields gps_fix, sats, speed_kmh, heading_deg, alt_m, hdop and sms_enabled in telemetry and alerts. Alerts also get peak_g, sms_sent, sms_failed and sms_skipped or sms_pending. New sms_report builder. Existing fields unchanged.
- src/hospitals.cpp: queries Overpass again after moving more than 2 km (WiFi only, since Overpass needs HTTPS). Results combine the last online answer with the offline table, with distances recomputed from the current GPS position.
- src/main.cpp: init order remote config, then modem, then GPS, then network. Calls locationPoll() every loop. Passes peak g and the AI result to the alert logic. Uses the per-transport telemetry interval.
- src/sensors.h/.cpp: added sensorsTempOk(), so the SMS leaves out the temperature when no DS18B20 is fitted. MPU gyro range was set to 2000 dps in v1 (unchanged).
- README.md: Bengali-English updates for GPS wiring and the pin conflict check, pin table, ASCII wiring, power notes, NET_MODE/APN table, data usage, modem sharing, new payload fields, sms_report, config topics and acks, SMS format with samples, hospital lookup over GPRS, and caveats.

# v3 (contacts format fix, dashboard contract check)
ADDED
- src/config_parse.h/.cpp: parsers for contacts and sms_config with no Arduino dependencies (ArduinoJson only). Contacts may be {name, phone} objects or plain strings. Names are cut to 24 characters, phones are checked, and up to MAX_CONTACTS are kept. Checks device_id ("*" or ours), type, token and version. Error codes: bad_json, bad_type, bad_token, bad_version, stale_version, contacts_not_array, no_valid_phone, sms_enabled_not_bool.
- tools/config_parse_test.cpp: host test of both parsers (dashboard object format, plain/mixed lists, name cut, every error path, wildcard and other device, empty list, cap). All pass.
CHANGED
- src/remote_config.h/.cpp: uses config_parse. Saves names and phones in flash (NVS) under keys cnN and ctN. contactsGet() now returns Contact{name, phone}. Logs names with masked numbers. contacts_ack is {type, device_id, version, count, ok, error?}. sms_ack is {type, device_id, version, sms_enabled, ok}, plus error when ok is false.
- src/sms.cpp: works from the name+phone contacts; the per-number log shows the name and the masked number.
- src/payload.h/.cpp: the alert now has ai_detected (bool), and sms_skipped and sms_pending are always present (bool), next to peak_g, sms_enabled, sms_sent and sms_failed.
- src/alert.cpp: fills ai_detected from the ML result. sms_report.alert_ts uses the same value as the alert's ts (unchanged logic, checked).
- README.md: updated the contacts and sms config tables (object format, error codes, ack fields), alert fields (ai_detected, sms_skipped), sms_report alert_ts note, project tree and build numbers.

# v4 (small fixes)
- include/config.h + config.example.h: MAX_CONTACTS 8 -> 10, matching the dashboard limit.
- src/config_parse.h/.cpp: contact names are now cut to 24 characters, not 24 bytes, without splitting a UTF-8 character (buffer 73 bytes), so Bangla names stay intact.
- src/net.cpp: MQTT buffer raised from 1024 to 2048 bytes so a 10-contact message with Bangla names fits.
- tools/config_parse_test.cpp: default cap is now 10; added a test that 10 contacts are accepted and the 11th rejected, and a test for cutting a Bangla name. All pass.
- README.md: contact limit now says 10 (MAX_CONTACTS), notes Bangla names, says GPS_NONE is only an option and the default is GPS_UART (GPS module on), and updates the build numbers. No leftover text says there is no GPS module.
