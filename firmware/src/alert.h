#pragma once
#include <stdint.h>
#include "payload.h"

// IDLE --trigger--> PENDING (cancel window, button cancels) --timeout-->
// DISPATCH (MQTT alert -> modem SMS to all contacts -> sms_report) --> COOLDOWN
enum class AlertState { Idle, Pending, Cooldown };

void alertBegin();
// Call every detection cycle. peakG = max |a| of the current window,
// ml = 1 anomaly / 0 normal / -1 model unavailable.
void alertUpdate(Status detected, const Sample& latest, float score, float threshold,
                 float peakG, int ml);
AlertState alertState();
Status alertActiveStatus();   // reason of the pending/last alert (Normal if idle)
