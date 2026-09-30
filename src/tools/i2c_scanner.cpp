#include <Arduino.h>
#include <Wire.h>

#include "config.h"

namespace {
// Dedicated height-sensor bus (Wire1 in the production firmware).
constexpr uint32_t I2C_CLOCK_HZ = 50000;
constexpr uint16_t I2C_TIMEOUT_MS = 150;
constexpr uint32_t SCAN_INTERVAL_MS = 3000;

uint32_t lastScanAt = 0;
TwoWire &scannerWire = Wire;
bool scannerBusStarted = false;

bool busLinesReleased(uint8_t sda, uint8_t scl) {
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  delay(5);

  const int sdaLevel = digitalRead(sda);
  const int sclLevel = digitalRead(scl);
  Serial.printf("[BUS] SDA=%s, SCL=%s\n",
                sdaLevel == HIGH ? "HIGH" : "LOW",
                sclLevel == HIGH ? "HIGH" : "LOW");

  if (sdaLevel == LOW || sclLevel == LOW) {
    Serial.println("[RESULT] I2C bus is stuck LOW; address scan skipped");
    if (sdaLevel == LOW) {
      Serial.println("Check SDA wiring, shorts, power and connector order.");
    }
    if (sclLevel == LOW) {
      Serial.println("Check SCL wiring, shorts, power and connector order.");
    }
    return false;
  }
  return true;
}

bool startI2cBus(uint8_t sda, uint8_t scl) {
  if (scannerBusStarted) {
    scannerWire.end();
    scannerBusStarted = false;
    delay(10);
  }

  if (!busLinesReleased(sda, scl)) {
    return false;
  }

  if (!scannerWire.begin(sda, scl, I2C_CLOCK_HZ)) {
    Serial.println("[BUS] Wire.begin failed");
    return false;
  }
  scannerBusStarted = true;
  scannerWire.setClock(I2C_CLOCK_HZ);
  scannerWire.setTimeOut(I2C_TIMEOUT_MS);
  return true;
}

bool scanAddress(uint8_t sda, uint8_t scl) {
  Serial.printf("SDA=GPIO%u, SCL=GPIO%u: ", sda, scl);
  if (!startI2cBus(sda, scl)) {
    Serial.println("BUS NOT READY");
    return false;
  }
  scannerWire.beginTransmission(0x29);
  const uint8_t result = scannerWire.endTransmission();
  if (result == 0) {
    Serial.println("FOUND 0x29");
    return true;
  }
  Serial.printf("no 0x29 (error=%u)\n", result);
  return false;
}

void scanI2cBus() {
  Serial.println();
  Serial.println("========== VL53 pin mapping test ==========");
  const bool found = scanAddress(1, 2) || scanAddress(2, 1) ||
                     scanAddress(8, 9) || scanAddress(9, 8);
  Serial.println(found ? "[RESULT] VL53 address found" :
                         "[RESULT] VL53 address not found on any mapping");
  Serial.println("===========================================");
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1200);

  // Keep XSHUT asserted HIGH.  Without this, GPIO3 is floating in the
  // standalone scanner and the VL53 can randomly disappear between scans.
  pinMode(Config::Pins::HEIGHT_XSHUT, OUTPUT);
  digitalWrite(Config::Pins::HEIGHT_XSHUT, LOW);
  delay(50);
  digitalWrite(Config::Pins::HEIGHT_XSHUT, HIGH);
  delay(350);
  Serial.printf("[VL53L1X] XSHUT held HIGH on GPIO%u\n",
                Config::Pins::HEIGHT_XSHUT);

  Serial.println("ESP32-S3 VL53 pin mapping scanner");
  scanI2cBus();
  lastScanAt = millis();
}

void loop() {
  if (millis() - lastScanAt >= SCAN_INTERVAL_MS) {
    scanI2cBus();
    lastScanAt = millis();
  }
  delay(10);
}
