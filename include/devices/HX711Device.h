#pragma once

#include <Arduino.h>

namespace HX711Device {

bool begin();
bool isReady();
bool readRaw(long &rawValue);
bool readWeightKg(float &weightKg);
bool tare(uint8_t samples = 10);

}  // namespace HX711Device

