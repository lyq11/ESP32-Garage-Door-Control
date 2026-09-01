#include "notify_feishu.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

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
static uint32_t sentCount = 0;
static uint32_t failCount = 0;
static uint32_t suppressedCount = 0;
static String lastError;
static String lastMessage;

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

void notifyTick() {
  if (millis() - lastTickMs < 10000) {
    return;
  }
  lastTickMs = millis();

  for (uint8_t i = 0; i < 2; i++) {
    Rs485Port &port = rs485Ports[i];
    if (!port.enabled) {
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
