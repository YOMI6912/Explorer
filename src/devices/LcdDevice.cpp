#include "devices/LcdDevice.h"

#include <Wire.h>

#include "config.h"

namespace LcdDevice {
namespace {
// LCD uses the second ESP32 I2C controller on GPIO1/2. VL53L1X uses Wire0
// on GPIO8/9, so LCD traffic cannot disturb height measurements.
TwoWire lcdWire(1);
bool initialized = false;
bool backlightEnabled = true;
bool suspended = false;
String cachedLines[Config::LCD::ROWS];

constexpr uint8_t LCD_ENABLE = 0x04;
constexpr uint8_t LCD_REGISTER_SELECT = 0x01;
constexpr uint8_t LCD_BACKLIGHT = 0x08;

void expanderWrite(uint8_t value) {
  lcdWire.beginTransmission(Config::LCD::ADDRESS);
  lcdWire.write(value | (backlightEnabled ? LCD_BACKLIGHT : 0));
  lcdWire.endTransmission();
}

void pulseEnable(uint8_t value) {
  expanderWrite(value | LCD_ENABLE);
  delayMicroseconds(1);
  expanderWrite(value & ~LCD_ENABLE);
  delayMicroseconds(50);
}

void write4Bits(uint8_t value) {
  expanderWrite(value);
  pulseEnable(value);
}

void sendByte(uint8_t value, uint8_t mode) {
  write4Bits((value & 0xF0) | mode);
  write4Bits(((value << 4) & 0xF0) | mode);
}

void command(uint8_t value) {
  sendByte(value, 0);
}

void writeCharacter(uint8_t value) {
  sendByte(value, LCD_REGISTER_SELECT);
}

void setCursorRaw(uint8_t column, uint8_t row) {
  static const uint8_t rowOffsets[] = {0x00, 0x40, 0x14, 0x54};
  command(0x80 | (column + rowOffsets[row]));
}

void initializeController() {
  delay(50);
  expanderWrite(0);
  delay(5);
  write4Bits(0x30);
  delayMicroseconds(4500);
  write4Bits(0x30);
  delayMicroseconds(4500);
  write4Bits(0x30);
  delayMicroseconds(150);
  write4Bits(0x20);
  command(0x28);  // 4-bit, 2-line font mode (also used by 20x4 modules)
  command(0x0C);  // display on, cursor and blink off
  command(0x06);  // left-to-right entry mode
  command(0x01);  // clear
  delayMicroseconds(2000);
}

bool addressResponds() {
  lcdWire.beginTransmission(Config::LCD::ADDRESS);
  return lcdWire.endTransmission() == 0;
}

String stateText(bool ready) {
  return ready ? "OK" : "ERR";
}

String normalizedLine(const String &text) {
  String line = text.substring(0, Config::LCD::COLUMNS);
  while (line.length() < Config::LCD::COLUMNS) {
    line += ' ';
  }
  return line;
}

void writeCachedLines() {
  for (uint8_t row = 0; row < Config::LCD::ROWS; ++row) {
    setCursorRaw(0, row);
    for (size_t column = 0; column < cachedLines[row].length(); ++column) {
      writeCharacter(cachedLines[row].charAt(column));
    }
  }
}
}  // namespace

bool begin() {
  lcdWire.begin(Config::Pins::LCD_I2C_SDA,
                Config::Pins::LCD_I2C_SCL,
                Config::I2C::CLOCK_HZ);
  lcdWire.setClock(Config::I2C::CLOCK_HZ);
  lcdWire.setTimeOut(50);
  if (!addressResponds()) {
    Serial.printf("[LCD] ERROR: no device at 0x%02X\n", Config::LCD::ADDRESS);
    return false;
  }

  initializeController();
  for (uint8_t row = 0; row < Config::LCD::ROWS; ++row) {
    cachedLines[row] = normalizedLine("");
  }
  initialized = true;
  Serial.println("[LCD] Ready");
  return true;
}

bool isReady() {
  return initialized;
}

void clear() {
  if (initialized) {
    command(0x01);
    delayMicroseconds(2000);
    for (uint8_t row = 0; row < Config::LCD::ROWS; ++row) {
      cachedLines[row] = normalizedLine("");
    }
  }
}

void setLine(uint8_t row, const String &text) {
  if (!initialized || row >= Config::LCD::ROWS) {
    return;
  }

  const String line = normalizedLine(text);
  cachedLines[row] = line;
  setCursorRaw(0, row);
  for (size_t column = 0; column < line.length(); ++column) {
    writeCharacter(line.charAt(column));
  }
}

void showScreen(const String &line0,
                const String &line1,
                const String &line2,
                const String &line3) {
  if (!initialized) {
    return;
  }

  setLine(0, line0);
  setLine(1, line1);
  setLine(2, line2);
  setLine(3, line3);
}

void recover() {
  if (!initialized) {
    return;
  }

  initializeController();
  writeCachedLines();
  Serial.println("[LCD] Reinitialized and restored cached screen");
}

void setBacklight(bool enabled) {
  if (!initialized) {
    return;
  }
  backlightEnabled = enabled;
  expanderWrite(0);
}

void suspendForSensor() {
  if (!initialized || suspended) {
    return;
  }
  lcdWire.end();
  suspended = true;
  Serial.println("[LCD] I2C paused during VL53 measurement");
}

void resumeAfterSensor() {
  if (!initialized || !suspended) {
    return;
  }
  lcdWire.begin(Config::Pins::LCD_I2C_SDA,
                Config::Pins::LCD_I2C_SCL,
                Config::I2C::CLOCK_HZ);
  lcdWire.setClock(Config::I2C::CLOCK_HZ);
  lcdWire.setTimeOut(50);
  initializeController();
  writeCachedLines();
  suspended = false;
  Serial.println("[LCD] I2C resumed after VL53 measurement");
}

void showStartup() {
  if (!initialized) {
    return;
  }

  setLine(0, "CLINIC SYSTEM");
  setLine(1, "Starting modules...");
  setLine(2, "Please wait");
  setLine(3, "ESP32-S3");
}

void showHardwareStatus(bool loadCellReady,
                        bool heightSensorReady,
                        bool keypadReady,
                        bool printerHostReady) {
  if (!initialized) {
    return;
  }

  setLine(0, "HARDWARE MODULES");
  setLine(1, "LOAD:" + stateText(loadCellReady) +
                 " HGT:" + stateText(heightSensorReady));
  setLine(2, "KEY:" + stateText(keypadReady) + " LCD:OK");
  setLine(3, "USB HOST:" + stateText(printerHostReady));
}

}  // namespace LcdDevice
