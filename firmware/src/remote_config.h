#pragma once
#include <stddef.h>
#include <stdint.h>
#include "config_parse.h"

// Dashboard-controlled settings persisted in NVS (flash):
//  - SMS contact list   (MQTT_CONTACTS_TOPIC, fallback SMS_RECIPIENTS)
//  - SMS on/off switch  (MQTT_SMS_CFG_TOPIC, default SMS_ENABLED_DEFAULT)
void remoteConfigBegin();
bool smsEnabled();
// Fills contacts (name may be ""). fromSync=false -> config.h fallback list.
size_t contactsGet(Contact* out, size_t maxN, bool* fromSync = nullptr);

// MQTT message handler; fills ackTopic/ackJson if an ack must be published.
// Returns true if the topic was handled.
bool remoteConfigHandle(const char* topic, const uint8_t* payload, size_t len,
                        char* ackTopic, size_t ackTopicLen, char* ackJson, size_t ackLen);
