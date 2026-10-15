#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include "config.h"
#include "devices/HX711Device.h"
#include "devices/KeypadDevice.h"
#include "devices/LcdDevice.h"
#include "devices/RemoteHeightLink.h"
#include "devices/UsbPrinter.h"
#include "devices/VL53L1XDevice.h"
#include "workflows/OnlineQueueWorkflow.h"

namespace {
bool lcdReady = false;
bool keypadReady = false;
bool loadCellReady = false;
bool heightSensorReady = false;
bool printerHostReady = false;

uint32_t lastHeightSensorRetryAt = 0;
constexpr uint32_t HEIGHT_SENSOR_RETRY_MS = 5000;
constexpr uint8_t MEASUREMENT_SENSOR_START_ATTEMPTS = 4;
bool lastPrinterReady = false;
uint32_t lastMeasurementFinishedAt = 0;
bool linkScreenShown = false;
bool lastApShown = false;
bool lastTcpShown = false;
bool lastC3WifiShown = false;
uint32_t lastLinkDisplayAt = 0;
constexpr uint32_t LINK_DISPLAY_REFRESH_MS = 2000;

void updateLinkDisplay() {
  if (!Config::Features::REMOTE_HEIGHT_ENABLED || !lcdReady ||
      millis() - lastMeasurementFinishedAt < 4000) return;
  const bool apReady = RemoteHeightLink::isReady();
  const bool c3WifiReady = RemoteHeightLink::stationCount() > 0;
  const bool tcpReady = RemoteHeightLink::isConnected();
  const uint32_t now = millis();
  if (linkScreenShown && apReady == lastApShown &&
      c3WifiReady == lastC3WifiShown && tcpReady == lastTcpShown &&
      now - lastLinkDisplayAt < LINK_DISPLAY_REFRESH_MS) return;
  lastApShown = apReady;
  lastC3WifiShown = c3WifiReady;
  lastTcpShown = tcpReady;
  lastLinkDisplayAt = now;
  linkScreenShown = true;
  LcdDevice::showScreen("HEIGHT LINK STATUS",
                        apReady ? "S3 AP: READY" : "S3 AP: RESTARTING",
                        c3WifiReady ? "C3 WIFI: CONNECTED" : "C3 WIFI: WAITING",
                        tcpReady ? "TCP: CONNECTED B=GO"
                                 : (c3WifiReady ? "TCP: CONNECTING"
                                                : "CHECK C3 POWER"));
}

void beginI2cBus() {
  Serial.printf("[I2C] VL53 Wire0 SDA=GPIO%u SCL=GPIO%u XSHUT=GPIO%u addr=0x29; LCD Wire1 SDA=GPIO%u SCL=GPIO%u addr=0x%02X\n",
                Config::Pins::I2C_SDA,
                Config::Pins::I2C_SCL,
                Config::Pins::HEIGHT_XSHUT,
                Config::Pins::LCD_I2C_SDA,
                Config::Pins::LCD_I2C_SCL,
                Config::LCD::ADDRESS);
}

bool isI2cRequired() {
  return Config::Features::ENABLE_LCD ||
         (Config::Features::ENABLE_KEYPAD &&
          !Config::Keypad::DIRECT_GPIO);
}

void beginDevices() {

  if (Config::Features::ENABLE_HEIGHT_SENSOR) {
    heightSensorReady = Config::Features::REMOTE_HEIGHT_ENABLED
                            ? RemoteHeightLink::begin()
                            : VL53L1XDevice::begin();
  }

  if (Config::Features::ENABLE_LCD) {
    lcdReady = LcdDevice::begin();
    if (lcdReady) {
      LcdDevice::showStartup();
    }
  }

  if (Config::Features::ENABLE_KEYPAD) {
    keypadReady = KeypadDevice::begin();
  }

  if (Config::Features::ENABLE_LOAD_CELL) {
    loadCellReady = HX711Device::begin();
  }

  if (Config::Features::ENABLE_USB_PRINTER) {
    printerHostReady = UsbPrinter::begin();
  }

  if (lcdReady) {
    LcdDevice::showHardwareStatus(loadCellReady,
                                  heightSensorReady,
                                  keypadReady,
                                  printerHostReady);
  }
}

void retryHeightSensorIfNeeded() {
  if (!Config::Features::ENABLE_HEIGHT_SENSOR ||
      Config::Features::REMOTE_HEIGHT_ENABLED || heightSensorReady) {
    return;
  }

  const uint32_t now = millis();
  if (now - lastHeightSensorRetryAt < HEIGHT_SENSOR_RETRY_MS) {
    return;
  }
  lastHeightSensorRetryAt = now;
  Serial.println("[VL53L1X] Retrying sensor initialization...");
  heightSensorReady = VL53L1XDevice::begin();
  if (heightSensorReady) {
    Serial.println("[VL53L1X] Recovery successful");
    if (lcdReady) {
      OnlineQueueWorkflow::refreshDisplay();
    }
  }
}

bool startHeightSensorForMeasurement() {

  if (heightSensorReady && VL53L1XDevice::activeSessionHealthy()) {
    Serial.println("[VL53L1X] Reusing healthy continuous ranging session");
    return true;
  }

  if (heightSensorReady) {
    Serial.println("[VL53L1X] Existing session unhealthy; starting XSHUT recovery");
  }
  for (uint8_t attempt = 1;
       attempt <= MEASUREMENT_SENSOR_START_ATTEMPTS;
       ++attempt) {
    Serial.printf("[VL53L1X] XSHUT recovery start %u/%u\n",
                  attempt, MEASUREMENT_SENSOR_START_ATTEMPTS);
    if (VL53L1XDevice::begin()) {
      return true;
    }
    if (attempt < MEASUREMENT_SENSOR_START_ATTEMPTS) {
      delay(250);
    }
  }
  Serial.println("[VL53L1X] Could not start after all XSHUT attempts");
  return false;
}


void submitHardwareMeasurement(bool walkIn) {
  if (Config::Features::ENABLE_HEIGHT_SENSOR) {
    Serial.println(Config::Features::REMOTE_HEIGHT_ENABLED
                       ? "[REMOTE] Preparing calibrated C3 height request..."
                       : "[VL53L1X] Preparing height sensor (reuse session; reset only if unhealthy)...");
    if (lcdReady) {
      LcdDevice::showScreen("PREPARING SENSOR", "PLEASE WAIT",
                            "DO NOT PRESS AGAIN");
    }
  }

  if (!loadCellReady) {
    Serial.println("[วัดจริง] HX711 หรือ VL53L1X ยังไม่พร้อม จึงยังส่งค่าวัดไม่ได้");
    if (lcdReady) {
      LcdDevice::showScreen("SENSOR NOT READY",
                            !loadCellReady ? "CHECK LOAD CELL"
                                           : "CHECK HEIGHT SENSOR",
                            "ASK STAFF FOR HELP");
    }
    return;
  }

  Serial.println("[วัดจริง] เริ่มวัดทันที: ปล่อยปุ่ม วางแขนข้างลำตัว และยืนนิ่ง");
  if (lcdReady) {
    LcdDevice::showScreen(walkIn ? "WALK-IN QUEUE B" : "ONLINE QUEUE A",
                          "HANDS DOWN - STILL", "MEASURING NOW",
                          "PLEASE DO NOT MOVE");
  }

  if (Config::Features::ENABLE_HEIGHT_SENSOR &&
      !Config::Features::REMOTE_HEIGHT_ENABLED) {
    heightSensorReady = startHeightSensorForMeasurement();
  }
  if (Config::Features::ENABLE_HEIGHT_SENSOR && !heightSensorReady &&
      !Config::Features::REMOTE_HEIGHT_ENABLED) {
    Serial.println("[วัดจริง] VL53L1X ยังไม่พร้อมหลังรีเซต XSHUT; ไม่ส่งคิว");
    if (lcdReady) {
      LcdDevice::showScreen("SENSOR NOT READY", "CHECK HEIGHT SENSOR",
                            "PLEASE TRY AGAIN");
    }
    return;
  }

  if (lcdReady) {
    LcdDevice::showScreen("MEASURING...", "PLEASE STAND STILL",
                          "KEEP BOTH FEET ON", "PLEASE WAIT");
  }

  // ชั่งก่อนวัดส่วนสูงเพื่อยืนยันว่าเครื่องนิ่ง
  float weightBeforeKg = 0.0f;
  if (!HX711Device::readWeightKg(weightBeforeKg)) {
    Serial.println("[HX711] Initial weight is absent or unstable; queue not sent");
    if (lcdReady) {
      LcdDevice::showScreen("MEASURE FAILED", "WEIGHT UNSTABLE",
                            "STAND IN CENTER", "PLEASE TRY AGAIN");
    }
    return;
  }
  Serial.printf("[HX711] Weight before height scan=%.1f kg\n", weightBeforeKg);

  float heightCm = 0.0f;
  bool heightReadOk = false;
  if (Config::Features::REMOTE_HEIGHT_ENABLED) {
    heightReadOk = RemoteHeightLink::requestHeightCm(heightCm);
    heightSensorReady = RemoteHeightLink::isReady();
  } else {

  if (lcdReady) {

    LcdDevice::setBacklight(false);
    LcdDevice::suspendForSensor();
  }

  if (heightSensorReady) {
      Serial.printf("[VL53L1X] Using %s %ux%u ranging session...\n",
                    Config::HeightSensor::USE_SINGLE_SHOT ? "single-shot" :
                                                             "continuous",
                    Config::HeightSensor::ROI_WIDTH,
                    Config::HeightSensor::ROI_HEIGHT);
  }
  float heightScans[Config::HeightSensor::HEIGHT_CONSENSUS_MAX_ATTEMPTS] = {};
  uint8_t validHeightScans = 0;

  for (uint8_t scan = 0;
       scan < Config::HeightSensor::HEIGHT_CONSENSUS_MAX_ATTEMPTS &&
       !heightReadOk;
       ++scan) {
    if (!heightSensorReady) {
      Serial.printf("[VL53L1X] Recovering before consensus scan %u...\n",
                    static_cast<unsigned>(scan + 1));
      heightSensorReady = VL53L1XDevice::begin();
    }
    bool scanOk = false;
    for (uint8_t retry = 0; retry < 2 && !scanOk; ++retry) {
      if (retry > 0) {

        const bool resetNeeded = true;
        Serial.printf("[VL53L1X] Height scan %u retry 2/2 (%s)\n",
                      static_cast<unsigned>(scan + 1),
                      resetNeeded ? "XSHUT reset" : "same ranging session");
        if (resetNeeded) {
          heightSensorReady = VL53L1XDevice::begin();
        }
      }
      if (heightSensorReady) {
        float candidateHeight = 0.0f;
        scanOk = VL53L1XDevice::readHeightCmFixedRoi(candidateHeight);
        if (scanOk) {
          heightScans[validHeightScans++] = candidateHeight;
          Serial.printf("[VL53L1X] Height consensus scan %u/%u = %.1f cm\n",
                        static_cast<unsigned>(validHeightScans),
                        static_cast<unsigned>(Config::HeightSensor::HEIGHT_CONSENSUS_MAX_ATTEMPTS),
                        candidateHeight);

          for (uint8_t previous = 0;
               !heightReadOk && previous + 1 < validHeightScans;
               ++previous) {
            const float pairDiff =
                fabsf(heightScans[previous] - heightScans[validHeightScans - 1]);
            if (pairDiff <= Config::HeightSensor::HEIGHT_CONSENSUS_MAX_DIFF_CM) {
              heightCm =
                  (heightScans[previous] + heightScans[validHeightScans - 1]) /
                  2.0f;
              Serial.printf("[VL53L1X] Height consensus PASS: %.1f cm (pair %.1f/%.1f, diff %.1f cm)\n",
                            heightCm, heightScans[previous],
                            heightScans[validHeightScans - 1], pairDiff);
              heightReadOk = true;
              break;
            }
          }
        }
      }
    }
    if (!scanOk) {
      Serial.printf("[VL53L1X] Height consensus scan %u failed\n",
                    static_cast<unsigned>(scan + 1));

      heightSensorReady = false;
    }
  }
  if (!heightReadOk && validHeightScans >= Config::HeightSensor::HEIGHT_CONSENSUS_SCANS) {
    Serial.printf("[VL53L1X] Height consensus reject after %u valid scans; no pair within %.1f cm\n",
                  static_cast<unsigned>(validHeightScans),
                  Config::HeightSensor::HEIGHT_CONSENSUS_MAX_DIFF_CM);
  }
  if (lcdReady) {
    LcdDevice::resumeAfterSensor();
    LcdDevice::setBacklight(true);
  }
  heightSensorReady = VL53L1XDevice::isReady();
  }

  if (!heightReadOk) {
    if (Config::Features::REMOTE_HEIGHT_ENABLED) {
      Serial.println("[REMOTE] Height unavailable or quality rejected; no ticket");
      if (lcdReady) {
        LcdDevice::showScreen("MEASURE FAILED", "HEIGHT LINK FAILED",
                              "NO RECEIPT PRINTED", "PLEASE TRY AGAIN");
      }
      return;
    }
    Serial.printf("[วัดจริง] คำนวณส่วนสูงไม่สำเร็จ ตรวจ VL53L1X และ SENSOR_MOUNT_HEIGHT_CM=%.1f\n",
                  Config::HeightSensor::SENSOR_MOUNT_HEIGHT_CM);
    Serial.println("[ทดสอบระบบชั่วคราว] ใช้คำสั่ง Serial: วัด 60 170");
    if (lcdReady) {
      if (VL53L1XDevice::lastFailureCause() ==
          VL53L1XDevice::FailureCause::Target) {
        LcdDevice::showScreen("MEASURE FAILED", "HEIGHT READ FAILED",
                              "STAND STILL", "PLEASE TRY AGAIN");
      } else {
        LcdDevice::showScreen("MEASURE FAILED", "CHECK HEIGHT SENSOR",
                              "PLEASE TRY AGAIN");
      }
    }
    return;
  }

  float weightKg = 0.0f;
  if (lcdReady) {
    LcdDevice::showScreen("CHECKING WEIGHT", "KEEP BOTH FEET ON",
                          "DO NOT STEP OFF", "PLEASE WAIT");
  }
  if (!HX711Device::readWeightKg(weightKg)) {
    Serial.printf("[วัดจริง] อ่านน้ำหนักเป็นกิโลกรัมไม่สำเร็จ ตรวจ Load Cell และค่า CALIBRATION_FACTOR=%.6f\n",
                  Config::LoadCell::CALIBRATION_FACTOR);
    Serial.println("[ทดสอบระบบชั่วคราว] ใช้คำสั่ง Serial: วัด 60 170");
    if (lcdReady) {
      LcdDevice::showScreen("MEASURE FAILED", "CHECK LOAD CELL",
                            "PLEASE TRY AGAIN");
    }
    return;
  }

  Serial.printf("[HX711] Weight after height scan=%.1f kg\n", weightKg);
  if (fabsf(weightKg - weightBeforeKg) >
      Config::LoadCell::MAX_WEIGHT_DRIFT_KG) {
    Serial.printf("[HX711] Weight changed %.1f kg during scan; queue not sent\n",
                  weightKg - weightBeforeKg);
    if (lcdReady) {
      LcdDevice::showScreen("MEASURE FAILED", "WEIGHT CHANGED",
                            "STAND IN CENTER", "PLEASE TRY AGAIN");
    }
    return;
  }
  weightKg = (weightBeforeKg + weightKg) / 2.0f;

  Serial.printf("[วัดจริง] น้ำหนัก %.1f kg, ส่วนสูง %.1f cm\n", weightKg, heightCm);
  if (lcdReady) {
    LcdDevice::showScreen("MEASUREMENT READY",
                          "WEIGHT: " + String(weightKg, 1) + " kg",
                          "HEIGHT: " + String(heightCm, 1) + " cm",
                          "SENDING...");
  }
  if (walkIn) {
    OnlineQueueWorkflow::submitWalkInMeasurement(weightKg, heightCm);
  } else {
    OnlineQueueWorkflow::submitMeasurement(weightKg, heightCm);
  }
}

void updateKeypad() {
  if (!keypadReady) return;

  KeypadDevice::update();
  char key = 0;
  if (!KeypadDevice::getKey(key)) return;

  Serial.printf("[Keypad] key=%c\n", key);
  if (key == 'B' || key == 'b') {
    if (OnlineQueueWorkflow::isReadyForMeasurement()) {
      submitHardwareMeasurement(false);
    } else if (!OnlineQueueWorkflow::isActive()) {
      submitHardwareMeasurement(true);
    } else {
      OnlineQueueWorkflow::handleKey(key);
    }
  } else {
    OnlineQueueWorkflow::handleKey(key);
  }
}






void updatePrinter() {
  if (!printerHostReady) {
    return;
  }

  UsbPrinter::update();

  const bool readyNow = UsbPrinter::isReady();
  if (readyNow == lastPrinterReady) {
    return;
  }

  lastPrinterReady = readyNow;
  Serial.println(readyNow ? "[เครื่องพิมพ์] พร้อมใช้งาน"
                          : "[เครื่องพิมพ์] ยังไม่พร้อมใช้งาน");
  if (lcdReady) {
    OnlineQueueWorkflow::refreshDisplay();
  }
}
}

void setup() {
  Serial.begin(Config::System::SERIAL_BAUD);
  Serial.setTimeout(Config::System::SERIAL_READ_TIMEOUT_MS);
  delay(Config::System::SERIAL_START_DELAY_MS);

  Serial.println();
  Serial.println("================================");
  Serial.println(" ระบบคลินิก ESP32-S3 แบบแยกโมดูล ");
  Serial.println("================================");

  if (isI2cRequired()) {
    beginI2cBus();
  } else {
    Serial.println("[I2C] ข้ามการเริ่มต้น เพราะปิดอุปกรณ์ I2C ทั้งหมดไว้");
  }
  beginDevices();

  OnlineQueueWorkflow::begin();
  OnlineQueueWorkflow::setMeasurementHardwareReady(loadCellReady,
                                                     heightSensorReady);
  Serial.println("[ระบบ] เริ่มต้นระบบเรียบร้อย");
  lastMeasurementFinishedAt = millis();
}

void loop() {
  if (Config::Features::REMOTE_HEIGHT_ENABLED) {
    RemoteHeightLink::update();
  }
  updateKeypad();
  retryHeightSensorIfNeeded();
  updatePrinter();
  updateLinkDisplay();

  OnlineQueueWorkflow::update();
  delay(Config::System::LOOP_DELAY_MS);
}
