#include <Arduino.h>
#include <HX711.h>
#include <VL53L1X.h>
#include <Wire.h>
#include <math.h>

namespace {
constexpr uint8_t I2C_SDA_PIN = 1;
constexpr uint8_t I2C_SCL_PIN = 2;
constexpr uint8_t HX711_DT_PIN = 4;
constexpr uint8_t HX711_SCK_PIN = 5;

constexpr float LOAD_CELL_FACTOR = 8848.67f;
constexpr float SENSOR_MOUNT_HEIGHT_CM = 197.3f;
constexpr float HEIGHT_OFFSET_CM = 0.6f;
constexpr float DEFAULT_REFERENCE_HEIGHT_CM = 171.0f;

constexpr float PERSON_ENTER_WEIGHT_KG = 20.0f;
constexpr float PERSON_EXIT_WEIGHT_KG = 15.0f;
constexpr uint8_t PRESENCE_CONFIRM_READS = 3;
constexpr uint32_t SETTLE_TIME_MS = 2000;
constexpr uint32_t SAMPLE_INTERVAL_MS = 150;
constexpr size_t MAX_DISTANCE_SAMPLES = 120;
constexpr float HEIGHT_PASS_TOLERANCE_CM = 3.0f;
constexpr uint16_t MAX_DISTANCE_IQR_MM = 20;

HX711 scale;
VL53L1X sensor;
TwoWire heightWire(1);

bool sensorReady = false;
bool trialActive = false;
uint8_t enterCount = 0;
uint8_t exitCount = 0;
uint32_t trialStartedAt = 0;
uint32_t lastSampleAt = 0;
uint16_t distanceSamples[MAX_DISTANCE_SAMPLES];
size_t distanceSampleCount = 0;
uint32_t invalidDistanceCount = 0;
float referenceHeightCm = DEFAULT_REFERENCE_HEIGHT_CM;
float lastWeightKg = 0.0f;

bool readValidDistance(uint16_t &distanceMm) {
  if (!sensorReady) {
    return false;
  }

  distanceMm = sensor.read();
  return !sensor.timeoutOccurred() &&
         sensor.ranging_data.range_status == VL53L1X::RangeValid &&
         distanceMm > 0 && distanceMm < 4000;
}

void sortSamples() {
  for (size_t i = 1; i < distanceSampleCount; ++i) {
    const uint16_t value = distanceSamples[i];
    size_t j = i;
    while (j > 0 && distanceSamples[j - 1] > value) {
      distanceSamples[j] = distanceSamples[j - 1];
      --j;
    }
    distanceSamples[j] = value;
  }
}

void printTrialSummary() {
  Serial.println();
  Serial.println("============================================================");
  Serial.println("[POSITION RESULT] You may now look at this CMD window");

  if (distanceSampleCount < 5) {
    Serial.printf("[RESULT] FAIL: only %u valid samples (%lu invalid)\n",
                  static_cast<unsigned>(distanceSampleCount),
                  static_cast<unsigned long>(invalidDistanceCount));
    Serial.println("[FIX] Stand still for at least 5 seconds, arms at sides, then step off.");
    Serial.println("============================================================");
    return;
  }

  sortSamples();
  const uint16_t medianMm = distanceSamples[distanceSampleCount / 2];
  const uint16_t q1Mm = distanceSamples[distanceSampleCount / 4];
  const uint16_t q3Mm = distanceSamples[(distanceSampleCount * 3) / 4];
  const uint16_t iqrMm = q3Mm - q1Mm;
  const float measuredHeightCm =
      SENSOR_MOUNT_HEIGHT_CM - (static_cast<float>(medianMm) / 10.0f) +
      HEIGHT_OFFSET_CM;
  const float errorCm = measuredHeightCm - referenceHeightCm;

  Serial.printf("[RESULT] samples=%u, invalid=%lu\n",
                static_cast<unsigned>(distanceSampleCount),
                static_cast<unsigned long>(invalidDistanceCount));
  Serial.printf("[RESULT] distance median=%u mm, IQR=%u mm\n",
                medianMm,
                iqrMm);
  Serial.printf("[RESULT] calculated height=%.1f cm, reference=%.1f cm, error=%+.1f cm\n",
                measuredHeightCm,
                referenceHeightCm,
                errorCm);

  if (iqrMm > MAX_DISTANCE_IQR_MM) {
    Serial.println("[VERDICT] NOT STABLE");
    Serial.println("[FIX] Keep the head still and keep both arms down.");
  } else if (fabsf(errorCm) <= HEIGHT_PASS_TOLERANCE_CM) {
    Serial.println("[VERDICT] PASS - head position is correct");
    Serial.println("[NEXT] This is the position to use before pressing B in normal mode.");
  } else if (errorCm > 0.0f) {
    Serial.println("[VERDICT] TOO CLOSE / OBJECT ABOVE HEAD");
    Serial.println("[FIX] Remove hands or objects above the head and retry with arms down.");
  } else {
    Serial.println("[VERDICT] HEAD NOT CENTERED / SENSOR HIT SHOULDER OR BODY");
    Serial.println("[FIX] Move slightly left/right/front/back and retry.");
  }
  Serial.println("============================================================");
  Serial.println("[READY] Platform must be empty before the next attempt.");
}

void startTrial() {
  trialActive = true;
  exitCount = 0;
  trialStartedAt = millis();
  distanceSampleCount = 0;
  invalidDistanceCount = 0;
  Serial.println();
  Serial.println("[POSITION] Person detected. Arms down; stand still for 5 seconds.");
  Serial.println("[POSITION] Then step off the platform and read the result.");
}

void finishTrial() {
  trialActive = false;
  enterCount = 0;
  exitCount = 0;
  printTrialSummary();
}

void handleSerialCommand() {
  if (!Serial.available()) {
    return;
  }

  String command = Serial.readStringUntil('\n');
  command.trim();
  if (command.equalsIgnoreCase("R") ||
      command.equalsIgnoreCase("RESULT")) {
    if (trialActive) {
      finishTrial();
    } else {
      Serial.println("[RESULT] No active trial");
    }
    return;
  }
  if (command.equalsIgnoreCase("S") ||
      command.equalsIgnoreCase("STATUS")) {
    Serial.printf("[STATUS] weight=%.1f kg, active=%s, samples=%u\n",
                  lastWeightKg,
                  trialActive ? "yes" : "no",
                  static_cast<unsigned>(distanceSampleCount));
    return;
  }
  if (command.length() > 1 &&
      (command[0] == 'H' || command[0] == 'h')) {
    const float requestedHeight = command.substring(1).toFloat();
    if (requestedHeight >= 100.0f && requestedHeight <= 220.0f) {
      referenceHeightCm = requestedHeight;
      Serial.printf("[CONFIG] Reference height set to %.1f cm\n",
                    referenceHeightCm);
    } else {
      Serial.println("[CONFIG] Invalid height. Example: H171");
    }
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(100);
  delay(1200);

  Serial.println();
  Serial.println("============================================================");
  Serial.println(" SOLO POSITION GUIDE - HX711 + VL53L1X");
  Serial.println("============================================================");

  heightWire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 100000);
  heightWire.setTimeOut(50);
  sensor.setBus(&heightWire);

  sensor.setTimeout(500);
  if (sensor.init()) {
    sensor.setDistanceMode(VL53L1X::Long);
    sensor.setROISize(8, 8);
    sensor.setMeasurementTimingBudget(50000);
    sensor.startContinuous(100);
    sensorReady = true;
    Serial.println("[VL53L1X] Ready");
  } else {
    Serial.println("[VL53L1X] ERROR: sensor not found");
  }

  scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
  if (!scale.wait_ready_timeout(3000)) {
    Serial.println("[HX711] ERROR: device not found");
    return;
  }
  scale.set_scale(LOAD_CELL_FACTOR);
  Serial.println("[HX711] Keep the platform EMPTY; taring now...");
  scale.tare(15);
  Serial.println("[HX711] Ready");
  Serial.printf("[CONFIG] Reference height: %.1f cm (change with H171)\n",
                referenceHeightCm);
  Serial.println("[READY] Step onto the platform with arms down. Do not press B.");
}

void loop() {
  handleSerialCommand();

  const uint32_t now = millis();
  if (now - lastSampleAt < SAMPLE_INTERVAL_MS || !scale.is_ready()) {
    delay(5);
    return;
  }
  lastSampleAt = now;

  const float weightKg = scale.get_units(3);
  if (!isfinite(weightKg)) {
    return;
  }
  lastWeightKg = weightKg;

  uint16_t distanceMm = 0;
  const bool distanceValid = readValidDistance(distanceMm);

  if (!trialActive) {
    if (weightKg >= PERSON_ENTER_WEIGHT_KG) {
      if (++enterCount >= PRESENCE_CONFIRM_READS) {
        startTrial();
      }
    } else {
      enterCount = 0;
    }
    return;
  }

  if (weightKg <= PERSON_EXIT_WEIGHT_KG) {
    if (++exitCount >= PRESENCE_CONFIRM_READS) {
      finishTrial();
    }
    return;
  }
  exitCount = 0;

  if (now - trialStartedAt < SETTLE_TIME_MS) {
    return;
  }

  if (distanceValid) {
    if (distanceSampleCount < MAX_DISTANCE_SAMPLES) {
      distanceSamples[distanceSampleCount++] = distanceMm;
    }
  } else {
    ++invalidDistanceCount;
  }
}
