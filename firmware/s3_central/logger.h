#pragma once

#include <Arduino.h>

struct LogEntry {
  uint32_t time;
  String level;
  String module;
  String message;
};

void loggerBegin();
void loggerSetWifiReady(bool ready);
void logInfo(const char *module, const String &message);
void logWarn(const char *module, const String &message);
void logError(const char *module, const String &message);
void logDebug(const char *module, const String &message);
String logsAsJson();
String jsonEscape(const String &value);
