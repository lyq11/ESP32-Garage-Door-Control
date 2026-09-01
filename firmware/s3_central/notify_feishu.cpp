#include "notify_feishu.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "app_config.h"
#include "garage_espnow.h"
#include "logger.h"
#include "rs485_modbus.h"

static bool notifyEnabled = false;
static String feishuWebhook;
static uint32_t alertCooldownMs = 300000;
static uint32_t maxAlertCount = 10;
static uint32_t lastAnyAlertMs = 0;
static uint32_t lastRs485AlertMs[2] = {0, 0};
static uint32_t lastGarageAlertMs = 0;
static uint32_t lastTickMs = 0;
static uint32_t lastDoorCheckMs = 0;
static uint32_t sentCount = 0;
static uint32_t failCount = 0;
static uint32_t suppressedCount = 0;
static String lastError;
static String lastMessage;
static bool doorMonitorInitialized = false;
static bool doorCycleActive = false;
static bool longOpenAlertSent = false;
static GarageDoorState lastDoorState = GARAGE_DOOR_UNKNOWN;
static uint16_t lastOpenDurationSec = 0;
static String currentOpenMethod;

static String escapeJson(const String &value) {
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
    } else {
      out += c;
    }
  }
  return out;
}

static bool cooldownReady(uint32_t &lastMs) {
  if (lastMs != 0 && millis() - lastMs < alertCooldownMs) {
    return false;
  }
  lastMs = millis();
  return true;
}

bool notifyAlert(const char *module, const String &message) {
  if (!notifyEnabled || !feishuWebhook.length()) {
    return false;
  }
  if (maxAlertCount > 0 && sentCount >= maxAlertCount) {
    lastError = "alert_limit_reached";
    suppressedCount++;
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    lastError = "wifi_not_connected";
    failCount++;
    return false;
  }

  String text = "[centr-reader][" + String(module) + "] " + message;
  String payload = F("{\"msg_type\":\"text\",\"content\":{\"text\":\"");
  payload += escapeJson(text);
  payload += F("\"}}");

  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  HTTPClient http;
  bool https = feishuWebhook.startsWith("https://");
  if (https) {
    secureClient.setInsecure();
    if (!http.begin(secureClient, feishuWebhook)) {
      lastError = "http_begin_failed";
      failCount++;
      return false;
    }
  } else {
    if (!http.begin(plainClient, feishuWebhook)) {
      lastError = "http_begin_failed";
      failCount++;
      return false;
    }
  }

  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  String response = http.getString();
  http.end();

  lastMessage = text;
  if (code >= 200 && code < 300) {
    sentCount++;
    lastAnyAlertMs = millis();
    lastError = "";
    logInfo("FEISHU", "alert sent module=" + String(module));
    return true;
  }

  failCount++;
  lastError = "http_code=" + String(code) + " response=" + response;
  logWarn("FEISHU", "alert failed " + lastError);
  return false;
}

bool notifyConfigure(bool enabled, const String &webhook, uint32_t cooldownSec, uint32_t maxAlerts) {
  Preferences prefs;
  prefs.begin("notify", false);
  prefs.putBool("enabled", enabled);
  prefs.putString("webhook", webhook);
  prefs.putUInt("cooldown", cooldownSec);
  prefs.putUInt("maxAlerts", maxAlerts);
  prefs.end();

  notifyEnabled = enabled;
  feishuWebhook = webhook;
  alertCooldownMs = max<uint32_t>(cooldownSec, 30) * 1000UL;
  maxAlertCount = maxAlerts;
  logInfo("FEISHU", "config saved enabled=" + String(enabled ? "true" : "false"));
  return true;
}

bool notifyTest() {
  return notifyAlert("TEST", "test alert from centr-reader");
}

void notifyBegin() {
  Preferences prefs;
  prefs.begin("notify", true);
  notifyEnabled = prefs.getBool("enabled", false);
  feishuWebhook = prefs.getString("webhook", "");
  alertCooldownMs = max<uint32_t>(prefs.getUInt("cooldown", 300), 30) * 1000UL;
  maxAlertCount = prefs.getUInt("maxAlerts", 10);
  prefs.end();
  logInfo("FEISHU", "notify ready enabled=" + String(notifyEnabled ? "true" : "false"));
}

static void notifyDoorStateEvents() {
  GarageDoorState state = garageDoorState();
  uint16_t stateDurationSec = garageDoorStateDurationSeconds();

  if (!doorMonitorInitialized) {
    doorMonitorInitialized = true;
    lastDoorState = state;
    doorCycleActive = state == GARAGE_DOOR_OPEN || state == GARAGE_DOOR_MOVING ||
                      state == GARAGE_DOOR_STOPPED || state == GARAGE_DOOR_TIMEOUT;
    if (state == GARAGE_DOOR_OPEN) {
      lastOpenDurationSec = stateDurationSec;
      currentOpenMethod = garageLastTriggerMethod(GARAGE_OPEN_METHOD_WINDOW_MS);
      notifyAlert("GARAGE", "中控启动时检测到车库门已打开；开门方式=" + currentOpenMethod);
    }
    return;
  }

  if (state == GARAGE_DOOR_OPEN) {
    lastOpenDurationSec = stateDurationSec;
    if (lastDoorState != GARAGE_DOOR_OPEN) {
      doorCycleActive = true;
      longOpenAlertSent = false;
      currentOpenMethod = garageLastTriggerMethod(GARAGE_OPEN_METHOD_WINDOW_MS);
      notifyAlert("GARAGE", "车库门已打开；开门方式=" + currentOpenMethod);
      logInfo("GARAGE", "door opened notification method=" + currentOpenMethod);
    }
    if (!longOpenAlertSent && stateDurationSec >= DOOR_OPEN_LONG_ALERT_SECONDS) {
      String message = "车库门长时间未关闭；已打开=" + String(stateDurationSec) +
                       "秒；开门方式=" + currentOpenMethod;
      notifyAlert("GARAGE", message);
      logWarn("GARAGE", "door open too long seconds=" + String(stateDurationSec));
      longOpenAlertSent = true;
    }
  } else if (state == GARAGE_DOOR_MOVING || state == GARAGE_DOOR_STOPPED ||
             state == GARAGE_DOOR_TIMEOUT) {
    if (lastDoorState == GARAGE_DOOR_CLOSED) {
      doorCycleActive = true;
      longOpenAlertSent = false;
      currentOpenMethod = garageLastTriggerMethod(GARAGE_OPEN_METHOD_WINDOW_MS);
    }
    if (state == GARAGE_DOOR_TIMEOUT && lastDoorState != GARAGE_DOOR_TIMEOUT) {
      String direction = garageMotionDirectionText();
      if (direction == "OPENING") direction = "开门";
      else if (direction == "CLOSING") direction = "关门";
      else direction = "未知";
      notifyAlert("GARAGE", "车库门行程超时，未到达上限或下限；方向=" + direction);
      logWarn("GARAGE", "door travel timeout");
    }
  } else if (state == GARAGE_DOOR_CLOSED) {
    if (doorCycleActive) {
      String message = F("车库门已关闭");
      if (lastOpenDurationSec > 0) {
        message += F("；本次打开约=");
        message += lastOpenDurationSec;
        message += F("秒");
      }
      notifyAlert("GARAGE", message);
      logInfo("GARAGE", "door closed notification");
    }
    doorCycleActive = false;
    longOpenAlertSent = false;
    lastOpenDurationSec = 0;
    currentOpenMethod = "";
  }

  lastDoorState = state;
}

void notifyTick() {
  if (millis() - lastDoorCheckMs >= 1000) {
    lastDoorCheckMs = millis();
    notifyDoorStateEvents();
  }
  if (millis() - lastTickMs < 10000) {
    return;
  }
  lastTickMs = millis();

  for (uint8_t i = 0; i < 2; i++) {
    Rs485Port &port = rs485Ports[i];
    if (!rs485PortActiveForDoorMode(i)) {
      continue;
    }
    if (!port.lastReadOk && port.lastReadMs > 0 && cooldownReady(lastRs485AlertMs[i])) {
      notifyAlert(port.name, "sensor read failed, failCount=" + String(port.failCount));
    }
  }

  GarageDoorState state = garageDoorState();
  if (state == GARAGE_DOOR_CONFLICT && cooldownReady(lastGarageAlertMs)) {
    notifyAlert("GARAGE", "door sensor conflict: upper and lower sensors both active");
  }
}

String notifyStatusJson() {
  String json = F("{\"enabled\":");
  json += notifyEnabled ? F("true") : F("false");
  json += F(",\"webhookConfigured\":");
  json += feishuWebhook.length() ? F("true") : F("false");
  json += F(",\"cooldownSec\":");
  json += alertCooldownMs / 1000UL;
  json += F(",\"maxAlerts\":");
  json += maxAlertCount;
  json += F(",\"sentCount\":");
  json += sentCount;
  json += F(",\"failCount\":");
  json += failCount;
  json += F(",\"suppressedCount\":");
  json += suppressedCount;
  json += F(",\"lastAlertMs\":");
  json += lastAnyAlertMs;
  json += F(",\"lastError\":\"");
  json += escapeJson(lastError);
  json += F("\",\"lastMessage\":\"");
  json += escapeJson(lastMessage);
  json += F("\"}");
  return json;
}
