#include "face_fp001.h"

#include <Preferences.h>

#include "config_pins.h"
#include "face_wake_input.h"
#include "logger.h"

static const uint32_t FACE_COMMAND_TIMEOUT_MS = 8000;
static const uint32_t FACE_AUTO_RETRY_DELAY_MS = 2000;
static const uint32_t FACE_SUCCESS_COOLDOWN_MS = 12000;
static const uint8_t FACE_AUTO_VERIFY_TIMEOUT_SEC = 3;

static const uint8_t MID_REPLY = 0x00;
static const uint8_t MID_NOTE = 0x01;
static const uint8_t MID_RESET = 0x10;
static const uint8_t MID_GETSTATUS = 0x11;
static const uint8_t MID_VERIFY = 0x12;
static const uint8_t MID_ENROLL = 0x13;
static const uint8_t MID_ENROLL_SINGLE = 0x1D;
static const uint8_t MID_DELUSER = 0x20;
static const uint8_t MID_DELUSER_HAND = 0x80;
static const uint8_t MID_DELALL = 0x21;
static const uint8_t MID_GET_ALL_USERID = 0x24;
static const uint8_t MID_LED_CONTROL = 0x90;

static const uint8_t FACE_DIR_FACE_SINGLE = 0x00;
static const uint8_t FACE_DIR_MIDDLE = 0x01;
static const uint8_t FACE_DIR_RIGHT = 0x02;
static const uint8_t FACE_DIR_LEFT = 0x04;
static const uint8_t FACE_DIR_DOWN = 0x08;
static const uint8_t FACE_DIR_UP = 0x10;
static const uint8_t FACE_DIR_HAND = 0xFD;
static const uint8_t FACE_ENROLL_STEP_COUNT = 5;
static const uint32_t FACE_ENROLL_STEP_DELAY_MS = 600;
static const uint8_t NID_READY = 0x00;
static const uint8_t NID_FACE_STATE = 0x01;
static const uint8_t USER_LIST_FACE_LONG = 0x00;
static const uint8_t USER_LIST_HAND_SHORT = 0x02;

static const uint8_t FACE_ENROLL_DIRS[FACE_ENROLL_STEP_COUNT] = {
    FACE_DIR_MIDDLE, FACE_DIR_RIGHT, FACE_DIR_LEFT, FACE_DIR_DOWN, FACE_DIR_UP};

struct Fp001Frame {
  uint8_t msgId;
  uint16_t size;
  uint8_t data[512];
  uint8_t checksum;
  bool checksumOk;
};

struct FaceEnrollSequence {
  bool active;
  bool waitingToSend;
  bool admin;
  uint8_t step;
  uint8_t timeoutSec;
  uint32_t nextSendMs;
  char name[33];
};

enum FpParserState {
  FP_WAIT_EF,
  FP_WAIT_AA,
  FP_READ_MSGID,
  FP_READ_SIZE_H,
  FP_READ_SIZE_L,
  FP_READ_DATA,
  FP_READ_CHECKSUM
};

static HardwareSerial FaceSerial(0);
static FaceStatus faceStatus = {false, false, FACE_BAUD, 0, 0, 0, 0, 0, 0, 0,
                                -1, -1, -1, -1, {0}, -1, {0}, 0, {0}, 0, FACE_MODULE_UNKNOWN,
                                FACE_MODE_IDLE, false, false, 0, 0, false, false, false};
static FaceEnrollSequence enrollSeq = {false, false, false, 0, 20, 0, {0}};
static FpParserState parserState = FP_WAIT_EF;
static Fp001Frame frame = {0, 0, {0}, 0, false};
static uint16_t dataIndex = 0;
static uint32_t nextAutoVerifyMs = 0;
static uint32_t lastSuccessMs = 0;
static uint32_t ledIndicatorUntilMs = 0;
static uint32_t autoVerifyWindowUntilMs = 0;
static uint32_t currentCommandTimeoutMs = FACE_COMMAND_TIMEOUT_MS;
static bool ledReady = false;
static bool userListRefreshAll = false;
static uint8_t userListPendingFmt = USER_LIST_FACE_LONG;
static FaceVerifySuccessCallback verifySuccessCallback = nullptr;

static void closeAutoVerifyWindow(const char *reason) {
  if (autoVerifyWindowUntilMs == 0) return;
  autoVerifyWindowUntilMs = 0;
  nextAutoVerifyMs = millis() + FACE_AUTO_RETRY_DELAY_MS;
  logInfo("FACE", String("auto verify window closed: ") + reason);
}

static void saveAutoVerifyPreference(bool enabled) {
  Preferences prefs;
  prefs.begin("face", false);
  prefs.putBool("autoVerify", enabled);
  prefs.end();
}

static bool loadAutoVerifyPreference() {
  Preferences prefs;
  prefs.begin("face", true);
  bool enabled = prefs.getBool("autoVerify", true);
  prefs.end();
  return enabled;
}

const char *faceModuleStatusText(uint8_t status) {
  switch (status) {
    case FACE_MODULE_STANDBY: return "STANDBY";
    case FACE_MODULE_BUSY: return "BUSY";
    case FACE_MODULE_ERROR: return "ERROR";
    case FACE_MODULE_INVALID: return "INVALID";
    case FACE_MODULE_OTA: return "OTA";
    default: return "UNKNOWN";
  }
}

const char *faceModeText(uint8_t mode) {
  switch (mode) {
    case FACE_MODE_IDLE: return "IDLE";
    case FACE_MODE_MANUAL: return "MANUAL";
    case FACE_MODE_AUTO_VERIFY: return "AUTO_VERIFY";
    case FACE_MODE_ENROLLING: return "ENROLLING";
    case FACE_MODE_VERIFYING: return "VERIFYING";
    case FACE_MODE_COOLDOWN: return "COOLDOWN";
    default: return "UNKNOWN";
  }
}

const char *faceResultText(uint8_t result) {
  switch (result) {
    case 0x00: return "SUCCESS";
    case 0x01: return "REJECTED";
    case 0x02: return "ABORTED";
    case 0x04: return "CAMERA_FAIL";
    case 0x05: return "UNKNOWN_FAIL";
    case 0x06: return "INVALID_PARAM";
    case 0x07: return "NO_MEMORY";
    case 0x08: return "UNKNOWN_USER";
    case 0x09: return "MAX_USER";
    case 0x0A: return "FACE_ALREADY_ENROLLED";
    case 0x0C: return "LIVENESS_FAIL";
    case 0x0D: return "TIMEOUT";
    case 0x0E: return "AUTH_FAIL";
    case 0x13: return "READ_FILE_FAIL";
    case 0x14: return "WRITE_FILE_FAIL";
    case 0x15: return "NO_ENCRYPT";
    case 0x17: return "NO_RGBIMAGE";
    case 0xEF: return "HAND_VERIFY_FAIL";
    case 0xF0: return "NO_CAMERA";
    case 0xF1: return "HAND_ALREADY_ENROLLED";
    default: return "UNKNOWN_RESULT";
  }
}

const char *faceUnlockStatusText(int status) {
  switch (status) {
    case 0xC8:
      return "FACE_OK";
    case 0xCC:
      return "FACE_EYE_CLOSED_OK";
    case 0xFA:
      return "HAND_OK";
    default:
      return "UNKNOWN_UNLOCK_STATUS";
  }
}

static const char *faceEnrollDirectionText(uint8_t direction) {
  switch (direction) {
    case FACE_DIR_MIDDLE:
    case FACE_DIR_FACE_SINGLE:
      return "FRONT";
    case FACE_DIR_RIGHT:
      return "RIGHT";
    case FACE_DIR_LEFT:
      return "LEFT";
    case FACE_DIR_DOWN:
      return "DOWN";
    case FACE_DIR_UP:
      return "UP";
    case FACE_DIR_HAND:
      return "HAND";
    default:
      return "UNKNOWN";
  }
}

const char *faceStateText(int state) {
  switch (state) {
    case 0x0000: return "FACE_NORMAL";
    case 0x0001: return "NO_FACE";
    case 0x0006: return "FACE_TOO_FAR";
    case 0x0007: return "FACE_TOO_CLOSE";
    case 0x0080: return "HAND_NORMAL";
    case 0x0081: return "HAND_TOO_FAR";
    case 0x0082: return "HAND_TOO_CLOSE";
    case 0x0083: return "HAND_TOO_UP";
    case 0x0084: return "HAND_TOO_DOWN";
    case 0x0085: return "HAND_TOO_LEFT";
    case 0x0086: return "HAND_TOO_RIGHT";
    default: return "UNKNOWN_FACE_STATE";
  }
}

static void appendHexByte(String &out, uint8_t value) {
  const char *hex = "0123456789ABCDEF";
  out += hex[(value >> 4) & 0x0F];
  out += hex[value & 0x0F];
}

static uint8_t checksum(uint8_t msgId, uint16_t size, const uint8_t *data);

static String frameHex(uint8_t msgId, const uint8_t *data, uint16_t size) {
  String out;
  out.reserve((size + 6) * 3);
  uint8_t head[] = {0xEF, 0xAA, msgId, (uint8_t)((size >> 8) & 0xFF), (uint8_t)(size & 0xFF)};
  for (uint8_t i = 0; i < sizeof(head); i++) {
    if (out.length()) out += ' ';
    appendHexByte(out, head[i]);
  }
  for (uint16_t i = 0; i < size; i++) {
    out += ' ';
    appendHexByte(out, data[i]);
  }
  out += ' ';
  appendHexByte(out, checksum(msgId, size, data));
  return out;
}

static int16_t readI16LE(const uint8_t *p) {
  return (int16_t)(((uint16_t)p[1] << 8) | p[0]);
}

static uint8_t checksum(uint8_t msgId, uint16_t size, const uint8_t *data) {
  uint8_t c = 0;
  c ^= msgId;
  c ^= (size >> 8) & 0xFF;
  c ^= size & 0xFF;
  for (uint16_t i = 0; i < size; i++) {
    c ^= data[i];
  }
  return c;
}

static void sendFrame(uint8_t msgId, const uint8_t *data, uint16_t size) {
  uint8_t check = checksum(msgId, size, data);
  FaceSerial.write(0xEF);
  FaceSerial.write(0xAA);
  FaceSerial.write(msgId);
  FaceSerial.write((size >> 8) & 0xFF);
  FaceSerial.write(size & 0xFF);
  for (uint16_t i = 0; i < size; i++) {
    FaceSerial.write(data[i]);
  }
  FaceSerial.write(check);
  FaceSerial.flush();
  faceStatus.txBytes += 6 + size;
}

static void sendCommand(uint8_t mid, const uint8_t *data, uint16_t size) {
  faceStatus.pendingMid = mid;
  faceStatus.commandStartedMs = millis();
  currentCommandTimeoutMs = FACE_COMMAND_TIMEOUT_MS;
  sendFrame(mid, data, size);
}

static void sendCommandWithTimeout(uint8_t mid, const uint8_t *data, uint16_t size, uint32_t timeoutMs) {
  faceStatus.pendingMid = mid;
  faceStatus.commandStartedMs = millis();
  currentCommandTimeoutMs = timeoutMs < FACE_COMMAND_TIMEOUT_MS ? FACE_COMMAND_TIMEOUT_MS : timeoutMs;
  sendFrame(mid, data, size);
}

static void sendLedRaw(uint8_t color, bool on) {
  uint8_t data[2] = {color, (uint8_t)(on ? 0x00 : 0x01)};
  sendFrame(MID_LED_CONTROL, data, sizeof(data));
}

static void setLedState(bool green, bool red, bool white) {
  if (!ledReady) {
    faceStatus.ledGreenOn = green;
    faceStatus.ledRedOn = red;
    faceStatus.ledWhiteOn = white;
    return;
  }
  if (faceStatus.ledGreenOn != green) {
    faceStatus.ledGreenOn = green;
    sendLedRaw(0, green);
  }
  if (faceStatus.ledRedOn != red) {
    faceStatus.ledRedOn = red;
    sendLedRaw(1, red);
  }
  if (faceStatus.ledWhiteOn != white) {
    faceStatus.ledWhiteOn = white;
    sendLedRaw(2, white);
  }
}

static void showIdleLed() {
  ledIndicatorUntilMs = 0;
  setLedState(false, false, false);
}

static void enableLedAndShowIdle() {
  ledReady = true;
  faceStatus.ledGreenOn = true;
  faceStatus.ledRedOn = true;
  faceStatus.ledWhiteOn = true;
  showIdleLed();
}

static void showResultLed(bool success) {
  ledIndicatorUntilMs = millis() + (success ? 5000UL : 2500UL);
  setLedState(success, !success, false);
}

void faceGetStatus() {
  sendCommand(MID_GETSTATUS, nullptr, 0);
}

void faceReset() {
  sendCommand(MID_RESET, nullptr, 0);
}

void faceVerify(uint8_t timeoutSec) {
  uint8_t data[2] = {0x00, timeoutSec};
  faceStatus.mode = FACE_MODE_VERIFYING;
  setLedState(false, false, true);
  sendCommandWithTimeout(MID_VERIFY, data, sizeof(data), ((uint32_t)timeoutSec + 5UL) * 1000UL);
}

static void fillName(uint8_t *dst32, const char *name) {
  memset(dst32, 0, 32);
  if (name) {
    strncpy((char *)dst32, name, 31);
  }
}

static void enrollCommand(uint8_t mid, const char *name, bool admin, uint8_t direction, uint8_t timeoutSec) {
  uint8_t data[35];
  memset(data, 0, sizeof(data));
  data[0] = admin ? 0x01 : 0x00;
  fillName(&data[1], name);
  data[33] = direction;
  data[34] = timeoutSec;
  faceStatus.mode = FACE_MODE_ENROLLING;
  logInfo("FACE", "enroll tx " + frameHex(mid, data, sizeof(data)));
  sendCommandWithTimeout(mid, data, sizeof(data), ((uint32_t)timeoutSec + 5UL) * 1000UL);
}

void faceEnrollSingleFace(const char *name, bool admin, uint8_t timeoutSec) {
  logInfo("FACE", "single face enroll timeoutSec=" + String(timeoutSec));
  enrollCommand(MID_ENROLL_SINGLE, name, admin, FACE_DIR_FACE_SINGLE, timeoutSec);
}

void faceEnrollHand(const char *name, bool admin, uint8_t timeoutSec) {
  logInfo("FACE", "hand vein enroll timeoutSec=" + String(timeoutSec));
  enrollCommand(MID_ENROLL_SINGLE, name, admin, FACE_DIR_HAND, timeoutSec);
}

void faceLedControl(uint8_t color, bool on) {
  if (color == 0) {
    faceStatus.ledGreenOn = on;
  } else if (color == 1) {
    faceStatus.ledRedOn = on;
  } else if (color == 2) {
    faceStatus.ledWhiteOn = on;
  }
  ledIndicatorUntilMs = 0;
  sendLedRaw(color, on);
}

void faceDeleteUser(uint16_t id) {
  bool hand = id >= 501;
  uint8_t data[3] = {(uint8_t)((id >> 8) & 0xFF), (uint8_t)(id & 0xFF), (uint8_t)(hand ? 1 : 0)};
  faceStatus.mode = FACE_MODE_MANUAL;
  sendCommand(hand ? MID_DELUSER_HAND : MID_DELUSER, data, sizeof(data));
}

void faceDeleteAllUsers() {
  uint8_t data[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
  faceStatus.mode = FACE_MODE_MANUAL;
  sendCommand(MID_DELALL, data, sizeof(data));
}

void faceGetAllUserIds() {
  uint8_t fmt = USER_LIST_FACE_LONG;
  userListRefreshAll = true;
  userListPendingFmt = fmt;
  sendCommand(MID_GET_ALL_USERID, &fmt, sizeof(fmt));
}

static void parseUserIds(const Fp001Frame &f) {
  uint16_t *ids = userListPendingFmt == USER_LIST_HAND_SHORT ? faceStatus.handUserIds : faceStatus.userIds;
  uint16_t *countTarget = userListPendingFmt == USER_LIST_HAND_SHORT ? &faceStatus.handUserCount : &faceStatus.userCount;
  *countTarget = 0;
  if (f.size <= 2) return;

  if (userListPendingFmt == USER_LIST_HAND_SHORT) {
    if (f.size <= 4) return;
    uint16_t reportedCount = ((uint16_t)f.data[2] << 8) | f.data[3];
    uint16_t found = 0;
    uint16_t bitmapBytes = f.size - 4;
    for (uint16_t byteIndex = 0; byteIndex < bitmapBytes && found < 128 && found < reportedCount; byteIndex++) {
      uint8_t bits = f.data[4 + byteIndex];
      for (uint8_t bit = 0; bit < 8 && found < 128 && found < reportedCount; bit++) {
        if (bits & (1 << bit)) {
          ids[found++] = 501 + byteIndex * 8 + bit;
        }
      }
    }
    *countTarget = found;
    return;
  }

  uint16_t payloadBytes = f.size - 2;
  uint16_t offset = 2;
  if (payloadBytes >= 2 && payloadBytes == (((uint16_t)f.data[2] << 8) | f.data[3]) * 2 + 2) {
    offset = 4;
    payloadBytes -= 2;
  }
  uint16_t count = payloadBytes / 2;
  if (count > 128) count = 128;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pos = offset + i * 2;
    faceStatus.userIds[i] = ((uint16_t)f.data[pos] << 8) | f.data[pos + 1];
  }
  faceStatus.userCount = count;
}

static void parseVerifySuccess(const Fp001Frame &f) {
  faceStatus.lastUnlockStatus = -1;
  if (f.size >= 4) {
    faceStatus.lastUserId = ((uint16_t)f.data[2] << 8) | f.data[3];
  }
  if (f.size >= 36) {
    memset(faceStatus.lastUserName, 0, sizeof(faceStatus.lastUserName));
    memcpy(faceStatus.lastUserName, &f.data[4], 32);
  }
  if (f.size >= 38) {
    faceStatus.lastUnlockStatus = f.data[37];
  }
  lastSuccessMs = millis();
  if (verifySuccessCallback) {
    verifySuccessCallback(faceStatus.lastUserId, faceStatus.lastUserName,
                          faceUnlockStatusText(faceStatus.lastUnlockStatus));
  }
}

static void setModeAfterReply(uint8_t replyMid, uint8_t result) {
  if (replyMid == MID_VERIFY) {
    if (faceStatus.autoVerifyEnabled) {
      closeAutoVerifyWindow(result == 0x00 ? "success" : "finished");
      faceStatus.mode = result == 0x00 ? FACE_MODE_COOLDOWN : FACE_MODE_IDLE;
    } else {
      faceStatus.mode = FACE_MODE_IDLE;
    }
    return;
  }
  if (replyMid == MID_ENROLL && enrollSeq.active) {
    faceStatus.mode = FACE_MODE_ENROLLING;
    return;
  }
  if (replyMid == MID_ENROLL_SINGLE || replyMid == MID_ENROLL || replyMid == MID_RESET ||
      replyMid == MID_DELUSER || replyMid == MID_DELALL || replyMid == MID_LED_CONTROL) {
    faceStatus.mode = FACE_MODE_IDLE;
  }
}

static void sendCurrentEnrollStep() {
  if (!enrollSeq.active || enrollSeq.step >= FACE_ENROLL_STEP_COUNT) return;
  uint8_t direction = FACE_ENROLL_DIRS[enrollSeq.step];
  faceStatus.enroll5Step = enrollSeq.step + 1;
  faceStatus.enroll5Direction = direction;
  enrollSeq.waitingToSend = false;
  enrollCommand(MID_ENROLL, enrollSeq.name, enrollSeq.admin, direction, enrollSeq.timeoutSec);
  logInfo("FACE", "5-step enroll sent step=" + String(enrollSeq.step + 1) +
                  " direction=" + faceEnrollDirectionText(direction));
}

void faceStartEnrollSequence(const char *name, bool admin, uint8_t timeoutSec) {
  memset(&enrollSeq, 0, sizeof(enrollSeq));
  enrollSeq.active = true;
  enrollSeq.admin = admin;
  enrollSeq.timeoutSec = timeoutSec;
  strncpy(enrollSeq.name, name && name[0] ? name : "test001", sizeof(enrollSeq.name) - 1);
  faceStatus.mode = FACE_MODE_ENROLLING;
  faceStatus.enroll5Active = true;
  faceStatus.enroll5Step = 1;
  sendCurrentEnrollStep();
}

static void handleEnrollReply(uint8_t result, const Fp001Frame &f) {
  if (!enrollSeq.active) return;
  if (result != 0x00) {
    logWarn("FACE", "5-step enroll failed step=" + String(faceStatus.enroll5Step) +
                    " direction=" + faceEnrollDirectionText(faceStatus.enroll5Direction) +
                    " result=" + faceResultText(result));
    enrollSeq.active = false;
    enrollSeq.waitingToSend = false;
    faceStatus.enroll5Active = false;
    faceStatus.enroll5Direction = 0;
    showResultLed(false);
    return;
  }
  uint16_t userId = 0xFFFF;
  if (f.size >= 4) {
    userId = ((uint16_t)f.data[2] << 8) | f.data[3];
  }
  logInfo("FACE", "5-step enroll reply step=" + String(faceStatus.enroll5Step) +
                  " direction=" + faceEnrollDirectionText(faceStatus.enroll5Direction) +
                  " userId=" + String(userId));
  if (userId != 0xFFFF) {
    enrollSeq.active = false;
    enrollSeq.waitingToSend = false;
    faceStatus.enroll5Active = false;
    faceStatus.enroll5Direction = 0;
    showResultLed(true);
    return;
  }
  enrollSeq.step++;
  faceStatus.enroll5Step = enrollSeq.step + 1;
  if (enrollSeq.step >= FACE_ENROLL_STEP_COUNT) {
    enrollSeq.waitingToSend = false;
    enrollSeq.nextSendMs = millis() + FACE_COMMAND_TIMEOUT_MS;
    return;
  }
  enrollSeq.waitingToSend = true;
  enrollSeq.nextSendMs = millis() + FACE_ENROLL_STEP_DELAY_MS;
}

static void handleReply(const Fp001Frame &f) {
  if (f.size < 2) {
    logWarn("FACE", "short reply dropped");
    return;
  }
  uint8_t replyMid = f.data[0];
  uint8_t result = f.data[1];
  bool matchedPending = faceStatus.pendingMid == replyMid;
  if (replyMid != MID_LED_CONTROL) {
    faceStatus.lastResultCode = result;
  }
  if (matchedPending) {
    faceStatus.pendingMid = 0;
  }
  logInfo("FACE", "reply mid=0x" + String(replyMid, HEX) + " result=" + faceResultText(result));
  if (replyMid == MID_GETSTATUS && result == 0x00 && f.size >= 3) {
    switch (f.data[2]) {
      case 0: faceStatus.moduleStatus = FACE_MODULE_STANDBY; break;
      case 1: faceStatus.moduleStatus = FACE_MODULE_BUSY; break;
      case 2: faceStatus.moduleStatus = FACE_MODULE_ERROR; break;
      case 3: faceStatus.moduleStatus = FACE_MODULE_INVALID; break;
      case 4: faceStatus.moduleStatus = FACE_MODULE_OTA; break;
      default: faceStatus.moduleStatus = FACE_MODULE_UNKNOWN; break;
    }
    faceStatus.online = true;
    if (!ledReady) {
      enableLedAndShowIdle();
    }
  }
  if (replyMid == MID_VERIFY) {
    showResultLed(result == 0x00);
    if (result == 0x00) {
      parseVerifySuccess(f);
    } else {
      faceStatus.lastUnlockStatus = -1;
    }
  }
  if ((replyMid == MID_ENROLL_SINGLE || replyMid == MID_ENROLL) && result == 0x00 && f.size >= 4) {
    faceStatus.lastUserId = ((uint16_t)f.data[2] << 8) | f.data[3];
  }
  if ((replyMid == MID_ENROLL_SINGLE || replyMid == MID_ENROLL) && result == 0x00 && f.size >= 5) {
    faceStatus.lastEnrollDirection = f.data[4];
    logInfo("FACE", "enroll reply userId=" + String(faceStatus.lastUserId) +
                    " direction=0x" + String(faceStatus.lastEnrollDirection, HEX) +
                    " " + faceEnrollDirectionText((uint8_t)faceStatus.lastEnrollDirection));
  }
  if (replyMid == MID_ENROLL_SINGLE) {
    showResultLed(result == 0x00);
  } else if (replyMid == MID_ENROLL && (!enrollSeq.active || result != 0x00)) {
    showResultLed(result == 0x00);
  }
  if (replyMid == MID_ENROLL) handleEnrollReply(result, f);
  if (replyMid == MID_GET_ALL_USERID && result == 0x00) parseUserIds(f);
  if (replyMid == MID_GET_ALL_USERID && result != 0x00) {
    userListRefreshAll = false;
  }
  if (replyMid == MID_GET_ALL_USERID && userListRefreshAll &&
      userListPendingFmt == USER_LIST_FACE_LONG) {
    uint8_t fmt = USER_LIST_HAND_SHORT;
    userListPendingFmt = fmt;
    sendCommand(MID_GET_ALL_USERID, &fmt, sizeof(fmt));
    return;
  }
  if (replyMid == MID_GET_ALL_USERID && userListPendingFmt == USER_LIST_HAND_SHORT) {
    userListRefreshAll = false;
  }
  if (matchedPending || replyMid == MID_VERIFY || replyMid == MID_GET_ALL_USERID) {
    setModeAfterReply(replyMid, result);
  }
}

static void handleNote(const Fp001Frame &f) {
  if (f.size < 1) return;
  uint8_t nid = f.data[0];
  if (nid == NID_READY) {
    faceStatus.online = true;
    logInfo("FACE", "note ready");
    if (!ledReady && faceStatus.pendingMid == 0) {
      enableLedAndShowIdle();
    }
    return;
  }
  if (nid == NID_FACE_STATE && f.size >= 17) {
    int previousState = faceStatus.lastFaceState;
    faceStatus.lastFaceState = readI16LE(&f.data[1]);
    if (faceStatus.lastFaceState != previousState) {
      logInfo("FACE", "state=" + String(faceStateText(faceStatus.lastFaceState)));
    }
    return;
  }
}

static void handleFrame(const Fp001Frame &f) {
  faceStatus.frameCount++;
  if (!f.checksumOk) {
    faceStatus.badFrameCount++;
    logWarn("FACE", "bad checksum");
    return;
  }
  if (f.msgId == MID_REPLY) handleReply(f);
  else if (f.msgId == MID_NOTE) handleNote(f);
}

static void consumeByte(uint8_t b) {
  switch (parserState) {
    case FP_WAIT_EF:
      if (b == 0xEF) parserState = FP_WAIT_AA;
      break;
    case FP_WAIT_AA:
      parserState = (b == 0xAA) ? FP_READ_MSGID : FP_WAIT_EF;
      break;
    case FP_READ_MSGID:
      frame.msgId = b;
      parserState = FP_READ_SIZE_H;
      break;
    case FP_READ_SIZE_H:
      frame.size = ((uint16_t)b << 8);
      parserState = FP_READ_SIZE_L;
      break;
    case FP_READ_SIZE_L:
      frame.size |= b;
      dataIndex = 0;
      if (frame.size > sizeof(frame.data)) {
        faceStatus.badFrameCount++;
        parserState = FP_WAIT_EF;
      } else {
        parserState = frame.size == 0 ? FP_READ_CHECKSUM : FP_READ_DATA;
      }
      break;
    case FP_READ_DATA:
      frame.data[dataIndex++] = b;
      if (dataIndex >= frame.size) parserState = FP_READ_CHECKSUM;
      break;
    case FP_READ_CHECKSUM:
      frame.checksum = b;
      frame.checksumOk = (b == checksum(frame.msgId, frame.size, frame.data));
      handleFrame(frame);
      parserState = FP_WAIT_EF;
      break;
  }
}

void faceBegin() {
  FaceSerial.begin(FACE_BAUD, SERIAL_8N1, FACE_RX_PIN, FACE_TX_PIN);
  faceStatus.initialized = true;
  faceStatus.baud = FACE_BAUD;
  faceStatus.initMs = millis();
  faceStatus.autoVerifyEnabled = loadAutoVerifyPreference();
  nextAutoVerifyMs = millis() + 2000;
  logInfo("FACE", "uart ready baud=" + String(FACE_BAUD) +
                  " rx=IO" + String(FACE_RX_PIN) +
                  " tx=IO" + String(FACE_TX_PIN));
  logInfo("FACE", "auto verify default=" + String(faceStatus.autoVerifyEnabled ? "true" : "false"));
  delay(300);
  faceGetStatus();
}

void facePoll() {
  while (FaceSerial.available() > 0) {
    uint8_t b = FaceSerial.read();
    faceStatus.rxBytes++;
    consumeByte(b);
  }
}

bool faceCommandAllowed() {
  return faceStatus.pendingMid == 0 &&
         (faceStatus.mode == FACE_MODE_IDLE ||
          faceStatus.mode == FACE_MODE_MANUAL ||
          faceStatus.mode == FACE_MODE_AUTO_VERIFY);
}

static void tickCommandTimeout() {
  if (faceStatus.pendingMid == 0) return;
  if (millis() - faceStatus.commandStartedMs < currentCommandTimeoutMs) return;
  logWarn("FACE", "command timeout mid=0x" + String(faceStatus.pendingMid, HEX));
  uint8_t timedOutMid = faceStatus.pendingMid;
  faceStatus.pendingMid = 0;
  faceStatus.lastResultCode = 0x0D;
  if (timedOutMid == MID_VERIFY) {
    showResultLed(false);
    closeAutoVerifyWindow("timeout");
  }
  if (timedOutMid == MID_ENROLL && enrollSeq.active) {
    logWarn("FACE", "5-step enroll timeout step=" + String(faceStatus.enroll5Step) +
                    " direction=" + faceEnrollDirectionText(faceStatus.enroll5Direction));
    enrollSeq.active = false;
    enrollSeq.waitingToSend = false;
    faceStatus.enroll5Active = false;
    faceStatus.enroll5Direction = 0;
    showResultLed(false);
  }
  if (faceStatus.mode == FACE_MODE_VERIFYING ||
      faceStatus.mode == FACE_MODE_ENROLLING ||
      faceStatus.mode == FACE_MODE_MANUAL) {
    faceStatus.mode = FACE_MODE_IDLE;
  }
  if (faceStatus.autoVerifyEnabled) {
    nextAutoVerifyMs = millis() + FACE_AUTO_RETRY_DELAY_MS;
  }
}

static void tickAutoVerify() {
  if (!faceStatus.autoVerifyEnabled) return;
  if (!faceStatus.online || faceStatus.pendingMid != 0) return;
  uint32_t now = millis();
  if (autoVerifyWindowUntilMs == 0 || now >= autoVerifyWindowUntilMs) {
    if (faceStatus.mode == FACE_MODE_VERIFYING || faceStatus.mode == FACE_MODE_COOLDOWN) {
      faceStatus.mode = FACE_MODE_IDLE;
    }
    return;
  }
  if (faceStatus.mode == FACE_MODE_COOLDOWN && now - lastSuccessMs >= FACE_SUCCESS_COOLDOWN_MS) {
    faceStatus.mode = FACE_MODE_IDLE;
  }
  if (faceStatus.mode != FACE_MODE_IDLE || now < nextAutoVerifyMs) return;
  faceVerify(FACE_AUTO_VERIFY_TIMEOUT_SEC);
}

static void tickLedIndicator() {
  if (faceStatus.pendingMid != 0) return;
  if (ledIndicatorUntilMs != 0 && millis() >= ledIndicatorUntilMs) {
    showIdleLed();
  }
}

static void tickEnrollSequence() {
  if (!enrollSeq.active) return;
  if (enrollSeq.waitingToSend) {
    if (millis() >= enrollSeq.nextSendMs) sendCurrentEnrollStep();
    return;
  }
  if (enrollSeq.step >= FACE_ENROLL_STEP_COUNT && millis() >= enrollSeq.nextSendMs) {
    enrollSeq.active = false;
    faceStatus.enroll5Active = false;
    faceStatus.mode = FACE_MODE_IDLE;
    faceStatus.lastResultCode = 0x0D;
    logWarn("FACE", "5-step enroll final reply timeout");
  }
}

void faceTick() {
  facePoll();
  tickCommandTimeout();
  tickEnrollSequence();
  tickLedIndicator();
  tickAutoVerify();
}

void faceAutoVerifySet(bool enabled) {
  faceStatus.autoVerifyEnabled = enabled;
  saveAutoVerifyPreference(enabled);
  if (enabled) {
    faceStatus.mode = FACE_MODE_IDLE;
    autoVerifyWindowUntilMs = 0;
    nextAutoVerifyMs = millis() + 500;
  } else if (faceStatus.mode == FACE_MODE_AUTO_VERIFY || faceStatus.mode == FACE_MODE_COOLDOWN) {
    faceStatus.mode = FACE_MODE_IDLE;
    autoVerifyWindowUntilMs = 0;
  }
  logInfo("FACE", "auto verify saved=" + String(enabled ? "true" : "false"));
}

bool faceWakeActive() {
  uint32_t now = millis();
  return (autoVerifyWindowUntilMs != 0 && now < autoVerifyWindowUntilMs) ||
         faceStatus.mode == FACE_MODE_VERIFYING ||
         faceStatus.mode == FACE_MODE_AUTO_VERIFY ||
         faceStatus.mode == FACE_MODE_COOLDOWN;
}

void faceWakeAutoVerify(uint32_t windowMs) {
  if (!faceStatus.autoVerifyEnabled) {
    return;
  }
  uint32_t until = millis() + windowMs;
  if (autoVerifyWindowUntilMs < until) {
    autoVerifyWindowUntilMs = until;
  }
  if (faceStatus.mode == FACE_MODE_COOLDOWN) {
    faceStatus.mode = FACE_MODE_IDLE;
  }
  setLedState(false, false, true);
  nextAutoVerifyMs = millis() + 200;
  logInfo("FACE", "auto verify wake windowMs=" + String(windowMs));
}

void faceSetVerifySuccessCallback(FaceVerifySuccessCallback callback) {
  verifySuccessCallback = callback;
}

String faceStatusJson() {
  String json = F("{\"initialized\":");
  json += faceStatus.initialized ? F("true") : F("false");
  json += F(",\"online\":");
  json += faceStatus.online ? F("true") : F("false");
  json += F(",\"baud\":");
  json += faceStatus.baud;
  json += F(",\"rxPin\":");
  json += FACE_RX_PIN;
  json += F(",\"txPin\":");
  json += FACE_TX_PIN;
  json += F(",\"rxBytes\":");
  json += faceStatus.rxBytes;
  json += F(",\"txBytes\":");
  json += faceStatus.txBytes;
  json += F(",\"frameCount\":");
  json += faceStatus.frameCount;
  json += F(",\"badFrameCount\":");
  json += faceStatus.badFrameCount;
  json += F(",\"pendingMid\":");
  json += faceStatus.pendingMid;
  json += F(",\"moduleStatus\":\"");
  json += faceModuleStatusText(faceStatus.moduleStatus);
  json += F("\",\"mode\":\"");
  json += faceModeText(faceStatus.mode);
  json += F("\",\"autoVerifyEnabled\":");
  json += faceStatus.autoVerifyEnabled ? F("true") : F("false");
  json += F(",\"autoVerifyActive\":");
  json += (autoVerifyWindowUntilMs != 0 && millis() < autoVerifyWindowUntilMs) ? F("true") : F("false");
  json += F(",\"autoVerifyWindowUntilMs\":");
  json += autoVerifyWindowUntilMs;
  json += F(",\"lastResultCode\":");
  json += faceStatus.lastResultCode;
  json += F(",\"lastResultText\":\"");
  json += faceStatus.lastResultCode >= 0 ? faceResultText((uint8_t)faceStatus.lastResultCode) : "NONE";
  json += F("\",\"lastUnlockStatus\":");
  json += faceStatus.lastUnlockStatus;
  json += F(",\"lastUnlockStatusText\":\"");
  json += faceStatus.lastUnlockStatus >= 0 ? faceUnlockStatusText(faceStatus.lastUnlockStatus) : "NONE";
  json += F("\",\"lastUserId\":");
  json += faceStatus.lastUserId;
  json += F(",\"lastEnrollDirection\":");
  json += faceStatus.lastEnrollDirection;
  json += F(",\"lastEnrollDirectionText\":\"");
  json += faceStatus.lastEnrollDirection >= 0 ? faceEnrollDirectionText((uint8_t)faceStatus.lastEnrollDirection) : "NONE";
  json += F("\",\"lastUserName\":\"");
  json += jsonEscape(faceStatus.lastUserName);
  json += F("\",\"lastFaceState\":");
  json += faceStatus.lastFaceState;
  json += F(",\"lastFaceStateText\":\"");
  json += faceStateText(faceStatus.lastFaceState);
  json += F("\",\"userCount\":");
  json += faceStatus.userCount;
  json += F(",\"handUserCount\":");
  json += faceStatus.handUserCount;
  json += F(",\"faceEnroll5Active\":");
  json += faceStatus.enroll5Active ? F("true") : F("false");
  json += F(",\"faceEnroll5Step\":");
  json += faceStatus.enroll5Step;
  json += F(",\"faceEnroll5Direction\":");
  json += faceStatus.enroll5Direction;
  json += F(",\"faceEnroll5DirectionText\":\"");
  json += faceEnrollDirectionText(faceStatus.enroll5Direction);
  json += F("\",\"rxAvailable\":");
  json += FaceSerial.available();
  json += F(",\"led\":{\"green\":");
  json += faceStatus.ledGreenOn ? F("true") : F("false");
  json += F(",\"red\":");
  json += faceStatus.ledRedOn ? F("true") : F("false");
  json += F(",\"white\":");
  json += faceStatus.ledWhiteOn ? F("true") : F("false");
  json += F("},\"wakeInput\":");
  json += faceWakeInputStatusJson();
  json += F("}");
  return json;
}

String faceUsersJson(bool refreshSent) {
  String json = F("{\"ok\":true,\"refreshSent\":");
  json += refreshSent ? F("true") : F("false");
  json += F(",\"count\":");
  json += faceStatus.userCount + faceStatus.handUserCount;
  json += F(",\"faceCount\":");
  json += faceStatus.userCount;
  json += F(",\"handCount\":");
  json += faceStatus.handUserCount;
  json += F(",\"ids\":[");
  for (uint16_t i = 0; i < faceStatus.userCount; i++) {
    if (i > 0) json += ',';
    json += faceStatus.userIds[i];
  }
  json += F("],\"faceIds\":[");
  for (uint16_t i = 0; i < faceStatus.userCount; i++) {
    if (i > 0) json += ',';
    json += faceStatus.userIds[i];
  }
  json += F("],\"handIds\":[");
  for (uint16_t i = 0; i < faceStatus.handUserCount; i++) {
    if (i > 0) json += ',';
    json += faceStatus.handUserIds[i];
  }
  json += F("]}");
  return json;
}
