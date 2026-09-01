#pragma once

#include <Arduino.h>

enum FaceModuleStatus {
  FACE_MODULE_STANDBY,
  FACE_MODULE_BUSY,
  FACE_MODULE_ERROR,
  FACE_MODULE_INVALID,
  FACE_MODULE_OTA,
  FACE_MODULE_UNKNOWN
};

enum FaceMode {
  FACE_MODE_IDLE,
  FACE_MODE_MANUAL,
  FACE_MODE_AUTO_VERIFY,
  FACE_MODE_ENROLLING,
  FACE_MODE_VERIFYING,
  FACE_MODE_COOLDOWN
};

typedef void (*FaceVerifySuccessCallback)(int userId, const char *name, const char *verifyType);

struct FaceStatus {
  bool initialized;
  bool online;
  uint32_t baud;
  uint32_t initMs;
  uint32_t rxBytes;
  uint32_t txBytes;
  uint32_t frameCount;
  uint32_t badFrameCount;
  uint8_t pendingMid;
  uint32_t commandStartedMs;
  int lastResultCode;
  int lastUnlockStatus;
  int lastEnrollDirection;
  int lastUserId;
  char lastUserName[33];
  int lastFaceState;
  uint16_t userIds[128];
  uint16_t userCount;
  uint16_t handUserIds[128];
  uint16_t handUserCount;
  uint8_t moduleStatus;
  uint8_t mode;
  bool autoVerifyEnabled;
  bool enroll5Active;
  uint8_t enroll5Step;
  uint8_t enroll5Direction;
  bool ledGreenOn;
  bool ledRedOn;
  bool ledWhiteOn;
};

void faceBegin();
void facePoll();
void faceTick();
bool faceCommandAllowed();
void faceGetStatus();
void faceVerify(uint8_t timeoutSec);
void faceEnrollSingleFace(const char *name, bool admin, uint8_t timeoutSec);
void faceStartEnrollSequence(const char *name, bool admin, uint8_t timeoutSec);
void faceEnrollHand(const char *name, bool admin, uint8_t timeoutSec);
void faceReset();
void faceLedControl(uint8_t color, bool on);
void faceDeleteUser(uint16_t id);
void faceDeleteAllUsers();
void faceGetAllUserIds();
void faceAutoVerifySet(bool enabled);
bool faceWakeActive();
void faceWakeAutoVerify(uint32_t windowMs);
void faceSetVerifySuccessCallback(FaceVerifySuccessCallback callback);
String faceStatusJson();
String faceUsersJson(bool refreshSent);
const char *faceModuleStatusText(uint8_t status);
const char *faceModeText(uint8_t mode);
const char *faceResultText(uint8_t result);
const char *faceStateText(int state);
const char *faceUnlockStatusText(int status);
