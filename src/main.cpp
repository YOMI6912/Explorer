#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include "config.h"
#include "devices/HX711Device.h"
#include "devices/KeypadDevice.h"
#include "devices/LcdDevice.h"
#include "devices/UsbPrinter.h"
#include "devices/VL53L1XDevice.h"
#include "workflows/OnlineQueueWorkflow.h"

namespace {
bool lcdReady = false;
bool keypadReady = false;
bool loadCellReady = false;
bool heightSensorReady = false;
bool printerHostReady = false;

uint32_t lastSensorPrintAt = 0;
uint32_t lastHeightSensorRetryAt = 0;
constexpr uint32_t HEIGHT_SENSOR_RETRY_MS = 5000;
constexpr uint8_t MEASUREMENT_SENSOR_START_ATTEMPTS = 4;
bool lastPrinterReady = false;

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
  // Start the height sensor first using the same order as the proven
  // standalone sketch. LCD is isolated on Wire1 and starts afterwards.
  if (Config::Features::ENABLE_HEIGHT_SENSOR) {
    heightSensorReady = VL53L1XDevice::begin();
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
  if (!Config::Features::ENABLE_HEIGHT_SENSOR || heightSensorReady) {
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
    OnlineQueueWorkflow::setMeasurementHardwareReady(loadCellReady, true);
    if (lcdReady) {
      OnlineQueueWorkflow::refreshDisplay();
    }
  }
}

bool startHeightSensorForMeasurement() {
  // Prefer the already-running session.  Resetting XSHUT on every B press can
  // leave this clone between boot and ranging-ready states, which caused an
  // otherwise working sensor to fail on the next measurement.  Probe the
  // existing session first and use XSHUT only when communication is actually
  // unhealthy.
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
    Serial.println("[VL53L1X] Preparing height sensor (reuse session; reset only if unhealthy)...");
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

  Serial.println("[วัดจริง] ปล่อยปุ่ม วางแขนข้างลำตัว และยืนนิ่ง");
  for (int8_t seconds = Config::System::MEASUREMENT_COUNTDOWN_SECONDS;
       seconds > 0;
       --seconds) {
    Serial.printf("[วัดจริง] เริ่มวัดใน %d...\n", seconds);
    if (lcdReady) {
      LcdDevice::showScreen(walkIn ? "WALK-IN QUEUE B" : "ONLINE QUEUE A",
                            "HANDS DOWN - STILL",
                            "MEASURE IN " + String(seconds),
                            "PLEASE DO NOT MOVE");
    }
    delay(1000);
  }

  // Start the sensor only after the countdown. Leaving a continuous session
  // running while the person gets ready was the main source of the
  // first-frame-good-then-I2C-corrupt pattern in the logs.
  if (Config::Features::ENABLE_HEIGHT_SENSOR) {
    heightSensorReady = startHeightSensorForMeasurement();
    OnlineQueueWorkflow::setMeasurementHardwareReady(loadCellReady,
                                                       heightSensorReady);
  }
  if (Config::Features::ENABLE_HEIGHT_SENSOR && !heightSensorReady) {
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

  // Capture weight before the potentially long VL53 measurement. Reading it
  // only afterwards can average a person stepping off into a plausible but
  // incorrect value, which previously reached the backend and printer.
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

  if (lcdReady) {
    // Keep the display contents but turn off its high-current backlight
    // during the VL53 burst.  This helps when both modules share a small
    // 3.3 V regulator and is harmless when the LCD is powered separately.
    LcdDevice::setBacklight(false);
    LcdDevice::suspendForSensor();
  }

  // The session was checked/recovered after the countdown. Keep it running
  // through the burst; a second reset here would reintroduce startup races.
  if (heightSensorReady) {
    Serial.printf("[VL53L1X] Using continuous %ux%u ranging session...\n",
                  Config::HeightSensor::ROI_WIDTH,
                  Config::HeightSensor::ROI_HEIGHT);
  }
  float heightCm = 0.0f;
  const bool heightReadOk =
      heightSensorReady && VL53L1XDevice::readHeightCmFixedRoi(heightCm);
  if (lcdReady) {
    LcdDevice::resumeAfterSensor();
    LcdDevice::setBacklight(true);
  }
  heightSensorReady = VL53L1XDevice::isReady();
  OnlineQueueWorkflow::setMeasurementHardwareReady(loadCellReady,
                                                     heightSensorReady);

  if (!heightReadOk) {
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
  if (!keypadReady) {
    return;
  }

  KeypadDevice::update();

  char key = 0;
  if (KeypadDevice::getKey(key)) {
    Serial.printf("[Keypad] กดปุ่ม: %c\n", key);
    if (Config::Features::ENABLE_QUEUE_A_SERIAL_TEST) {
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
  }
}

void runHeightDiagnostic(uint8_t mode) {
  if (OnlineQueueWorkflow::isActive()) {
    Serial.println("[HEIGHTTEST] Finish or cancel the active queue first");
    return;
  }

  Serial.printf("[HEIGHTTEST] Reading VL53 with %s; NO MQTT queue or print\n",
                mode == 2 ? "fixed production ROI" :
                mode == 1 ? "12x12 diagnostic ROI" :
                            "16x16 + centered 12x12 quality scan");
  Serial.println("[HEIGHTTEST] Stand centered and still under the sensor");
  if (!VL53L1XDevice::isReady()) {
    heightSensorReady = VL53L1XDevice::begin();
  }
  if (!VL53L1XDevice::isReady()) {
    Serial.println("[HEIGHTTEST] Sensor is offline; see I2C diagnostics above");
    OnlineQueueWorkflow::setMeasurementHardwareReady(loadCellReady, false);
    return;
  }

  if (lcdReady) {
    LcdDevice::setBacklight(false);
    LcdDevice::suspendForSensor();
  }
  float heightCm = 0.0f;
  const bool measured = mode == 2
                            ? VL53L1XDevice::readHeightCmFixedRoi(heightCm)
                            : mode == 1
                                  ? VL53L1XDevice::readHeightCmRoi12Diagnostic(heightCm)
                                  : VL53L1XDevice::readHeightCmMultiRoi(heightCm);
  if (lcdReady) {
    LcdDevice::resumeAfterSensor();
    LcdDevice::setBacklight(true);
    OnlineQueueWorkflow::refreshDisplay();
  }
  heightSensorReady = VL53L1XDevice::isReady();
  OnlineQueueWorkflow::setMeasurementHardwareReady(loadCellReady,
                                                    heightSensorReady);
  if (measured) {
    Serial.printf("[HEIGHTTEST] Height=%.1f cm accepted with %s\n",
                  heightCm,
                  mode == 2 ? "fixed production ROI" :
                  mode == 1 ? "12x12 diagnostic ROI" :
                              "multi-ROI quality check");
  } else {
    Serial.println("[HEIGHTTEST] No reliable height; no queue was sent");
  }
}

// A provisional fit from three reference people (159.0, 172.0, 183.2 cm).
// Their ten-reading medians were 143.15, 164.35, and 181.60 cm. Keep this
// separate from the queue workflow until it has been checked on new people:
// an occasional false head return near 174 cm for the 159 cm person must not
// silently become a plausible-looking clinical measurement.
constexpr float HEIGHT_TEST_CAL_SCALE = 0.62877993f;
constexpr float HEIGHT_TEST_CAL_INTERCEPT_CM = 68.887912f;
constexpr uint8_t HEIGHT_TEST_CAL_SCANS = 5;
constexpr uint8_t HEIGHT_TEST_CAL_MIN_AGREEING = 3;
constexpr float HEIGHT_TEST_CAL_AGREEMENT_CM = 4.0f;

void runHeightCalibrationDiagnostic(float referenceCm) {
  if (OnlineQueueWorkflow::isActive()) {
    Serial.println("[HEIGHTTESTCAL] Finish or cancel the active queue first");
    return;
  }

  Serial.println("[HEIGHTTESTCAL] Five independent height scans; NO MQTT queue or print");
  Serial.println("[HEIGHTTESTCAL] Stand centered and still until all five scans finish");
  if (lcdReady) {
    LcdDevice::setBacklight(false);
    LcdDevice::suspendForSensor();
  }

  float rawHeights[HEIGHT_TEST_CAL_SCANS] = {};
  uint8_t validCount = 0;
  for (uint8_t scan = 0; scan < HEIGHT_TEST_CAL_SCANS; ++scan) {
    Serial.printf("[HEIGHTTESTCAL] Starting scan %u/%u...\n",
                  scan + 1, HEIGHT_TEST_CAL_SCANS);
    if (!VL53L1XDevice::isReady() && !VL53L1XDevice::begin()) {
      Serial.printf("[HEIGHTTESTCAL] scan %u/%u: sensor offline\n",
                    scan + 1, HEIGHT_TEST_CAL_SCANS);
      continue;
    }
    float rawCm = 0.0f;
    if (!VL53L1XDevice::readHeightCmFixedRoi(rawCm)) {
      Serial.printf("[HEIGHTTESTCAL] scan %u/%u: no reliable height\n",
                    scan + 1, HEIGHT_TEST_CAL_SCANS);
      continue;
    }
    rawHeights[validCount++] = rawCm;
    Serial.printf("[HEIGHTTESTCAL] scan %u/%u: raw=%.1f cm, provisional=%.1f cm\n",
                  scan + 1, HEIGHT_TEST_CAL_SCANS, rawCm,
                  HEIGHT_TEST_CAL_SCALE * rawCm +
                      HEIGHT_TEST_CAL_INTERCEPT_CM);
  }

  if (lcdReady) {
    LcdDevice::resumeAfterSensor();
    LcdDevice::setBacklight(true);
    OnlineQueueWorkflow::refreshDisplay();
  }
  heightSensorReady = VL53L1XDevice::isReady();
  OnlineQueueWorkflow::setMeasurementHardwareReady(loadCellReady,
                                                    heightSensorReady);

  if (validCount < HEIGHT_TEST_CAL_MIN_AGREEING) {
    Serial.printf("[HEIGHTTESTCAL] Reject: only %u/%u scans succeeded\n",
                  validCount, HEIGHT_TEST_CAL_SCANS);
    return;
  }
  for (uint8_t i = 1; i < validCount; ++i) {
    const float value = rawHeights[i];
    uint8_t j = i;
    while (j > 0 && rawHeights[j - 1] > value) {
      rawHeights[j] = rawHeights[j - 1];
      --j;
    }
    rawHeights[j] = value;
  }
  const float medianRawCm = rawHeights[validCount / 2];
  uint8_t agreeing = 0;
  for (uint8_t i = 0; i < validCount; ++i) {
    if (fabsf(rawHeights[i] - medianRawCm) <=
        HEIGHT_TEST_CAL_AGREEMENT_CM) {
      ++agreeing;
    }
  }
  if (agreeing < HEIGHT_TEST_CAL_MIN_AGREEING) {
    Serial.printf("[HEIGHTTESTCAL] Reject: only %u/%u scans agree within %.1f cm of median %.1f cm\n",
                  agreeing, validCount, HEIGHT_TEST_CAL_AGREEMENT_CM,
                  medianRawCm);
    return;
  }

  const float correctedCm = HEIGHT_TEST_CAL_SCALE * medianRawCm +
                            HEIGHT_TEST_CAL_INTERCEPT_CM;
  Serial.printf("[HEIGHTTESTCAL] raw median=%.1f cm, provisional corrected=%.1f cm, agreeing=%u/%u\n",
                medianRawCm, correctedCm, agreeing, validCount);
  if (isfinite(referenceCm) && referenceCm >= 100.0f &&
      referenceCm <= 220.0f) {
    Serial.printf("[HEIGHTTESTCAL] reference=%.1f cm, corrected error=%+.1f cm\n",
                  referenceCm, correctedCm - referenceCm);
  }
  Serial.println("[HEIGHTTESTCAL] Diagnostic only; queue measurements still use the original formula");
}

void updateQueueASerialInput() {
  if (!Config::Features::ENABLE_QUEUE_A_SERIAL_TEST || Serial.available() <= 0) {
    return;
  }

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.equalsIgnoreCase("LCDRESET")) {
    if (lcdReady) {
      LcdDevice::recover();
    } else {
      Serial.println("[LCD] Not ready");
    }
    return;
  }
  if (line.equalsIgnoreCase("HEIGHTTEST")) {
    runHeightDiagnostic(0);
    return;
  }
  if (line.equalsIgnoreCase("HEIGHTTEST12")) {
    runHeightDiagnostic(1);
    return;
  }
  if (line.equalsIgnoreCase("HEIGHTTESTFIXED")) {
    runHeightDiagnostic(2);
    return;
  }
  if (line.substring(0, 13).equalsIgnoreCase("HEIGHTTESTCAL")) {
    String referenceText = line.substring(13);
    referenceText.trim();
    runHeightCalibrationDiagnostic(referenceText.length() > 0
                                       ? referenceText.toFloat()
                                       : NAN);
    return;
  }
  OnlineQueueWorkflow::handleSerialLine(line);
}

void updateSensorDiagnostics() {
  const uint32_t now = millis();
  if (now - lastSensorPrintAt < Config::Diagnostics::SENSOR_PRINT_INTERVAL_MS) {
    return;
  }
  lastSensorPrintAt = now;

  if (loadCellReady) {
    long rawValue = 0;
    if (HX711Device::readRaw(rawValue)) {
      Serial.printf("[HX711] RAW=%ld\n", rawValue);
    } else {
      Serial.println("[HX711] อ่านค่าน้ำหนักไม่สำเร็จ");
    }
  }

  // Do not poll VL53L1X here. Keeping the ranging bus quiet between users
  // prevents diagnostic traffic from consuming or corrupting measurements.
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
}  // namespace

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

  if (Config::Features::ENABLE_QUEUE_A_SERIAL_TEST) {
    OnlineQueueWorkflow::begin();
    OnlineQueueWorkflow::setMeasurementHardwareReady(loadCellReady,
                                                       heightSensorReady);
  }
  Serial.println("[ระบบ] เริ่มต้นระบบเรียบร้อย");
}

void loop() {
  updateQueueASerialInput();
  updateKeypad();
  retryHeightSensorIfNeeded();
  updateSensorDiagnostics();
  updatePrinter();

  if (Config::Features::ENABLE_QUEUE_A_SERIAL_TEST) {
    OnlineQueueWorkflow::update();
  }
  delay(Config::System::LOOP_DELAY_MS);
}
