#include "face_wake_input.h"

#include <Arduino.h>

#include "app_config.h"
#include "config_pins.h"
#include "face_fp001.h"
#include "garage_espnow.h"
#include "logger.h"
#include "rs485_modbus.h"

static volatile bool interruptPending = false;
static uint32_t lowDetectedMs = 0;
static uint32_t lastWakeMs = 0;
static uint32_t lastEventMs = 0;
static uint32_t requestCount = 0;
static uint32_t acceptedCount = 0;
static bool waitingForDebounce = false;
static const char *lastResult = "NONE";

static void ARDUINO_ISR_ATTR onWakeInputFalling() {
  interruptPending = true;
}

static bool inputCanWakeFace() {
  if (faceWakeActive()) {
    lastResult = "FACE_ACTIVE";
    logInfo("FACE", "IO8 wake ignored, face window active");
    return false;
  }

  if (garageRecentlyTriggered(GARAGE_TRIGGER_FACE_WAKE_INHIBIT_MS)) {
    lastResult = "RECENT_GARAGE_TRIGGER";
    logInfo("FACE", "IO8 wake ignored, recent garage trigger");
    return false;
  }

  rs485RefreshDoorState();
  GarageDoorState state = garageDoorState();
  if (state != GARAGE_DOOR_CLOSED) {
    lastResult = "DOOR_NOT_CLOSED";
    logInfo("FACE", "IO8 wake ignored, door state=" + String(garageDoorStateText(state)));
    return false;
  }
  return true;
}

void faceWakeInputBegin() {
  pinMode(FACE_WAKE_INPUT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FACE_WAKE_INPUT_PIN), onWakeInputFalling, FALLING);
  interruptPending = digitalRead(FACE_WAKE_INPUT_PIN) == LOW;
  logInfo("FACE", "wake input ready pin=IO" + String(FACE_WAKE_INPUT_PIN) + " active=LOW");
}

void faceWakeInputTick() {
  if (interruptPending) {
    noInterrupts();
    interruptPending = false;
    interrupts();
    requestCount++;
    lowDetectedMs = millis();
    waitingForDebounce = true;
  }

  if (!waitingForDebounce || millis() - lowDetectedMs < FACE_WAKE_INPUT_DEBOUNCE_MS) {
    return;
  }
  waitingForDebounce = false;
  lastEventMs = millis();

  if (digitalRead(FACE_WAKE_INPUT_PIN) != LOW) {
    lastResult = "DEBOUNCE_REJECTED";
    return;
  }
  uint32_t now = millis();
  if (lastWakeMs != 0 && now - lastWakeMs < FACE_WAKE_INPUT_COOLDOWN_MS) {
    lastResult = "COOLDOWN";
    logInfo("FACE", "IO8 wake ignored by cooldown");
    return;
  }
  if (!inputCanWakeFace()) {
    return;
  }

  lastWakeMs = now;
  acceptedCount++;
  lastResult = "ACCEPTED";
  logInfo("FACE", "IO8 low wake accepted");
  faceWakeAutoVerify(FACE_WAKE_WINDOW_MS);
}

String faceWakeInputStatusJson() {
  int level = digitalRead(FACE_WAKE_INPUT_PIN);
  String json = F("{\"pin\":");
  json += FACE_WAKE_INPUT_PIN;
  json += F(",\"activeLow\":true,\"level\":");
  json += level;
  json += F(",\"active\":");
  json += level == LOW ? F("true") : F("false");
  json += F(",\"requestCount\":");
  json += requestCount;
  json += F(",\"acceptedCount\":");
  json += acceptedCount;
  json += F(",\"lastEventMs\":");
  json += lastEventMs;
  json += F(",\"lastResult\":\"");
  json += lastResult;
  json += F("\",\"debounceMs\":");
  json += FACE_WAKE_INPUT_DEBOUNCE_MS;
  json += F(",\"cooldownMs\":");
  json += FACE_WAKE_INPUT_COOLDOWN_MS;
  json += F("}");
  return json;
}
