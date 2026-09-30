#include <Arduino.h>
#include <VL53L1X.h>
#include <Wire.h>
#include <math.h>

#include "config.h"

namespace {
VL53L1X sensor;
TwoWire &heightWire = Wire;

// Ten reference points.  The operator moves a matte target to the requested
// distance and sends one capital B in Serial Monitor to capture that point.
constexpr uint8_t POINT_COUNT = 10;
constexpr float EXPECTED_CM[POINT_COUNT] = {
    10.0f, 20.0f, 30.0f, 40.0f, 50.0f,
    60.0f, 70.0f, 80.0f, 90.0f, 100.0f};
constexpr uint8_t CAPTURE_SAMPLES = 9;
constexpr uint8_t MIN_CAPTURE_VALID = 7;
// One B command performs one bounded capture.  It must return to idle after
// failure so the operator must select a distance and explicitly press B again.
constexpr uint16_t MAX_CAPTURE_ATTEMPTS = 30;
constexpr float MAX_ACCEPTED_SPAN_MM = 20.0f;
// A grossly wrong but repeated return (for example 503 mm while the target is
// 100 mm) must not be saved as a calibration point.  Small optical offsets
// remain acceptable and are fitted by the final scale/offset regression.
constexpr float MAX_ACCEPTED_ABS_ERROR_MM = 150.0f;

uint8_t nextPoint = 0;
float capturedRawMm[POINT_COUNT] = {};
bool captured[POINT_COUNT] = {};
bool keyBWasDown = false;
int8_t armedPoint = -1;

void resetSensorViaXshut() {
  pinMode(Config::Pins::HEIGHT_XSHUT, OUTPUT);
  digitalWrite(Config::Pins::HEIGHT_XSHUT, LOW);
  delay(50);
  digitalWrite(Config::Pins::HEIGHT_XSHUT, HIGH);
  delay(350);
  Serial.printf("[VL53L1X] XSHUT reset on GPIO%u\n",
                Config::Pins::HEIGHT_XSHUT);
}

float median(float values[], uint8_t count) {
  for (uint8_t i = 1; i < count; ++i) {
    const float value = values[i];
    uint8_t j = i;
    while (j > 0 && values[j - 1] > value) {
      values[j] = values[j - 1];
      --j;
    }
    values[j] = value;
  }
  return count == 0 ? NAN : values[count / 2];
}

bool validRangeStatus(VL53L1X::RangeStatus status) {
  return status == VL53L1X::RangeValid ||
         status == VL53L1X::RangeValidMinRangeClipped ||
         status == VL53L1X::RangeValidNoWrapCheckFail;
}

void recoverI2CBus() {
  const uint8_t sda = Config::Pins::HEIGHT_I2C_SDA;
  const uint8_t scl = Config::Pins::HEIGHT_I2C_SCL;
  heightWire.end();
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, OUTPUT_OPEN_DRAIN);
  digitalWrite(scl, HIGH);
  delayMicroseconds(10);
  // Release a slave that was left mid-transaction by providing clock pulses.
  for (uint8_t i = 0; i < 9; ++i) {
    digitalWrite(scl, LOW);
    delayMicroseconds(8);
    digitalWrite(scl, HIGH);
    delayMicroseconds(8);
  }
  // Generate a STOP condition: SDA low while SCL high, then release SDA.
  pinMode(sda, OUTPUT_OPEN_DRAIN);
  digitalWrite(sda, LOW);
  delayMicroseconds(8);
  digitalWrite(scl, HIGH);
  delayMicroseconds(8);
  digitalWrite(sda, HIGH);
  delayMicroseconds(20);
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  Serial.printf("[I2C] bus recovery SDA=%d SCL=%d\n",
                digitalRead(sda), digitalRead(scl));
}

bool initializeSensor() {
  resetSensorViaXshut();
  recoverI2CBus();
  delay(25);
  heightWire.begin(Config::Pins::HEIGHT_I2C_SDA,
                   Config::Pins::HEIGHT_I2C_SCL,
                   50000);
  heightWire.setClock(50000);
  heightWire.setTimeOut(150);
  sensor.setBus(&heightWire);
  sensor.setTimeout(Config::HeightSensor::TIMEOUT_MS);
  if (!sensor.init()) {
    Serial.println("[VL53L1X] Recovery init failed; check VCC/GND/SDA/SCL.");
    return false;
  }
  sensor.setDistanceMode(VL53L1X::Long);
  sensor.setROISize(Config::HeightSensor::ROI_WIDTH,
                    Config::HeightSensor::ROI_HEIGHT);
  sensor.setMeasurementTimingBudget(Config::HeightSensor::TIMING_BUDGET_US);
  sensor.startContinuous(Config::HeightSensor::CONTINUOUS_PERIOD_MS);
  Serial.println("[VL53L1X] Recovery init OK; retry the same point.");
  return true;
}

bool readValidDistance(uint16_t &distanceMm) {
  distanceMm = sensor.read();
  const bool valid = !sensor.timeoutOccurred() &&
                     sensor.last_status == 0 &&
                     distanceMm >= 40 && distanceMm <= 4000 &&
                     validRangeStatus(sensor.ranging_data.range_status);
  return valid;
}

void printSummary() {
  Serial.println();
  Serial.println("========== VL53 CALIBRATION SUMMARY ==========");
  Serial.println("expected_cm,raw_mm,error_mm");

  uint8_t count = 0;
  double sumX = 0.0;
  double sumY = 0.0;
  double sumXX = 0.0;
  double sumXY = 0.0;
  float minError = INFINITY;
  float maxError = -INFINITY;

  for (uint8_t i = 0; i < POINT_COUNT; ++i) {
    if (!captured[i]) {
      continue;
    }
    const float expectedMm = EXPECTED_CM[i] * 10.0f;
    const float errorMm = capturedRawMm[i] - expectedMm;
    Serial.printf("%.1f,%.1f,%.1f\n",
                  EXPECTED_CM[i], capturedRawMm[i], errorMm);
    minError = fminf(minError, errorMm);
    maxError = fmaxf(maxError, errorMm);

    const double x = capturedRawMm[i];
    const double y = expectedMm;
    sumX += x;
    sumY += y;
    sumXX += x * x;
    sumXY += x * y;
    ++count;
  }

  if (count >= 2) {
    const double denominator = count * sumXX - sumX * sumX;
    if (fabs(denominator) > 1e-9) {
      const double scale = (count * sumXY - sumX * sumY) / denominator;
      const double offsetMm = (sumY - scale * sumX) / count;
      Serial.printf("error_range_mm=%.1f..%.1f\n", minError, maxError);
      Serial.printf("distance_scale=%.9f\n", scale);
      Serial.printf("distance_offset_mm=%.3f\n", offsetMm);
      Serial.println("Use these two values in the production height formula.");
    }
  }
  Serial.println("==============================================");
}

void updateNextPoint() {
  while (nextPoint < POINT_COUNT && captured[nextPoint]) {
    ++nextPoint;
  }
}

void capturePoint(uint8_t pointIndex) {
  if (pointIndex >= POINT_COUNT) {
    Serial.println("[CAL] Point must be from 10 to 100 cm in 10 cm steps.");
    return;
  }
  const float expectedCm = EXPECTED_CM[pointIndex];
  Serial.printf("[CAL] Capturing%s target at %.1f cm (%u/%u)...\n",
                captured[pointIndex] ? " again" : "",
                expectedCm,
                pointIndex + 1,
                POINT_COUNT);

  float samples[CAPTURE_SAMPLES] = {};
  uint8_t validCount = 0;
  uint16_t attempts = 0;
  uint16_t lastDistance = 0;
  while (validCount < CAPTURE_SAMPLES &&
         attempts < MAX_CAPTURE_ATTEMPTS) {
    ++attempts;
    uint16_t distanceMm = 0;
    if (readValidDistance(distanceMm)) {
      samples[validCount++] = distanceMm;
      lastDistance = distanceMm;
    }
    delay(100);
  }

  if (validCount < MIN_CAPTURE_VALID) {
    Serial.printf("[CAL] FAILED: valid=%u/%u attempts=%u last=%u mm\n",
                  validCount,
                  CAPTURE_SAMPLES,
                  attempts,
                  lastDistance);
    Serial.println("[CAL] No average saved; capture stopped. Select the distance again, then press B.");
    Serial.println("[CAL] Attempting one sensor re-initialization while idle...");
    initializeSensor();
    return;
  }

  const float rawMedianMm = median(samples, validCount);
  float minMm = samples[0];
  float maxMm = samples[0];
  for (uint8_t i = 1; i < validCount; ++i) {
    minMm = fminf(minMm, samples[i]);
    maxMm = fmaxf(maxMm, samples[i]);
  }
  const float spanMm = maxMm - minMm;
  if (spanMm > MAX_ACCEPTED_SPAN_MM) {
    Serial.printf("[CAL] QUALITY REJECT point=%.1f cm span=%.1f mm (max %.1f mm); "
                  "old value kept, measure this point again.\n",
                  expectedCm,
                  spanMm,
                  MAX_ACCEPTED_SPAN_MM);
    return;
  }
  const float errorMm = rawMedianMm - expectedCm * 10.0f;
  if (fabsf(errorMm) > MAX_ACCEPTED_ABS_ERROR_MM) {
    Serial.printf("[CAL] QUALITY REJECT point=%.1f cm median=%.1f mm error=%.1f mm "
                  "(max absolute %.1f mm); target likely wrong or stale.\n",
                  expectedCm,
                  rawMedianMm,
                  errorMm,
                  MAX_ACCEPTED_ABS_ERROR_MM);
    return;
  }
  float sumMm = 0.0f;
  for (uint8_t i = 0; i < validCount; ++i) {
    sumMm += samples[i];
  }
  const float meanMm = sumMm / validCount;
  capturedRawMm[pointIndex] = rawMedianMm;
  captured[pointIndex] = true;

  Serial.printf("[CAL] OK point=%u expected=%.1f cm mean=%.1f mm median=%.1f mm "
                "error_median=%.1f mm span=%.1f mm valid=%u/%u\n",
                pointIndex + 1,
                expectedCm,
                meanMm,
                rawMedianMm,
                errorMm,
                spanMm,
                validCount,
                attempts);
  updateNextPoint();

  if (nextPoint < POINT_COUNT) {
    Serial.printf("[CAL] Next suggested target %.1f cm. Send its number or B.\n",
                  EXPECTED_CM[nextPoint]);
  } else {
    Serial.println("[CAL] Completed all 10 points.");
    printSummary();
  }
}

void captureNextPoint() {
  updateNextPoint();
  if (nextPoint >= POINT_COUNT) {
    Serial.println("[CAL] All points already captured. Send S for summary or R to reset.");
    printSummary();
    return;
  }
  capturePoint(nextPoint);
}

void resetCapture() {
  nextPoint = 0;
  armedPoint = -1;
  for (uint8_t i = 0; i < POINT_COUNT; ++i) {
    captured[i] = false;
    capturedRawMm[i] = 0.0f;
  }
  Serial.println("[CAL] Reset. Place target at 10.0 cm, send 10, then press B.");
}

void printHelp() {
  Serial.println();
  Serial.println("VL53 distance calibration commands:");
  Serial.println("  10..100  select/arm that target distance (does not start read)");
  Serial.println("  B  start the armed point; keypad B also works");
  Serial.println("  S  print captured points and scale/offset");
  Serial.println("  R  reset and start again at 10 cm");
  Serial.println("  ?  show this help");
  Serial.println();
}

// The production keypad is a direct 4x4 matrix.  Supporting its B key here
// lets the operator press the physical B button while still seeing all data
// in the Serial Monitor.  Matrix layout is the usual 1 2 3 A / 4 5 6 B / ...
bool physicalBPressed() {
  bool pressed = false;
  for (uint8_t row = 0; row < 4; ++row) {
    for (uint8_t r = 0; r < 4; ++r) {
      digitalWrite(Config::Keypad::DIRECT_ROW_PINS[r], r == row ? LOW : HIGH);
    }
    delayMicroseconds(150);
    for (uint8_t column = 0; column < 4; ++column) {
      if (digitalRead(Config::Keypad::DIRECT_COLUMN_PINS[column]) == LOW &&
          Config::Keypad::DIRECT_KEY_MAP[row][column] == 'B') {
        pressed = true;
      }
    }
  }
  for (uint8_t r = 0; r < 4; ++r) {
    digitalWrite(Config::Keypad::DIRECT_ROW_PINS[r], HIGH);
  }
  return pressed;
}

void handleSerial() {
  if (!Serial.available()) {
    return;
  }
  String command = Serial.readStringUntil('\n');
  command.trim();
  if (command.equalsIgnoreCase("B")) {
    if (armedPoint >= 0) {
      const uint8_t point = static_cast<uint8_t>(armedPoint);
      armedPoint = -1;
      capturePoint(point);
    } else {
      Serial.println("[CAL] B ignored: select a distance (10..100) first.");
    }
  } else if (command.equalsIgnoreCase("S")) {
    printSummary();
  } else if (command.equalsIgnoreCase("R")) {
    resetCapture();
  } else {
    const float requestedCm = command.toFloat();
    int pointIndex = static_cast<int>(lroundf(requestedCm / 10.0f)) - 1;
    if (pointIndex >= 0 && pointIndex < POINT_COUNT &&
        fabsf(requestedCm - EXPECTED_CM[pointIndex]) < 0.01f) {
      armedPoint = static_cast<int8_t>(pointIndex);
      Serial.printf("[CAL] Armed %.1f cm%s. Press physical B (or send B) to start.\n",
                    EXPECTED_CM[pointIndex],
                    captured[pointIndex] ? " for recapture" : "");
    } else {
      printHelp();
    }
  }
}

void handlePhysicalKeypad() {
  const bool keyBDown = physicalBPressed();
  if (keyBDown && !keyBWasDown) {
    Serial.println("[CAL] Physical keypad B pressed");
    if (armedPoint >= 0) {
      const uint8_t point = static_cast<uint8_t>(armedPoint);
      armedPoint = -1;
      capturePoint(point);
    } else {
      Serial.println("[CAL] B ignored: select a distance (10..100) first.");
    }
  }
  keyBWasDown = keyBDown;
}
}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(200);
  delay(1200);

  for (uint8_t row = 0; row < 4; ++row) {
    pinMode(Config::Keypad::DIRECT_ROW_PINS[row], OUTPUT);
    digitalWrite(Config::Keypad::DIRECT_ROW_PINS[row], HIGH);
  }
  for (uint8_t column = 0; column < 4; ++column) {
    pinMode(Config::Keypad::DIRECT_COLUMN_PINS[column], INPUT_PULLUP);
  }

  Serial.println();
  Serial.println("========== VL53 B-STEP CALIBRATION ==========");
  Serial.println("Target points: 10,20,30,...,100 cm");
  Serial.println("Use a matte flat target perpendicular to the sensor.");
  Serial.println("You may type B in this monitor OR press physical keypad B.");

  if (!initializeSensor()) {
    Serial.println("[VL53L1X] ERROR: device not found; send the same numeric point after fixing wiring.");
    return;
  }

  Serial.println("[VL53L1X] READY");
  printHelp();
  resetCapture();
}

void loop() {
  handleSerial();
  handlePhysicalKeypad();
  delay(5);
}
