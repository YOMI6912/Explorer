#include "devices/VL53L1XDevice.h"

#include <VL53L1X.h>
#include <Wire.h>
#include <math.h>

#include "config.h"

namespace VL53L1XDevice {
namespace {
VL53L1X sensor;
TwoWire &heightWire = Wire;
bool initialized = false;
uint8_t currentRoiWidth = Config::HeightSensor::ROI_WIDTH;
uint8_t currentRoiHeight = Config::HeightSensor::ROI_HEIGHT;
uint8_t consecutiveCommunicationFailures = 0;
FailureCause lastMeasurementFailure = FailureCause::None;
uint8_t lastReadRetryCount = 0;
bool hasLastFrameStreamCount = false;
uint8_t lastFrameStreamCount = 0;
uint16_t lastFrameDistanceMm = 0;
float lastFrameSignalMCPS = NAN;
float lastFrameAmbientMCPS = NAN;
bool lastReadWasDuplicateFrame = false;

struct HeightSample {
  float heightCm;
  uint16_t distanceMm;
  float signalMCPS;
  float ambientMCPS;
};

struct ScanStats {
  uint8_t validSamples = 0;
  uint8_t retryReads = 0;
  uint8_t weakSignalSamples = 0;
  uint8_t phaseCheckSamples = 0;
  float medianDistanceMm = NAN;
  float medianSignalMCPS = NAN;
  float medianAmbientMCPS = NAN;
  float spanCm = NAN;
};

ScanStats lastScanStats;

bool rejectNoisyScan(uint16_t retryReads) {
  if (retryReads <= Config::HeightSensor::I2C_RETRY_REJECT_PER_ROI) {
    return false;
  }
  lastMeasurementFailure = FailureCause::Communication;
  Serial.printf(
      "[VL53L1X] Quality reject: I2C retry reads=%u exceeds max=%u; no height accepted\n",
      static_cast<unsigned>(retryReads),
      static_cast<unsigned>(Config::HeightSensor::I2C_RETRY_REJECT_PER_ROI));
  return true;
}

constexpr uint32_t HEIGHT_I2C_CLOCK_HZ = 25000;
constexpr uint32_t HEIGHT_WIRE_TIMEOUT_MS = 80;
constexpr uint32_t FALLBACK_TIMING_BUDGET_US = 100000;
constexpr float FALLBACK_HEIGHT_THRESHOLD_CM = 155.0f;
constexpr uint8_t MAX_COMMUNICATION_FAILURES = 3;
constexpr uint8_t STARTUP_SAMPLE_ATTEMPTS = 8;
constexpr uint8_t MEASUREMENT_RECOVERY_ATTEMPTS = 1;
constexpr uint32_t MAX_MEASUREMENT_MS = 4000;

constexpr uint8_t EARLY_STABLE_SAMPLE_COUNT = 7;
constexpr float EARLY_STABLE_MAX_SPAN_CM = 0.8f;

constexpr uint16_t SIGNAL_FAIL_HEAD_MIN_DISTANCE_MM = 120;
constexpr uint16_t SIGNAL_FAIL_HEAD_MAX_DISTANCE_MM = 450;
constexpr float SIGNAL_FAIL_MIN_MCPS = 0.05f;

constexpr uint16_t SINGLE_HEAD_MIN_DISTANCE_MM = 60;
constexpr uint16_t SINGLE_HEAD_MAX_DISTANCE_MM = 550;
constexpr float SINGLE_HEAD_MIN_MCPS = 6.0f;

bool isRecoverableHeadStatus(VL53L1X::RangeStatus status,
                             uint16_t distanceMm) {
  if (distanceMm < SIGNAL_FAIL_HEAD_MIN_DISTANCE_MM ||
      distanceMm > SIGNAL_FAIL_HEAD_MAX_DISTANCE_MM) {
    return false;
  }
  return status == VL53L1X::SignalFail ||
         status == VL53L1X::SigmaFail ||
         status == VL53L1X::XtalkSignalFail;
}
bool recoveryResetPending = false;
VL53L1X::DistanceMode activeDistanceMode = VL53L1X::Long;
uint32_t activeTimingBudgetUs = Config::HeightSensor::TIMING_BUDGET_US;

void resetSensorViaXshut() {

  digitalWrite(Config::Pins::HEIGHT_XSHUT, LOW);
  pinMode(Config::Pins::HEIGHT_XSHUT, OUTPUT);
  delay(50);
  digitalWrite(Config::Pins::HEIGHT_XSHUT, HIGH);
  delay(350);
  recoveryResetPending = false;
  Serial.printf("[VL53L1X] XSHUT hardware reset on GPIO%u\n",
                Config::Pins::HEIGHT_XSHUT);
}

void recoverI2CBus() {
  const uint8_t sda = Config::Pins::HEIGHT_I2C_SDA;
  const uint8_t scl = Config::Pins::HEIGHT_I2C_SCL;

  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  delayMicroseconds(10);

  pinMode(scl, OUTPUT_OPEN_DRAIN);
  digitalWrite(scl, HIGH);
  for (uint8_t pulse = 0; pulse < 9; ++pulse) {
    digitalWrite(scl, LOW);
    delayMicroseconds(8);
    digitalWrite(scl, HIGH);
    delayMicroseconds(8);
  }

  pinMode(sda, OUTPUT_OPEN_DRAIN);
  digitalWrite(sda, LOW);
  delayMicroseconds(8);
  digitalWrite(scl, HIGH);
  delayMicroseconds(8);
  digitalWrite(sda, HIGH);
  delayMicroseconds(8);

  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
}

float medianOf(float values[], uint8_t count) {
  if (count == 0) {
    return NAN;
  }
  for (uint8_t i = 1; i < count; ++i) {
    const float value = values[i];
    uint8_t j = i;
    while (j > 0 && values[j - 1] > value) {
      values[j] = values[j - 1];
      --j;
    }
    values[j] = value;
  }
  return values[count / 2];
}

bool setRoiAndDiscard(uint8_t width, uint8_t height) {
  if (!initialized && !begin()) {
    return false;
  }

  if (currentRoiWidth == width && currentRoiHeight == height) {
    return true;
  }
  if (!Config::HeightSensor::USE_SINGLE_SHOT) {
    sensor.stopContinuous();
    if (sensor.communicationError()) {
      initialized = false;
      recoveryResetPending = true;
      return false;
    }
  }

  sensor.setROISize(width, height);
  uint8_t actualWidth = 0;
  uint8_t actualHeight = 0;
  sensor.getROISize(&actualWidth, &actualHeight);
  if (sensor.communicationError() || actualWidth != width ||
      actualHeight != height) {
    initialized = false;
    recoveryResetPending = true;
    Serial.printf("[VL53L1X] ROI setup rejected: requested=%ux%u actual=%ux%u\n",
                  width, height, actualWidth, actualHeight);
    return false;
  }
  currentRoiWidth = width;
  currentRoiHeight = height;

  if (!Config::HeightSensor::USE_SINGLE_SHOT) {
    sensor.startContinuous(Config::HeightSensor::CONTINUOUS_PERIOD_MS);
    if (sensor.communicationError()) {
      initialized = false;
      recoveryResetPending = true;
      return false;
    }
    delay(Config::HeightSensor::CONTINUOUS_PERIOD_MS * 2 + 20);
  }
  for (uint8_t frame = 0; frame < 2; ++frame) {
    if (Config::HeightSensor::USE_SINGLE_SHOT) {
      sensor.readSingle();
    } else {
      sensor.read();
    }
    sensor.timeoutOccurred();
    if (sensor.communicationError()) {
      initialized = false;
      recoveryResetPending = true;
      return false;
    }
  }
  return true;
}
}

bool begin() {
  Serial.println("[VL53L1X] Initializing...");
  hasLastFrameStreamCount = false;
  lastReadWasDuplicateFrame = false;

  initialized = false;
  heightWire.end();
  delay(20);
  resetSensorViaXshut();
  recoverI2CBus();
  if (!heightWire.begin(Config::Pins::HEIGHT_I2C_SDA,
                        Config::Pins::HEIGHT_I2C_SCL,
                        HEIGHT_I2C_CLOCK_HZ)) {
    Serial.println("[VL53L1X] ERROR: Wire0 begin failed");
    return false;
  }

  heightWire.setClock(HEIGHT_I2C_CLOCK_HZ);
  heightWire.setTimeOut(HEIGHT_WIRE_TIMEOUT_MS);
  sensor.setBus(&heightWire);
  Serial.printf("[VL53L1X] Wire0 SDA=GPIO%u SCL=GPIO%u Clock=%lu Hz\n",
                Config::Pins::HEIGHT_I2C_SDA,
                Config::Pins::HEIGHT_I2C_SCL,
                static_cast<unsigned long>(HEIGHT_I2C_CLOCK_HZ));
  sensor.setTimeout(Config::HeightSensor::TIMEOUT_MS);

  delay(100);

  heightWire.beginTransmission(0x29);
  uint8_t ackStatus = heightWire.endTransmission();

  for (uint8_t probe = 0; ackStatus != 0 && probe < 3; ++probe) {
    heightWire.end();
    recoverI2CBus();
    delay(40);
    if (!heightWire.begin(Config::Pins::HEIGHT_I2C_SDA,
                          Config::Pins::HEIGHT_I2C_SCL,
                          HEIGHT_I2C_CLOCK_HZ)) {
      Serial.println("[VL53L1X] ERROR: Wire0 retry begin failed");
      return false;
    }
    heightWire.setClock(HEIGHT_I2C_CLOCK_HZ);
    heightWire.setTimeOut(HEIGHT_WIRE_TIMEOUT_MS);
    heightWire.beginTransmission(0x29);
    ackStatus = heightWire.endTransmission();
  }
  if (ackStatus != 0) {
    Serial.printf("[VL53L1X] No ACK at 0x29 (I2C status=%u); sensor disabled safely\n",
                  ackStatus);
    return false;
  }

  sensor.timeoutOccurred();

  if (!sensor.init()) {
    Serial.printf("[VL53L1X] ERROR: init failed, I2C status=%u bus_error=%d timeout=%d SDA=%d SCL=%d\n",
                  static_cast<unsigned>(sensor.last_status),
                  sensor.communicationError(),
                  sensor.timeoutOccurred(),
                  digitalRead(Config::Pins::HEIGHT_I2C_SDA),
                  digitalRead(Config::Pins::HEIGHT_I2C_SCL));
    recoveryResetPending = true;
    return false;
  }

  const bool modeReady = sensor.setDistanceMode(activeDistanceMode);
  sensor.setROISize(Config::HeightSensor::ROI_WIDTH,
                    Config::HeightSensor::ROI_HEIGHT);
  currentRoiWidth = Config::HeightSensor::ROI_WIDTH;
  currentRoiHeight = Config::HeightSensor::ROI_HEIGHT;
  const bool timingReady = sensor.setMeasurementTimingBudget(
      activeTimingBudgetUs);
  if (!Config::HeightSensor::USE_SINGLE_SHOT) {
    sensor.startContinuous(Config::HeightSensor::CONTINUOUS_PERIOD_MS);
  }
  if (!modeReady || !timingReady || sensor.communicationError()) {
    Serial.println("[VL53L1X] ERROR: ranging configuration failed");
    recoveryResetPending = true;
    return false;
  }
  Serial.printf("[VL53L1X] Distance mode=%s timing=%lu us mode=%s\n",
                activeDistanceMode == VL53L1X::Long ? "Long" :
                activeDistanceMode == VL53L1X::Medium ? "Medium" : "Short",
                static_cast<unsigned long>(activeTimingBudgetUs),
                Config::HeightSensor::USE_SINGLE_SHOT ? "single-shot" :
                                                         "continuous");

  bool rangingStarted = false;
  for (uint8_t attempt = 0; attempt < STARTUP_SAMPLE_ATTEMPTS; ++attempt) {
    if (!Config::HeightSensor::USE_SINGLE_SHOT) {
      delay(Config::HeightSensor::CONTINUOUS_PERIOD_MS + 20);
    }
    if (Config::HeightSensor::USE_SINGLE_SHOT) {
      sensor.readSingle();
    } else {
      sensor.read();
    }
    const uint16_t startupDistance = sensor.ranging_data.range_mm;

    const bool plausibleStartupDistance = startupDistance >= 40 &&
                                          startupDistance <= 4000;
    const bool usableStartupStatus =
        sensor.ranging_data.range_status != VL53L1X::None &&
        sensor.ranging_data.range_status != VL53L1X::HardwareFail;

    if (!sensor.timeoutOccurred() && !sensor.communicationError() &&
        sensor.last_status == 0 && plausibleStartupDistance &&
        usableStartupStatus) {
      rangingStarted = true;
      break;
    }
  }
  if (!rangingStarted) {
    Serial.println("[VL53L1X] ERROR: ranging did not start; will retry safely");
    recoveryResetPending = true;
    return false;
  }

  consecutiveCommunicationFailures = 0;
  initialized = true;
  Serial.println("[VL53L1X] Ready");
  return true;
}

bool isReady() {
  return initialized;
}

bool activeSessionHealthy() {
  if (!initialized) {
    return false;
  }

  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    if (Config::HeightSensor::USE_SINGLE_SHOT) {
      sensor.readSingle();
    } else {
      sensor.read();
    }
    const bool timedOut = sensor.timeoutOccurred();
    const bool communicationFailed = sensor.communicationError();
    if (!timedOut && !communicationFailed && sensor.last_status == 0 &&
        sensor.ranging_data.range_status != VL53L1X::None &&
        sensor.ranging_data.range_status != VL53L1X::HardwareFail) {
      consecutiveCommunicationFailures = 0;
      Serial.println("[VL53L1X] Existing ranging session is healthy");
      return true;
    }
    delay(Config::HeightSensor::USE_SINGLE_SHOT
              ? 10
              : Config::HeightSensor::CONTINUOUS_PERIOD_MS + 10);
  }

  initialized = false;
  recoveryResetPending = true;
  Serial.println("[VL53L1X] Existing session unhealthy; XSHUT recovery required");
  return false;
}

FailureCause lastFailureCause() {
  return lastMeasurementFailure;
}

bool lastScanHighConfidence() {
  return lastMeasurementFailure == FailureCause::None &&
         lastScanStats.validSamples >=
             Config::HeightSensor::MIN_HEAD_CLUSTER_SAMPLES &&
         isfinite(lastScanStats.spanCm) &&
         lastScanStats.spanCm <= 0.8f &&
         isfinite(lastScanStats.medianDistanceMm) &&
         lastScanStats.medianDistanceMm >= SINGLE_HEAD_MIN_DISTANCE_MM &&
         lastScanStats.medianDistanceMm <= SINGLE_HEAD_MAX_DISTANCE_MM &&
         isfinite(lastScanStats.medianSignalMCPS) &&
         lastScanStats.medianSignalMCPS >= 8.0f;
}

bool readDistanceMm(uint16_t &distanceMm) {
  lastReadRetryCount = 0;
  lastReadWasDuplicateFrame = false;
  if (!initialized) {
    return false;
  }

  bool readOk = false;
  bool communicationFailureThisRead = false;
  uint32_t retryEvents = 0;
  for (uint8_t retry = 0; retry < 2; ++retry) {
    distanceMm = Config::HeightSensor::USE_SINGLE_SHOT
                     ? sensor.readSingle()
                     : sensor.read();
    retryEvents += sensor.lastReadI2CRetryCount();
    const bool timedOut = sensor.timeoutOccurred();
    const bool busError = sensor.communicationError() || sensor.last_status != 0;
    if (!timedOut && !busError &&
        sensor.last_status == 0 &&
        sensor.ranging_data.range_status != VL53L1X::None) {
      readOk = true;
      break;
    }
    if (busError) {
      communicationFailureThisRead = true;
      break;
    }
    if (retry < 1) {
      delay(8);
    }
  }
  lastReadRetryCount = static_cast<uint8_t>(
      retryEvents > 255 ? 255 : retryEvents);

  if (!readOk) {
    ++consecutiveCommunicationFailures;
    if (communicationFailureThisRead) {
      initialized = false;
      recoveryResetPending = true;
      Serial.printf("[VL53L1X] I2C communication failure; frame discarded, "
                    "XSHUT recovery armed (%u retry reads)\n",
                    static_cast<unsigned>(retryEvents));
    } else {
      Serial.printf("[VL53L1X] Read timeout; frame discarded (%u/%u)\n",
                    consecutiveCommunicationFailures,
                    MAX_COMMUNICATION_FAILURES);
      if (consecutiveCommunicationFailures >= MAX_COMMUNICATION_FAILURES) {
        initialized = false;
        recoveryResetPending = true;
        Serial.println("[VL53L1X] Sensor offline; recovery armed");
      }
    }
    return false;
  }

  consecutiveCommunicationFailures = 0;

  const uint8_t streamCount = sensor.lastStreamCount();
  const float frameSignalMCPS =
      sensor.ranging_data.peak_signal_count_rate_MCPS;
  const float frameAmbientMCPS =
      sensor.ranging_data.ambient_count_rate_MCPS;

  if (hasLastFrameStreamCount && streamCount == lastFrameStreamCount &&
      distanceMm == lastFrameDistanceMm &&
      frameSignalMCPS == lastFrameSignalMCPS &&
      frameAmbientMCPS == lastFrameAmbientMCPS) {
    lastReadWasDuplicateFrame = true;
    Serial.println("[VL53L1X] Identical stream frame; skipping");
    return false;
  }
  lastFrameStreamCount = streamCount;
  lastFrameDistanceMm = distanceMm;
  lastFrameSignalMCPS = frameSignalMCPS;
  lastFrameAmbientMCPS = frameAmbientMCPS;
  hasLastFrameStreamCount = true;

  const bool phaseCheckCandidate =
      sensor.ranging_data.range_status == VL53L1X::OutOfBoundsFail;
  const bool recoverableHeadCandidate = isRecoverableHeadStatus(
      sensor.ranging_data.range_status, distanceMm);
  if (sensor.ranging_data.range_status != VL53L1X::RangeValid &&
      sensor.ranging_data.range_status != VL53L1X::RangeValidMinRangeClipped &&
      sensor.ranging_data.range_status != VL53L1X::RangeValidNoWrapCheckFail &&
      !phaseCheckCandidate && !recoverableHeadCandidate) {
    Serial.printf("[VL53L1X] Invalid range: %s\n",
                  VL53L1X::rangeStatusToString(sensor.ranging_data.range_status));

    return false;
  }

  if (recoverableHeadCandidate) {
    Serial.printf("[VL53L1X] Recoverable head candidate: status=%s raw=%u mm signal=%.2f MCPS\n",
                  VL53L1X::rangeStatusToString(
                      sensor.ranging_data.range_status),
                  distanceMm,
                  sensor.ranging_data.peak_signal_count_rate_MCPS);
  }

  if (phaseCheckCandidate) {
    Serial.printf("[VL53L1X] Phase-check candidate: raw=%u mm signal=%.2f MCPS\n",
                  distanceMm,
                  sensor.ranging_data.peak_signal_count_rate_MCPS);
  }

  if (distanceMm < 80 || distanceMm > 1200) {
    static uint32_t lastOutsideLogAt = 0;
    if (millis() - lastOutsideLogAt >= 250) {
      lastOutsideLogAt = millis();
      Serial.printf("[VL53L1X] Outside head-distance window: raw=%u mm signal=%.2f MCPS status=%s\n",
                    distanceMm,
                    sensor.ranging_data.peak_signal_count_rate_MCPS,
                    VL53L1X::rangeStatusToString(sensor.ranging_data.range_status));
    }
    return false;
  }
  return true;
}

bool readHeightCm(float &heightCm) {
  lastMeasurementFailure = FailureCause::None;
  lastScanStats = ScanStats{};
  if (Config::HeightSensor::SENSOR_MOUNT_HEIGHT_CM <= 0.0f) {
    lastMeasurementFailure = FailureCause::Hardware;
    return false;
  }

  HeightSample samples[Config::HeightSensor::MAX_SAMPLE_ATTEMPTS];
  uint8_t validCount = 0;
  uint8_t communicationFailures = 0;
  uint8_t opticalFailures = 0;
  uint8_t hardwareFailures = 0;
  uint8_t distancesOutsideWindow = 0;
  uint8_t weakSignalSamples = 0;
  uint8_t phaseCheckSamples = 0;
  uint8_t strongSingleHeadSamples = 0;
  float strongSingleHeadSum = 0.0f;
  float strongSingleHeadDistanceSum = 0.0f;
  float strongSingleHeadSignalSum = 0.0f;
  float strongSingleHeadMinHeight = INFINITY;
  float strongSingleHeadMaxHeight = -INFINITY;
  uint16_t totalRetryReads = 0;
  float lastSignalMCPS = NAN;
  float lastAmbientMCPS = NAN;
  bool earlyStableCluster = false;
  bool interruptedByCommunication = false;
  const uint32_t startedAt = millis();
  const uint16_t acceptedHeadMaxDistanceMm = SINGLE_HEAD_MAX_DISTANCE_MM;

  for (uint8_t recovery = 0;
       recovery <= MEASUREMENT_RECOVERY_ATTEMPTS &&
       millis() - startedAt < MAX_MEASUREMENT_MS;
       ++recovery) {
    bool communicationFailureSeen = false;

    if (!initialized) {

      if (validCount > 0) {
        Serial.printf("[VL53L1X] Discarding %u samples from the interrupted session\n",
                      validCount);
        validCount = 0;
        totalRetryReads = 0;
        opticalFailures = 0;
        hardwareFailures = 0;
        distancesOutsideWindow = 0;
        weakSignalSamples = 0;
        phaseCheckSamples = 0;
      }
      Serial.printf("[VL53L1X] Measurement recovery %u/%u\n",
                    recovery + 1,
                    MEASUREMENT_RECOVERY_ATTEMPTS + 1);
      if (!begin()) {
        lastMeasurementFailure = FailureCause::Communication;
        delay(100);
        continue;
      }
    }

    for (uint8_t attempt = 0;
         attempt < Config::HeightSensor::MAX_SAMPLE_ATTEMPTS &&
         validCount < Config::HeightSensor::MAX_SAMPLE_ATTEMPTS &&
         millis() - startedAt < MAX_MEASUREMENT_MS;
         ++attempt) {
      uint16_t distanceMm = 0;
      const bool distanceOk = readDistanceMm(distanceMm);
      totalRetryReads += lastReadRetryCount;
      if (!distanceOk) {
        if (lastReadWasDuplicateFrame) {
          delay(20);
          continue;
        }
        const bool busFailed = consecutiveCommunicationFailures > 0 ||
                               !initialized;
        communicationFailureSeen |= busFailed;
        if (busFailed) {
          ++communicationFailures;
        } else if (sensor.ranging_data.range_status ==
                   VL53L1X::HardwareFail) {
          ++hardwareFailures;
          if (hardwareFailures >= 3) {
            initialized = false;
            recoveryResetPending = true;
            lastMeasurementFailure = FailureCause::Hardware;
            communicationFailureSeen = true;
            Serial.println("[VL53L1X] Repeated hardware fail; recovering via XSHUT in this measurement");
            break;
          }
        } else if (sensor.ranging_data.range_status != VL53L1X::RangeValid &&
                   sensor.ranging_data.range_status !=
                       VL53L1X::RangeValidMinRangeClipped &&
                   sensor.ranging_data.range_status !=
                       VL53L1X::RangeValidNoWrapCheckFail) {
          ++opticalFailures;
          lastSignalMCPS = sensor.ranging_data.peak_signal_count_rate_MCPS;
          lastAmbientMCPS = sensor.ranging_data.ambient_count_rate_MCPS;
        } else {
          ++distancesOutsideWindow;
        }
        delay(30);
        if (!initialized) {
          break;
        }
        continue;
      }

      const bool phaseCheckCandidate =
          sensor.ranging_data.range_status == VL53L1X::OutOfBoundsFail;
      const bool signalFailCandidate =
          isRecoverableHeadStatus(sensor.ranging_data.range_status,
                                  distanceMm);

      const bool acceptedRangeStatus =
          sensor.ranging_data.range_status == VL53L1X::RangeValid ||
          sensor.ranging_data.range_status ==
              VL53L1X::RangeValidMinRangeClipped ||
          sensor.ranging_data.range_status ==
              VL53L1X::RangeValidNoWrapCheckFail ||
          phaseCheckCandidate || signalFailCandidate;
      if (!acceptedRangeStatus) {
        ++opticalFailures;
        lastSignalMCPS = sensor.ranging_data.peak_signal_count_rate_MCPS;
        lastAmbientMCPS = sensor.ranging_data.ambient_count_rate_MCPS;
        Serial.printf("[VL53L1X] Rejecting non-standard range status: %s\n",
                      VL53L1X::rangeStatusToString(
                          sensor.ranging_data.range_status));
        delay(20);
        continue;
      }

      const float signalMCPS =
          sensor.ranging_data.peak_signal_count_rate_MCPS;
      const float ambientMCPS = sensor.ranging_data.ambient_count_rate_MCPS;
      const bool plausibleRecoveredCandidate =
          signalFailCandidate && isfinite(signalMCPS) &&
          signalMCPS >= SIGNAL_FAIL_MIN_MCPS;
      const float minimumSignal = plausibleRecoveredCandidate
                                      ? SIGNAL_FAIL_MIN_MCPS
                                      : Config::HeightSensor::MIN_SIGNAL_MCPS;
      if (!isfinite(signalMCPS) ||
          signalMCPS < minimumSignal ||
          signalMCPS >= 500.0f) {
        ++weakSignalSamples;
        lastSignalMCPS = signalMCPS;
        lastAmbientMCPS = ambientMCPS;
        Serial.printf("[VL53L1X] Rejecting weak/saturated return: signal=%.2f MCPS\n",
                      signalMCPS);
        delay(20);
        continue;
      }

      const float candidate =
          Config::HeightSensor::SENSOR_MOUNT_HEIGHT_CM -
          (static_cast<float>(distanceMm) / 10.0f) +
          Config::HeightSensor::HEIGHT_OFFSET_CM;
      Serial.printf("[VL53L1X][RAW][ROI %ux%u] distance=%u mm, calculated=%.1f cm, status=%s, signal=%.2f, ambient=%.2f MCPS\n",
                    currentRoiWidth, currentRoiHeight,
                    distanceMm,
                    candidate,
                    VL53L1X::rangeStatusToString(sensor.ranging_data.range_status),
                    signalMCPS,
                    ambientMCPS);
      if (isfinite(candidate) &&
          candidate >= Config::HeightSensor::MIN_HEIGHT_CM &&
          candidate <= Config::HeightSensor::MAX_HEIGHT_CM) {
        samples[validCount++] = {candidate, distanceMm, signalMCPS,
                                 ambientMCPS};
        if (distanceMm >= SINGLE_HEAD_MIN_DISTANCE_MM &&
            distanceMm <= acceptedHeadMaxDistanceMm &&
            isfinite(signalMCPS) && signalMCPS >= SINGLE_HEAD_MIN_MCPS) {
          ++strongSingleHeadSamples;
          strongSingleHeadSum += candidate;
          strongSingleHeadDistanceSum += distanceMm;
          strongSingleHeadSignalSum += signalMCPS;
          strongSingleHeadMinHeight =
              fminf(strongSingleHeadMinHeight, candidate);
          strongSingleHeadMaxHeight =
              fmaxf(strongSingleHeadMaxHeight, candidate);
        }
        if (phaseCheckCandidate) {
          ++phaseCheckSamples;
        }

        if (validCount >= EARLY_STABLE_SAMPLE_COUNT) {
          const uint8_t first = validCount - EARLY_STABLE_SAMPLE_COUNT;
          float recentMin = samples[first].heightCm;
          float recentMax = samples[first].heightCm;
          for (uint8_t i = first + 1; i < validCount; ++i) {
            recentMin = fminf(recentMin, samples[i].heightCm);
            recentMax = fmaxf(recentMax, samples[i].heightCm);
          }
          if (recentMax - recentMin <= EARLY_STABLE_MAX_SPAN_CM) {
            earlyStableCluster = true;
            Serial.printf("[VL53L1X] Early stable cluster: %u frames span=%.1f cm\n",
                          EARLY_STABLE_SAMPLE_COUNT,
                          recentMax - recentMin);
            break;
          }
        }
      }
      delay(20);
    }

    if (earlyStableCluster) {
      break;
    }

    if (!communicationFailureSeen && validCount == 0 &&
        (distancesOutsideWindow >= 6 || opticalFailures >= 6) &&
        recovery < MEASUREMENT_RECOVERY_ATTEMPTS) {
      initialized = false;
      recoveryResetPending = true;
      communicationFailureSeen = true;
      Serial.printf("[VL53L1X] Optical/I2C lock-up (%u outside, %u optical); re-initializing\n",
                    distancesOutsideWindow, opticalFailures);
    }

    interruptedByCommunication |= communicationFailureSeen;

    if (!communicationFailureSeen || recovery == MEASUREMENT_RECOVERY_ATTEMPTS) {
      Serial.println("[VL53L1X] No stable valid range; check target and sensor alignment");
      break;
    }

    initialized = false;
    recoveryResetPending = true;
    Serial.println("[VL53L1X] Communication failed; re-initializing once");
  }

  if (!initialized && validCount < Config::HeightSensor::MIN_VALID_SAMPLES &&
      strongSingleHeadSamples == 0) {
    lastMeasurementFailure = FailureCause::Communication;
    Serial.println("[VL53L1X] Sensor still offline at end of scan; queue not sent");
    return false;
  }

  if (!initialized && interruptedByCommunication) {
    lastMeasurementFailure = FailureCause::Communication;
    Serial.println("[VL53L1X] Discarding interrupted scan after I2C failure; no height accepted");
    return false;
  }

  if (validCount < Config::HeightSensor::MIN_VALID_SAMPLES &&
      strongSingleHeadSamples == 0) {
    lastScanStats.validSamples = validCount;
    lastScanStats.retryReads = static_cast<uint8_t>(
        totalRetryReads > 255 ? 255 : totalRetryReads);
    lastScanStats.weakSignalSamples = weakSignalSamples;
    lastScanStats.phaseCheckSamples = phaseCheckSamples;

    lastMeasurementFailure =
        initialized ? FailureCause::Target : FailureCause::Communication;
    Serial.printf("[VL53L1X] Quality reject: valid=%u (need >=%u, attempts<=%u) retryReads=%u weakSignal=%u I2Cfail=%u optical=%u hardware=%u outside=%u lastSignal=%.2f ambient=%.2f MCPS\n",
                  validCount,
                  Config::HeightSensor::MIN_VALID_SAMPLES,
                  Config::HeightSensor::MAX_SAMPLE_ATTEMPTS,
                  static_cast<unsigned>(totalRetryReads), weakSignalSamples,
                  communicationFailures, opticalFailures, hardwareFailures,
                  distancesOutsideWindow, lastSignalMCPS, lastAmbientMCPS);
    return false;
  }

  if (validCount < Config::HeightSensor::MIN_HEAD_CLUSTER_SAMPLES &&
      strongSingleHeadSamples >= Config::HeightSensor::MIN_HEAD_CLUSTER_SAMPLES &&
      strongSingleHeadMaxHeight - strongSingleHeadMinHeight <=
          Config::HeightSensor::MAX_SAMPLE_SPAN_CM) {
    heightCm = strongSingleHeadSum /
               static_cast<float>(strongSingleHeadSamples);
    lastScanStats.validSamples = strongSingleHeadSamples;
    lastScanStats.retryReads = static_cast<uint8_t>(
        totalRetryReads > 255 ? 255 : totalRetryReads);
    lastScanStats.medianDistanceMm =
        strongSingleHeadDistanceSum /
        static_cast<float>(strongSingleHeadSamples);
    lastScanStats.medianSignalMCPS =
        strongSingleHeadSignalSum /
        static_cast<float>(strongSingleHeadSamples);
    lastScanStats.spanCm = 0.0f;
    if (rejectNoisyScan(totalRetryReads)) {
      return false;
    }
    lastMeasurementFailure = FailureCause::None;
    Serial.printf("[VL53L1X] Stable single-head fallback=%.1f cm from %u strong frame(s)\n",
                  heightCm, strongSingleHeadSamples);
    return true;
  }

  if (strongSingleHeadSamples >= Config::HeightSensor::MIN_HEAD_CLUSTER_SAMPLES &&
      strongSingleHeadMaxHeight - strongSingleHeadMinHeight <=
          Config::HeightSensor::MAX_SAMPLE_SPAN_CM) {
    heightCm = strongSingleHeadSum /
               static_cast<float>(strongSingleHeadSamples);
    lastScanStats.validSamples = strongSingleHeadSamples;
    lastScanStats.retryReads = static_cast<uint8_t>(
        totalRetryReads > 255 ? 255 : totalRetryReads);
    lastScanStats.medianDistanceMm =
        strongSingleHeadDistanceSum /
        static_cast<float>(strongSingleHeadSamples);
    lastScanStats.medianSignalMCPS =
        strongSingleHeadSignalSum /
        static_cast<float>(strongSingleHeadSamples);
    lastScanStats.spanCm =
        strongSingleHeadMaxHeight - strongSingleHeadMinHeight;
    if (rejectNoisyScan(totalRetryReads)) {
      return false;
    }
    lastMeasurementFailure = FailureCause::None;
    Serial.printf("[VL53L1X] Prioritized head cluster=%.1f cm from %u frame(s); ignored %u background frame(s)\n",
                  heightCm, strongSingleHeadSamples,
                  validCount > strongSingleHeadSamples
                      ? validCount - strongSingleHeadSamples : 0);
    return true;
  }

  for (uint8_t i = 1; i < validCount; ++i) {
    const HeightSample value = samples[i];
    uint8_t j = i;
    while (j > 0 && samples[j - 1].heightCm > value.heightCm) {
      samples[j] = samples[j - 1];
      --j;
    }
    samples[j] = value;
  }

  int bestStart = -1;
  int bestEnd = -1;
  float bestMedianHeight = -INFINITY;
  for (uint8_t start = 0;
       start + Config::HeightSensor::MIN_HEAD_CLUSTER_SAMPLES <= validCount;
       ++start) {
    for (uint8_t end = static_cast<uint8_t>(
             start + Config::HeightSensor::MIN_HEAD_CLUSTER_SAMPLES - 1);
         end < validCount; ++end) {
      const float clusterSpan =
          samples[end].heightCm - samples[start].heightCm;
      if (clusterSpan > Config::HeightSensor::MAX_SAMPLE_SPAN_CM) {
        break;
      }
      const float clusterMedian =
          samples[(start + end) / 2].heightCm;
      if (clusterMedian > bestMedianHeight) {
        bestMedianHeight = clusterMedian;
        bestStart = start;
        bestEnd = end;
      }
    }
  }

  if (bestStart < 0 || bestEnd < bestStart) {
    lastMeasurementFailure =
        initialized ? FailureCause::Target : FailureCause::Communication;
    Serial.printf("[VL53L1X] Quality reject: no stable cluster in %u valid frames\n",
                  validCount);
    return false;
  }

  const uint8_t selectedCount =
      static_cast<uint8_t>(bestEnd - bestStart + 1);
  float heights[Config::HeightSensor::MAX_SAMPLE_ATTEMPTS];
  float distances[Config::HeightSensor::MAX_SAMPLE_ATTEMPTS];
  float signals[Config::HeightSensor::MAX_SAMPLE_ATTEMPTS];
  float ambients[Config::HeightSensor::MAX_SAMPLE_ATTEMPTS];
  for (uint8_t i = 0; i < selectedCount; ++i) {
    const HeightSample &sample = samples[bestStart + i];
    heights[i] = sample.heightCm;
    distances[i] = sample.distanceMm;
    signals[i] = sample.signalMCPS;
    ambients[i] = sample.ambientMCPS;
  }

  const float q1 = heights[selectedCount / 4];
  const float q3 = heights[(selectedCount * 3) / 4];
  const float spanCm = heights[selectedCount - 1] - heights[0];
  const float medianDistanceMm = medianOf(distances, selectedCount);
  const float medianSignalMCPS = medianOf(signals, selectedCount);
  const float medianAmbientMCPS = medianOf(ambients, selectedCount);

  if (!isfinite(medianDistanceMm) ||
      medianDistanceMm < SINGLE_HEAD_MIN_DISTANCE_MM ||
      medianDistanceMm > acceptedHeadMaxDistanceMm) {
    lastMeasurementFailure = FailureCause::Target;
    Serial.printf("[VL53L1X] Quality reject: stable cluster distance=%.0f mm outside head band %u..%u mm\n",
                  medianDistanceMm,
                  static_cast<unsigned>(SINGLE_HEAD_MIN_DISTANCE_MM),
                  static_cast<unsigned>(acceptedHeadMaxDistanceMm));
    return false;
  }
  lastScanStats.validSamples = selectedCount;
  lastScanStats.retryReads = static_cast<uint8_t>(
      totalRetryReads > 255 ? 255 : totalRetryReads);
  lastScanStats.weakSignalSamples = weakSignalSamples;
  lastScanStats.phaseCheckSamples = phaseCheckSamples;
  lastScanStats.medianDistanceMm = medianDistanceMm;
  lastScanStats.medianSignalMCPS = medianSignalMCPS;
  lastScanStats.medianAmbientMCPS = medianAmbientMCPS;
  lastScanStats.spanCm = spanCm;

  Serial.printf("[VL53L1X][ROI %ux%u] selected=%u/%u totalValid=%u retryReads=%u weakSignal=%u phaseCheck=%u medianDistance=%.0fmm medianSignal=%.2fMCPS span=%.1fcm\n",
                currentRoiWidth, currentRoiHeight, selectedCount,
                Config::HeightSensor::MAX_SAMPLE_ATTEMPTS, validCount,
                static_cast<unsigned>(totalRetryReads), weakSignalSamples,
                phaseCheckSamples,
                medianDistanceMm, medianSignalMCPS, spanCm);

  Serial.printf("[VL53L1X] Nearest stable cluster: height %.1f..%.1f cm (%u/%u frames)\n",
                heights[0], heights[selectedCount - 1], selectedCount,
                validCount);

  if ((q3 - q1) > Config::HeightSensor::MAX_IQR_CM) {
    Serial.printf("[VL53L1X] Raw spread IQR=%.1f cm; checking median cluster\n",
                  q3 - q1);
  }

  const float median = heights[selectedCount / 2];
  float sum = 0.0f;
  uint8_t stableCount = 0;
  for (uint8_t i = 0; i < selectedCount; ++i) {
    if (fabsf(heights[i] - median) <=
        Config::HeightSensor::MAX_IQR_CM) {
      sum += heights[i];
      ++stableCount;
    }
  }
  if (stableCount < Config::HeightSensor::MIN_HEAD_CLUSTER_SAMPLES ||
      spanCm > Config::HeightSensor::MAX_SAMPLE_SPAN_CM) {
    lastMeasurementFailure =
        initialized ? FailureCause::Target : FailureCause::Communication;
    Serial.printf("[VL53L1X] Quality reject: stable=%u/%u selected, totalValid=%u span=%.1fcm\n",
                  stableCount, selectedCount, validCount, spanCm);
    return false;
  }
  if (rejectNoisyScan(totalRetryReads)) {
    return false;
  }
  heightCm = sum / static_cast<float>(stableCount);
  lastMeasurementFailure = FailureCause::None;
  Serial.printf("[VL53L1X] Stable height=%.1f cm from %u/%u selected (%u total valid)\n",
                heightCm,
                stableCount,
                selectedCount,
                validCount);
  return true;
}

bool readHeightCmFixedRoi(float &heightCm) {
  if (!initialized && !begin()) {
    lastMeasurementFailure = FailureCause::Communication;
    return false;
  }

  if (!setRoiAndDiscard(Config::HeightSensor::ROI_WIDTH,
                        Config::HeightSensor::ROI_HEIGHT)) {
    Serial.println("[VL53L1X] Production ROI switch/read failed; no result accepted");
    lastMeasurementFailure = FailureCause::Communication;
    return false;
  }
  const bool measured = readHeightCm(heightCm);
  if (currentRoiWidth != Config::HeightSensor::ROI_WIDTH ||
      currentRoiHeight != Config::HeightSensor::ROI_HEIGHT) {
    lastMeasurementFailure = FailureCause::Communication;
    return false;
  }
  return measured;
}

}
