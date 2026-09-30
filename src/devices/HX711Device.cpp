#include "devices/HX711Device.h"

#include <HX711.h>
#include <math.h>

#include "config.h"

namespace HX711Device {
namespace {
HX711 scale;
bool initialized = false;
}  // namespace

bool begin() {
  Serial.println("[HX711] Initializing...");
  scale.begin(Config::Pins::HX711_DT, Config::Pins::HX711_SCK);

  if (!scale.wait_ready_timeout(Config::LoadCell::READY_TIMEOUT_MS)) {
    Serial.println("[HX711] ERROR: device not found");
    return false;
  }

  scale.set_scale(Config::LoadCell::CALIBRATION_FACTOR);
  if (Config::LoadCell::TARE_ON_START) {
    scale.tare(Config::LoadCell::WEIGHT_AVERAGE_SAMPLES);
    Serial.printf("[HX711] Startup tare offset=%ld (platform must be empty)\n",
                  scale.get_offset());
  }

  initialized = true;
  Serial.println("[HX711] Ready");
  return true;
}

bool isReady() {
  return initialized && scale.is_ready();
}

bool readRaw(long &rawValue) {
  if (!initialized ||
      !scale.wait_ready_timeout(Config::LoadCell::READ_TIMEOUT_MS)) {
    return false;
  }

  rawValue = scale.read_average(Config::LoadCell::RAW_AVERAGE_SAMPLES);
  return true;
}

bool readWeightKg(float &weightKg) {
  if (!initialized) {
    return false;
  }

  // A single ten-sample average can look plausible while a person is
  // stepping off the platform. Keep individual samples so motion is visible.
  float samples[Config::LoadCell::STABLE_WEIGHT_SAMPLES];
  float sorted[Config::LoadCell::STABLE_WEIGHT_SAMPLES];
  for (uint8_t i = 0; i < Config::LoadCell::STABLE_WEIGHT_SAMPLES; ++i) {
    if (!scale.wait_ready_timeout(Config::LoadCell::READ_TIMEOUT_MS)) {
      Serial.println("[HX711] Weight sample timed out");
      return false;
    }
    samples[i] = scale.get_units(1);
    if (!isfinite(samples[i])) {
      return false;
    }
    sorted[i] = samples[i];
  }

  for (uint8_t i = 1; i < Config::LoadCell::STABLE_WEIGHT_SAMPLES; ++i) {
    const float value = sorted[i];
    uint8_t j = i;
    while (j > 0 && sorted[j - 1] > value) {
      sorted[j] = sorted[j - 1];
      --j;
    }
    sorted[j] = value;
  }

  const float median = sorted[Config::LoadCell::STABLE_WEIGHT_SAMPLES / 2];
  float sum = 0.0f;
  uint8_t stableCount = 0;
  for (float sample : samples) {
    if (fabsf(sample - median) <= Config::LoadCell::MAX_SAMPLE_DEVIATION_KG) {
      sum += sample;
      ++stableCount;
    }
  }
  const float startMean = (samples[0] + samples[1] + samples[2]) / 3.0f;
  const uint8_t last = Config::LoadCell::STABLE_WEIGHT_SAMPLES - 1;
  const float endMean =
      (samples[last - 2] + samples[last - 1] + samples[last]) / 3.0f;
  if (stableCount < Config::LoadCell::MIN_STABLE_WEIGHT_SAMPLES ||
      fabsf(endMean - startMean) > Config::LoadCell::MAX_SAMPLE_DEVIATION_KG) {
    Serial.printf("[HX711] Unstable weight: median=%.1f kg, inliers=%u/%u, drift=%.1f kg\n",
                  median, stableCount, Config::LoadCell::STABLE_WEIGHT_SAMPLES,
                  endMean - startMean);
    return false;
  }

  weightKg = sum / stableCount;
  return isfinite(weightKg) &&
         weightKg >= Config::LoadCell::MIN_WEIGHT_KG &&
         weightKg <= Config::LoadCell::MAX_WEIGHT_KG;
}

bool tare(uint8_t samples) {
  if (!initialized ||
      !scale.wait_ready_timeout(Config::LoadCell::READ_TIMEOUT_MS)) {
    return false;
  }

  scale.tare(samples);
  return true;
}

}  // namespace HX711Device
