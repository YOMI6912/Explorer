#pragma once

#include <Arduino.h>

namespace VL53L1XDevice {

enum class FailureCause : uint8_t {
  None,
  Communication,
  Target,
  Hardware,
};

bool begin();
bool isReady();
bool activeSessionHealthy();
bool readDistanceMm(uint16_t &distanceMm);
bool readHeightCm(float &heightCm);
bool readHeightCmFixedRoi(float &heightCm);
bool readHeightCmMultiRoi(float &heightCm);
bool readHeightCmRoi12Diagnostic(float &heightCm);
FailureCause lastFailureCause();

}  // namespace VL53L1XDevice

