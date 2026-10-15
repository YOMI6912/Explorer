#pragma once

#include <stdint.h>

namespace RemoteHeightLink {
// ขอส่วนสูงจาก C3 ผ่าน TCP
bool begin();
bool isReady();
bool isConnected();
uint8_t stationCount();
bool clinicStationStarted();
void enableClinicStation();
void update();

bool requestHeightCm(float &heightCm);
}
