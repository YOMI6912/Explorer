#pragma once

#include <Arduino.h>

namespace OnlineQueueWorkflow {

// เริ่ม Wi-Fi, MQTT และ state machine ของคิว A
void begin();
void setMeasurementHardwareReady(bool loadCellReady, bool heightSensorReady);
void refreshDisplay();

// เรียกซ้ำใน loop() เพื่อดูแล MQTT, reconnect และขั้นตอนอัตโนมัติ
void update();

// รับปุ่มทีละตัวจาก Keypad จริง หรือใช้จำลองปุ่มผ่าน Serial
void handleKey(char key);

// รับคำสั่งหนึ่งบรรทัดจาก Serial Monitor เช่น A 123456 หรือ T 123456 60 170
void handleSerialLine(const String &line);

bool isActive();
bool isNetworkReady();

// ใช้โดย main.cpp เพื่อส่งค่าวัดจาก HX711/VL53L1X จริงหลัง OTP ผ่าน
bool isReadyForMeasurement();
bool submitMeasurement(float weightKg, float heightCm);

// คิว B Walk-in: ส่งค่าวัดได้ทันทีโดยไม่ใช้ OTP/session
bool submitWalkInMeasurement(float weightKg, float heightCm);

}  // namespace OnlineQueueWorkflow
