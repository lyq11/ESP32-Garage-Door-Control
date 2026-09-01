#include "garage_espnow.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_system.h>

#include "app_config.h"
#include "logger.h"
#include "rs485_modbus.h"

static const uint32_t GARAGE_MAGIC = 0x47415231; // GAR1
static const uint8_t GARAGE_CMD_TRIGGER = 1;
static const uint8_t GARAGE_RECORD_CAPACITY = 30;
static const GarageTimingConfig GARAGE_DEFAULT_TIMING = {
  5000,   // sendCooldownMs
  45,     // openStableSeconds
  5000,   // openRecheckMs
  10000,  // movingRecheckMs
  45000,  // postTriggerRecheckMs
  60000,  // maxAttemptRecheckMs
  3       // maxAutoCloseAttempts
};

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

struct GarageRecord {
  uint32_t timeMs;
  uint32_t seq;
  int userId;
  bool sent;
  bool ackOk;
  GarageDoorState doorState;
  char source[18];
  char reason[40];
};

enum GarageLogicStage {
  GARAGE_STAGE_IDLE,
  GARAGE_STAGE_OPEN_WAIT,
  GARAGE_STAGE_MOVING_WAIT
};

static bool espNowReady = false;
static bool garageEnabled = false;
static bool autoCloseEnabled = true;
static GarageTimingConfig timingConfig = GARAGE_DEFAULT_TIMING;
static bool peerConfigured = false;
static uint8_t peerMac[6] = {0};
static String peerMacText;
static String sharedSecret = "garage-secret";
static uint32_t seqNo = 0;
static uint32_t lastSendMs = 0;
static uint32_t lastAckMs = 0;
static uint32_t lastAckSeq = 0;
static bool lastSendOk = false;
static bool lastAckOk = false;
static String lastReason;
static String lastTriggerSource;
static String lastTriggerReason;
static int lastTriggerUserId = -1;
static GarageLogicStage logicStage = GARAGE_STAGE_IDLE;
static uint32_t nextLogicMs = 0;
static uint8_t autoCloseAttempts = 0;
static bool autoCloseMaxLogged = false;
static GarageRecord records[GARAGE_RECORD_CAPACITY];
static uint8_t recordHead = 0;
static uint8_t recordCount = 0;

static uint32_t fnv1a(const uint8_t *data, size_t len, uint32_t hash = 2166136261UL) {
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}

static uint32_t packetSignature(const GarageCommandPacket &packet) {
  uint32_t hash = fnv1a((const uint8_t *)sharedSecret.c_str(), sharedSecret.length());
  hash = fnv1a((const uint8_t *)&packet.magic, sizeof(packet.magic), hash);
  hash = fnv1a((const uint8_t *)&packet.version, sizeof(packet.version), hash);
  hash = fnv1a((const uint8_t *)&packet.command, sizeof(packet.command), hash);
  hash = fnv1a((const uint8_t *)&packet.userId, sizeof(packet.userId), hash);
  hash = fnv1a((const uint8_t *)&packet.seq, sizeof(packet.seq), hash);
  hash = fnv1a((const uint8_t *)&packet.millisStamp, sizeof(packet.millisStamp), hash);
  return hash;
}

static bool parseMac(const String &text, uint8_t out[6]) {
  int values[6];
  if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x",
             &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) != 6) {
    return false;
  }
  for (uint8_t i = 0; i < 6; i++) {
    if (values[i] < 0 || values[i] > 255) {
      return false;
    }
    out[i] = (uint8_t)values[i];
  }
  return true;
}

static String macToString(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

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

static void addRecord(const char *source, const char *reason, int userId,
                      GarageDoorState state, bool sent, uint32_t seq = 0) {
  GarageRecord &record = records[recordHead];
  record.timeMs = millis();
  record.seq = seq;
  record.userId = userId;
  record.sent = sent;
  record.ackOk = false;
  record.doorState = state;
  snprintf(record.source, sizeof(record.source), "%s", source ? source : "");
  snprintf(record.reason, sizeof(record.reason), "%s", reason ? reason : "");
  recordHead = (recordHead + 1) % GARAGE_RECORD_CAPACITY;
  if (recordCount < GARAGE_RECORD_CAPACITY) {
    recordCount++;
  }
}

static void persistSeqNo() {
  Preferences prefs;
  prefs.begin("garage", false);
  prefs.putUInt("seqNo", seqNo);
  prefs.end();
}

static void markRecordAck(uint32_t seq, bool ok) {
  for (uint8_t i = 0; i < recordCount; i++) {
    uint8_t index = (recordHead + GARAGE_RECORD_CAPACITY - 1 - i) % GARAGE_RECORD_CAPACITY;
    if (records[index].seq == seq) {
      records[index].ackOk = ok;
      return;
    }
  }
}

const char *garageDoorStateText(GarageDoorState state) {
  switch (state) {
    case GARAGE_DOOR_CLOSED:
      return "CLOSED";
    case GARAGE_DOOR_OPEN:
      return "OPEN";
    case GARAGE_DOOR_MOVING:
      return "MOVING";
    case GARAGE_DOOR_CONFLICT:
      return "CONFLICT";
    default:
      return "UNKNOWN";
  }
}

GarageDoorState garageDoorState() {
  Rs485Port &doorPort = rs485Ports[DOOR_SENSOR_PORT];
  if (!doorPort.lastReadOk) {
    return GARAGE_DOOR_UNKNOWN;
  }
  bool closed = doorPort.regs[DOOR_HALL_REG_INDEX] == 1;
  bool moving = doorPort.regs[DOOR_VIBRATION_REG_INDEX] != 0;
  if (moving) {
    return GARAGE_DOOR_MOVING;
  }
  if (closed) {
    return GARAGE_DOOR_CLOSED;
  }
  return GARAGE_DOOR_OPEN;
}

uint16_t garageDoorStateDurationSeconds() {
  Rs485Port &doorPort = rs485Ports[DOOR_SENSOR_PORT];
  if (!doorPort.lastReadOk) {
    return 0;
  }
  return doorPort.regs[STATE_DURATION_REG_INDEX];
}

String garageLastTriggerMethod(uint32_t maxAgeMs) {
  if (lastSendMs == 0 || millis() - lastSendMs > maxAgeMs) {
    return F("手动/外部按钮（未检测到近期中控指令）");
  }
  if (lastTriggerReason == "manual_api") {
    return F("Web/API 手动开门");
  }
  if (lastTriggerReason == "auto_close_open_timeout") {
    return F("自动关门指令后仍处于开启状态");
  }

  String method;
  if (lastTriggerSource == "FACE_OK") {
    method = F("人脸识别");
  } else if (lastTriggerSource == "FACE_EYE_CLOSED_OK") {
    method = F("人脸识别（闭眼通过）");
  } else if (lastTriggerSource == "HAND_OK") {
    method = F("掌静脉识别");
  } else {
    method = lastTriggerSource.length() ? lastTriggerSource : String(F("中控指令"));
  }
  if (lastTriggerReason.length()) {
    method += F("，人员=");
    method += lastTriggerReason;
  }
  if (lastTriggerUserId >= 0) {
    method += F("，用户ID=");
    method += lastTriggerUserId;
  }
  return method;
}

bool garageRecentlyTriggered(uint32_t windowMs) {
  return lastSendMs != 0 && millis() - lastSendMs < windowMs;
}

GarageTimingConfig garageTiming() {
  return timingConfig;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onEspNowSent(const wifi_tx_info_t *txInfo, esp_now_send_status_t status) {
  (void)txInfo;
#else
static void onEspNowSent(const uint8_t *macAddr, esp_now_send_status_t status) {
  (void)macAddr;
#endif
  lastSendOk = status == ESP_NOW_SEND_SUCCESS;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;
#else
static void onEspNowRecv(const uint8_t *macAddr, const uint8_t *data, int len) {
  (void)macAddr;
#endif
  if (len != sizeof(GarageAckPacket)) {
    return;
  }
  GarageAckPacket ack;
  memcpy(&ack, data, sizeof(ack));
  if (ack.magic != GARAGE_MAGIC) {
    return;
  }
  lastAckMs = millis();
  lastAckSeq = ack.seq;
  lastAckOk = ack.ok != 0;
  markRecordAck(ack.seq, lastAckOk);
  logInfo("GARAGE", "ack seq=" + String(ack.seq) + " ok=" + String(lastAckOk ? "true" : "false"));
}

static bool addPeer() {
  if (!peerConfigured || !espNowReady) {
    return false;
  }
  esp_now_del_peer(peerMac);
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, peerMac, 6);
  peerInfo.channel = WiFi.channel();
  peerInfo.encrypt = false;
  esp_err_t err = esp_now_add_peer(&peerInfo);
  if (err != ESP_OK) {
    logError("GARAGE", "esp_now_add_peer failed err=" + String((int)err));
    return false;
  }
  logInfo("GARAGE", "peer added mac=" + peerMacText + " channel=" + String(WiFi.channel()));
  return true;
}

void garageRefreshPeerAfterWifiReconnect() {
  if (!espNowReady || !peerConfigured) {
    return;
  }
  if (!addPeer()) {
    logWarn("GARAGE", "peer refresh after WiFi reconnect failed");
  }
}

static bool garageTriggerFrom(uint8_t command, const char *reason, int userId, const char *source) {
  GarageDoorState state = garageDoorState();
  if (!garageEnabled || !peerConfigured || !espNowReady) {
    logWarn("GARAGE", "trigger skipped, not configured");
    addRecord(source, "not_configured", userId, state, false);
    return false;
  }
  if (millis() - lastSendMs < timingConfig.sendCooldownMs) {
    logWarn("GARAGE", "trigger skipped by cooldown");
    addRecord(source, "cooldown", userId, state, false);
    return false;
  }

  GarageCommandPacket packet = {};
  packet.magic = GARAGE_MAGIC;
  packet.version = 1;
  packet.command = command;
  packet.userId = userId < 0 ? 0xFFFF : (uint16_t)userId;
  packet.seq = ++seqNo;
  persistSeqNo();
  packet.millisStamp = millis();
  packet.signature = packetSignature(packet);

  esp_err_t err = esp_now_send(peerMac, (const uint8_t *)&packet, sizeof(packet));
  lastSendMs = millis();
  lastReason = reason ? reason : "";
  if (err != ESP_OK) {
    logError("GARAGE", "send failed err=" + String((int)err));
    addRecord(source, "send_failed", userId, state, false, packet.seq);
    return false;
  }
  lastTriggerSource = source ? source : "";
  lastTriggerReason = reason ? reason : "";
  lastTriggerUserId = userId;
  addRecord(source, reason ? reason : "sent", userId, state, true, packet.seq);
  logInfo("GARAGE", "trigger sent seq=" + String(packet.seq) +
                      " userId=" + String(userId) +
                      " reason=" + lastReason);
  return true;
}

bool garageTrigger(uint8_t command, const char *reason, int userId) {
  return garageTriggerFrom(command, reason, userId, "trigger");
}

void garageOnFaceSuccess(int userId, const char *name, const char *verifyType) {
  const char *source = verifyType && verifyType[0] ? verifyType : "FACE_OK";
  rs485RefreshDoorState();
  GarageDoorState state = garageDoorState();
  if (state == GARAGE_DOOR_OPEN || state == GARAGE_DOOR_MOVING) {
    logWarn("GARAGE", String(source) + " ignored, door state=" + String(garageDoorStateText(state)));
    addRecord(source, "door_not_closed", userId, state, false);
    return;
  }
  if (state == GARAGE_DOOR_UNKNOWN || state == GARAGE_DOOR_CONFLICT) {
    logWarn("GARAGE", String(source) + " unsafe, door state=" + String(garageDoorStateText(state)));
    addRecord(source, "unsafe_door_state", userId, state, false);
    return;
  }
  garageTriggerFrom(GARAGE_CMD_TRIGGER, name && name[0] ? name : source, userId, source);
}

static void tickAutoClose() {
  if (!autoCloseEnabled) {
    logicStage = GARAGE_STAGE_IDLE;
    return;
  }

  GarageDoorState state = garageDoorState();
  uint32_t now = millis();

  if (logicStage == GARAGE_STAGE_IDLE) {
    if (state == GARAGE_DOOR_OPEN) {
      logicStage = GARAGE_STAGE_OPEN_WAIT;
      nextLogicMs = now + timingConfig.openRecheckMs;
      autoCloseAttempts = 0;
      autoCloseMaxLogged = false;
      logInfo("GARAGE", "door open detected, wait for stable open");
    }
    return;
  }

  if (now < nextLogicMs) {
    if (state == GARAGE_DOOR_CLOSED) {
      logicStage = GARAGE_STAGE_IDLE;
      autoCloseAttempts = 0;
      autoCloseMaxLogged = false;
      logInfo("GARAGE", "door closed, auto close idle");
    }
    return;
  }

  if (state == GARAGE_DOOR_CLOSED) {
    logicStage = GARAGE_STAGE_IDLE;
    autoCloseAttempts = 0;
    autoCloseMaxLogged = false;
    logInfo("GARAGE", "door closed, no action");
    return;
  }

  if (state == GARAGE_DOOR_MOVING || state == GARAGE_DOOR_UNKNOWN) {
    logicStage = GARAGE_STAGE_MOVING_WAIT;
    nextLogicMs = now + timingConfig.movingRecheckMs;
    logInfo("GARAGE", "door moving/unknown, recheck in " + String(timingConfig.movingRecheckMs / 1000) + "s");
    return;
  }

  if (state == GARAGE_DOOR_OPEN) {
    uint16_t openSeconds = garageDoorStateDurationSeconds();
    if (openSeconds < timingConfig.openStableSeconds) {
      logicStage = GARAGE_STAGE_OPEN_WAIT;
      nextLogicMs = now + timingConfig.openRecheckMs;
      logInfo("GARAGE", "door open stable wait " + String(openSeconds) +
                          "/" + String(timingConfig.openStableSeconds) + "s");
      return;
    }
    if (autoCloseAttempts >= timingConfig.maxAutoCloseAttempts) {
      if (!autoCloseMaxLogged) {
        logError("GARAGE", "auto close max attempts reached");
        autoCloseMaxLogged = true;
      }
      logicStage = GARAGE_STAGE_MOVING_WAIT;
      nextLogicMs = now + timingConfig.maxAttemptRecheckMs;
      return;
    }
    autoCloseAttempts++;
    garageTrigger(GARAGE_CMD_TRIGGER, "auto_close_open_timeout", -1);
    logicStage = GARAGE_STAGE_MOVING_WAIT;
    nextLogicMs = now + timingConfig.postTriggerRecheckMs;
    return;
  }

  if (state == GARAGE_DOOR_CONFLICT) {
    logError("GARAGE", "door sensor conflict");
    logicStage = GARAGE_STAGE_MOVING_WAIT;
    nextLogicMs = now + timingConfig.movingRecheckMs;
  }
}

bool garageConfigure(const String &mac, const String &secret, bool enabled, bool autoClose,
                     const GarageTimingConfig &timing) {
  uint8_t parsed[6];
  if (enabled && !parseMac(mac, parsed)) {
    return false;
  }

  Preferences prefs;
  prefs.begin("garage", false);
  prefs.putBool("enabled", enabled);
  prefs.putBool("autoClose", autoClose);
  prefs.putString("mac", mac);
  prefs.putString("secret", secret);
  prefs.putUInt("sendCd", timing.sendCooldownMs);
  prefs.putUShort("openStable", timing.openStableSeconds);
  prefs.putUInt("openChk", timing.openRecheckMs);
  prefs.putUInt("movingChk", timing.movingRecheckMs);
  prefs.putUInt("postTrig", timing.postTriggerRecheckMs);
  prefs.putUInt("maxChk", timing.maxAttemptRecheckMs);
  prefs.putUChar("maxAtt", timing.maxAutoCloseAttempts);
  prefs.end();

  garageEnabled = enabled;
  autoCloseEnabled = autoClose;
  timingConfig = timing;
  sharedSecret = secret.length() ? secret : "garage-secret";
  peerConfigured = enabled;
  if (enabled) {
    memcpy(peerMac, parsed, 6);
    peerMacText = macToString(peerMac);
    addPeer();
  }
  logInfo("GARAGE", "config saved enabled=" + String(enabled ? "true" : "false") +
                      " autoClose=" + String(autoClose ? "true" : "false") +
                      " openStable=" + String(timingConfig.openStableSeconds) + "s");
  return true;
}

void garageBegin() {
  Preferences prefs;
  prefs.begin("garage", true);
  garageEnabled = prefs.getBool("enabled", false);
  autoCloseEnabled = prefs.getBool("autoClose", true);
  peerMacText = prefs.getString("mac", "");
  sharedSecret = prefs.getString("secret", "garage-secret");
  timingConfig.sendCooldownMs = prefs.getUInt("sendCd", GARAGE_DEFAULT_TIMING.sendCooldownMs);
  timingConfig.openStableSeconds = prefs.getUShort("openStable", GARAGE_DEFAULT_TIMING.openStableSeconds);
  timingConfig.openRecheckMs = prefs.getUInt("openChk", GARAGE_DEFAULT_TIMING.openRecheckMs);
  timingConfig.movingRecheckMs = prefs.getUInt("movingChk", GARAGE_DEFAULT_TIMING.movingRecheckMs);
  timingConfig.postTriggerRecheckMs = prefs.getUInt("postTrig", GARAGE_DEFAULT_TIMING.postTriggerRecheckMs);
  timingConfig.maxAttemptRecheckMs = prefs.getUInt("maxChk", GARAGE_DEFAULT_TIMING.maxAttemptRecheckMs);
  timingConfig.maxAutoCloseAttempts = prefs.getUChar("maxAtt", GARAGE_DEFAULT_TIMING.maxAutoCloseAttempts);
  seqNo = prefs.getUInt("seqNo", 0);
  prefs.end();
  if (seqNo == 0) {
    seqNo = 100000UL + (esp_random() & 0x3FFFFFFFUL);
    persistSeqNo();
  }
  peerConfigured = garageEnabled && parseMac(peerMacText, peerMac);
  if (peerConfigured) {
    peerMacText = macToString(peerMac);
  }

  if (esp_now_init() != ESP_OK) {
    logError("GARAGE", "esp_now_init failed");
    return;
  }
  espNowReady = true;
  esp_now_register_send_cb(onEspNowSent);
  esp_now_register_recv_cb(onEspNowRecv);
  addPeer();
  logInfo("GARAGE", "esp-now ready enabled=" + String(garageEnabled ? "true" : "false") +
                      " channel=" + String(WiFi.channel()) +
                      " seqNo=" + String(seqNo));
}

void garageTick() {
  tickAutoClose();
}

String garageStatusJson() {
  GarageDoorState state = garageDoorState();
  String json = F("{\"enabled\":");
  json += garageEnabled ? F("true") : F("false");
  json += F(",\"espNowReady\":");
  json += espNowReady ? F("true") : F("false");
  json += F(",\"peerConfigured\":");
  json += peerConfigured ? F("true") : F("false");
  json += F(",\"peerMac\":\"");
  json += peerMacText;
  json += F("\",\"channel\":");
  json += WiFi.channel();
  json += F(",\"autoCloseEnabled\":");
  json += autoCloseEnabled ? F("true") : F("false");
  json += F(",\"sendCooldownMs\":");
  json += timingConfig.sendCooldownMs;
  json += F(",\"openStableSeconds\":");
  json += timingConfig.openStableSeconds;
  json += F(",\"openRecheckMs\":");
  json += timingConfig.openRecheckMs;
  json += F(",\"movingRecheckMs\":");
  json += timingConfig.movingRecheckMs;
  json += F(",\"postTriggerRecheckMs\":");
  json += timingConfig.postTriggerRecheckMs;
  json += F(",\"maxAttemptRecheckMs\":");
  json += timingConfig.maxAttemptRecheckMs;
  json += F(",\"maxAutoCloseAttempts\":");
  json += timingConfig.maxAutoCloseAttempts;
  json += F(",\"doorState\":\"");
  json += garageDoorStateText(state);
  json += F("\",\"logicStage\":");
  json += (int)logicStage;
  json += F(",\"nextLogicMs\":");
  json += nextLogicMs;
  json += F(",\"autoCloseAttempts\":");
  json += autoCloseAttempts;
  json += F(",\"lastSendMs\":");
  json += lastSendMs;
  json += F(",\"lastSendOk\":");
  json += lastSendOk ? F("true") : F("false");
  json += F(",\"lastAckMs\":");
  json += lastAckMs;
  json += F(",\"lastAckSeq\":");
  json += lastAckSeq;
  json += F(",\"lastAckOk\":");
  json += lastAckOk ? F("true") : F("false");
  json += F(",\"lastReason\":\"");
  json += lastReason;
  json += F("\"}");
  return json;
}

String garageRecordsJson() {
  String json = F("{\"records\":[");
  for (uint8_t i = 0; i < recordCount; i++) {
    uint8_t index = (recordHead + GARAGE_RECORD_CAPACITY - 1 - i) % GARAGE_RECORD_CAPACITY;
    GarageRecord &record = records[index];
    if (i > 0) {
      json += ',';
    }
    json += F("{\"time\":");
    json += record.timeMs;
    json += F(",\"seq\":");
    json += record.seq;
    json += F(",\"userId\":");
    json += record.userId;
    json += F(",\"sent\":");
    json += record.sent ? F("true") : F("false");
    json += F(",\"ackOk\":");
    json += record.ackOk ? F("true") : F("false");
    json += F(",\"doorState\":\"");
    json += garageDoorStateText(record.doorState);
    json += F("\",\"source\":\"");
    json += escapeJson(record.source);
    json += F("\",\"reason\":\"");
    json += escapeJson(record.reason);
    json += F("\"}");
  }
  json += F("]}");
  return json;
}
