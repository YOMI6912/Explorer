#include "devices/KeypadDevice.h"

#include <Wire.h>

#include "config.h"

namespace KeypadDevice {
namespace {
uint8_t keypadAddress = Config::Keypad::ADDRESS;
char lastReading = 0;
char stableReading = 0;
uint32_t changedAt = 0;
char pendingKey = 0;
bool initialized = false;

void releaseDirectRows() {
  for (uint8_t row = 0; row < 4; ++row) {
    digitalWrite(Config::Keypad::DIRECT_ROW_PINS[row], HIGH);
  }
}

bool scanDirectKey(char &key) {
  key = 0;
  releaseDirectRows();

  for (uint8_t row = 0; row < 4; ++row) {
    const uint8_t rowPin = Config::Keypad::DIRECT_ROW_PINS[row];
    digitalWrite(rowPin, LOW);
    delayMicroseconds(50);

    for (uint8_t column = 0; column < 4; ++column) {
      if (digitalRead(Config::Keypad::DIRECT_COLUMN_PINS[column]) == LOW) {
        key = Config::Keypad::DIRECT_KEY_MAP[row][column];
        releaseDirectRows();
        return true;
      }
    }

    digitalWrite(rowPin, HIGH);
  }

  return true;
}

bool addressResponds(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool findKeypadAddress() {
  if (Config::Keypad::ADDRESS != Config::LCD::ADDRESS &&
      addressResponds(Config::Keypad::ADDRESS)) {
    keypadAddress = Config::Keypad::ADDRESS;
    return true;
  }

  // PCF8574 uses 0x20-0x27 and PCF8574A uses 0x38-0x3F. Skip the LCD
  // address so a missing keypad cannot be mistaken for the LCD backpack.
  const uint8_t ranges[][2] = {{0x20, 0x27}, {0x38, 0x3F}};
  for (const auto &range : ranges) {
    for (uint8_t address = range[0]; address <= range[1]; ++address) {
      if (address == Config::LCD::ADDRESS ||
          address == Config::Keypad::ADDRESS) {
        continue;
      }
      if (addressResponds(address)) {
        keypadAddress = address;
        Serial.printf("[KEYPAD] PCF8574 auto-detected at 0x%02X\n",
                      keypadAddress);
        return true;
      }
    }
  }
  return false;
}

bool writePort(uint8_t value) {
  Wire.beginTransmission(keypadAddress);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readPort(uint8_t &value) {
  const uint8_t count =
      Wire.requestFrom(keypadAddress, static_cast<uint8_t>(1));

  if (count < 1 || Wire.available() < 1) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }

  value = Wire.read();
  return true;
}

bool scanKey(char &key) {
  key = 0;

  for (uint8_t row = 0; row < 4; ++row) {
    // PCF8574 pins written HIGH act as inputs. Drive one row LOW at a time
    // and inspect the four column pins for a pressed key.
    const uint8_t output =
        static_cast<uint8_t>(0xFFU & ~(1U << Config::Keypad::ROW_BITS[row]));
    if (!writePort(output)) {
      writePort(0xFF);
      return false;
    }

    delayMicroseconds(50);

    uint8_t input = 0xFF;
    if (!readPort(input)) {
      writePort(0xFF);
      return false;
    }

    for (uint8_t column = 0; column < 4; ++column) {
      if ((input & (1U << Config::Keypad::COLUMN_BITS[column])) == 0) {
        key = Config::Keypad::PCF_KEY_MAP[row][column];
        writePort(0xFF);
        return true;
      }
    }
  }

  // Release every PCF8574 pin after each scan.
  return writePort(0xFF);
}
}  // namespace

bool begin() {
  if (Config::Keypad::DIRECT_GPIO) {
    Serial.println("[KEYPAD] Initializing direct 4x4 matrix keypad...");

    for (uint8_t row = 0; row < 4; ++row) {
      const uint8_t pin = Config::Keypad::DIRECT_ROW_PINS[row];
      pinMode(pin, OUTPUT);
      digitalWrite(pin, HIGH);
    }
    for (uint8_t column = 0; column < 4; ++column) {
      pinMode(Config::Keypad::DIRECT_COLUMN_PINS[column], INPUT_PULLUP);
    }

    delay(Config::Keypad::STARTUP_SETTLE_MS);
    char key = 0;
    scanDirectKey(key);

    lastReading = key;
    stableReading = key;
    changedAt = millis();
    pendingKey = 0;
    initialized = true;

    Serial.println("[KEYPAD] Direct GPIO mode ready");
    Serial.println("[KEYPAD] Rows: GPIO6, GPIO7, GPIO10, GPIO11");
    Serial.println("[KEYPAD] Columns: GPIO12, GPIO13, GPIO14, GPIO15");
    if (key != 0) {
      Serial.println("[KEYPAD] WARNING: release all keys before testing");
    }
    return true;
  }

  Serial.println("[KEYPAD] Initializing PCF8574 matrix keypad...");
  delay(Config::Keypad::STARTUP_SETTLE_MS);

  if (!findKeypadAddress()) {
    Serial.println("[KEYPAD] ERROR: PCF8574 not found");
    return false;
  }

  if (!writePort(0xFF)) {
    Serial.println("[KEYPAD] ERROR: PCF8574 did not accept output state");
    return false;
  }

  char key = 0;
  if (!scanKey(key)) {
    Serial.println("[KEYPAD] ERROR: could not read PCF8574");
    return false;
  }

  lastReading = key;
  stableReading = key;
  changedAt = millis();
  pendingKey = 0;
  initialized = true;

  Serial.printf("[KEYPAD] Ready at I2C address 0x%02X\n", keypadAddress);
  if (key != 0) {
    Serial.println("[KEYPAD] WARNING: release all keys before testing");
  }
  return true;
}

void update() {
  if (!initialized) {
    return;
  }

  char currentReading = 0;
  const bool scanOk = Config::Keypad::DIRECT_GPIO
                          ? scanDirectKey(currentReading)
                          : scanKey(currentReading);
  if (!scanOk) {
    return;
  }

  if (currentReading != lastReading) {
    lastReading = currentReading;
    changedAt = millis();
  }

  if (millis() - changedAt < Config::Keypad::DEBOUNCE_MS ||
      currentReading == stableReading) {
    return;
  }

  stableReading = currentReading;
  if (stableReading != 0) {
    pendingKey = stableReading;
  }
}

bool isReady() {
  return initialized;
}

bool getKey(char &key) {
  if (pendingKey == 0) {
    return false;
  }

  key = pendingKey;
  pendingKey = 0;
  return true;
}

}  // namespace KeypadDevice
