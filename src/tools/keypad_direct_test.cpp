#include <Arduino.h>

namespace {
constexpr uint8_t DRIVE_PINS[4] = {6, 7, 10, 11};
constexpr uint8_t SENSE_PINS[4] = {12, 13, 14, 15};
constexpr char KEY_MAP[4][4] = {
    {'1', '4', '7', '*'},
    {'2', '5', '8', '0'},
    {'3', '6', '9', '#'},
    {'A', 'B', 'C', 'D'}};
constexpr uint32_t DEBOUNCE_MS = 5;

char lastReading = 0;
char stableReading = 0;
uint32_t changedAt = 0;

void releaseDrivePins() {
  for (const uint8_t pin : DRIVE_PINS) {
    digitalWrite(pin, HIGH);
  }
}

char scanKey() {
  releaseDrivePins();

  for (uint8_t drive = 0; drive < 4; ++drive) {
    digitalWrite(DRIVE_PINS[drive], LOW);
    delayMicroseconds(50);

    for (uint8_t sense = 0; sense < 4; ++sense) {
      if (digitalRead(SENSE_PINS[sense]) == LOW) {
        releaseDrivePins();
        return KEY_MAP[drive][sense];
      }
    }

    digitalWrite(DRIVE_PINS[drive], HIGH);
  }

  return 0;
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1200);

  for (const uint8_t pin : DRIVE_PINS) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
  for (const uint8_t pin : SENSE_PINS) {
    pinMode(pin, INPUT_PULLUP);
  }

  Serial.println();
  Serial.println("ESP32-S3 direct 4x4 keypad test READY");
  Serial.println("GPIO: 6,7,10,11 / 12,13,14,15");
  Serial.println("Press and release one key at a time.");
}

void loop() {
  const char reading = scanKey();
  const uint32_t now = millis();

  if (reading != lastReading) {
    lastReading = reading;
    changedAt = now;
  }

  if (now - changedAt >= DEBOUNCE_MS && reading != stableReading) {
    stableReading = reading;
    if (stableReading != 0) {
      Serial.printf("[KEY] %c\n", stableReading);
    }
  }

  delay(2);
}
