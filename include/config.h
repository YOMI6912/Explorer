#pragma once

#include <Arduino.h>

#include "secrets.h"

// รวมค่าตั้งต้นของระบบไว้ที่ไฟล์นี้
namespace Config {

namespace System {
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t SERIAL_START_DELAY_MS = 1200;
constexpr uint32_t SERIAL_READ_TIMEOUT_MS = 200;
constexpr uint32_t LOOP_DELAY_MS = 5;
}

namespace Features {

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
#ifndef CLINIC_KEYPAD_DIRECT_GPIO
#define CLINIC_KEYPAD_DIRECT_GPIO 0
#endif
#ifndef CLINIC_REMOTE_HEIGHT
#define CLINIC_REMOTE_HEIGHT 0
#endif

constexpr bool ENABLE_LCD = CLINIC_ENABLE_LCD != 0;
constexpr bool ENABLE_KEYPAD = CLINIC_ENABLE_KEYPAD != 0;
constexpr bool ENABLE_LOAD_CELL = CLINIC_ENABLE_LOAD_CELL != 0;
constexpr bool ENABLE_HEIGHT_SENSOR = CLINIC_ENABLE_HEIGHT_SENSOR != 0;
constexpr bool ENABLE_USB_PRINTER = CLINIC_ENABLE_USB_PRINTER != 0;
constexpr bool REMOTE_HEIGHT_ENABLED = CLINIC_REMOTE_HEIGHT != 0;
}

namespace RemoteHeight {
// ช่อง AP ส่วนตัวต้องคงที่เพื่อให้ C3 เชื่อม TCP ได้

constexpr char AP_SSID[] = "Clinic-Height-Link-90";
constexpr char AP_PASSWORD[] = "ClinicHeight90!";

constexpr uint8_t AP_CHANNEL = 1;
constexpr uint16_t TCP_PORT = 45831;
constexpr uint32_t CLIENT_WAIT_MS = 5000;
constexpr float MOUNT_HEIGHT_CM = 197.0f;

constexpr uint16_t MIN_RAW_MM = 100;
constexpr uint16_t MAX_RAW_MM = 430;
constexpr uint8_t MIN_VALID_FRAMES = 42;
constexpr uint8_t MAX_SPAN_MM = 8;

constexpr uint32_t RESPONSE_TIMEOUT_MS = 18000;
}

namespace Pins {
constexpr uint8_t I2C_SDA = 8;
constexpr uint8_t I2C_SCL = 9;

constexpr uint8_t HEIGHT_I2C_SDA = I2C_SDA;
constexpr uint8_t HEIGHT_I2C_SCL = I2C_SCL;

constexpr uint8_t HEIGHT_XSHUT = 3;
constexpr uint8_t LCD_I2C_SDA = 1;
constexpr uint8_t LCD_I2C_SCL = 2;
constexpr uint8_t HX711_DT = 4;
constexpr uint8_t HX711_SCK = 5;

constexpr uint8_t STATUS_LED = 48;
}

namespace I2C {
constexpr uint32_t CLOCK_HZ = 100000;
}

namespace LCD {
constexpr uint8_t ADDRESS = 0x27;
constexpr uint8_t COLUMNS = 20;
constexpr uint8_t ROWS = 4;
}

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
}

namespace LoadCell {
constexpr uint32_t READY_TIMEOUT_MS = 3000;
constexpr uint32_t READ_TIMEOUT_MS = 1000;
constexpr uint8_t RAW_AVERAGE_SAMPLES = 10;
constexpr uint8_t WEIGHT_AVERAGE_SAMPLES = 10;
constexpr uint8_t STABLE_WEIGHT_SAMPLES = 9;
constexpr uint8_t MIN_STABLE_WEIGHT_SAMPLES = 7;
constexpr float MAX_SAMPLE_DEVIATION_KG = 1.5f;
constexpr float MAX_WEIGHT_DRIFT_KG = 2.0f;

// ค่าจากการสอบเทียบด้วยน้ำหนักอ้างอิง
constexpr float CALIBRATION_FACTOR = 11036.75f;
constexpr bool TARE_ON_START = true;

constexpr float MIN_WEIGHT_KG = 1.0f;
constexpr float MAX_WEIGHT_KG = 300.0f;
}

namespace HeightSensor {
// ค่ากรองต้องผ่านก่อนจึงยอมรับผลวัด

constexpr bool USE_SINGLE_SHOT = true;

constexpr uint32_t TIMEOUT_MS = 300;
constexpr uint32_t TIMING_BUDGET_US = 50000;

constexpr uint32_t CONTINUOUS_PERIOD_MS = 80;

#ifndef CLINIC_HEIGHT_ROI_SIZE
#define CLINIC_HEIGHT_ROI_SIZE 16
#endif
constexpr uint8_t ROI_WIDTH = CLINIC_HEIGHT_ROI_SIZE;
constexpr uint8_t ROI_HEIGHT = CLINIC_HEIGHT_ROI_SIZE;
constexpr uint8_t FULL_ROI_WIDTH = 16;
constexpr uint8_t FULL_ROI_HEIGHT = 16;

constexpr uint8_t SAMPLE_COUNT = 7;

constexpr uint8_t HEIGHT_CONSENSUS_SCANS = 2;
constexpr float HEIGHT_CONSENSUS_MAX_DIFF_CM = 0.8f;

constexpr uint8_t HEIGHT_CONSENSUS_MAX_ATTEMPTS = 3;

constexpr uint8_t MIN_VALID_SAMPLES = 3;
constexpr uint8_t MIN_HEAD_CLUSTER_SAMPLES = 7;

constexpr uint8_t MAX_SAMPLE_ATTEMPTS = 16;
constexpr float MAX_IQR_CM = 2.0f;
constexpr float MAX_SAMPLE_SPAN_CM = 0.8f;

constexpr float MIN_SIGNAL_MCPS = 3.0f;

constexpr uint8_t I2C_RETRY_WARNING_PER_ROI = 3;

constexpr uint8_t I2C_RETRY_REJECT_PER_ROI = 3;

constexpr uint16_t MAX_ROI_DISTANCE_DIFFERENCE_MM = 25;

constexpr float SENSOR_MOUNT_HEIGHT_CM = 197.3f;

constexpr float HEIGHT_OFFSET_CM = 0.0f;
constexpr float MIN_HEIGHT_CM = 30.0f;
constexpr float MAX_HEIGHT_CM = 250.0f;
}

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

constexpr uint8_t PAPER_FEED_LINES = 8;

}

namespace Runtime {
constexpr uint8_t OTP_LENGTH = 6;

constexpr uint32_t WIFI_RETRY_MS = 15000;
constexpr uint32_t MQTT_RETRY_MS = 5000;
constexpr uint32_t OTP_RESULT_TIMEOUT_MS = 30000;
constexpr uint32_t MEASUREMENT_RETRY_MS = 10000;
constexpr uint32_t MEASUREMENT_RETRY_MAX_MS = 60000;
constexpr uint32_t PRINT_JOB_WAIT_TIMEOUT_MS = 30000;

constexpr uint16_t MQTT_READ_BUFFER_SIZE = 17 * 1024;
constexpr uint16_t MQTT_WRITE_BUFFER_SIZE = 2048;
constexpr uint16_t MQTT_KEEP_ALIVE_SECONDS = 60;
constexpr uint16_t MQTT_TIMEOUT_MS = 10000;
constexpr uint8_t TLS_HANDSHAKE_TIMEOUT_SECONDS = 15;
constexpr uint8_t MQTT_QOS = 1;
}

namespace Network {
constexpr const char *WIFI_SSID = Secrets::WIFI_SSID;
constexpr const char *WIFI_PASSWORD = Secrets::WIFI_PASSWORD;
}

namespace MQTT {
constexpr char BROKER[] = "hf391a51.ala.asia-southeast1.emqxsl.com";
constexpr uint16_t PORT = 8883;
constexpr char USERNAME[] = "scale_001";
constexpr const char *PASSWORD = Secrets::MQTT_PASSWORD;
constexpr char DEVICE_ID[] = "SCALE-001";
constexpr char CLIENT_ID[] = "SCALE-001";
constexpr char TOPIC_PREFIX[] = "clinic/v1";
}

namespace Time {
constexpr char NTP_SERVER_1[] = "pool.ntp.org";
constexpr char NTP_SERVER_2[] = "time.google.com";
}

}
