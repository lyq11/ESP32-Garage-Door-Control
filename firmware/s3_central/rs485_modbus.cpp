#include "rs485_modbus.h"

#include <Preferences.h>

#include "app_config.h"
#include "config_pins.h"
#include "logger.h"

static HardwareSerial RS485Serial1(1);
static HardwareSerial RS485Serial2(2);
static uint32_t lastVibrationPollMs = 0;
static uint32_t lastDoorPollMs = 0;
static uint16_t lastVibrationCounter = 0;
static uint16_t lastOutsideHallState = 0;
static bool vibrationCounterInitialized = false;
static bool outsideHallInitialized = false;
static const uint16_t MAX_MANUAL_READ_REGS = 16;
static const uint8_t SENSOR_DEFAULT_ADDR = 2;
static const uint8_t CONFIG_APPLY_MAX_ATTEMPTS = 5;
static const uint32_t CONFIG_APPLY_FIRST_DELAY_MS = 1500;
static const uint32_t CONFIG_APPLY_RETRY_MS = 3000;

struct SavedSensorConfig {
  bool valid;
  bool applied;
  uint8_t writeAddr;
  uint8_t attempts;
  uint32_t nextApplyMs;
  uint16_t values[SENSOR_CONFIG_REG_COUNT];
};

static SavedSensorConfig savedConfigs[2] = {};

Rs485Port rs485Ports[2] = {
    {"RS485-1", &RS485Serial1, RS485_1_TX_PIN, RS485_1_RX_PIN, RS485_1_DE_RE_PIN, 2, ENABLE_RS485_1, {0}, false, 0, 0, 0, 0},
    {"RS485-2", &RS485Serial2, RS485_2_TX_PIN, RS485_2_RX_PIN, RS485_2_DE_RE_PIN, 2, ENABLE_RS485_2, {0}, false, 0, 0, 0, 0},
};

static uint16_t modbusCrc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x0001) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
  }
  return crc;
}

static void appendHexBytes(String &out, const uint8_t *data, uint8_t len) {
  char buf[4];
  for (uint8_t i = 0; i < len; i++) {
    snprintf(buf, sizeof(buf), " %02X", data[i]);
    out += buf;
  }
}

static String hexBytes(const uint8_t *data, uint8_t len) {
  String out;
  appendHexBytes(out, data, len);
  out.trim();
  return out;
}

static String configKey(uint8_t index, const char *name) {
  return String("p") + String(index) + "_" + name;
}

static void loadSavedConfig(uint8_t index) {
  if (index >= 2) return;
  Preferences prefs;
  prefs.begin("rs485cfg", true);
  SavedSensorConfig &cfg = savedConfigs[index];
  cfg.valid = prefs.getBool(configKey(index, "valid").c_str(), false);
  cfg.writeAddr = prefs.getUChar(configKey(index, "addr").c_str(), rs485Ports[index].nodeAddr);
  for (uint8_t i = 0; i < SENSOR_CONFIG_REG_COUNT; i++) {
    cfg.values[i] = prefs.getUShort(configKey(index, ("v" + String(i)).c_str()).c_str(), 0);
  }
  prefs.end();
  cfg.applied = false;
  cfg.attempts = 0;
  cfg.nextApplyMs = millis() + CONFIG_APPLY_FIRST_DELAY_MS;
  if (cfg.valid) {
    logInfo(rs485Ports[index].name, "saved sensor config loaded targetAddr=" + String(cfg.values[0]) +
                                  " writeAddr=" + String(cfg.writeAddr));
  }
}

static void persistSavedConfig(uint8_t index) {
  if (index >= 2) return;
  Preferences prefs;
  prefs.begin("rs485cfg", false);
  SavedSensorConfig &cfg = savedConfigs[index];
  prefs.putBool(configKey(index, "valid").c_str(), cfg.valid);
  prefs.putUChar(configKey(index, "addr").c_str(), cfg.writeAddr);
  for (uint8_t i = 0; i < SENSOR_CONFIG_REG_COUNT; i++) {
    prefs.putUShort(configKey(index, ("v" + String(i)).c_str()).c_str(), cfg.values[i]);
  }
  prefs.end();
}

static bool sameAddressAlreadyTried(const uint8_t *addrs, uint8_t count, uint8_t addr) {
  for (uint8_t i = 0; i < count; i++) {
    if (addrs[i] == addr) return true;
  }
  return false;
}

static void tickSavedConfigApply();

static void set485Transmit(Rs485Port &port, bool enabled) {
  digitalWrite(port.deRePin, enabled ? HIGH : LOW);
  delayMicroseconds(80);
}

static void clearRs485Rx(Rs485Port &port) {
  while (port.serial->available() > 0) {
    port.serial->read();
  }
}

static bool readRegisters(Rs485Port &port, uint8_t functionCode, uint16_t startReg, uint16_t regCount,
                          uint16_t *outRegs, uint32_t timeoutMs, String *result = nullptr) {
  if (regCount == 0 || regCount > MAX_MANUAL_READ_REGS) {
    logError(port.name, "invalid register count=" + String(regCount));
    return false;
  }
  if (functionCode != 0x03 && functionCode != 0x04) {
    if (result) *result = "bad read function";
    return false;
  }

  uint8_t request[8];
  request[0] = port.nodeAddr;
  request[1] = functionCode;
  request[2] = highByte(startReg);
  request[3] = lowByte(startReg);
  request[4] = highByte(regCount);
  request[5] = lowByte(regCount);
  uint16_t requestCrc = modbusCrc16(request, 6);
  request[6] = lowByte(requestCrc);
  request[7] = highByte(requestCrc);

  clearRs485Rx(port);
  if (ENABLE_MODBUS_TRACE_LOGS) {
    logDebug(port.name, "TX " + hexBytes(request, sizeof(request)));
  }

  set485Transmit(port, true);
  port.serial->write(request, sizeof(request));
  port.serial->flush();
  delayMicroseconds(300);
  set485Transmit(port, false);

  const uint8_t expectedLen = 5 + regCount * 2;
  uint8_t response[80];
  uint8_t received = 0;
  uint32_t startMs = millis();
  while ((millis() - startMs) < timeoutMs && received < sizeof(response)) {
    while (port.serial->available() > 0 && received < sizeof(response)) {
      response[received++] = port.serial->read();
    }
    if (received >= expectedLen) {
      break;
    }
    delay(1);
  }

  for (uint8_t frameStart = 0; frameStart + expectedLen <= received; frameStart++) {
    if (response[frameStart] != port.nodeAddr ||
        response[frameStart + 1] != functionCode ||
        response[frameStart + 2] != regCount * 2) {
      continue;
    }

    uint8_t *frame = response + frameStart;
    uint16_t gotCrc = (uint16_t(frame[expectedLen - 1]) << 8) | frame[expectedLen - 2];
    uint16_t calcCrc = modbusCrc16(frame, expectedLen - 2);
    if (gotCrc != calcCrc) {
      logWarn(port.name, "CRC error RX=" + hexBytes(frame, expectedLen));
      continue;
    }

    for (uint16_t i = 0; i < regCount; i++) {
      uint8_t offset = 3 + i * 2;
      outRegs[i] = (uint16_t(frame[offset]) << 8) | frame[offset + 1];
    }
    port.lastReadOk = true;
    port.lastReadMs = millis();
    port.successCount++;
    if (result) {
      *result = port.name;
      *result += " read 0x";
      *result += String(functionCode, HEX);
      *result += " OK RX:";
      appendHexBytes(*result, frame, expectedLen);
    }
    return true;
  }

  if (millis() - port.lastWarnLogMs >= RS485_WARN_INTERVAL_MS) {
    String msg = "sensor timeout got=" + String(received) + "/" + String(expectedLen);
    if (received > 0) {
      msg += " RX=" + hexBytes(response, received);
    }
    logWarn(port.name, msg);
    port.lastWarnLogMs = millis();
  }
  port.lastReadOk = false;
  port.lastReadMs = millis();
  port.failCount++;
  if (result) {
    *result = port.name;
    *result += " read timeout";
  }
  return false;
}

static bool readHoldingRegisters(Rs485Port &port, uint16_t startReg, uint16_t regCount,
                                 uint32_t timeoutMs) {
  return readRegisters(port, 0x03, startReg, regCount, port.regs, timeoutMs);
}

static void beginPort(Rs485Port &port) {
  pinMode(port.deRePin, OUTPUT);
  set485Transmit(port, false);
  port.serial->begin(MODBUS_BAUD, SERIAL_8N1, port.rxPin, port.txPin);
  delay(30);
  clearRs485Rx(port);
  logInfo(port.name, "init baud=" + String(MODBUS_BAUD) +
                       " tx=IO" + String(port.txPin) +
                       " rx=IO" + String(port.rxPin) +
                       " deRe=IO" + String(port.deRePin));
}

void rs485Begin() {
  for (size_t i = 0; i < 2; i++) {
    if (rs485Ports[i].enabled) {
      beginPort(rs485Ports[i]);
      loadSavedConfig(i);
    }
  }
}

void rs485Poll() {
  uint32_t now = millis();
  tickSavedConfigApply();

  if (now - lastVibrationPollMs >= VIBRATION_POLL_INTERVAL_MS) {
    lastVibrationPollMs = now;
    if (rs485PollPort(OUTSIDE_VIBRATION_PORT)) {
      Rs485Port &port = rs485Ports[OUTSIDE_VIBRATION_PORT];
      uint16_t counter = port.regs[VIBRATION_COUNTER_REG_INDEX];
      uint16_t hallState = port.regs[HALL_FILTER_REG_INDEX];
      if (!vibrationCounterInitialized) {
        vibrationCounterInitialized = true;
        lastVibrationCounter = counter;
      } else if (counter != lastVibrationCounter) {
        uint16_t oldCounter = lastVibrationCounter;
        lastVibrationCounter = counter;
        logInfo("RS485-1", "outside vibration value " + String(oldCounter) + " -> " + String(counter));
      }

      if (!outsideHallInitialized) {
        outsideHallInitialized = true;
        lastOutsideHallState = hallState;
      } else if (hallState != lastOutsideHallState) {
        uint16_t oldHallState = lastOutsideHallState;
        lastOutsideHallState = hallState;
        logInfo("RS485-1", "outside hall value " + String(oldHallState) + " -> " + String(hallState));
      }
    }
  }

  if (now - lastDoorPollMs >= DOOR_POLL_INTERVAL_MS) {
    lastDoorPollMs = now;
    rs485PollPort(DOOR_SENSOR_PORT);
  }
}

bool rs485PollPort(uint8_t index) {
  if (index >= 2 || !rs485Ports[index].enabled) {
    return false;
  }
  readHoldingRegisters(rs485Ports[index], READ_START_REG, READ_REG_COUNT, RESPONSE_TIMEOUT_MS);
  return true;
}

bool rs485ReadRegisters(uint8_t portIndex, uint8_t functionCode, uint16_t startReg, uint16_t regCount,
                        uint16_t *outRegs, String &result) {
  if (portIndex >= 2 || outRegs == nullptr || regCount == 0 || regCount > MAX_MANUAL_READ_REGS) {
    result = "bad read params";
    return false;
  }
  return readRegisters(rs485Ports[portIndex], functionCode, startReg, regCount, outRegs, RESPONSE_TIMEOUT_MS, &result);
}

void rs485RefreshDoorState() {
  rs485PollPort(DOOR_SENSOR_PORT);
}

bool rs485WriteSingleRegister(uint8_t portIndex, uint8_t nodeAddr, uint16_t regAddr,
                              uint16_t value, String &result) {
  if (portIndex >= 2) {
    result = "invalid port";
    return false;
  }
  Rs485Port &port = rs485Ports[portIndex];
  uint8_t request[8];
  request[0] = nodeAddr;
  request[1] = 0x06;
  request[2] = highByte(regAddr);
  request[3] = lowByte(regAddr);
  request[4] = highByte(value);
  request[5] = lowByte(value);
  uint16_t requestCrc = modbusCrc16(request, 6);
  request[6] = lowByte(requestCrc);
  request[7] = highByte(requestCrc);

  clearRs485Rx(port);
  set485Transmit(port, true);
  port.serial->write(request, sizeof(request));
  port.serial->flush();
  delayMicroseconds(300);
  set485Transmit(port, false);

  uint8_t response[16];
  uint8_t received = 0;
  uint32_t startMs = millis();
  while ((millis() - startMs) < RESPONSE_TIMEOUT_MS && received < sizeof(request)) {
    while (port.serial->available() > 0 && received < sizeof(response)) {
      response[received++] = port.serial->read();
    }
    if (received >= sizeof(request)) {
      break;
    }
    delay(1);
  }

  result = port.name;
  result += " write 0x06 TX:";
  appendHexBytes(result, request, sizeof(request));
  for (uint8_t frameStart = 0; frameStart + sizeof(request) <= received; frameStart++) {
    uint8_t *frame = response + frameStart;
    if (frame[0] != nodeAddr || frame[1] != 0x06) {
      continue;
    }
    uint16_t gotCrc = (uint16_t(frame[7]) << 8) | frame[6];
    uint16_t calcCrc = modbusCrc16(frame, 6);
    if (gotCrc != calcCrc) {
      result += " CRC error RX:";
      appendHexBytes(result, frame, sizeof(request));
      logWarn(port.name, result);
      return false;
    }
    result += " OK RX:";
    appendHexBytes(result, frame, sizeof(request));
    logInfo(port.name, result);
    return true;
  }

  result += " timeout";
  if (received > 0) {
    result += " RX:";
    appendHexBytes(result, response, received);
  }
  logWarn(port.name, result);
  return false;
}

bool rs485WriteMultipleRegisters(uint8_t portIndex, uint8_t nodeAddr, uint16_t startReg,
                                 const uint16_t *values, uint16_t count, String &result) {
  if (portIndex >= 2 || values == nullptr || count == 0 || count > 16) {
    result = "bad write multiple params";
    return false;
  }
  Rs485Port &port = rs485Ports[portIndex];
  uint8_t request[9 + 16 * 2];
  uint8_t byteCount = count * 2;
  uint8_t len = 7 + byteCount + 2;
  request[0] = nodeAddr;
  request[1] = 0x10;
  request[2] = highByte(startReg);
  request[3] = lowByte(startReg);
  request[4] = highByte(count);
  request[5] = lowByte(count);
  request[6] = byteCount;
  for (uint16_t i = 0; i < count; i++) {
    request[7 + i * 2] = highByte(values[i]);
    request[8 + i * 2] = lowByte(values[i]);
  }
  uint16_t crc = modbusCrc16(request, len - 2);
  request[len - 2] = lowByte(crc);
  request[len - 1] = highByte(crc);

  clearRs485Rx(port);
  set485Transmit(port, true);
  port.serial->write(request, len);
  port.serial->flush();
  delayMicroseconds(300);
  set485Transmit(port, false);

  uint8_t response[16];
  uint8_t received = 0;
  uint32_t startMs = millis();
  while ((millis() - startMs) < RESPONSE_TIMEOUT_MS && received < 8) {
    while (port.serial->available() > 0 && received < sizeof(response)) {
      response[received++] = port.serial->read();
    }
    if (received >= 8) {
      break;
    }
    delay(1);
  }

  result = port.name;
  result += " write 0x10 TX:";
  appendHexBytes(result, request, len);
  for (uint8_t frameStart = 0; frameStart + 8 <= received; frameStart++) {
    uint8_t *frame = response + frameStart;
    if (frame[0] != nodeAddr || frame[1] != 0x10 ||
        frame[2] != highByte(startReg) || frame[3] != lowByte(startReg) ||
        frame[4] != highByte(count) || frame[5] != lowByte(count)) {
      continue;
    }
    uint16_t gotCrc = (uint16_t(frame[7]) << 8) | frame[6];
    uint16_t calcCrc = modbusCrc16(frame, 6);
    if (gotCrc != calcCrc) {
      result += " CRC error RX:";
      appendHexBytes(result, frame, 8);
      logWarn(port.name, result);
      return false;
    }
    result += " OK RX:";
    appendHexBytes(result, frame, 8);
    logInfo(port.name, result);
    return true;
  }

  result += " timeout";
  if (received > 0) {
    result += " RX:";
    appendHexBytes(result, response, received);
  }
  logWarn(port.name, result);
  return false;
}

static bool applySavedConfig(uint8_t index, String &result) {
  if (index >= 2 || !savedConfigs[index].valid) {
    result = "no saved config";
    return false;
  }
  SavedSensorConfig &cfg = savedConfigs[index];
  uint8_t candidates[4];
  uint8_t candidateCount = 0;
  uint8_t desiredAddr = (uint8_t)constrain((int)cfg.values[0], 1, 247);

  if (!sameAddressAlreadyTried(candidates, candidateCount, cfg.writeAddr)) {
    candidates[candidateCount++] = cfg.writeAddr;
  }
  if (!sameAddressAlreadyTried(candidates, candidateCount, desiredAddr)) {
    candidates[candidateCount++] = desiredAddr;
  }
  if (!sameAddressAlreadyTried(candidates, candidateCount, rs485Ports[index].nodeAddr)) {
    candidates[candidateCount++] = rs485Ports[index].nodeAddr;
  }
  if (!sameAddressAlreadyTried(candidates, candidateCount, SENSOR_DEFAULT_ADDR)) {
    candidates[candidateCount++] = SENSOR_DEFAULT_ADDR;
  }

  for (uint8_t i = 0; i < candidateCount; i++) {
    uint8_t addr = candidates[i];
    bool ok = rs485WriteMultipleRegisters(index, addr, SENSOR_CONFIG_START_REG,
                                          cfg.values, SENSOR_CONFIG_REG_COUNT, result);
    if (ok) {
      cfg.writeAddr = addr;
      cfg.applied = true;
      rs485Ports[index].nodeAddr = desiredAddr;
      persistSavedConfig(index);
      logInfo(rs485Ports[index].name, "saved sensor config applied via addr=" + String(addr) +
                                    " targetAddr=" + String(desiredAddr));
      return true;
    }
  }
  return false;
}

static void tickSavedConfigApply() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < 2; i++) {
    SavedSensorConfig &cfg = savedConfigs[i];
    if (!cfg.valid || cfg.applied || cfg.attempts >= CONFIG_APPLY_MAX_ATTEMPTS) continue;
    if (now < cfg.nextApplyMs) continue;
    cfg.attempts++;
    String result;
    if (!applySavedConfig(i, result)) {
      cfg.nextApplyMs = now + CONFIG_APPLY_RETRY_MS;
      logWarn(rs485Ports[i].name, "saved sensor config apply failed attempt=" +
                               String(cfg.attempts) + " result=" + result);
    }
  }
}

bool rs485SaveSensorConfig(uint8_t portIndex, uint8_t writeAddr, const uint16_t *values, uint16_t count) {
  if (portIndex >= 2 || values == nullptr || count != SENSOR_CONFIG_REG_COUNT) {
    return false;
  }
  SavedSensorConfig &cfg = savedConfigs[portIndex];
  cfg.valid = true;
  cfg.applied = true;
  cfg.writeAddr = writeAddr;
  cfg.attempts = 0;
  cfg.nextApplyMs = 0;
  for (uint8_t i = 0; i < SENSOR_CONFIG_REG_COUNT; i++) {
    cfg.values[i] = values[i];
  }
  rs485Ports[portIndex].nodeAddr = (uint8_t)constrain((int)values[0], 1, 247);
  persistSavedConfig(portIndex);
  logInfo(rs485Ports[portIndex].name, "sensor config saved targetAddr=" + String(values[0]) +
                                  " writeAddr=" + String(writeAddr));
  return true;
}

String rs485StatusJson(uint8_t index) {
  if (index >= 2) {
    return F("{\"error\":\"bad_port\"}");
  }
  Rs485Port &port = rs485Ports[index];
  String json = F("{\"name\":\"");
  json += port.name;
  json += F("\",\"enabled\":");
  json += port.enabled ? F("true") : F("false");
  json += F(",\"nodeAddr\":");
  json += port.nodeAddr;
  json += F(",\"baud\":");
  json += MODBUS_BAUD;
  json += F(",\"txPin\":");
  json += port.txPin;
  json += F(",\"rxPin\":");
  json += port.rxPin;
  json += F(",\"deRePin\":");
  json += port.deRePin;
  json += F(",\"lastReadOk\":");
  json += port.lastReadOk ? F("true") : F("false");
  json += F(",\"lastReadMs\":");
  json += port.lastReadMs;
  json += F(",\"successCount\":");
  json += port.successCount;
  json += F(",\"failCount\":");
  json += port.failCount;
  json += F(",\"savedConfig\":");
  json += savedConfigs[index].valid ? F("true") : F("false");
  json += F(",\"savedConfigApplied\":");
  json += savedConfigs[index].applied ? F("true") : F("false");
  json += F(",\"savedConfigAttempts\":");
  json += savedConfigs[index].attempts;
  json += F(",\"registers\":[");
  for (uint8_t i = 0; i < READ_REG_COUNT; i++) {
    if (i > 0) {
      json += ',';
    }
    json += port.regs[i];
  }
  json += F("]}");
  if (index == OUTSIDE_VIBRATION_PORT) {
    json.remove(json.length() - 1);
    json += F(",\"role\":\"outside_vibration\",\"vibrationCounter\":");
    json += port.regs[VIBRATION_COUNTER_REG_INDEX];
    json += F(",\"lastSeenCounter\":");
    json += lastVibrationCounter;
    json += F(",\"hallClosed\":");
    json += (port.lastReadOk && port.regs[HALL_FILTER_REG_INDEX] == 1) ? F("true") : F("false");
    json += F(",\"hallValue\":");
    json += port.regs[HALL_FILTER_REG_INDEX];
    json += F(",\"lastSeenHall\":");
    json += lastOutsideHallState;
    json += F("}");
  } else if (index == DOOR_SENSOR_PORT) {
    json.remove(json.length() - 1);
    json += F(",\"role\":\"door_state\",\"hallClosed\":");
    json += (port.lastReadOk && port.regs[DOOR_HALL_REG_INDEX] == 1) ? F("true") : F("false");
    json += F(",\"doorVibration\":");
    json += port.regs[DOOR_VIBRATION_REG_INDEX];
    json += F("}");
  }
  return json;
}

String rs485ConfigJson(uint8_t index) {
  if (index >= 2) {
    return F("{\"error\":\"bad_port\"}");
  }
  uint16_t cfg[SENSOR_CONFIG_REG_COUNT] = {0};
  String result;
  bool ok = rs485ReadRegisters(index, 0x03, SENSOR_CONFIG_START_REG, SENSOR_CONFIG_REG_COUNT, cfg, result);
  String json = F("{\"ok\":");
  json += ok ? F("true") : F("false");
  json += F(",\"name\":\"");
  json += rs485Ports[index].name;
  json += F("\",\"result\":\"");
  for (uint16_t i = 0; i < result.length(); i++) {
    char c = result[i];
    if (c == '"' || c == '\\') json += '\\';
    json += c;
  }
  json += F("\",\"config\":{\"modbusAddr\":");
  json += cfg[0];
  json += F(",\"magHoldMs\":");
  json += cfg[1];
  json += F(",\"magReleaseMs\":");
  json += cfg[2];
  json += F(",\"vibrationWindowMs\":");
  json += cfg[3];
  json += F(",\"vibrationThreshold\":");
  json += cfg[4];
  json += F(",\"runHoldMs\":");
  json += cfg[5];
  json += F(",\"ledEnabled\":");
  json += cfg[6] ? F("true") : F("false");
  json += F("},\"raw\":[");
  for (uint8_t i = 0; i < SENSOR_CONFIG_REG_COUNT; i++) {
    if (i > 0) json += ',';
    json += cfg[i];
  }
  json += F("]}");
  return json;
}
