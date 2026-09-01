#include "web_api.h"

#include <WebServer.h>
#include <WiFi.h>
#include <ctype.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

#include "app_config.h"
#include "face_fp001.h"
#include "garage_espnow.h"
#include "logger.h"
#include "notify_feishu.h"
#include "rs485_modbus.h"
#include "time_service.h"

static WebServer server(80);

extern uint32_t wifiReconnectAttempts();
extern int wifiRssi();

static void sendJsonCode(int code, const String &json) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(code, "application/json; charset=utf-8", json);
}

static void sendCorsOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204, "text/plain", "");
}

static void sendOk(const char *message) {
  sendJsonCode(200, String("{\"ok\":true,\"message\":\"") + message + "\"}");
}

static void sendBusy() {
  sendJsonCode(409, F("{\"ok\":false,\"error\":\"busy\"}"));
}

static String bodyArg(const String &key) {
  if (server.hasArg(key)) {
    return server.arg(key);
  }
  String body = server.arg("plain");
  String quoted = "\"" + key + "\"";
  int keyPos = body.indexOf(quoted);
  if (keyPos < 0) return "";
  int colon = body.indexOf(':', keyPos + quoted.length());
  if (colon < 0) return "";
  int start = colon + 1;
  while (start < body.length() && isspace((unsigned char)body[start])) start++;
  if (start >= body.length()) return "";
  if (body[start] == '"') {
    int end = body.indexOf('"', start + 1);
    return end > start ? body.substring(start + 1, end) : "";
  }
  int end = start;
  while (end < body.length() && body[end] != ',' && body[end] != '}') end++;
  String value = body.substring(start, end);
  value.trim();
  return value;
}

static bool bodyBool(const String &key, bool fallback) {
  String v = bodyArg(key);
  if (!v.length()) return fallback;
  v.toLowerCase();
  return v == "true" || v == "1" || v == "on";
}

static int bodyInt(const String &key, int fallback) {
  String v = bodyArg(key);
  return v.length() ? v.toInt() : fallback;
}

static uint8_t ledColorFromString(String color) {
  color.toLowerCase();
  if (color == "red") return 1;
  if (color == "white") return 2;
  return 0;
}

String systemStatusJson() {
  String json = F("{\"uptimeMs\":");
  json += millis();
  json += F(",\"wifiReady\":");
  json += wifiReady ? F("true") : F("false");
  json += F(",\"webReady\":");
  json += webReady ? F("true") : F("false");
  json += F(",\"wifiMode\":\"");
  json += setupApRunning ? F("AP") : F("STA");
  json += F("\",\"ip\":\"");
  json += (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  json += F("\",\"hostname\":\"");
  json += HOSTNAME;
  json += F("\",\"wifiStatus\":");
  json += (int)WiFi.status();
  json += F(",\"ssid\":\"");
  json += jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String(""));
  json += F("\",\"wifiChannel\":");
  json += WiFi.status() == WL_CONNECTED ? WiFi.channel() : 0;
  json += F(",\"rssi\":");
  const int rssi = wifiRssi();
  json += rssi;
  json += F(",\"signalPercent\":");
  json += WiFi.status() == WL_CONNECTED ? constrain(2 * (rssi + 100), 0, 100) : 0;
  json += F(",\"reconnectAttempts\":");
  json += wifiReconnectAttempts();
  json += F(",\"freeHeap\":");
  json += ESP.getFreeHeap();
  json += F(",\"minFreeHeap\":");
  json += ESP.getMinFreeHeap();
  json += F(",\"largestFreeBlock\":");
  json += heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  json += F(",\"timeReady\":");
  json += timeServiceReady() ? F("true") : F("false");
  json += F(",\"localTime\":\"");
  json += timeServiceLocalTime();
  json += F("\",\"nextScheduledRestart\":\"");
  json += timeServiceNextRestart();
  json += F("\",\"ota\":\"arduino_ota\"}");
  return json;
}

static void handleStatus() {
  sendJsonCode(200, systemStatusJson());
}

static void handleLogs() {
  sendJsonCode(200, logsAsJson());
}

static void handleFaceVerify() {
  if (!faceCommandAllowed()) return sendBusy();
  faceVerify((uint8_t)constrain(bodyInt("timeout", 10), 1, 60));
  sendOk("face_verify_started");
}

static void handleFaceEnroll() {
  if (!faceCommandAllowed()) return sendBusy();
  String name = bodyArg("name");
  if (!name.length()) name = "test001";
  faceEnrollSingleFace(name.c_str(), bodyBool("admin", false), (uint8_t)constrain(bodyInt("timeout", 20), 1, 60));
  sendOk("face_enroll_started");
}

static void handleFaceEnroll5() {
  if (!faceCommandAllowed()) return sendBusy();
  String name = bodyArg("name");
  if (!name.length()) name = "test001";
  faceStartEnrollSequence(name.c_str(), bodyBool("admin", false), (uint8_t)constrain(bodyInt("timeout", 20), 1, 60));
  sendOk("face_5step_enroll_started");
}

static void handleFaceEnrollHand() {
  if (!faceCommandAllowed()) return sendBusy();
  String name = bodyArg("name");
  if (!name.length()) name = "hand001";
  int timeout = constrain(bodyInt("timeout", 120), 1, 255);
  if (timeout < 120) timeout = 120;
  faceEnrollHand(name.c_str(), bodyBool("admin", false), (uint8_t)timeout);
  sendOk("hand_enroll_started");
}

static void handleFaceReset() {
  faceReset();
  sendOk("face_reset_sent");
}

static void handleFaceLed() {
  faceLedControl(ledColorFromString(bodyArg("color")), bodyBool("on", true));
  sendOk("face_led_sent");
}

static void handleFaceAutoStart() {
  faceAutoVerifySet(true);
  sendOk("face_auto_verify_enabled");
}

static void handleFaceAutoStop() {
  faceAutoVerifySet(false);
  sendOk("face_auto_verify_disabled");
}

static void handleFaceUsers() {
  if (server.hasArg("cached")) {
    sendJsonCode(200, faceUsersJson(false));
    return;
  }
  if (!faceCommandAllowed()) return sendBusy();
  faceGetAllUserIds();
  sendJsonCode(200, faceUsersJson(true));
}

static void handleFaceDeleteUser() {
  if (!faceCommandAllowed()) return sendBusy();
  int rawId = bodyInt("id", -1);
  if (rawId < 0 || rawId > 65535) {
    sendJsonCode(400, F("{\"ok\":false,\"error\":\"bad_user_id\"}"));
    return;
  }
  faceDeleteUser((uint16_t)rawId);
  sendOk("face_delete_user_sent");
}

static void handleFaceDeleteAll() {
  if (!faceCommandAllowed()) return sendBusy();
  faceDeleteAllUsers();
  sendOk("face_delete_all_sent");
}

static void handleRs485Write() {
  uint8_t portIndex = (uint8_t)bodyInt("port", 0);
  uint8_t addr = (uint8_t)bodyInt("addr", 2);
  uint16_t reg = (uint16_t)bodyInt("reg", 0);
  uint16_t value = (uint16_t)bodyInt("value", 0);
  if (portIndex >= 2 || addr < 1 || addr > 247) {
    sendJsonCode(400, F("{\"ok\":false,\"error\":\"bad_rs485_params\"}"));
    return;
  }
  bool ok = rs485WriteSingleRegister(portIndex, addr, reg, value, lastCommandResult);
  String json = F("{\"ok\":");
  json += ok ? F("true") : F("false");
  json += F(",\"result\":\"");
  json += jsonEscape(lastCommandResult);
  json += F("\"}");
  sendJsonCode(200, json);
}

static void handleRs485Read() {
  uint8_t portIndex = (uint8_t)bodyInt("port", 0);
  uint8_t functionCode = (uint8_t)bodyInt("function", 3);
  uint16_t startReg = (uint16_t)bodyInt("startReg", 0);
  uint16_t count = (uint16_t)constrain(bodyInt("count", 1), 1, 16);
  if (portIndex >= 2 || (functionCode != 3 && functionCode != 4)) {
    sendJsonCode(400, F("{\"ok\":false,\"error\":\"bad_rs485_read_params\"}"));
    return;
  }
  uint16_t values[16] = {0};
  bool ok = rs485ReadRegisters(portIndex, functionCode, startReg, count, values, lastCommandResult);
  String json = F("{\"ok\":");
  json += ok ? F("true") : F("false");
  json += F(",\"result\":\"");
  json += jsonEscape(lastCommandResult);
  json += F("\",\"values\":[");
  for (uint16_t i = 0; i < count; i++) {
    if (i > 0) json += ',';
    json += values[i];
  }
  json += F("]}");
  sendJsonCode(200, json);
}

static void handleRs485ConfigWrite() {
  uint8_t portIndex = (uint8_t)bodyInt("port", 0);
  if (portIndex >= 2) {
    sendJsonCode(400, F("{\"ok\":false,\"error\":\"bad_port\"}"));
    return;
  }
  uint8_t nodeAddr = (uint8_t)constrain(bodyInt("addr", rs485Ports[portIndex].nodeAddr), 1, 247);
  uint16_t values[SENSOR_CONFIG_REG_COUNT];
  values[0] = (uint16_t)constrain(bodyInt("modbusAddr", rs485Ports[portIndex].nodeAddr), 1, 247);
  values[1] = (uint16_t)constrain(bodyInt("magHoldMs", 3000), 100, 30000);
  values[2] = (uint16_t)constrain(bodyInt("magReleaseMs", 300), 50, 5000);
  values[3] = (uint16_t)constrain(bodyInt("vibrationWindowMs", 500), 50, 5000);
  values[4] = (uint16_t)constrain(bodyInt("vibrationThreshold", 3), 1, 100);
  values[5] = (uint16_t)constrain(bodyInt("runHoldMs", 1500), 100, 10000);
  values[6] = bodyBool("ledEnabled", true) ? 1 : 0;

  bool ok = rs485WriteMultipleRegisters(portIndex, nodeAddr, SENSOR_CONFIG_START_REG,
                                        values, SENSOR_CONFIG_REG_COUNT, lastCommandResult);
  bool saved = false;
  if (ok) {
    saved = rs485SaveSensorConfig(portIndex, nodeAddr, values, SENSOR_CONFIG_REG_COUNT);
  }
  String json = F("{\"ok\":");
  json += ok ? F("true") : F("false");
  json += F(",\"saved\":");
  json += saved ? F("true") : F("false");
  json += F(",\"result\":\"");
  json += jsonEscape(lastCommandResult);
  json += F("\"}");
  sendJsonCode(200, json);
}

static void handleDoorLimitConfig() {
  String mode = bodyArg("mode");
  uint8_t singlePort = (uint8_t)bodyInt("singlePort", 1);
  uint16_t travelTimeoutSeconds = (uint16_t)bodyInt("travelTimeoutSeconds", 60);
  if (!rs485ConfigureDoorLimits(mode, singlePort, travelTimeoutSeconds)) {
    sendJsonCode(400, F("{\"ok\":false,\"error\":\"bad_door_limit_config\"}"));
    return;
  }
  String json = F("{\"ok\":true,\"message\":\"door_limit_config_saved\",\"config\":");
  json += rs485DoorLimitConfigJson();
  json += '}';
  sendJsonCode(200, json);
}

static void handleGarageTrigger() {
  bool ok = garageTrigger(1, "manual_api", bodyInt("userId", -1));
  sendJsonCode(200, ok ? F("{\"ok\":true,\"message\":\"garage_trigger_sent\"}")
                       : F("{\"ok\":false,\"error\":\"garage_trigger_failed\"}"));
}

static void handleGarageConfig() {
  String mac = bodyArg("mac");
  String secret = bodyArg("secret");
  bool enabled = bodyBool("enabled", false);
  bool autoClose = bodyBool("autoCloseEnabled", true);
  GarageTimingConfig timing = garageTiming();
  timing.sendCooldownMs = (uint32_t)constrain(bodyInt("sendCooldownMs", timing.sendCooldownMs), 1000, 60000);
  timing.openStableSeconds = (uint16_t)constrain(bodyInt("openStableSeconds", timing.openStableSeconds), 5, 600);
  timing.openRecheckMs = (uint32_t)constrain(bodyInt("openRecheckMs", timing.openRecheckMs), 1000, 60000);
  timing.movingRecheckMs = (uint32_t)constrain(bodyInt("movingRecheckMs", timing.movingRecheckMs), 1000, 120000);
  timing.postTriggerRecheckMs = (uint32_t)constrain(bodyInt("postTriggerRecheckMs", timing.postTriggerRecheckMs), 10000, 180000);
  timing.maxAttemptRecheckMs = (uint32_t)constrain(bodyInt("maxAttemptRecheckMs", timing.maxAttemptRecheckMs), 10000, 300000);
  timing.maxAutoCloseAttempts = (uint8_t)constrain(bodyInt("maxAutoCloseAttempts", timing.maxAutoCloseAttempts), 1, 10);
  if (!garageConfigure(mac, secret, enabled, autoClose, timing)) {
    sendJsonCode(400, F("{\"ok\":false,\"error\":\"bad_garage_config\"}"));
    return;
  }
  sendOk("garage_config_saved");
}

static void handleNotifyConfig() {
  bool enabled = bodyBool("enabled", false);
  String webhook = bodyArg("webhook");
  uint32_t cooldown = (uint32_t)bodyInt("cooldownSec", 300);
  uint32_t maxAlerts = (uint32_t)bodyInt("maxAlerts", 10);
  notifyConfigure(enabled, webhook, cooldown, maxAlerts);
  sendOk("notify_config_saved");
}

static void handleNotifyTest() {
  bool ok = notifyTest();
  sendJsonCode(200, ok ? F("{\"ok\":true,\"message\":\"notify_test_sent\"}")
                       : F("{\"ok\":false,\"error\":\"notify_test_failed\"}"));
}

static void handleWifiSave() {
  String ssid = bodyArg("ssid");
  String pass = bodyArg("pass");
  ssid.trim();
  if (!ssid.length()) {
    sendJsonCode(400, F("{\"ok\":false,\"error\":\"ssid_required\"}"));
    return;
  }
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  logInfo("WIFI", "credentials saved, rebooting");
  sendOk("wifi_saved_rebooting");
  delay(500);
  ESP.restart();
}

static void handleWifiClear() {
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  logWarn("WIFI", "credentials cleared, rebooting");
  sendOk("wifi_cleared_rebooting");
  delay(500);
  ESP.restart();
}

static void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) {
    sendCorsOptions();
    return;
  }
  sendJsonCode(404, F("{\"ok\":false,\"error\":\"not_found\"}"));
}

void webApiBegin() {
  server.on("/", HTTP_GET, []() {
    sendJsonCode(200, F("{\"ok\":true,\"device\":\"centr-reader\",\"ui\":\"pc-dashboard\"}"));
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/logs", HTTP_GET, handleLogs);
  server.on("/api/face/status", HTTP_GET, []() { sendJsonCode(200, faceStatusJson()); });
  server.on("/api/face/verify", HTTP_POST, handleFaceVerify);
  server.on("/api/face/enroll", HTTP_POST, handleFaceEnroll);
  server.on("/api/face/enroll5", HTTP_POST, handleFaceEnroll5);
  server.on("/api/face/enroll-hand", HTTP_POST, handleFaceEnrollHand);
  server.on("/api/face/reset", HTTP_POST, handleFaceReset);
  server.on("/api/face/led", HTTP_POST, handleFaceLed);
  server.on("/api/face/auto/start", HTTP_POST, handleFaceAutoStart);
  server.on("/api/face/auto/stop", HTTP_POST, handleFaceAutoStop);
  server.on("/api/face/users", HTTP_GET, handleFaceUsers);
  server.on("/api/face/users/delete", HTTP_POST, handleFaceDeleteUser);
  server.on("/api/face/users/delete-all", HTTP_POST, handleFaceDeleteAll);
  server.on("/api/rs485/1/status", HTTP_GET, []() { sendJsonCode(200, rs485StatusJson(0)); });
  server.on("/api/rs485/2/status", HTTP_GET, []() { sendJsonCode(200, rs485StatusJson(1)); });
  server.on("/api/rs485/1/config", HTTP_GET, []() { sendJsonCode(200, rs485ConfigJson(0)); });
  server.on("/api/rs485/2/config", HTTP_GET, []() { sendJsonCode(200, rs485ConfigJson(1)); });
  server.on("/api/rs485/read", HTTP_POST, handleRs485Read);
  server.on("/api/rs485/write", HTTP_POST, handleRs485Write);
  server.on("/api/rs485/config", HTTP_POST, handleRs485ConfigWrite);
  server.on("/api/rs485/door-config", HTTP_GET, []() { sendJsonCode(200, rs485DoorLimitConfigJson()); });
  server.on("/api/rs485/door-config", HTTP_POST, handleDoorLimitConfig);
  server.on("/api/garage/status", HTTP_GET, []() { sendJsonCode(200, garageStatusJson()); });
  server.on("/api/garage/records", HTTP_GET, []() { sendJsonCode(200, garageRecordsJson()); });
  server.on("/api/garage/trigger", HTTP_POST, handleGarageTrigger);
  server.on("/api/garage/config", HTTP_POST, handleGarageConfig);
  server.on("/api/notify/status", HTTP_GET, []() { sendJsonCode(200, notifyStatusJson()); });
  server.on("/api/notify/config", HTTP_POST, handleNotifyConfig);
  server.on("/api/notify/test", HTTP_POST, handleNotifyTest);
  server.on("/api/wifi/save", HTTP_POST, handleWifiSave);
  server.on("/api/wifi/clear", HTTP_POST, handleWifiClear);
  server.onNotFound(handleNotFound);
  server.begin();
  webReady = true;
  logInfo("WEB_READY", "api server started");
  loggerSetWifiReady(true);
}

void webApiHandleClient() {
  server.handleClient();
}
