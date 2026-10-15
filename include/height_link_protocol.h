#pragma once
#include <stdint.h>

namespace HeightLink {
constexpr uint32_t MAGIC = 0x31474848;

constexpr uint8_t VERSION = 2;
constexpr uint8_t REQUEST = 1;
constexpr uint8_t RESPONSE = 2;
constexpr uint8_t HEARTBEAT = 3;
enum Status : uint8_t {
  OK = 0,
  SENSOR_OFFLINE = 1,
  I2C_ERROR = 2,
  NO_STABLE_TARGET = 3,
  OUTSIDE_CALIBRATION = 4,
};
struct __attribute__((packed)) Request {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint16_t reserved;
  uint32_t request_id;
};
struct __attribute__((packed)) Response {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t status;
  uint8_t calibration_valid;
  uint32_t request_id;
  uint32_t boot_id;
  uint32_t measured_at_ms;
  uint16_t raw_distance_mm;
  uint16_t corrected_distance_mm;
  uint16_t median_signal_mcps_x100;
  uint8_t valid_frames;
  uint8_t i2c_retries;
  uint8_t span_mm;
  uint8_t reserved;
};
static_assert(sizeof(Request) == 12, "Request wire layout changed");
static_assert(sizeof(Response) == 30, "Response wire layout changed");
}
