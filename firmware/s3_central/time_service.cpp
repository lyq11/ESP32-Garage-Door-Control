#include "time_service.h"

#include <time.h>

#include "app_config.h"
#include "logger.h"

static const time_t MIN_VALID_EPOCH = 1704067200;  // 2024-01-01 00:00:00 UTC
static const uint32_t SCHEDULED_RESTART_MARKER = 0x43525431;  // CRT1

RTC_DATA_ATTR static uint32_t rtcRestartMarker = 0;

static bool clockReady = false;
static time_t nextRestartEpoch = 0;
static uint32_t lastTickMs = 0;

static String formatLocalTime(time_t value) {
  if (value < MIN_VALID_EPOCH) {
    return "";
  }
  struct tm local = {};
  localtime_r(&value, &local);
  char text[24];
  if (strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local) == 0) {
    return "";
  }
  return String(text);
}

static void scheduleNextRestart(time_t now) {
  struct tm target = {};
  localtime_r(&now, &target);
  target.tm_hour = SCHEDULED_RESTART_HOUR;
  target.tm_min = SCHEDULED_RESTART_MINUTE;
  target.tm_sec = 0;
  time_t candidate = mktime(&target);
  if (candidate <= now) {
    target.tm_mday++;
    candidate = mktime(&target);
  }
  nextRestartEpoch = candidate;
  logInfo("TIME", "next scheduled restart=" + formatLocalTime(nextRestartEpoch));
}

void timeServiceBegin() {
  configTzTime(NTP_TIMEZONE, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  logInfo("TIME", "NTP started timezone=" + String(NTP_TIMEZONE));
}

void timeServiceTick() {
  uint32_t nowMs = millis();
  if (nowMs - lastTickMs < 1000) {
    return;
  }
  lastTickMs = nowMs;

  time_t now = time(nullptr);
  if (now < MIN_VALID_EPOCH) {
    return;
  }

  if (!clockReady) {
    clockReady = true;
    logInfo("TIME", "NTP synchronized localTime=" + formatLocalTime(now));
    scheduleNextRestart(now);
  } else if (nextRestartEpoch == 0) {
    scheduleNextRestart(now);
  }

  if (nextRestartEpoch != 0 && now >= nextRestartEpoch) {
    rtcRestartMarker = SCHEDULED_RESTART_MARKER;
    logWarn("TIME", "scheduled daily restart now=" + formatLocalTime(now));
    delay(100);
    ESP.restart();
  }
}

bool timeServiceReady() {
  return clockReady;
}

String timeServiceLocalTime() {
  return clockReady ? formatLocalTime(time(nullptr)) : "";
}

String timeServiceNextRestart() {
  return formatLocalTime(nextRestartEpoch);
}

bool timeServiceConsumeScheduledRestartMarker() {
  if (rtcRestartMarker != SCHEDULED_RESTART_MARKER) {
    return false;
  }
  rtcRestartMarker = 0;
  return true;
}
