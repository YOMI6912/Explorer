#include <Arduino.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>

namespace {
constexpr uint8_t SDA_PIN = 8;
constexpr uint8_t SCL_PIN = 9;
constexpr uint8_t LCD_ADDRESS = 0x27;
hd44780_I2Cexp lcd;

bool addressResponds(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void scanBus() {
  Serial.println("[I2C] Scanning...");
  uint8_t count = 0;
  for (uint8_t address = 1; address < 127; ++address) {
    if (addressResponds(address)) {
      Serial.printf("[I2C] Found 0x%02X\n", address);
      ++count;
    }
  }
  Serial.printf("[I2C] Found %u device(s)\n", count);
}

void drawTestScreen() {
  if (!addressResponds(LCD_ADDRESS)) {
    Serial.println("[LCD] ERROR: address 0x27 did not respond");
    return;
  }

  const int status = lcd.begin(20, 4);
  if (status != 0) {
    Serial.printf("[LCD] ERROR: hd44780 auto-config failed, status=%d\n", status);
    return;
  }
  lcd.backlight();
  delay(100);
  lcd.clear();
  delay(20);
  lcd.setCursor(0, 0);
  lcd.print("LCD STATIC TEST");
  delay(20);
  lcd.setCursor(0, 1);
  lcd.print("ADDRESS 0x27");
  delay(20);
  lcd.setCursor(0, 2);
  lcd.print("12345678901234567890");
  delay(20);
  lcd.setCursor(0, 3);
  lcd.print("ROW 4: DISPLAY OK");
  Serial.println("[LCD] Static screen written once");
  Serial.println("[LCD] hd44780_I2Cexp auto pin mapping active");
  Serial.println("[LCD] Send R to initialize and draw again");
}
}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(100);
  delay(1200);

  Serial.println();
  Serial.println("========== LCD STATIC TEST ==========");
  Wire.begin(SDA_PIN, SCL_PIN, 100000);
  Wire.setTimeOut(50);
  scanBus();
  drawTestScreen();
}

void loop() {
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    if (command.equalsIgnoreCase("R") ||
        command.equalsIgnoreCase("RESET")) {
      drawTestScreen();
    } else if (command.equalsIgnoreCase("S") ||
               command.equalsIgnoreCase("SCAN")) {
      scanBus();
    }
  }
  delay(10);
}
