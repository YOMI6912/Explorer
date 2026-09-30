#include <Arduino.h>
#include <HX711.h>

#include "config.h"

namespace {
constexpr uint8_t HX711_DT_PIN = 4;
constexpr uint8_t HX711_SCK_PIN = 5;
constexpr uint8_t DISPLAY_SAMPLES = 10;
constexpr uint8_t CALIBRATION_SAMPLES = 50;
constexpr uint32_t DISPLAY_INTERVAL_MS = 1000;

HX711 scale;
long zeroRaw = 0;
float calibrationFactor = 0.0f;
bool zeroCaptured = false;
bool calibrated = false;
uint32_t lastDisplayAt = 0;

void printHelp() {
  Serial.println();
  Serial.println("HX711 calibration commands:");
  Serial.println("  Z        capture zero with EMPTY platform");
  Serial.println("  W <kg>   capture a known weight, example: W 20");
  Serial.println("  ?        show this help");
  Serial.println();
}

void captureZero() {
  Serial.println("[CAL] Keep the platform empty and still. Sampling zero...");
  zeroRaw = scale.read_average(CALIBRATION_SAMPLES);
  zeroCaptured = true;
  calibrated = false;
  Serial.printf("[CAL] ZERO_RAW=%ld\n", zeroRaw);
  Serial.println("[CAL] Put the known weight at the center, wait until stable,");
  Serial.println("[CAL] then send W <kg>, for example: W 20");
}

void captureKnownWeight(float knownKg) {
  if (!zeroCaptured) {
    Serial.println("[CAL] ERROR: send Z with an empty platform first");
    return;
  }
  if (!isfinite(knownKg) || knownKg <= 0.0f) {
    Serial.println("[CAL] ERROR: known weight must be greater than zero");
    return;
  }

  Serial.printf("[CAL] Sampling %.3f kg at the center...\n", knownKg);
  const long loadedRaw = scale.read_average(CALIBRATION_SAMPLES);
  const long delta = loadedRaw - zeroRaw;
  calibrationFactor = static_cast<float>(delta) / knownKg;

  Serial.printf("[CAL] LOADED_RAW=%ld\n", loadedRaw);
  Serial.printf("[CAL] DELTA=%ld\n", delta);
  Serial.printf("[CAL] CALIBRATION_FACTOR=%.6f\n", calibrationFactor);

  if (delta == 0 || fabsf(calibrationFactor) < 1.0f) {
    Serial.println("[CAL] ERROR: raw value did not change enough; check mechanics/wiring");
    calibrated = false;
    return;
  }

  scale.set_offset(zeroRaw);
  scale.set_scale(calibrationFactor);
  calibrated = true;
  Serial.println("[CAL] Calibration active for verification.");
  Serial.println("[CAL] Remove/add weights and compare the live KG value.");
}

void handleCommand(String command) {
  command.trim();
  if (command.length() == 0) {
    return;
  }

  if (command.equalsIgnoreCase("Z")) {
    captureZero();
    return;
  }

  if (command.startsWith("W ") || command.startsWith("w ")) {
    captureKnownWeight(command.substring(2).toFloat());
    return;
  }

  if (command == "?") {
    printHelp();
    return;
  }

  Serial.println("[CAL] Unknown command. Send ? for help.");
}
}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(200);
  delay(1200);

  Serial.println();
  Serial.println("ESP32-S3 HX711 calibration tool");
  Serial.printf("DT=GPIO%u, SCK=GPIO%u\n", HX711_DT_PIN, HX711_SCK_PIN);
  scale.begin(HX711_DT_PIN, HX711_SCK_PIN);

  if (!scale.wait_ready_timeout(3000)) {
    Serial.println("[HX711] ERROR: device not ready; check power, DT and SCK");
    return;
  }

  Serial.println("[HX711] READY");
  scale.set_scale(Config::LoadCell::CALIBRATION_FACTOR);
  Serial.println("[VERIFY] Keep the platform empty. Capturing startup tare...");
  scale.tare(CALIBRATION_SAMPLES);
  zeroRaw = scale.get_offset();
  calibrationFactor = Config::LoadCell::CALIBRATION_FACTOR;
  zeroCaptured = true;
  calibrated = true;
  Serial.printf("[VERIFY] ZERO_RAW=%ld\n", zeroRaw);
  Serial.printf("[VERIFY] CALIBRATION_FACTOR=%.6f\n", calibrationFactor);
  Serial.println("[VERIFY] Live RAW and KG values are now active.");
  printHelp();
  Serial.println("[NEXT] Put the 1.5 kg bottle at the center to verify.");
}

void loop() {
  if (Serial.available() > 0) {
    handleCommand(Serial.readStringUntil('\n'));
  }

  if (!scale.is_ready()) {
    delay(5);
    return;
  }

  const uint32_t now = millis();
  if (now - lastDisplayAt >= DISPLAY_INTERVAL_MS) {
    lastDisplayAt = now;
    if (calibrated) {
      Serial.printf("[LIVE] RAW=%ld  KG=%.3f\n",
                    scale.read_average(DISPLAY_SAMPLES),
                    scale.get_units(DISPLAY_SAMPLES));
    } else {
      Serial.printf("[LIVE] RAW=%ld\n",
                    scale.read_average(DISPLAY_SAMPLES));
    }
  }

  delay(5);
}
