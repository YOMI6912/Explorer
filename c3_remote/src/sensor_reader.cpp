#include "sensor_reader.h"

#include <Arduino.h>
#include <VL53L1X.h>
#include <Wire.h>
#include <math.h>
#include <string.h>

#include "remote_config.h"

namespace SensorReader {
namespace {
VL53L1X sensor;
bool sensorReady = false;

struct Scan {
  uint16_t distanceMm = 0;
  uint16_t signalX100 = 0;
  uint8_t validFrames = 0;
  uint8_t spanMm = 0;
};

void sortSamples(uint16_t *distances, uint16_t *signals, uint8_t count) {
  for (uint8_t i = 1; i < count; ++i) {
    const uint16_t distance = distances[i];
    const uint16_t signal = signals[i];
    uint8_t j = i;
    while (j > 0 && distances[j - 1] > distance) {
      distances[j] = distances[j - 1];
      signals[j] = signals[j - 1];
      --j;
    }
    distances[j] = distance;
    signals[j] = signal;
  }
}

HeightLink::Status collectScan(Scan &out, uint8_t &retryTotal,
                               uint32_t startedAt) {
  uint16_t distances[RemoteConfig::MAX_ATTEMPTS_PER_SCAN] = {};
  uint16_t signals[RemoteConfig::MAX_ATTEMPTS_PER_SCAN] = {};
  uint8_t valid = 0;

  for (uint8_t attempt = 0;
       attempt < RemoteConfig::MAX_ATTEMPTS_PER_SCAN &&
       millis() - startedAt < RemoteConfig::MAX_MEASUREMENT_MS;
       ++attempt) {
    sensor.readSingle(false);
    if (sensor.communicationError()) {
      sensorReady = false;
      return HeightLink::I2C_ERROR;
    }

    const uint16_t distanceMm = sensor.read(true);
    const uint16_t retries = sensor.lastReadI2CRetryCount();
    if (sensor.communicationError() || sensor.timeoutOccurred()) {
      sensorReady = false;
      return HeightLink::I2C_ERROR;
    }
    if (retryTotal + retries >
        RemoteConfig::MAX_I2C_RETRIES_PER_MEASUREMENT) {
      sensorReady = false;
      return HeightLink::I2C_ERROR;
    }
    retryTotal += retries;

    const float signal = sensor.ranging_data.peak_signal_count_rate_MCPS;
    const uint8_t rangeStatus = sensor.ranging_data.range_status;
    if (rangeStatus != VL53L1X::RangeValid ||
        distanceMm < RemoteConfig::MIN_DISTANCE_MM ||
        distanceMm > RemoteConfig::MAX_DISTANCE_MM ||
        !isfinite(signal) || signal < 3.0f || signal >= 500.0f) {
      continue;
    }

    distances[valid] = distanceMm;
    signals[valid] = static_cast<uint16_t>(lroundf(signal * 100.0f));
    ++valid;
    if (valid >= RemoteConfig::MIN_CLUSTER) {
      uint16_t sorted[RemoteConfig::MAX_ATTEMPTS_PER_SCAN];
      memcpy(sorted, distances, valid * sizeof(uint16_t));
      for (uint8_t i = 1; i < valid; ++i) {
        const uint16_t value = sorted[i];
        uint8_t j = i;
        while (j > 0 && sorted[j - 1] > value) {
          sorted[j] = sorted[j - 1];
          --j;
        }
        sorted[j] = value;
      }
      for (uint8_t i = 0; i + RemoteConfig::MIN_CLUSTER <= valid; ++i) {
        if (sorted[i + RemoteConfig::MIN_CLUSTER - 1] - sorted[i] <=
            RemoteConfig::MAX_SPAN_MM) {
          attempt = RemoteConfig::MAX_ATTEMPTS_PER_SCAN;
          break;
        }
      }
    }
  }

  if (valid < RemoteConfig::MIN_CLUSTER) {
    return HeightLink::NO_STABLE_TARGET;
  }
  sortSamples(distances, signals, valid);
  uint8_t bestStart = 0;
  uint16_t bestSpan = UINT16_MAX;
  for (uint8_t i = 0; i + RemoteConfig::MIN_CLUSTER <= valid; ++i) {
    const uint16_t span =
        distances[i + RemoteConfig::MIN_CLUSTER - 1] - distances[i];
    if (span < bestSpan) {
      bestSpan = span;
      bestStart = i;
    }
  }
  if (bestSpan > RemoteConfig::MAX_SPAN_MM) {
    return HeightLink::NO_STABLE_TARGET;
  }

  const uint8_t selected = bestStart + RemoteConfig::MIN_CLUSTER / 2;
  out.distanceMm = distances[selected];
  out.signalX100 = signals[selected];
  out.validFrames = valid;
  out.spanMm = static_cast<uint8_t>(bestSpan);
  return HeightLink::OK;
}

bool setHeadRoi() {
  sensor.setROISize(RemoteConfig::HEAD_CAL_ROI_SIZE,
                    RemoteConfig::HEAD_CAL_ROI_SIZE);
  sensor.setROICenter(199);
  uint8_t width = 0;
  uint8_t height = 0;
  sensor.getROISize(&width, &height);
  return !sensor.communicationError() &&
         width == RemoteConfig::HEAD_CAL_ROI_SIZE &&
         height == RemoteConfig::HEAD_CAL_ROI_SIZE &&
         sensor.getROICenter() == 199 && !sensor.communicationError();
}

bool restoreRoi() {
  sensor.setROISize(RemoteConfig::ROI_WIDTH, RemoteConfig::ROI_HEIGHT);
  sensor.setROICenter(RemoteConfig::ROI_CENTER_SPAD);
  uint8_t width = 0;
  uint8_t height = 0;
  sensor.getROISize(&width, &height);
  return !sensor.communicationError() &&
         width == RemoteConfig::ROI_WIDTH &&
         height == RemoteConfig::ROI_HEIGHT &&
         sensor.getROICenter() == RemoteConfig::ROI_CENTER_SPAD &&
         !sensor.communicationError();
}

HeightLink::Response measurePair(uint32_t requestId, uint32_t bootId) {
  HeightLink::Response reply = {};
  reply.magic = HeightLink::MAGIC;
  reply.version = HeightLink::VERSION;
  reply.type = HeightLink::RESPONSE;
  reply.request_id = requestId;
  reply.boot_id = bootId;
  reply.status = HeightLink::SENSOR_OFFLINE;
  if (!sensorReady && !begin()) return reply;

  if (sensor.getDistanceMode() != VL53L1X::Short &&
      (!sensor.setDistanceMode(VL53L1X::Short) ||
       sensor.communicationError())) {
    sensorReady = false;
    reply.status = HeightLink::I2C_ERROR;
    return reply;
  }

  uint8_t retries = 0;
  const uint32_t startedAt = millis();
  Scan first;
  Scan second;
  HeightLink::Status status = collectScan(first, retries, startedAt);
  if (status == HeightLink::OK) {
    status = collectScan(second, retries, startedAt);
  }
  if (status == HeightLink::OK &&
      abs(static_cast<int>(first.distanceMm) -
          static_cast<int>(second.distanceMm)) > RemoteConfig::MAX_SPAN_MM) {
    status = HeightLink::NO_STABLE_TARGET;
  }

  reply.status = status;
  reply.i2c_retries = retries;
  if (status != HeightLink::OK) return reply;

  reply.raw_distance_mm = static_cast<uint16_t>(
      (static_cast<uint32_t>(first.distanceMm) + second.distanceMm + 1) / 2);
  reply.corrected_distance_mm = reply.raw_distance_mm;
  reply.valid_frames = first.validFrames + second.validFrames;
  reply.span_mm = max(first.spanMm, second.spanMm);
  reply.median_signal_mcps_x100 =
      (static_cast<uint32_t>(first.signalX100) + second.signalX100) / 2;
  reply.calibration_valid = 0;
  reply.measured_at_ms = millis();
  return reply;
}
}

bool begin() {
  sensorReady = false;
  Wire.end();
  pinMode(RemoteConfig::XSHUT_PIN, OUTPUT);
  digitalWrite(RemoteConfig::XSHUT_PIN, LOW);
  delay(50);
  digitalWrite(RemoteConfig::XSHUT_PIN, HIGH);
  delay(150);
  if (!Wire.begin(RemoteConfig::SDA_PIN, RemoteConfig::SCL_PIN,
                  RemoteConfig::I2C_HZ)) return false;
  Wire.setClock(RemoteConfig::I2C_HZ);
  Wire.setTimeOut(80);
  Wire.beginTransmission(0x29);
  if (Wire.endTransmission() != 0) return false;

  sensor.setBus(&Wire);
  sensor.setTimeout(RemoteConfig::SENSOR_TIMEOUT_MS);
  if (!sensor.init() || !sensor.setDistanceMode(VL53L1X::Short) ||
      !sensor.setMeasurementTimingBudget(50000)) return false;
  sensor.setROISize(RemoteConfig::ROI_WIDTH, RemoteConfig::ROI_HEIGHT);
  sensor.setROICenter(RemoteConfig::ROI_CENTER_SPAD);
  if (!restoreRoi()) return false;
  sensorReady = true;
  return true;
}

HeightLink::Response measureHead(uint32_t requestId, uint32_t bootId) {
  HeightLink::Response reply = {};
  reply.magic = HeightLink::MAGIC;
  reply.version = HeightLink::VERSION;
  reply.type = HeightLink::RESPONSE;
  reply.request_id = requestId;
  reply.boot_id = bootId;
  reply.status = HeightLink::NO_STABLE_TARGET;

  uint16_t distances[RemoteConfig::HEAD_AUTO_CONFIRM_READS] = {};
  uint16_t minDistance = UINT16_MAX;
  uint16_t maxDistance = 0;
  uint32_t signalSum = 0;
  uint8_t widestScan = 0;
  for (uint8_t i = 0; i < RemoteConfig::HEAD_AUTO_CONFIRM_READS; ++i) {
    if (i > 0) delay(150);
    if (!sensorReady && !begin()) {
      reply.status = HeightLink::SENSOR_OFFLINE;
      return reply;
    }
    if (!setHeadRoi()) {
      sensorReady = false;
      reply.status = HeightLink::I2C_ERROR;
      return reply;
    }
    const HeightLink::Response scan = measurePair(requestId, bootId);
    const bool restored = restoreRoi();
    if (!restored) sensorReady = false;
    if (scan.status != HeightLink::OK || scan.i2c_retries != 0 ||
        scan.valid_frames < 2 * RemoteConfig::MIN_CLUSTER) {
      if (scan.i2c_retries != 0) sensorReady = false;
      reply.status = scan.status != HeightLink::OK
                         ? scan.status
                         : (scan.i2c_retries != 0 ? HeightLink::I2C_ERROR
                                                  : HeightLink::NO_STABLE_TARGET);
      return reply;
    }
    if (!restored) {
      reply.status = HeightLink::I2C_ERROR;
      return reply;
    }
    distances[i] = scan.raw_distance_mm;
    minDistance = min(minDistance, scan.raw_distance_mm);
    maxDistance = max(maxDistance, scan.raw_distance_mm);
    reply.valid_frames += scan.valid_frames;
    signalSum += scan.median_signal_mcps_x100;
    widestScan = max(widestScan, scan.span_mm);
  }

  const uint16_t spread = maxDistance - minDistance;
  if (spread > RemoteConfig::HEAD_AUTO_MAX_SPREAD_MM) return reply;
  const uint16_t raw = static_cast<uint16_t>(
      static_cast<uint32_t>(distances[0]) + distances[1] + distances[2] -
      minDistance - maxDistance);
  reply.raw_distance_mm = raw;
  reply.span_mm = max(static_cast<uint8_t>(spread), widestScan);
  if (raw < RemoteConfig::HEAD_CAL_RAW_MIN_MM ||
      raw > RemoteConfig::HEAD_CAL_RAW_MAX_MM) {
    reply.status = HeightLink::OUTSIDE_CALIBRATION;
    return reply;
  }

  const float heightCm = RemoteConfig::HEAD_HEIGHT_INTERCEPT_CM -
      RemoteConfig::HEAD_HEIGHT_SLOPE_CM_PER_MM * raw;
  const long correctedDistanceMm = lroundf(
      (RemoteConfig::HEAD_MOUNT_HEIGHT_CM - heightCm) * 10.0f);
  if (!isfinite(heightCm) ||
      correctedDistanceMm < RemoteConfig::MIN_DISTANCE_MM ||
      correctedDistanceMm > RemoteConfig::MAX_DISTANCE_MM) {
    reply.status = HeightLink::OUTSIDE_CALIBRATION;
    return reply;
  }

  reply.status = HeightLink::OK;
  reply.calibration_valid = 1;
  reply.corrected_distance_mm = static_cast<uint16_t>(correctedDistanceMm);
  reply.median_signal_mcps_x100 =
      signalSum / RemoteConfig::HEAD_AUTO_CONFIRM_READS;
  reply.measured_at_ms = millis();
  return reply;
}
}
