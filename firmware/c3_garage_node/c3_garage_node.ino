#include <Arduino.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_task_wdt.h>
#include <freertos/queue.h>

static const char *HOSTNAME = "garage-c3";
static const char *SETUP_AP_SSID = "garage-c3-setup";
static const char *SETUP_AP_PASSWORD = "12345678";
static const int RELAY_PIN = 10;
static const bool RELAY_ACTIVE_LEVEL = HIGH;
static const uint32_t DEFAULT_PULSE_MS = 700;
static const uint8_t DEFAULT_ESPNOW_CHANNEL = 11;
static const uint32_t COMMAND_COOLDOWN_MS = 5000;
static const uint32_t WIFI_RECONNECT_INTERVAL_MS = 10000;
static const uint32_t WIFI_AP_FALLBACK_MS = 60000;
static const uint32_t GARAGE_MAGIC = 0x47415231; // GAR1
static const uint8_t GARAGE_CMD_TRIGGER = 1;
static const char *AUTH_USER = "admin";
static const char *DEFAULT_ADMIN_PASSWORD = "12345678";

void logLine(const String &message) {
  Serial.print("[");
  Serial.print(millis());
  Serial.print("][GARAGE-C3] ");
  Serial.println(message);
}

struct GarageCommandPacket {
  uint32_t magic;
  uint8_t version;
  uint8_t command;
  uint16_t userId;
  uint32_t seq;
  uint32_t millisStamp;
  uint32_t signature;
};

struct GarageAckPacket {
  uint32_t magic;
  uint32_t seq;
  uint8_t ok;
  uint8_t state;
};

struct PendingGarageCommand {
  uint8_t mac[6];
  int len;
  GarageCommandPacket packet;
};

static_assert(sizeof(GarageCommandPacket) == 20, "Unexpected command packet size");
static_assert(sizeof(GarageAckPacket) == 12, "Unexpected ack packet size");

uint32_t packetSignature(const GarageCommandPacket &packet);
bool handleGarageCommand(const uint8_t *mac, const GarageCommandPacket &packet);

Preferences preferences;
WebServer server(80);
QueueHandle_t garageCommandQueue = nullptr;

bool wifiReady = false;
bool setupApRunning = false;
bool espNowReady = false;
bool relayActive = false;
bool commandEnabled = true;
uint32_t relayPulseMs = DEFAULT_PULSE_MS;
uint8_t espNowChannel = DEFAULT_ESPNOW_CHANNEL;
uint8_t fallbackChannel = DEFAULT_ESPNOW_CHANNEL;
String s3BaseUrl;
uint32_t relayPulseStartMs = 0;
uint32_t lastCommandMs = 0;
uint32_t lastSeq = 0;
uint32_t acceptedCount = 0;
uint32_t rejectedCount = 0;
uint32_t lastRxMs = 0;
uint32_t lastAckMs = 0;
bool lastAckSendOk = false;
int lastAckSendErr = 0;
uint16_t lastUserId = 0xFFFF;
String lastRejectReason;
String lastCommandSource;
String sharedSecret = "garage-secret";
String allowedS3MacText;
uint8_t allowedS3Mac[6] = {0};
bool allowedS3MacConfigured = false;
String savedSsid;
String savedPass;
String adminPassword = DEFAULT_ADMIN_PASSWORD;
uint32_t lastWifiCheckMs = 0;
uint32_t lastWifiReconnectMs = 0;
uint32_t wifiReconnectCount = 0;
uint32_t wifiDisconnectedSinceMs = 0;
uint32_t espNowQueueDrops = 0;
wl_status_t lastWifiStatus = WL_IDLE_STATUS;

uint32_t fnv1a(const uint8_t *data, size_t len, uint32_t hash = 2166136261UL) {
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}

uint32_t packetSignature(const GarageCommandPacket &packet) {
  uint32_t hash = fnv1a((const uint8_t *)sharedSecret.c_str(), sharedSecret.length());
  hash = fnv1a((const uint8_t *)&packet.magic, sizeof(packet.magic), hash);
  hash = fnv1a((const uint8_t *)&packet.version, sizeof(packet.version), hash);
  hash = fnv1a((const uint8_t *)&packet.command, sizeof(packet.command), hash);
  hash = fnv1a((const uint8_t *)&packet.userId, sizeof(packet.userId), hash);
  hash = fnv1a((const uint8_t *)&packet.seq, sizeof(packet.seq), hash);
  hash = fnv1a((const uint8_t *)&packet.millisStamp, sizeof(packet.millisStamp), hash);
  return hash;
}

bool parseMac(const String &text, uint8_t out[6]) {
  int values[6];
  if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x",
             &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) != 6) {
    return false;
  }
  for (uint8_t i = 0; i < 6; i++) {
    if (values[i] < 0 || values[i] > 255) return false;
    out[i] = (uint8_t)values[i];
  }
  return true;
}

String macToString(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

String jsonEscape(const String &value) {
  String out;
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

String htmlEscape(const String &value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    switch (value[i]) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += value[i]; break;
    }
  }
  return out;
}

void relayOff() {
  digitalWrite(RELAY_PIN, !RELAY_ACTIVE_LEVEL);
  relayActive = false;
  logLine("relay off");
}

void startRelayPulse() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LEVEL);
  relayPulseStartMs = millis();
  relayActive = true;
  logLine("relay pulse start pulseMs=" + String(relayPulseMs));
}

bool macAllowed(const uint8_t *mac) {
  if (!allowedS3MacConfigured) return true;
  return memcmp(mac, allowedS3Mac, 6) == 0;
}

void sendAck(const uint8_t *mac, uint32_t seq, bool ok, uint8_t state) {
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = WiFi.channel();
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  }
  GarageAckPacket ack = {};
  ack.magic = GARAGE_MAGIC;
  ack.seq = seq;
  ack.ok = ok ? 1 : 0;
  ack.state = state;
  esp_err_t err = esp_now_send(mac, (const uint8_t *)&ack, sizeof(ack));
  lastAckSendOk = err == ESP_OK;
  lastAckSendErr = (int)err;
  lastAckMs = millis();
  logLine("ack seq=" + String(seq) +
          " ok=" + String(ok ? "true" : "false") +
          " sendOk=" + String(lastAckSendOk ? "true" : "false") +
          " err=" + String(lastAckSendErr));
}

bool handleGarageCommand(const uint8_t *mac, const GarageCommandPacket &packet) {
  lastRxMs = millis();
  lastCommandSource = macToString(mac);

  if (!commandEnabled) {
    lastRejectReason = "disabled";
    logLine("reject command: disabled");
    return false;
  }
  if (!macAllowed(mac)) {
    lastRejectReason = "source_mac_not_allowed";
    logLine("reject command: source mac not allowed " + macToString(mac));
    return false;
  }
  if (packet.magic != GARAGE_MAGIC || packet.version != 1) {
    lastRejectReason = "bad_magic_or_version";
    logLine("reject command: bad magic/version");
    return false;
  }
  if (packet.command != GARAGE_CMD_TRIGGER) {
    lastRejectReason = "bad_command";
    logLine("reject command: bad command");
    return false;
  }
  if (packet.signature != packetSignature(packet)) {
    lastRejectReason = "bad_signature";
    logLine("reject command: bad signature");
    return false;
  }
  if (packet.seq <= lastSeq) {
    lastRejectReason = "replay_or_old_seq";
    logLine("reject command: replay seq=" + String(packet.seq));
    return false;
  }
  if (lastCommandMs != 0 && millis() - lastCommandMs < COMMAND_COOLDOWN_MS) {
    lastRejectReason = "cooldown";
    logLine("reject command: cooldown");
    return false;
  }

  lastSeq = packet.seq;
  lastCommandMs = millis();
  lastUserId = packet.userId;
  lastRejectReason = "";
  acceptedCount++;
  logLine("accept command seq=" + String(packet.seq) +
          " userId=" + String(packet.userId) +
          " from=" + lastCommandSource);
  startRelayPulse();
  return true;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  const uint8_t *mac = info->src_addr;
#else
void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (!garageCommandQueue) {
    __atomic_fetch_add(&espNowQueueDrops, 1, __ATOMIC_RELAXED);
    return;
  }
  PendingGarageCommand pending = {};
  memcpy(pending.mac, mac, sizeof(pending.mac));
  pending.len = len;
  if (len == sizeof(GarageCommandPacket)) {
    memcpy(&pending.packet, data, sizeof(pending.packet));
  }
  if (xQueueSend(garageCommandQueue, &pending, 0) != pdTRUE) {
    __atomic_fetch_add(&espNowQueueDrops, 1, __ATOMIC_RELAXED);
  }
}

void processEspNowQueue() {
  if (!garageCommandQueue) return;
  PendingGarageCommand pending;
  while (xQueueReceive(garageCommandQueue, &pending, 0) == pdTRUE) {
    if (pending.len != sizeof(GarageCommandPacket)) {
      rejectedCount++;
      lastRejectReason = "bad_packet_size";
      logLine("reject command: bad packet size=" + String(pending.len));
      continue;
    }
    bool ok = handleGarageCommand(pending.mac, pending.packet);
    if (!ok) rejectedCount++;
    sendAck(pending.mac, pending.packet.seq, ok, relayActive ? 1 : 0);
  }
}

void beginEspNow() {
  if (!garageCommandQueue) {
    garageCommandQueue = xQueueCreate(8, sizeof(PendingGarageCommand));
    if (!garageCommandQueue) {
      logLine("esp-now queue allocation failed");
      espNowReady = false;
      return;
    }
  }
  if (espNowReady) {
    esp_now_deinit();
    espNowReady = false;
  }
  if (esp_now_init() != ESP_OK) {
    espNowReady = false;
    logLine("esp-now init failed");
    return;
  }
  espNowReady = true;
  esp_now_register_recv_cb(onEspNowRecv);
  logLine("esp-now ready channel=" + String(WiFi.channel()));
}

void loadConfig() {
  preferences.begin("cfg", true);
  commandEnabled = preferences.getBool("enabled", true);
  relayPulseMs = preferences.getUInt("pulseMs", DEFAULT_PULSE_MS);
  fallbackChannel = preferences.getUChar("channel", DEFAULT_ESPNOW_CHANNEL);
  sharedSecret = preferences.getString("secret", "garage-secret");
  allowedS3MacText = preferences.getString("s3mac", "");
  s3BaseUrl = preferences.getString("s3url", "");
  adminPassword = preferences.getString("adminPass", DEFAULT_ADMIN_PASSWORD);
  preferences.end();
  if (!adminPassword.length()) {
    adminPassword = DEFAULT_ADMIN_PASSWORD;
  }
  allowedS3MacConfigured = parseMac(allowedS3MacText, allowedS3Mac);
  if (relayPulseMs < 100 || relayPulseMs > 5000) {
    relayPulseMs = DEFAULT_PULSE_MS;
  }
  if (fallbackChannel < 1 || fallbackChannel > 13) {
    fallbackChannel = DEFAULT_ESPNOW_CHANNEL;
  }
  espNowChannel = fallbackChannel;
  logLine("config enabled=" + String(commandEnabled ? "true" : "false") +
          " pulseMs=" + String(relayPulseMs) +
          " fallbackChannel=" + String(fallbackChannel) +
          " s3url=" + s3BaseUrl +
          " s3mac=" + allowedS3MacText);
}

void saveConfig(bool enabled, uint32_t pulseMs, uint8_t channel, const String &secret,
                const String &s3mac, const String &s3url) {
  preferences.begin("cfg", false);
  preferences.putBool("enabled", enabled);
  preferences.putUInt("pulseMs", pulseMs);
  preferences.putUChar("channel", channel);
  preferences.putString("secret", secret);
  preferences.putString("s3mac", s3mac);
  preferences.putString("s3url", s3url);
  preferences.end();
  loadConfig();
}

void saveAdminPassword(const String &password) {
  String next = password;
  next.trim();
  if (!next.length()) {
    return;
  }
  preferences.begin("cfg", false);
  preferences.putString("adminPass", next);
  preferences.end();
  adminPassword = next;
}

int fetchS3Channel() {
  if (WiFi.status() != WL_CONNECTED || !s3BaseUrl.length()) {
    return -1;
  }
  String url = s3BaseUrl;
  url.trim();
  while (url.endsWith("/")) {
    url.remove(url.length() - 1);
  }
  url += "/api/garage/status";

  HTTPClient http;
  http.setConnectTimeout(2500);
  http.setTimeout(2500);
  if (!http.begin(url)) {
    logLine("s3 channel query begin failed");
    return -1;
  }
  int code = http.GET();
  String body = http.getString();
  http.end();
  if (code != 200) {
    logLine("s3 channel query http=" + String(code));
    return -1;
  }
  int pos = body.indexOf("\"channel\":");
  if (pos < 0) {
    return -1;
  }
  int start = pos + 10;
  int end = start;
  while (end < body.length() && isDigit(body[end])) {
    end++;
  }
  int channel = body.substring(start, end).toInt();
  if (channel < 1 || channel > 13) {
    return -1;
  }
  return channel;
}

void resolveEspNowChannel() {
  if (WiFi.status() == WL_CONNECTED) {
    espNowChannel = WiFi.channel();
    int s3Channel = fetchS3Channel();
    if (s3Channel > 0 && s3Channel != espNowChannel) {
      logLine("warning: s3 channel=" + String(s3Channel) +
              " local wifi channel=" + String(espNowChannel));
    } else if (s3Channel > 0) {
      logLine("s3 channel confirmed=" + String(s3Channel));
    }
    return;
  }
  espNowChannel = fallbackChannel;
}

void startSetupAp() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  espNowChannel = fallbackChannel;
  WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD, espNowChannel);
  setupApRunning = true;
  wifiReady = true;
  logLine("setup AP started ssid=" + String(SETUP_AP_SSID) +
          " ip=" + WiFi.softAPIP().toString() +
          " channel=" + String(WiFi.channel()));
  if (espNowReady) {
    beginEspNow();
  }
}

void connectWifiOrStartAp() {
  WiFi.setHostname(HOSTNAME);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  preferences.begin("wifi", true);
  savedSsid = preferences.getString("ssid", "");
  savedPass = preferences.getString("pass", "");
  preferences.end();
  if (!savedSsid.length()) {
    startSetupAp();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000) {
    delay(300);
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiReady = true;
    lastWifiStatus = WL_CONNECTED;
    logLine("wifi connected ip=" + WiFi.localIP().toString() +
            " channel=" + String(WiFi.channel()));
  } else {
    logLine("wifi connect failed, starting AP");
    startSetupAp();
  }
}

void maintainWifi() {
  if (!savedSsid.length()) {
    return;
  }
  if (millis() - lastWifiCheckMs < 1000) {
    return;
  }
  lastWifiCheckMs = millis();

  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    if (lastWifiStatus != WL_CONNECTED) {
      wifiReady = true;
      wifiDisconnectedSinceMs = 0;
      if (setupApRunning) {
        WiFi.softAPdisconnect(true);
        setupApRunning = false;
        WiFi.mode(WIFI_STA);
      }
      logLine("wifi reconnected ip=" + WiFi.localIP().toString() +
              " channel=" + String(WiFi.channel()) +
              " rssi=" + String(WiFi.RSSI()));
      resolveEspNowChannel();
      beginEspNow();
    }
    lastWifiStatus = status;
    return;
  }

  if (lastWifiStatus == WL_CONNECTED) {
    wifiReady = false;
    wifiDisconnectedSinceMs = millis();
    logLine("wifi disconnected status=" + String((int)status));
  }
  if (wifiDisconnectedSinceMs == 0) {
    wifiDisconnectedSinceMs = millis();
  }
  lastWifiStatus = status;

  if (!setupApRunning && millis() - wifiDisconnectedSinceMs >= WIFI_AP_FALLBACK_MS) {
    logLine("wifi unavailable, enabling setup AP while retries continue");
    startSetupAp();
  }

  if (millis() - lastWifiReconnectMs >= WIFI_RECONNECT_INTERVAL_MS) {
    lastWifiReconnectMs = millis();
    wifiReconnectCount++;
    logLine("wifi reconnect attempt=" + String(wifiReconnectCount) +
            " status=" + String((int)status));
    WiFi.disconnect(false);
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  }
}

String bodyArg(const String &key) {
  if (server.hasArg(key)) return server.arg(key);
  return "";
}

bool bodyBool(const String &key, bool fallback) {
  String v = bodyArg(key);
  if (!v.length()) return fallback;
  v.toLowerCase();
  return v == "true" || v == "1" || v == "on";
}

int bodyInt(const String &key, int fallback) {
  String v = bodyArg(key);
  return v.length() ? v.toInt() : fallback;
}

void sendJson(int code, const String &json) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(code, "application/json; charset=utf-8", json);
}

bool requireAuth() {
  if (server.authenticate(AUTH_USER, adminPassword.c_str())) {
    return true;
  }
  server.requestAuthentication(BASIC_AUTH, "garage-c3");
  return false;
}

String statusJson() {
  String json = F("{\"ok\":true,\"hostname\":\"");
  json += HOSTNAME;
  json += F("\",\"mac\":\"");
  json += WiFi.macAddress();
  json += F("\",\"ip\":\"");
  json += (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  json += F("\",\"ssid\":\"");
  json += jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String(SETUP_AP_SSID));
  json += F("\",\"wifiMode\":\"");
  json += setupApRunning ? F("AP") : F("STA");
  json += F("\",\"wifiStatus\":");
  json += (int)WiFi.status();
  json += F(",\"rssi\":");
  json += WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  json += F(",\"reconnectAttempts\":");
  json += wifiReconnectCount;
  json += F(",\"espNowQueueDrops\":");
  json += __atomic_load_n(&espNowQueueDrops, __ATOMIC_RELAXED);
  json += F(",\"channel\":");
  json += WiFi.channel();
  json += F(",\"espNowReady\":");
  json += espNowReady ? F("true") : F("false");
  json += F(",\"commandEnabled\":");
  json += commandEnabled ? F("true") : F("false");
  json += F(",\"relayPin\":");
  json += RELAY_PIN;
  json += F(",\"relayActive\":");
  json += relayActive ? F("true") : F("false");
  json += F(",\"pulseMs\":");
  json += relayPulseMs;
  json += F(",\"espNowChannel\":");
  json += espNowChannel;
  json += F(",\"fallbackChannel\":");
  json += fallbackChannel;
  json += F(",\"acceptedCount\":");
  json += acceptedCount;
  json += F(",\"rejectedCount\":");
  json += rejectedCount;
  json += F(",\"lastSeq\":");
  json += lastSeq;
  json += F(",\"lastUserId\":");
  json += lastUserId;
  json += F(",\"lastRxMs\":");
  json += lastRxMs;
  json += F(",\"lastAckMs\":");
  json += lastAckMs;
  json += F(",\"lastAckSendOk\":");
  json += lastAckSendOk ? F("true") : F("false");
  json += F(",\"lastAckSendErr\":");
  json += lastAckSendErr;
  json += F(",\"lastCommandSource\":\"");
  json += lastCommandSource;
  json += F("\",\"lastRejectReason\":\"");
  json += jsonEscape(lastRejectReason);
  json += F("\",\"allowedS3Mac\":\"");
  json += allowedS3MacText;
  json += F("\",\"s3BaseUrl\":\"");
  json += jsonEscape(s3BaseUrl);
  json += F("\",\"ota\":\"arduino_ota\"}");
  return json;
}

void handleConfig() {
  if (!requireAuth()) return;
  uint32_t pulse = (uint32_t)bodyInt("pulseMs", relayPulseMs);
  pulse = constrain(pulse, 100UL, 5000UL);
  uint8_t channel = (uint8_t)constrain(bodyInt("channel", fallbackChannel), 1, 13);
  String secret = bodyArg("secret");
  if (!secret.length()) secret = sharedSecret;
  String s3mac = bodyArg("s3mac");
  String s3url = bodyArg("s3url");
  if (!s3url.length()) s3url = s3BaseUrl;
  saveConfig(bodyBool("enabled", commandEnabled), pulse, channel, secret, s3mac, s3url);
  sendJson(200, F("{\"ok\":true,\"message\":\"config_saved_rebooting\"}"));
  delay(500);
  ESP.restart();
}

void handleIndex() {
  if (!requireAuth()) return;
  bool connected = WiFi.status() == WL_CONNECTED;
  String displaySsid = connected ? WiFi.SSID() : String(SETUP_AP_SSID) + " (setup AP)";
  int rssi = connected ? WiFi.RSSI() : 0;
  int signalPercent = connected ? constrain(2 * (rssi + 100), 0, 100) : 0;
  const char *signalLabel = !connected ? "Disconnected" :
                            rssi >= -55 ? "Excellent" :
                            rssi >= -67 ? "Good" :
                            rssi >= -75 ? "Fair" : "Weak";
  String html = F("<!doctype html><html><head><meta charset='utf-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>garage-c3 setup</title>"
                  "<style>*{box-sizing:border-box}body{font-family:Arial,sans-serif;margin:20px;max-width:760px;overflow-wrap:anywhere}"
                  "input,select,button{box-sizing:border-box;width:100%;padding:9px;margin:5px 0 12px}"
                  ".status-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(145px,1fr));gap:10px;margin:12px 0 18px}"
                  ".status-card{background:#eef4f8;border-radius:8px;padding:12px;min-width:0}.status-label{font-size:12px;color:#52606d}.status-value{font-weight:bold;margin-top:5px;word-break:break-word}"
                  "details{margin:12px 0 20px}pre{background:#111;color:#d7f2ff;padding:10px;max-width:100%;white-space:pre-wrap;overflow-wrap:anywhere;word-break:break-all;overflow-x:auto}</style></head><body>");
  html += F("<h1>garage-c3 setup</h1><div class='status-grid'><div class='status-card'><div class='status-label'>WiFi name</div><div class='status-value'>");
  html += htmlEscape(displaySsid);
  html += F("</div></div><div class='status-card'><div class='status-label'>WiFi signal</div><div class='status-value'>");
  html += signalLabel;
  if (connected) {
    html += " · " + String(rssi) + " dBm · " + String(signalPercent) + "%";
  }
  html += F("</div></div><div class='status-card'><div class='status-label'>IP address</div><div class='status-value'>");
  html += connected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  html += F("</div></div><div class='status-card'><div class='status-label'>ESP-NOW</div><div class='status-value'>");
  html += espNowReady ? F("Ready") : F("Not ready");
  html += F("</div></div></div><details><summary>Device diagnostics</summary><pre>");
  html += htmlEscape(statusJson());
  html += F("</pre></details><h2>WiFi</h2><form method='post' action='/api/wifi/save'>"
            "<label>SSID</label><input name='ssid' value='");
  html += htmlEscape(savedSsid);
  html += F("'>"
            "<label>Password</label><input name='pass' type='password'>"
            "<button type='submit'>Save WiFi and reboot</button></form>"
            "<form method='post' action='/api/wifi/clear'><button type='submit'>Clear WiFi and reboot</button></form>");
  html += F("<h2>Garage</h2><form method='post' action='/api/config'>"
            "<label>Enabled</label><select name='enabled'><option value='true'>true</option><option value='false'>false</option></select>"
            "<label>Pulse ms</label><input name='pulseMs' type='number' min='100' max='5000' value='");
  html += relayPulseMs;
  html += F("'><label>Fallback channel (used when WiFi is not connected)</label><input name='channel' type='number' min='1' max='13' value='");
  html += fallbackChannel;
  html += F("'><label>S3 base URL (optional, for channel check)</label><input name='s3url' value='");
  html += htmlEscape(s3BaseUrl);
  html += F("'><label>S3 MAC whitelist (optional)</label><input name='s3mac' value='");
  html += htmlEscape(allowedS3MacText);
  html += F("'><label>Shared secret</label><input name='secret' value='");
  html += htmlEscape(sharedSecret);
  html += F("'><button type='submit'>Save garage config and reboot</button></form>"
            "<h2>Admin password</h2><form method='post' action='/api/auth/password'>"
            "<label>New password</label><input name='password' type='password' minlength='6'>"
            "<button type='submit'>Save password</button></form>"
            "<form method='post' action='/api/relay/test'><button type='submit'>Test relay pulse</button></form>"
            "</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

void handleRelayTest() {
  if (!requireAuth()) return;
  if (lastCommandMs != 0 && millis() - lastCommandMs < COMMAND_COOLDOWN_MS) {
    sendJson(409, F("{\"ok\":false,\"error\":\"cooldown\"}"));
    return;
  }
  lastCommandMs = millis();
  startRelayPulse();
  sendJson(200, F("{\"ok\":true,\"message\":\"relay_pulse_started\"}"));
}

void handleWifiSave() {
  if (!requireAuth()) return;
  String ssid = bodyArg("ssid");
  String pass = bodyArg("pass");
  ssid.trim();
  if (!ssid.length()) {
    sendJson(400, F("{\"ok\":false,\"error\":\"ssid_required\"}"));
    return;
  }
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.end();
  sendJson(200, F("{\"ok\":true,\"message\":\"wifi_saved_rebooting\"}"));
  delay(500);
  ESP.restart();
}

void handleWifiClear() {
  if (!requireAuth()) return;
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();
  sendJson(200, F("{\"ok\":true,\"message\":\"wifi_cleared_rebooting\"}"));
  delay(500);
  ESP.restart();
}

void handlePasswordSave() {
  if (!requireAuth()) return;
  String password = bodyArg("password");
  password.trim();
  if (password.length() < 6) {
    sendJson(400, F("{\"ok\":false,\"error\":\"password_too_short\"}"));
    return;
  }
  saveAdminPassword(password);
  sendJson(200, F("{\"ok\":true,\"message\":\"password_saved\"}"));
}

void beginWeb() {
  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/status", HTTP_GET, []() {
    if (!requireAuth()) return;
    sendJson(200, statusJson());
  });
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/relay/test", HTTP_POST, handleRelayTest);
  server.on("/api/wifi/save", HTTP_POST, handleWifiSave);
  server.on("/api/wifi/clear", HTTP_POST, handleWifiClear);
  server.on("/api/auth/password", HTTP_POST, handlePasswordSave);
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      sendJson(204, "");
    } else {
      if (!requireAuth()) return;
      sendJson(404, F("{\"ok\":false,\"error\":\"not_found\"}"));
    }
  });
  server.begin();
  logLine("http api ready");
}

void beginOta() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(adminPassword.c_str());
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    esp_task_wdt_reset();
  });
  ArduinoOTA.begin();
  logLine("arduino OTA ready hostname=" + String(HOSTNAME));
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  logLine("boot");
  logLine("reset reason=" + String((int)esp_reset_reason()));
  logLine("mac=" + WiFi.macAddress());

  pinMode(RELAY_PIN, OUTPUT);
  relayOff();
  loadConfig();
  connectWifiOrStartAp();
  resolveEspNowChannel();
  beginEspNow();
  beginWeb();
  beginOta();
  enableLoopWDT();
  logLine("loop watchdog enabled timeout=5s");
}

void loop() {
  maintainWifi();
  processEspNowQueue();
  server.handleClient();
  ArduinoOTA.handle();
  if (relayActive && millis() - relayPulseStartMs >= relayPulseMs) {
    relayOff();
  }
}
