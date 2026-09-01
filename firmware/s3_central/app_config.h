#pragma once

#include <Arduino.h>

#define ENABLE_RS485_1 1
#define ENABLE_RS485_2 1

static const uint32_t MODBUS_BAUD = 9600;
static const uint32_t VIBRATION_POLL_INTERVAL_MS = 400;
static const uint32_t DOOR_POLL_INTERVAL_MS = 1000;
static const uint32_t RESPONSE_TIMEOUT_MS = 120;
static const uint16_t READ_START_REG = 0;
static const uint16_t READ_REG_COUNT = 9;
static const uint8_t OUTSIDE_VIBRATION_PORT = 0;
static const uint8_t DOOR_SENSOR_PORT = 1;
static const uint8_t SENSOR_ROLE_REG_INDEX = 0;
static const uint8_t DOOR_STATE_REG_INDEX = 1;
static const uint8_t HALL_RAW_REG_INDEX = 2;
static const uint8_t HALL_FILTER_REG_INDEX = 3;
static const uint8_t VIBRATION_RAW_REG_INDEX = 4;
static const uint8_t RUN_FILTER_REG_INDEX = 5;
static const uint8_t VIBRATION_COUNTER_REG_INDEX = 6;
static const uint8_t STATE_DURATION_REG_INDEX = 7;
static const uint8_t MODBUS_ADDR_REG_INDEX = 8;
static const uint8_t SENSOR_CONFIG_START_REG = 8;
static const uint8_t SENSOR_CONFIG_REG_COUNT = 7;
static const uint8_t DOOR_HALL_REG_INDEX = HALL_FILTER_REG_INDEX;
static const uint8_t DOOR_VIBRATION_REG_INDEX = RUN_FILTER_REG_INDEX;
static const uint32_t FACE_WAKE_WINDOW_MS = 30000;
static const uint32_t FACE_WAKE_INPUT_DEBOUNCE_MS = 50;
static const uint32_t FACE_WAKE_INPUT_COOLDOWN_MS = 5000;
static const uint32_t GARAGE_TRIGGER_FACE_WAKE_INHIBIT_MS = 30000;
static const uint32_t DOOR_OPEN_LONG_ALERT_SECONDS = 300;
static const uint32_t GARAGE_OPEN_METHOD_WINDOW_MS = 120000;

static const char *HOSTNAME = "centr-reader";
static const char *SETUP_AP_SSID = "centr-setup";
static const char *SETUP_AP_PASSWORD = "12345678";

static const size_t LOG_CAPACITY = 120;
static const bool ENABLE_MODBUS_TRACE_LOGS = false;
static const uint32_t RS485_WARN_INTERVAL_MS = 5000;
static const uint32_t WATCHDOG_TIMEOUT_MS = 20000;

static const char *NTP_TIMEZONE = "CST-8";
static const char *NTP_SERVER_1 = "ntp.aliyun.com";
static const char *NTP_SERVER_2 = "ntp.tencent.com";
static const char *NTP_SERVER_3 = "pool.ntp.org";
static const uint8_t SCHEDULED_RESTART_HOUR = 4;
static const uint8_t SCHEDULED_RESTART_MINUTE = 0;
