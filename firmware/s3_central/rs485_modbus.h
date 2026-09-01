#pragma once

#include <Arduino.h>

struct Rs485Port {
  const char *name;
  HardwareSerial *serial;
  int txPin;
  int rxPin;
  int deRePin;
  uint8_t nodeAddr;
  bool enabled;
  uint16_t regs[9];
  bool lastReadOk;
  uint32_t lastReadMs;
  uint32_t lastWarnLogMs;
  uint32_t successCount;
  uint32_t failCount;
};

enum DoorLimitMode {
  DOOR_LIMIT_SINGLE = 0,
  DOOR_LIMIT_DUAL = 1
};

struct DoorLimitConfig {
  DoorLimitMode mode;
  uint8_t singlePort;
  uint16_t travelTimeoutSeconds;
};

extern Rs485Port rs485Ports[2];

void rs485Begin();
void rs485Poll();
bool rs485PollPort(uint8_t index);
void rs485RefreshDoorState();
DoorLimitConfig rs485DoorLimitConfig();
bool rs485PortActiveForDoorMode(uint8_t index);
bool rs485ConfigureDoorLimits(const String &mode, uint8_t singlePort, uint16_t travelTimeoutSeconds);
String rs485DoorLimitConfigJson();
const char *rs485DoorLimitModeText(DoorLimitMode mode);
bool rs485ReadRegisters(uint8_t portIndex, uint8_t functionCode, uint16_t startReg, uint16_t regCount,
                        uint16_t *outRegs, String &result);
bool rs485WriteSingleRegister(uint8_t portIndex, uint8_t nodeAddr, uint16_t regAddr,
                              uint16_t value, String &result);
bool rs485WriteMultipleRegisters(uint8_t portIndex, uint8_t nodeAddr, uint16_t startReg,
                                 const uint16_t *values, uint16_t count, String &result);
bool rs485SaveSensorConfig(uint8_t portIndex, uint8_t writeAddr, const uint16_t *values, uint16_t count);
String rs485StatusJson(uint8_t index);
String rs485ConfigJson(uint8_t index);
