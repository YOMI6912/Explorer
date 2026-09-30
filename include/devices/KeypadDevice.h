#pragma once

#include <Arduino.h>

namespace KeypadDevice {

bool begin();
void update();
bool isReady();
bool getKey(char &key);

}  // namespace KeypadDevice

