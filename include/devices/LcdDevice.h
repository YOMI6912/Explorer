#pragma once

#include <Arduino.h>

namespace LcdDevice {

bool begin();
bool isReady();
void clear();
void setLine(uint8_t row, const String &text);
void showScreen(const String &line0,
                const String &line1 = "",
                const String &line2 = "",
                const String &line3 = "");
void recover();

void setBacklight(bool enabled);

void suspendForSensor();
void resumeAfterSensor();
void showStartup();
void showHardwareStatus(bool loadCellReady,
                        bool heightSensorReady,
                        bool keypadReady,
                        bool printerHostReady);

}
