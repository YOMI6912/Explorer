#pragma once

#include <Arduino.h>

#include "secrets.h"

// ============================================================
// CONFIGURATION FILE
// แก้ค่าขา ที่อยู่ I2C การคาลิเบรต และค่าการเชื่อมต่อที่ไฟล์นี้
// ไฟล์อุปกรณ์และ main.cpp ไม่ควรมีเลขขาหรือค่าตั้งต้นซ้ำอีก
// ============================================================
namespace Config {

namespace System {
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t SERIAL_START_DELAY_MS = 1200;
constexpr uint32_t SERIAL_READ_TIMEOUT_MS = 200;
constexpr uint32_t LOOP_DELAY_MS = 5;
constexpr uint8_t MEASUREMENT_COUNTDOWN_SECONDS = 3;
}  // namespace System

namespace Features {
// Environment ปกติปิดอุปกรณ์ไว้ ส่วน esp32-s3-hardware-test จะกำหนดค่า
// CLINIC_ENABLE_* ผ่าน platformio.ini โดยไม่ต้องแก้ไฟล์นี้สลับไปมา
#ifndef CLINIC_ENABLE_LCD
#define CLINIC_ENABLE_LCD 0
#endif
#ifndef CLINIC_ENABLE_KEYPAD
#define CLINIC_ENABLE_KEYPAD 0
#endif
#ifndef CLINIC_ENABLE_LOAD_CELL
#define CLINIC_ENABLE_LOAD_CELL 0
#endif
#ifndef CLINIC_ENABLE_HEIGHT_SENSOR
#define CLINIC_ENABLE_HEIGHT_SENSOR 0
#endif
#ifndef CLINIC_ENABLE_USB_PRINTER
#define CLINIC_ENABLE_USB_PRINTER 0
#endif
#ifndef CLINIC_ENABLE_WALKIN_RETRY_TEST
#define CLINIC_ENABLE_WALKIN_RETRY_TEST 0
#endif
#ifndef CLINIC_ENABLE_ONLINE_RETRY_TEST
#define CLINIC_ENABLE_ONLINE_RETRY_TEST 0
#endif
#ifndef CLINIC_KEYPAD_DIRECT_GPIO
#define CLINIC_KEYPAD_DIRECT_GPIO 0
#endif

constexpr bool ENABLE_LCD = CLINIC_ENABLE_LCD != 0;
constexpr bool ENABLE_KEYPAD = CLINIC_ENABLE_KEYPAD != 0;
constexpr bool ENABLE_LOAD_CELL = CLINIC_ENABLE_LOAD_CELL != 0;
constexpr bool ENABLE_HEIGHT_SENSOR = CLINIC_ENABLE_HEIGHT_SENSOR != 0;
constexpr bool ENABLE_USB_PRINTER = CLINIC_ENABLE_USB_PRINTER != 0;
constexpr bool ENABLE_QUEUE_A_SERIAL_TEST = true;
constexpr bool ENABLE_WALKIN_RETRY_TEST =
    CLINIC_ENABLE_WALKIN_RETRY_TEST != 0;
constexpr bool ENABLE_ONLINE_RETRY_TEST =
    CLINIC_ENABLE_ONLINE_RETRY_TEST != 0;
}  // namespace Features

namespace Pins {
constexpr uint8_t I2C_SDA = 8;
constexpr uint8_t I2C_SCL = 9;
// Keep LCD and VL53L1X on separate ESP32 I2C controllers so neither device
// can reconfigure or stall the other device's bus.
constexpr uint8_t HEIGHT_I2C_SDA = I2C_SDA;
constexpr uint8_t HEIGHT_I2C_SCL = I2C_SCL;
// VL53L1X XSHUT is active-low. Driving this pin low power-resets the sensor
// core without restarting the ESP32, LCD, Wi-Fi or printer.
constexpr uint8_t HEIGHT_XSHUT = 3;
constexpr uint8_t LCD_I2C_SDA = 1;
constexpr uint8_t LCD_I2C_SCL = 2;
constexpr uint8_t HX711_DT = 4;
constexpr uint8_t HX711_SCK = 5;

// RGB LED บน ESP32-S3 DevKitC-1 มักเป็น GPIO48
// ถ้าไฟไม่ติด ให้ลองเปลี่ยนเป็น GPIO38
constexpr uint8_t STATUS_LED = 48;
}  // namespace Pins

namespace I2C {
constexpr uint32_t CLOCK_HZ = 100000;
}  // namespace I2C

namespace LCD {
constexpr uint8_t ADDRESS = 0x27;
constexpr uint8_t COLUMNS = 20;
constexpr uint8_t ROWS = 4;
}  // namespace LCD

namespace Keypad {
constexpr bool DIRECT_GPIO = CLINIC_KEYPAD_DIRECT_GPIO != 0;

constexpr uint8_t ADDRESS = 0x20;
constexpr uint32_t STARTUP_SETTLE_MS = 100;
constexpr uint32_t DEBOUNCE_MS = 2;


constexpr uint8_t ROW_BITS[4] = {0, 1, 2, 3};
constexpr uint8_t COLUMN_BITS[4] = {4, 5, 6, 7};

constexpr uint8_t DIRECT_ROW_PINS[4] = {6, 7, 10, 11};
constexpr uint8_t DIRECT_COLUMN_PINS[4] = {12, 13, 14, 15};

constexpr char PCF_KEY_MAP[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}};


constexpr char DIRECT_KEY_MAP[4][4] = {
    {'1', '4', '7', '*'},
    {'2', '5', '8', '0'},
    {'3', '6', '9', '#'},
    {'A', 'B', 'C', 'D'}};
}  // namespace Keypad

namespace LoadCell {
constexpr uint32_t READY_TIMEOUT_MS = 3000;
constexpr uint32_t READ_TIMEOUT_MS = 1000;
constexpr uint8_t RAW_AVERAGE_SAMPLES = 10;
constexpr uint8_t WEIGHT_AVERAGE_SAMPLES = 10;
constexpr uint8_t STABLE_WEIGHT_SAMPLES = 9;
constexpr uint8_t MIN_STABLE_WEIGHT_SAMPLES = 7;
constexpr float MAX_SAMPLE_DEVIATION_KG = 1.5f;
constexpr float MAX_WEIGHT_DRIFT_KG = 2.0f;

// Calibrated from the stable raw span for a centered 63.0 kg reference load.
constexpr float CALIBRATION_FACTOR = 10716.723f;
constexpr bool TARE_ON_START = true;

constexpr float MIN_WEIGHT_KG = 1.0f;
constexpr float MAX_WEIGHT_KG = 300.0f;
}  // namespace LoadCell

namespace HeightSensor {
// Conservative timing for the installed clone: keep a 50 ms ranging budget
// but leave 100 ms between results so the previous frame is fully latched.
constexpr uint32_t TIMEOUT_MS = 500;
constexpr uint32_t TIMING_BUDGET_US = 50000;
// Leave a full margin after the 50 ms ranging budget before requesting the
// next result.  This clone was returning 0/64351 mm when polled too closely.
constexpr uint32_t CONTINUOUS_PERIOD_MS = 100;
// Keep the measured 12x12 ROI running in production. On this installation,
// repeated fixed-ROI scans succeeded while switching 12 -> 16 -> 12 caused
// failed ranging and I2C recovery. The full ROI remains diagnostic only.
#ifndef CLINIC_HEIGHT_ROI_SIZE
#define CLINIC_HEIGHT_ROI_SIZE 16
#endif
constexpr uint8_t ROI_WIDTH = CLINIC_HEIGHT_ROI_SIZE;
constexpr uint8_t ROI_HEIGHT = CLINIC_HEIGHT_ROI_SIZE;
constexpr uint8_t FULL_ROI_WIDTH = 16;
constexpr uint8_t FULL_ROI_HEIGHT = 16;
// Use strict acceptance for clinic measurements. When quality is poor, fail
// the height read instead of queueing a stable but potentially wrong target.
constexpr uint8_t SAMPLE_COUNT = 7;
// The sensor can lose the I2C bus after returning a short, stable burst. Keep
// a three-frame cluster rather than discarding an otherwise usable target;
// span/IQR checks below still reject scattered returns.
constexpr uint8_t MIN_VALID_SAMPLES = 3;

constexpr uint8_t MAX_SAMPLE_ATTEMPTS = 24;
constexpr float MAX_IQR_CM = 2.0f;
constexpr float MAX_SAMPLE_SPAN_CM = 4.0f;
// The shorter 160 cm target is farther from the sensor and this installation
// returns about 5-6 MCPS from it. Keep rejecting very weak 1-2 MCPS returns,
// but allow the measured 3+ MCPS target signal; distance/status/span filters
// still reject stale and unstable frames.
constexpr float MIN_SIGNAL_MCPS = 3.0f;
// Report noisy I2C communication, but let independently validated ROI samples
// pass after a successful retry. A failed sensor read still gets discarded.
constexpr uint8_t I2C_RETRY_WARNING_PER_ROI = 3;
// Diagnostic multi-ROI scans compare raw medians; production keeps the
// fixed 12x12 ROI to avoid changing the sensor's optical configuration.
constexpr uint16_t MAX_ROI_DISTANCE_DIFFERENCE_MM = 25;


constexpr float SENSOR_MOUNT_HEIGHT_CM = 197.3f;


// Use the measured mounting height minus the live range. The earlier +3.1 cm
// offset made three consecutive fixed-ROI scans of the 184 cm reference read
// 186.8-187.5 cm. This zero offset is provisional until checked on multiple
// known heights; it cannot correct a return from the wrong target.
constexpr float HEIGHT_OFFSET_CM = 0.0f;
constexpr float MIN_HEIGHT_CM = 30.0f;
constexpr float MAX_HEIGHT_CM = 250.0f;
}  // namespace HeightSensor

namespace Printer {

constexpr uint16_t USB_VENDOR_ID = 0x0FE6;
constexpr uint16_t USB_PRODUCT_ID = 0x811E;
constexpr uint8_t INTERFACE_NUMBER = 0;
constexpr uint8_t ALTERNATE_SETTING = 0;
constexpr uint8_t ENDPOINT_OUT = 0x01;
constexpr uint8_t ENDPOINT_IN = 0x81;
constexpr uint8_t MAX_EVENT_MESSAGES = 5;
constexpr uint32_t HOST_TASK_STACK = 4096;
constexpr uint32_t CLIENT_TASK_STACK = 6144;
constexpr UBaseType_t HOST_TASK_PRIORITY = 2;
constexpr UBaseType_t CLIENT_TASK_PRIORITY = 3;
constexpr uint32_t READY_DELAY_MS = 500;
constexpr uint32_t TRANSFER_TIMEOUT_MS = 15000;
// Advance enough paper for the final ticket line to clear the print head.
constexpr uint8_t PAPER_FEED_LINES = 8;
// ปิดการพิมพ์กระดาษ TEST อัตโนมัติ เพื่อไม่ให้ชนกับงานบัตรคิวจาก Backend
// ยังสั่งทดสอบเองได้จาก Serial ด้วยคำสั่ง PRINTTEST หรือ พิมพ์ทดสอบ
constexpr bool PRINT_TEST_WHEN_CONNECTED = false;
}  // namespace Printer

namespace Diagnostics {
// Keep production input responsive; frequent blocking sensor diagnostics can
// cause short keypad presses to be missed and flood the serial monitor.
constexpr uint32_t SENSOR_PRINT_INTERVAL_MS = 5000;
}  // namespace Diagnostics

namespace QueueASerialTest {
constexpr uint8_t OTP_LENGTH = 6;
// Give phone hotspots enough time to finish association/authentication.
// Calling WiFi.begin() every five seconds can restart an in-progress join.
constexpr uint32_t WIFI_RETRY_MS = 15000;
constexpr uint32_t MQTT_RETRY_MS = 5000;
constexpr uint32_t OTP_RESULT_TIMEOUT_MS = 30000;
constexpr uint32_t MEASUREMENT_RETRY_MS = 10000;
constexpr uint32_t MEASUREMENT_RETRY_MAX_MS = 60000;
constexpr uint32_t PRINT_JOB_WAIT_TIMEOUT_MS = 30000;
// RX ต้องรับ payload ตาม contract ได้สูงสุด 16 KB รวม overhead ของ MQTT/topic
constexpr uint16_t MQTT_READ_BUFFER_SIZE = 17 * 1024;
constexpr uint16_t MQTT_WRITE_BUFFER_SIZE = 2048;
constexpr uint16_t MQTT_KEEP_ALIVE_SECONDS = 60;
constexpr uint16_t MQTT_TIMEOUT_MS = 10000;
constexpr uint8_t TLS_HANDSHAKE_TIMEOUT_SECONDS = 15;
constexpr uint8_t MQTT_QOS = 1;
}  // namespace QueueASerialTest

// การเชื่อมต่อ Local/Test สำหรับคิว A ผ่าน Serial Monitor
namespace Network {
constexpr const char *WIFI_SSID = Secrets::WIFI_SSID;
constexpr const char *WIFI_PASSWORD = Secrets::WIFI_PASSWORD;
}  // namespace Network

namespace MQTT {
constexpr char BROKER[] = "hf391a51.ala.asia-southeast1.emqxsl.com";
constexpr uint16_t PORT = 8883;
constexpr char USERNAME[] = "scale_001";
constexpr const char *PASSWORD = Secrets::MQTT_PASSWORD;
constexpr char DEVICE_ID[] = "SCALE-001";
constexpr char CLIENT_ID[] = "SCALE-001";
constexpr char TOPIC_PREFIX[] = "clinic/v1";
}  // namespace MQTT

namespace Time {
constexpr char NTP_SERVER_1[] = "pool.ntp.org";
constexpr char NTP_SERVER_2[] = "time.google.com";
}  // namespace Time

}  // namespace Config
