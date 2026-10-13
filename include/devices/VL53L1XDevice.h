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
// When extendedDistanceRange is true, only the diagnostic DISTTEST path may
// accept targets beyond the normal head-distance band. Production height
// measurements keep the stricter head band.
bool readHeightCmFixedRoi(float &heightCm,
                          bool extendedDistanceRange = false);
bool readHeightCmMultiRoi(float &heightCm);
bool readHeightCmRoi12Diagnostic(float &heightCm);
// True when the last accepted scan has a tight, strong head cluster and the
// second confirmation scan can be skipped safely.
bool lastScanHighConfidence();
FailureCause lastFailureCause();

}  // namespace VL53L1XDevice

