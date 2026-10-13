#pragma once

#include <stdint.h>

namespace RemoteHeightLink {
bool begin();
bool isReady();
bool isConnected();
uint8_t stationCount();
void update();
// False means no trustworthy height; caller must not print or enqueue.
bool requestHeightCm(float &heightCm);
}  // namespace RemoteHeightLink
