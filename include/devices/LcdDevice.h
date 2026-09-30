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
// Temporarily disable the LCD backlight while the VL53 performs a ranging
// burst.  This reduces supply noise on small 3.3 V regulators.
void setBacklight(bool enabled);
// Pause the LCD I2C controller while the height sensor performs its burst.
// The LCD remains powered; its cached screen is restored on resume.
void suspendForSensor();
void resumeAfterSensor();
void showStartup();
void showHardwareStatus(bool loadCellReady,
                        bool heightSensorReady,
                        bool keypadReady,
                        bool printerHostReady);

}  // namespace LcdDevice
