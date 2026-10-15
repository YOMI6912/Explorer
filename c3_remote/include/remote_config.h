#pragma once

#include <stdint.h>

namespace RemoteConfig {
constexpr uint8_t SDA_PIN = 4;
constexpr uint8_t SCL_PIN = 5;
constexpr uint8_t XSHUT_PIN = 3;
constexpr uint32_t I2C_HZ = 50000;


constexpr char LINK_SSID[] = "Clinic-Height-Link-90";
constexpr char LINK_PASSWORD[] = "ClinicHeight90!";
constexpr uint16_t LINK_TCP_PORT = 45831;


constexpr uint32_t LINK_RETRY_MS = 15000;

constexpr uint8_t S3_AP_BSSID[6] = {0xDC, 0xDA, 0x0C, 0x78, 0xD1, 0x91};
constexpr uint8_t LINK_CHANNEL = 6;

constexpr uint16_t SENSOR_TIMEOUT_MS = 300;
constexpr uint8_t MIN_CLUSTER = 7;
constexpr uint8_t MAX_ATTEMPTS_PER_SCAN = 16;
constexpr uint8_t MAX_I2C_RETRIES_PER_MEASUREMENT = 3;
constexpr uint8_t MAX_SPAN_MM = 8;

constexpr uint8_t ROI_WIDTH = 4;
constexpr uint8_t ROI_HEIGHT = 4;
constexpr uint8_t ROI_CENTER_SPAD = 28;
constexpr uint16_t MIN_DISTANCE_MM = 50;
constexpr uint16_t MAX_DISTANCE_MM = 4000;
constexpr uint32_t MAX_MEASUREMENT_MS = 4000;




constexpr bool CALIBRATION_VALID = true;
constexpr float DISTANCE_SCALE = 1.06824f;
constexpr float DISTANCE_OFFSET_MM = -21.32f;
constexpr uint16_t CAL_RAW_MIN_MM = 250;
constexpr uint16_t CAL_RAW_MAX_MM = 450;








constexpr float HEAD_MOUNT_HEIGHT_CM = 197.0f;
constexpr float HEAD_HEIGHT_INTERCEPT_CM = 201.44257f;
constexpr float HEAD_HEIGHT_SLOPE_CM_PER_MM = 0.11148977f;



constexpr uint16_t HEAD_CAL_RAW_MIN_MM = 100;
constexpr uint16_t HEAD_CAL_RAW_MAX_MM = 430;
constexpr uint8_t HEAD_CAL_ROI_SIZE = 12;


constexpr uint8_t HEAD_AUTO_CONFIRM_READS = 3;
constexpr uint16_t HEAD_AUTO_MAX_SPREAD_MM = 5;
constexpr uint8_t HEAD_SERIES_READS = 5;
}
