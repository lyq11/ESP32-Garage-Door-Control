#pragma once

#include <Arduino.h>

enum GarageDoorState {
  GARAGE_DOOR_UNKNOWN,
  GARAGE_DOOR_CLOSED,
  GARAGE_DOOR_OPEN,
  GARAGE_DOOR_MOVING,
  GARAGE_DOOR_CONFLICT
};

struct GarageTimingConfig {
  uint32_t sendCooldownMs;
  uint16_t openStableSeconds;
  uint32_t openRecheckMs;
  uint32_t movingRecheckMs;
  uint32_t postTriggerRecheckMs;
  uint32_t maxAttemptRecheckMs;
  uint8_t maxAutoCloseAttempts;
};

void garageBegin();
void garageTick();
void garageRefreshPeerAfterWifiReconnect();
void garageOnFaceSuccess(int userId, const char *name, const char *verifyType);
bool garageTrigger(uint8_t command, const char *reason, int userId);
bool garageConfigure(const String &mac, const String &secret, bool enabled, bool autoCloseEnabled,
                     const GarageTimingConfig &timing);
GarageTimingConfig garageTiming();
String garageStatusJson();
String garageRecordsJson();
GarageDoorState garageDoorState();
uint16_t garageDoorStateDurationSeconds();
String garageLastTriggerMethod(uint32_t maxAgeMs);
bool garageRecentlyTriggered(uint32_t windowMs);
const char *garageDoorStateText(GarageDoorState state);
