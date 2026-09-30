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
// Match the standalone VL53L1X sketch that returned stable distance readings.
// 25 kHz lengthens each transaction and did not eliminate this unit's I2C errors.
// The installed module/cabling produces corrupted range registers at 100 kHz
// (15-64 m values and frequent requestFrom errors). A slower clock gives the
// sensor enough margin on this separate Wire0 bus.
constexpr uint32_t HEIGHT_I2C_CLOCK_HZ = 50000;
constexpr uint32_t HEIGHT_WIRE_TIMEOUT_MS = 150;
constexpr uint32_t FALLBACK_TIMING_BUDGET_US = 100000;
constexpr float FALLBACK_HEIGHT_THRESHOLD_CM = 155.0f;
constexpr uint8_t MAX_COMMUNICATION_FAILURES = 3;
constexpr uint8_t STARTUP_SAMPLE_ATTEMPTS = 8;
constexpr uint8_t MEASUREMENT_RECOVERY_ATTEMPTS = 2;
constexpr uint32_t MAX_MEASUREMENT_MS = 10000;
constexpr uint8_t EARLY_STABLE_SAMPLE_COUNT = 5;
constexpr float EARLY_STABLE_MAX_SPAN_CM = 1.0f;
// Dark hair / low-reflectance heads on this unit are sometimes reported as
// SignalFail even though the range register still contains the head distance.
// Only recover a SignalFail value inside the plausible head interval; this
// prevents stale 500+ mm body/floor values from being accepted as a head.
constexpr uint16_t SIGNAL_FAIL_HEAD_MIN_DISTANCE_MM = 120;
constexpr uint16_t SIGNAL_FAIL_HEAD_MAX_DISTANCE_MM = 450;
constexpr float SIGNAL_FAIL_MIN_MCPS = 0.05f;
// If the bus dies after one clean head frame, retain it only when it is in
// the plausible head-distance band for this 197.3 cm mount.  This covers
// people roughly 150--190 cm tall while excluding the 500--800 mm torso/floor
// returns seen in the failing logs.
constexpr uint16_t SINGLE_HEAD_MIN_DISTANCE_MM = 100;
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
         status == VL53L1X::XtalkSignalFail ||
         status == VL53L1X::WrapTargetFail;
}
bool recoveryResetPending = false;
VL53L1X::DistanceMode activeDistanceMode = VL53L1X::Long;
uint32_t activeTimingBudgetUs = Config::HeightSensor::TIMING_BUDGET_US;

// XSHUT is the only reliable recovery when the sensor no longer ACKs 0x29.
// Reset only the VL53L1X; the ESP32, LCD, network and printer stay running.
void resetSensorViaXshut() {
  // This breakout does not release XSHUT reliably from an open-drain GPIO,
  // so drive the 3.3 V logic level explicitly from the ESP32.
  digitalWrite(Config::Pins::HEIGHT_XSHUT, LOW);
  pinMode(Config::Pins::HEIGHT_XSHUT, OUTPUT);
  delay(50);
  digitalWrite(Config::Pins::HEIGHT_XSHUT, HIGH);
  delay(350);
  recoveryResetPending = false;
  Serial.printf("[VL53L1X] XSHUT hardware reset on GPIO%u\n",
                Config::Pins::HEIGHT_XSHUT);
}

// Recover a slave that left SDA low after a partial/aborted transaction.
// This is the only reset available when XSHUT is not wired.  It is harmless
// when the bus is already idle and avoids requiring a hardware modification.
void recoverI2CBus() {
  const uint8_t sda = Config::Pins::HEIGHT_I2C_SDA;
  const uint8_t scl = Config::Pins::HEIGHT_I2C_SCL;

  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  delayMicroseconds(10);

  pinMode(scl, OUTPUT_OPEN_DRAIN);
  digitalWrite(scl, HIGH);  // release SCL
  for (uint8_t pulse = 0; pulse < 9; ++pulse) {
    digitalWrite(scl, LOW);
    delayMicroseconds(8);
    digitalWrite(scl, HIGH);
    delayMicroseconds(8);
  }

  // Generate an I2C STOP: SDA low -> SCL high -> SDA high.
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
  // A repeated diagnostic scan can reuse the running ROI. Stopping and
  // restarting the sensor on every scan creates several extra transactions
  // on this intermittently failing I2C link.
  if (currentRoiWidth == width && currentRoiHeight == height) {
    return true;
  }
  sensor.stopContinuous();
  if (sensor.communicationError()) {
    initialized = false;
    recoveryResetPending = true;
    return false;
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

  sensor.startContinuous(Config::HeightSensor::CONTINUOUS_PERIOD_MS);
  if (sensor.communicationError()) {
    initialized = false;
    recoveryResetPending = true;
    return false;
  }
  delay(Config::HeightSensor::CONTINUOUS_PERIOD_MS * 2 + 20);
  for (uint8_t frame = 0; frame < 2; ++frame) {
    sensor.read();
    sensor.timeoutOccurred();
    if (sensor.communicationError()) {
      initialized = false;
      recoveryResetPending = true;
      return false;
    }
  }
  return true;
}
}  // namespace

bool begin() {
  Serial.println("[VL53L1X] Initializing...");
  // VL53L1X owns Wire0 exclusively; LCD is isolated on Wire1.
  // Fully reset Wire0 before assigning its pins. This is the important part
  // of the standalone pin-mapping scanner that reliably finds 0x29. Without
  // end(), an earlier failed init/retry can leave the ESP32 I2C controller
  // attached to stale pins and every later probe returns NACK.
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
  // Keep the bus timeout short so a failed I2C packet cannot consume most of
  // the eight-second measurement window. The sensor's separate ranging
  // timeout remains 500 ms for a new optical result.
  heightWire.setClock(HEIGHT_I2C_CLOCK_HZ);
  heightWire.setTimeOut(HEIGHT_WIRE_TIMEOUT_MS);
  sensor.setBus(&heightWire);
  Serial.printf("[VL53L1X] Wire0 SDA=GPIO%u SCL=GPIO%u Clock=%lu Hz\n",
                Config::Pins::HEIGHT_I2C_SDA,
                Config::Pins::HEIGHT_I2C_SCL,
                static_cast<unsigned long>(HEIGHT_I2C_CLOCK_HZ));
  sensor.setTimeout(Config::HeightSensor::TIMEOUT_MS);

  // setup() already waits for board power-up. Keep retries short so network
  // and keypad processing are not blocked when the sensor is disconnected.
  delay(100);

  // Keep this sequence identical to the standalone sketch that is known to
  // work on this module: set timeout, then call init directly on Wire0.
  // Guard the library: on a disconnected/stuck bus its init path can divide
  // by a zero oscillator value and panic the ESP32 instead of returning false.
  heightWire.beginTransmission(0x29);
  uint8_t ackStatus = heightWire.endTransmission();
  // Probe a few times before declaring the module offline.  This handles the
  // short boot window seen when the separate 3.3 V rail rises more slowly than
  // the ESP32.
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

  // Clear a timeout latched by the previous ranging session before starting
  // a fresh one.
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

  // This module has produced the stable head returns in Long mode (roughly
  // 150-220 mm for the installed mount). Short mode on this clone often
  // saturates at 64 m / wrap values and loses the head completely, so use the
  // proven Long profile and reject only the out-of-window ranges below.
  const bool modeReady = sensor.setDistanceMode(activeDistanceMode);
  sensor.setROISize(Config::HeightSensor::ROI_WIDTH,
                    Config::HeightSensor::ROI_HEIGHT);
  currentRoiWidth = Config::HeightSensor::ROI_WIDTH;
  currentRoiHeight = Config::HeightSensor::ROI_HEIGHT;
  const bool timingReady = sensor.setMeasurementTimingBudget(
      activeTimingBudgetUs);
  sensor.startContinuous(Config::HeightSensor::CONTINUOUS_PERIOD_MS);
  if (!modeReady || !timingReady || sensor.communicationError()) {
    Serial.println("[VL53L1X] ERROR: ranging configuration failed");
    recoveryResetPending = true;
    return false;
  }
  Serial.printf("[VL53L1X] Distance mode=%s timing=%lu us\n",
                activeDistanceMode == VL53L1X::Long ? "Long" :
                activeDistanceMode == VL53L1X::Medium ? "Medium" : "Short",
                static_cast<unsigned long>(activeTimingBudgetUs));

  // init() can return true after a transient I2C error even though continuous
  // ranging never started. Require an actual sensor update before reporting
  // that this module is ready.
  bool rangingStarted = false;
  for (uint8_t attempt = 0; attempt < STARTUP_SAMPLE_ATTEMPTS; ++attempt) {
    delay(Config::HeightSensor::CONTINUOUS_PERIOD_MS + 20);
    sensor.read();
    const uint16_t startupDistance = sensor.ranging_data.range_mm;
    // At boot the platform is empty, so the first real return can be the
    // floor/background around 1.9 m.  Accept that as proof that ranging is
    // alive; the stricter 80..1200 mm head window is applied only by
    // readDistanceMm() during an actual height scan.
    const bool plausibleStartupDistance = startupDistance >= 40 &&
                                          startupDistance <= 4000;
    const bool usableStartupStatus =
        sensor.ranging_data.range_status != VL53L1X::None &&
        sensor.ranging_data.range_status != VL53L1X::HardwareFail;
    // Do not call the sensor Ready merely because a register transaction
    // completed.  This module can return a syntactically valid status with
    // raw=0 or raw=64351 while its ranging engine is still recovering.
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

  // A single fresh frame is enough to prove that the existing continuous
  // session and I2C link are alive. Optical statuses such as SignalFail are
  // acceptable here because this is a communication check, not a height.
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    sensor.read();
    const bool timedOut = sensor.timeoutOccurred();
    const bool communicationFailed = sensor.communicationError();
    if (!timedOut && !communicationFailed && sensor.last_status == 0 &&
        sensor.ranging_data.range_status != VL53L1X::None &&
        sensor.ranging_data.range_status != VL53L1X::HardwareFail) {
      consecutiveCommunicationFailures = 0;
      Serial.println("[VL53L1X] Existing ranging session is healthy");
      return true;
    }
    delay(Config::HeightSensor::CONTINUOUS_PERIOD_MS + 10);
  }

  initialized = false;
  recoveryResetPending = true;
  Serial.println("[VL53L1X] Existing session unhealthy; XSHUT recovery required");
  return false;
}

FailureCause lastFailureCause() {
  return lastMeasurementFailure;
}

bool readDistanceMm(uint16_t &distanceMm) {
  lastReadRetryCount = 0;
  if (!initialized) {
    return false;
  }

  // A single requestFrom() error is common on this clone while the next range
  // result is being latched. Retry the same sample before counting it as a
  // communication failure.
  bool readOk = false;
  uint32_t retryEvents = 0;
  for (uint8_t retry = 0; retry < 3; ++retry) {
    distanceMm = sensor.read();
    retryEvents += sensor.lastReadI2CRetryCount();
    if (!sensor.timeoutOccurred() && !sensor.communicationError() &&
        sensor.last_status == 0 &&
        sensor.ranging_data.range_status != VL53L1X::None) {
      readOk = true;
      break;
    }
    if (retry < 2) {
      delay(12);
    }
  }
  lastReadRetryCount = static_cast<uint8_t>(
      retryEvents > 255 ? 255 : retryEvents);

  if (!readOk) {
    ++consecutiveCommunicationFailures;
    Serial.printf("[VL53L1X] Read communication failure %u/%u\n",
                  consecutiveCommunicationFailures,
                  MAX_COMMUNICATION_FAILURES);
    if (consecutiveCommunicationFailures >= MAX_COMMUNICATION_FAILURES) {
      initialized = false;
      recoveryResetPending = true;
      Serial.println("[VL53L1X] Sensor offline; recovery armed");
    }
    return false;
  }

  consecutiveCommunicationFailures = 0;

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
    // A signal-fail value can contain the previous measurement (for example
    // the repeated 565 mm seen on this unit). Never use it for a height value;
    // force the recovery path to obtain a fresh, valid range instead.
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

  // A 1 mm result is a stale/invalid register value on this module, not a
  // possible head distance. Log occasional out-of-window ranges so a missed
  // head can be distinguished from weak signal or bus loss.
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

  // Keep all valid frames from the scan. The first valid frames can be a
  // farther shoulder/background return; selecting the nearest stable cluster
  // later gives the head a chance to win when it appears after those frames.
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
  const uint32_t startedAt = millis();

  for (uint8_t recovery = 0;
       recovery <= MEASUREMENT_RECOVERY_ATTEMPTS &&
       millis() - startedAt < MAX_MEASUREMENT_MS;
       ++recovery) {
    bool communicationFailureSeen = false;

    if (!initialized) {
      // A new ranging session can have a different target/ROI state. Never
      // combine samples from before a communication failure with samples
      // collected after reinitializing the sensor.
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
      // The VL53L1X library reports a successful measurement with a
      // "no wrap check fail" status on some targets/ROIs. It is still a
      // usable range result; rejecting it here made shorter people appear as
      // valid=0/7 even though the sensor had returned data.
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

      // A failed interrupt-clear transaction can make this clone return the
      // same completed frame again. Repeated frames are not new evidence of a
      // stable target, even when their range status and signal look valid.
      if (validCount > 0 &&
          samples[validCount - 1].distanceMm == distanceMm &&
          samples[validCount - 1].signalMCPS == signalMCPS &&
          samples[validCount - 1].ambientMCPS == ambientMCPS) {
        Serial.println("[VL53L1X] Duplicate ranging frame; skipping");
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
            distanceMm <= SINGLE_HEAD_MAX_DISTANCE_MM &&
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

    // An optical lock-up can return syntactically valid frames with absurd
    // distances without incrementing the I2C communication counter. Force one
    // clean reinitialization so the next recovery pass can obtain a fresh
    // ranging session instead of repeating the same corrupt register values.
    if (!communicationFailureSeen && validCount == 0 &&
        (distancesOutsideWindow >= 6 || opticalFailures >= 6) &&
        recovery < MEASUREMENT_RECOVERY_ATTEMPTS) {
      initialized = false;
      recoveryResetPending = true;
      communicationFailureSeen = true;
      Serial.printf("[VL53L1X] Optical/I2C lock-up (%u outside, %u optical); re-initializing\n",
                    distancesOutsideWindow, opticalFailures);
    }

    // If a stable cluster was already captured, keep it even when the bus
    // fails on a later frame. Reinitializing here would discard the good
    // measurements and make an otherwise readable person fail the whole scan.
    if (communicationFailureSeen &&
        (validCount >= Config::HeightSensor::MIN_VALID_SAMPLES ||
         strongSingleHeadSamples > 0)) {
      Serial.printf("[VL53L1X] Preserving %u valid + %u strong head samples before late I2C failure\n",
                    validCount, strongSingleHeadSamples);
      break;
    }

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
  if (!initialized) {
    Serial.println("[VL53L1X] Sensor went offline after usable samples; processing preserved cluster");
  }

  if (validCount < Config::HeightSensor::MIN_VALID_SAMPLES &&
      strongSingleHeadSamples == 0) {
    lastScanStats.validSamples = validCount;
    lastScanStats.retryReads = static_cast<uint8_t>(
        totalRetryReads > 255 ? 255 : totalRetryReads);
    lastScanStats.weakSignalSamples = weakSignalSamples;
    lastScanStats.phaseCheckSamples = phaseCheckSamples;
    lastMeasurementFailure =
        communicationFailures > 0 || !initialized
            ? FailureCause::Communication : FailureCause::Target;
    Serial.printf("[VL53L1X] Quality reject: valid=%u (need >=%u, attempts<=%u) retryReads=%u weakSignal=%u I2Cfail=%u optical=%u hardware=%u outside=%u lastSignal=%.2f ambient=%.2f MCPS\n",
                  validCount,
                  Config::HeightSensor::MIN_VALID_SAMPLES,
                  Config::HeightSensor::MAX_SAMPLE_ATTEMPTS,
                  static_cast<unsigned>(totalRetryReads), weakSignalSamples,
                  communicationFailures, opticalFailures, hardwareFailures,
                  distancesOutsideWindow, lastSignalMCPS, lastAmbientMCPS);
    return false;
  }

  if (validCount < Config::HeightSensor::MIN_VALID_SAMPLES &&
      strongSingleHeadSamples > 0) {
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
    lastMeasurementFailure = FailureCause::None;
    Serial.printf("[VL53L1X] Stable single-head fallback=%.1f cm from %u strong frame(s)\n",
                  heightCm, strongSingleHeadSamples);
    return true;
  }

  // A shorter person's head can appear only intermittently while the ROI
  // keeps returning a very stable torso/background distance (for example
  // 369 mm head versus 493-501 mm body). Prefer the tight, strong head band
  // whenever it is internally consistent, even if the background has more
  // frames; otherwise the nearest-cluster-by-count logic reports ~148 cm.
  if (strongSingleHeadSamples > 0 &&
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

  // The sorted list may contain more than one target: a shoulder/background
  // return followed by the actual head return. Select the highest (nearest)
  // contiguous stable cluster instead of taking the median of every frame.
  // Height is mount-height minus range, so the head is the larger height.
  int bestStart = -1;
  int bestEnd = -1;
  float bestMedianHeight = -INFINITY;
  for (uint8_t start = 0;
       start + Config::HeightSensor::MIN_VALID_SAMPLES <= validCount;
       ++start) {
    for (uint8_t end = static_cast<uint8_t>(
             start + Config::HeightSensor::MIN_VALID_SAMPLES - 1);
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
    lastMeasurementFailure = FailureCause::Target;
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

  // Accept only readings near the median. A trimmed mean of four samples can
  // still include a single shoulder/background hit and print a false height.
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
  if (stableCount < Config::HeightSensor::MIN_VALID_SAMPLES ||
      spanCm > Config::HeightSensor::MAX_SAMPLE_SPAN_CM) {
    lastMeasurementFailure = FailureCause::Target;
    Serial.printf("[VL53L1X] Quality reject: stable=%u/%u selected, totalValid=%u span=%.1fcm\n",
                  stableCount, selectedCount, validCount, spanCm);
    return false;
  }
  if (totalRetryReads >
      Config::HeightSensor::I2C_RETRY_WARNING_PER_ROI) {
    Serial.printf("[VL53L1X] I2C warning: %u retry reads; accepting stable ROI samples\n",
                  static_cast<unsigned>(totalRetryReads));
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

bool readHeightCmMultiRoi(float &heightCm) {
  if (!initialized && !begin()) {
    lastMeasurementFailure = FailureCause::Communication;
    return false;
  }

  Serial.println("[VL53L1X] Multi-ROI quality scan: 16x16 + centered 12x12");
  if ((currentRoiWidth != Config::HeightSensor::FULL_ROI_WIDTH ||
       currentRoiHeight != Config::HeightSensor::FULL_ROI_HEIGHT) &&
      !setRoiAndDiscard(Config::HeightSensor::FULL_ROI_WIDTH,
                        Config::HeightSensor::FULL_ROI_HEIGHT)) {
    lastMeasurementFailure = FailureCause::Communication;
    Serial.println("[VL53L1X] Quality reject: could not prepare 16x16 ROI");
    return false;
  }

  float fullHeightCm = 0.0f;
  const bool fullOk = readHeightCm(fullHeightCm);
  const FailureCause fullFailure = lastMeasurementFailure;
  const ScanStats fullStats = lastScanStats;

  bool narrowOk = setRoiAndDiscard(12, 12);
  FailureCause narrowFailure =
      narrowOk ? FailureCause::None : FailureCause::Communication;
  float narrowHeightCm = 0.0f;
  ScanStats narrowStats;
  if (narrowOk) {
    narrowOk = readHeightCm(narrowHeightCm);
    narrowFailure = lastMeasurementFailure;
    narrowStats = lastScanStats;
    narrowOk = narrowOk && initialized && currentRoiWidth == 12 &&
               currentRoiHeight == 12;
    if (!narrowOk && narrowFailure == FailureCause::None) {
      narrowFailure = FailureCause::Communication;
    }
  }

  // A communication failure in the secondary ROI must not delay the already
  // validated primary measurement. Recover the sensor after the queue path
  // finishes instead of blocking the person on another I2C initialization.
  const bool primaryFallback =
      fullOk && fullStats.phaseCheckSamples == 0 && !narrowOk &&
      narrowFailure == FailureCause::Communication;
  if (primaryFallback) {
    Serial.println("[VL53L1X] Secondary ROI lost communication; sensor recovery deferred");
  }

  // Keep the running 12x12 session. Only another explicit multi-ROI
  // diagnostic will switch back to 16x16. A full init after every scan
  // previously added dozens of I2C transfers and could fail after a valid
  // height was measured.

  if (!fullOk) {
    // This module can fail the phase check across the full field of view
    // while producing a clean, stable centered 12x12 range in Long mode.
    // Use that independent scan only when the primary ROI provided no usable
    // samples, so conflicting measurements cannot be silently overridden.
    if (narrowOk && fullStats.validSamples == 0 &&
        narrowStats.phaseCheckSamples == 0) {
      heightCm = narrowHeightCm;
      lastMeasurementFailure = FailureCause::None;
      Serial.printf("[VL53L1X] Fallback: 16x16 had no valid samples; using validated 12x12 height=%.1f cm\n",
                    heightCm);
      return true;
    }
    lastMeasurementFailure = fullFailure;
    Serial.printf("[VL53L1X] Quality reject: ROI16=%s ROI12=%s\n",
                  fullOk ? "PASS" : "FAIL",
                  narrowOk ? "PASS" : "FAIL");
    return false;
  }

  if (!narrowOk) {
    if (!primaryFallback) {
      lastMeasurementFailure = narrowFailure;
      Serial.println("[VL53L1X] Quality reject: 12x12 check failed; primary result requires secondary confirmation");
      return false;
    }
    heightCm = fullHeightCm;
    lastMeasurementFailure = FailureCause::None;
    Serial.printf("[VL53L1X] Fallback: 12x12 communication failed; using validated 16x16 height=%.1f cm\n",
                  heightCm);
    return true;
  }

  const float roiDistanceDifference =
      fabsf(fullStats.medianDistanceMm - narrowStats.medianDistanceMm);
  Serial.printf("[VL53L1X] ROI comparison: 16x16 %.0fmm / %.1fcm, 12x12 %.0fmm / %.1fcm, delta=%.0fmm\n",
                fullStats.medianDistanceMm, fullHeightCm,
                narrowStats.medianDistanceMm, narrowHeightCm,
                roiDistanceDifference);
  if (roiDistanceDifference >
      Config::HeightSensor::MAX_ROI_DISTANCE_DIFFERENCE_MM) {
    lastMeasurementFailure = FailureCause::Target;
    Serial.printf("[VL53L1X] Quality reject: ROI distance delta %.0fmm exceeds %umm\n",
                  roiDistanceDifference,
                  Config::HeightSensor::MAX_ROI_DISTANCE_DIFFERENCE_MM);
    return false;
  }

  // This diagnostic reports the full-field value after the ROI comparison.
  heightCm = fullHeightCm;
  lastMeasurementFailure = FailureCause::None;
  if (fullStats.phaseCheckSamples > 0 || narrowStats.phaseCheckSamples > 0) {
    Serial.printf("[VL53L1X] Warning: accepted phase-check candidates 16x16=%u 12x12=%u after stable cross-ROI agreement\n",
                  fullStats.phaseCheckSamples,
                  narrowStats.phaseCheckSamples);
  }
  Serial.printf("[VL53L1X] Multi-ROI quality PASS; accepted height=%.1f cm\n",
                heightCm);
  return true;
}

bool readHeightCmRoi12Diagnostic(float &heightCm) {
  Serial.println("[HEIGHTTEST12] Trying centered 12x12 ROI; NO queue/print");
  return readHeightCmFixedRoi(heightCm);
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

}  // namespace VL53L1XDevice
