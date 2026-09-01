#include "logger.h"

#include "app_config.h"
#include "config_pins.h"

static LogEntry logBuffer[LOG_CAPACITY];
static size_t logWriteIndex = 0;
static size_t logCount = 0;
static bool earlyLogEnabled = false;
static bool wifiLogReady = false;

static void earlyLogWriteChar(char c) {
  const uint32_t bitUs = 1000000UL / EARLY_LOG_BAUD;
  digitalWrite(EARLY_LOG_TX_PIN, LOW);
  delayMicroseconds(bitUs);
  for (uint8_t bit = 0; bit < 8; bit++) {
    digitalWrite(EARLY_LOG_TX_PIN, (c & (1 << bit)) ? HIGH : LOW);
    delayMicroseconds(bitUs);
  }
  digitalWrite(EARLY_LOG_TX_PIN, HIGH);
  delayMicroseconds(bitUs);
}

static void earlyLogWriteLine(const String &line) {
  if (!earlyLogEnabled || wifiLogReady) {
    return;
  }
  for (size_t i = 0; i < line.length(); i++) {
    earlyLogWriteChar(line[i]);
  }
  earlyLogWriteChar('\r');
  earlyLogWriteChar('\n');
}

void loggerBegin() {
  pinMode(EARLY_LOG_TX_PIN, OUTPUT);
  digitalWrite(EARLY_LOG_TX_PIN, HIGH);
  earlyLogEnabled = true;
}

void loggerSetWifiReady(bool ready) {
  wifiLogReady = ready;
  if (ready) {
    earlyLogEnabled = false;
    digitalWrite(EARLY_LOG_TX_PIN, HIGH);
  }
}

static void logMessage(const char *level, const char *module, const String &message) {
  LogEntry &entry = logBuffer[logWriteIndex];
  entry.time = millis();
  entry.level = level;
  entry.module = module;
  entry.message = message;
  logWriteIndex = (logWriteIndex + 1) % LOG_CAPACITY;
  if (logCount < LOG_CAPACITY) {
    logCount++;
  }

  String line;
  line.reserve(message.length() + 32);
  line += "[";
  line += entry.time;
  line += "][";
  line += level;
  line += "][";
  line += module;
  line += "] ";
  line += message;
  earlyLogWriteLine(line);
}

void logInfo(const char *module, const String &message) {
  logMessage("INFO", module, message);
}

void logWarn(const char *module, const String &message) {
  logMessage("WARN", module, message);
}

void logError(const char *module, const String &message) {
  logMessage("ERROR", module, message);
}

void logDebug(const char *module, const String &message) {
  logMessage("DEBUG", module, message);
}

String jsonEscape(const String &value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out += c;
    }
  }
  return out;
}

String logsAsJson() {
  String json;
  json.reserve(8192);
  json += F("{\"logs\":[");
  size_t start = (logWriteIndex + LOG_CAPACITY - logCount) % LOG_CAPACITY;
  for (size_t i = 0; i < logCount; i++) {
    size_t idx = (start + i) % LOG_CAPACITY;
    const LogEntry &entry = logBuffer[idx];
    if (i > 0) {
      json += ',';
    }
    json += F("{\"time\":");
    json += entry.time;
    json += F(",\"level\":\"");
    json += jsonEscape(entry.level);
    json += F("\",\"module\":\"");
    json += jsonEscape(entry.module);
    json += F("\",\"message\":\"");
    json += jsonEscape(entry.message);
    json += F("\"}");
  }
  json += F("]}");
  return json;
}
