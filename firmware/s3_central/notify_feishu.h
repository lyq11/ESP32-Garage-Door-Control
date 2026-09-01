#pragma once

#include <Arduino.h>

void notifyBegin();
void notifyTick();
bool notifyConfigure(bool enabled, const String &webhook, uint32_t cooldownSec, uint32_t maxAlerts);
bool notifyTest();
bool notifyAlert(const char *module, const String &message);
String notifyStatusJson();
