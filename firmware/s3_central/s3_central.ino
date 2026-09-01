#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "app_config.h"
#include "face_fp001.h"
#include "face_wake_input.h"
#include "garage_espnow.h"
#include "logger.h"
#include "notify_feishu.h"
#include "ota_update.h"
#include "rs485_modbus.h"
#include "time_service.h"
#include "web_api.h"

DNSServer dnsServer;
Preferences preferences;

bool setupApRunning = false;
bool webReady = false;
bool wifiReady = false;
String lastCommandResult;

static String savedSsid;
static String savedPass;
static uint32_t lastWifiCheckMs = 0;
static uint32_t lastWifiReconnectMs = 0;
static uint32_t wifiReconnectCount = 0;
static wl_status_t lastWifiStatus = WL_IDLE_STATUS;
static bool watchdogReady = false;

static const char *resetReasonText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWER_ON";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK_WATCHDOG";
    case ESP_RST_WDT: return "OTHER_WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default: return "UNKNOWN";
  }
}

static void watchdogBegin() {
  const esp_task_wdt_config_t config = {
      .timeout_ms = WATCHDOG_TIMEOUT_MS,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };

  esp_err_t err = esp_task_wdt_reconfigure(&config);
  if (err == ESP_ERR_INVALID_STATE) {
    err = esp_task_wdt_init(&config);
  }
  if (err != ESP_OK) {
    logError("WATCHDOG", "init failed err=" + String((int)err));
    return;
  }

  err = esp_task_wdt_status(nullptr);
  if (err == ESP_ERR_NOT_FOUND) {
    err = esp_task_wdt_add(nullptr);
  }
  if (err != ESP_OK) {
    logError("WATCHDOG", "loop task registration failed err=" + String((int)err));
    return;
  }

  watchdogReady = true;
  logInfo("WATCHDOG", "loop task armed timeoutMs=" + String(WATCHDOG_TIMEOUT_MS));
}

uint32_t wifiReconnectAttempts() {
  return wifiReconnectCount;
}

int wifiRssi() {
  return WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
}

static void startSetupAp() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD);
  dnsServer.start(53, "*", WiFi.softAPIP());
  setupApRunning = true;
  wifiReady = true;
  logWarn("WIFI_FAIL", "setup AP ip=" + WiFi.softAPIP().toString());
}

static void connectWifiOrStartAp() {
  WiFi.setHostname(HOSTNAME);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  preferences.begin("wifi", true);
  savedSsid = preferences.getString("ssid", "");
  savedPass = preferences.getString("pass", "");
  preferences.end();

  if (savedSsid.length() == 0) {
    logWarn("WIFI_START", "no saved credentials");
    startSetupAp();
    return;
  }

  setupApRunning = false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  logInfo("WIFI_START", "connecting ssid=" + savedSsid);
  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000) {
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiReady = true;
    lastWifiStatus = WL_CONNECTED;
    logInfo("WIFI_OK", "connected, ip=" + WiFi.localIP().toString());
  } else {
    startSetupAp();
  }
}

static void maintainWifi() {
  if (savedSsid.length() == 0) {
    return;
  }
  if (millis() - lastWifiCheckMs < 1000) {
    return;
  }
  lastWifiCheckMs = millis();

  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    bool justReconnected = lastWifiStatus != WL_CONNECTED;
    if (justReconnected) {
      wifiReady = true;
      logInfo("WIFI_OK", "reconnected, ip=" + WiFi.localIP().toString() +
                         " rssi=" + String(WiFi.RSSI()));
    }
    if (setupApRunning) {
      dnsServer.stop();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      setupApRunning = false;
      logInfo("WIFI", "setup AP stopped after STA reconnect");
    }
    if (justReconnected) {
      garageRefreshPeerAfterWifiReconnect();
    }
    lastWifiStatus = status;
    return;
  }

  if (lastWifiStatus == WL_CONNECTED) {
    wifiReady = setupApRunning;
    logWarn("WIFI", "disconnected status=" + String((int)status));
  }
  lastWifiStatus = status;

  uint32_t reconnectIntervalMs = setupApRunning ? 30000UL : 10000UL;
  if (millis() - lastWifiReconnectMs >= reconnectIntervalMs) {
    lastWifiReconnectMs = millis();
    wifiReconnectCount++;
    logWarn("WIFI", "reconnect attempt=" + String(wifiReconnectCount) +
                    " status=" + String((int)status) +
                    " intervalMs=" + String(reconnectIntervalMs));
    WiFi.disconnect(false);
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  }
}

static void serviceNetwork() {
  maintainWifi();
  if (setupApRunning) {
    dnsServer.processNextRequest();
  }
  webApiHandleClient();
  otaHandle();
}

void setup() {
  loggerBegin();
  logInfo("BOOT", "system boot");
  esp_reset_reason_t resetReason = esp_reset_reason();
  logInfo("BOOT", "reset reason=" + String(resetReasonText(resetReason)) +
                      " code=" + String((int)resetReason));
  if (timeServiceConsumeScheduledRestartMarker()) {
    logInfo("BOOT", "previous restart=scheduled_daily_restart");
  }

  connectWifiOrStartAp();
  timeServiceBegin();
  webApiBegin();
  otaBegin();

  garageBegin();
  notifyBegin();
  faceSetVerifySuccessCallback(garageOnFaceSuccess);
  faceBegin();
  rs485Begin();
  faceWakeInputBegin();
  watchdogBegin();
}

void loop() {
  serviceNetwork();
  timeServiceTick();
  faceWakeInputTick();
  faceTick();
  rs485Poll();
  garageTick();
  notifyTick();
  if (watchdogReady) {
    esp_task_wdt_reset();
  }
}
