#include "workflows/OnlineQueueWorkflow.h"

#include <ArduinoJson.h>
#include <MQTT.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
#include <esp_system.h>
#include <math.h>
#include <time.h>

#include "config.h"
#include "devices/LcdDevice.h"
#include "devices/RemoteHeightLink.h"
#include "devices/UsbPrinter.h"
#include "emqx_ca.h"

namespace OnlineQueueWorkflow {
namespace {
enum class State : uint8_t {
  Idle,
  EnteringOtp,
  WaitingOtpResult,
  ReadyForMeasurement,
  WaitingMeasurementAck
};

WiFiClientSecure networkClient;
MQTTClient mqtt(Config::Runtime::MQTT_READ_BUFFER_SIZE,
                Config::Runtime::MQTT_WRITE_BUFFER_SIZE);
Preferences preferences;
bool preferencesReady = false;
bool clinicStaDeferredForHeight = false;

State state = State::Idle;
String otpBuffer;
String measurementSessionId;
String measurementSessionExpiresAt;
time_t measurementSessionExpiresEpoch = 0;
String expectedOnlineQueueNumber;
String pendingOtpRequestId;
uint32_t otpRequestSentAt = 0;

struct PendingMeasurement {
  bool active = false;
  String messageId;
  String topic;
  String payload;
  uint32_t lastSentAt = 0;
  uint8_t retryCount = 0;
};

PendingMeasurement pendingMeasurement;

constexpr size_t PRINT_HISTORY_SIZE = 16;
struct PrintHistoryEntry {
  String printJobId;
  String status;
  String errorCode;
};

PrintHistoryEntry printHistory[PRINT_HISTORY_SIZE];
size_t printHistoryCount = 0;

bool autoMeasurementAfterOtp = false;
bool autoMeasurementReady = false;
float pendingWeightKg = 0.0f;
float pendingHeightCm = 0.0f;

bool pendingPrintAck = false;
String pendingPrintJobId;
String pendingPrintStatus;
String pendingPrintErrorCode;

bool printTransferPending = false;
String activePrintJobId;
String activePrintQueueNumber;
uint32_t printTransferStartedAt = 0;

bool awaitingPrintJob = false;
String awaitingPrintMessageId;
String awaitingPrintQueueNumber;
uint32_t printJobWaitStartedAt = 0;
String lastHandledPrintMessageId;

uint32_t lastWifiAttemptAt = 0;
uint32_t lastMqttAttemptAt = 0;
uint32_t lastClockWaitLogAt = 0;
bool wifiWasConnected = false;
uint32_t lastClockAttemptAt = 0;
uint32_t lcdReturnToReadyAt = 0;
bool loadCellHardwareReady = true;
bool heightSensorHardwareReady = true;

const String baseTopic = String(Config::MQTT::TOPIC_PREFIX) +
                         "/devices/" + Config::MQTT::DEVICE_ID;

void showReadyScreen() {
  if (!LcdDevice::isReady()) return;
  if (!loadCellHardwareReady || !heightSensorHardwareReady) {
    LcdDevice::showScreen("SYSTEM NOT READY",
                          loadCellHardwareReady ? "LOAD CELL: OK"
                                                : "CHECK LOAD CELL",
                          heightSensorHardwareReady ? "HEIGHT SENSOR: OK"
                                                    : "CHECK HEIGHT SENSOR",
                          "CHECK POWER/WIRING");
    return;
  }
  const bool networkReady = WiFi.status() == WL_CONNECTED && mqtt.connected();
  const String status = String("NET:") + (networkReady ? "OK" : "WAIT") +
                        " PRINT:" + (UsbPrinter::isReady() ? "OK" : "WAIT");
  LcdDevice::showScreen("CLINIC QUEUE READY", status,
                        "A:ONLINE B:WALK-IN", "PRESS A OR B");
}

void showOtpScreen() {

  String enteredOtp = "OTP: " + otpBuffer;
  LcdDevice::showScreen("ONLINE QUEUE A", enteredOtp,
                        "ENTER 6 DIGITS", "#OK C:DEL D:CANCEL");
}

void showTemporaryScreen(const String &line0,
                         const String &line1,
                         const String &line2 = "",
                         const String &line3 = "") {
  LcdDevice::showScreen(line0, line1, line2, line3);
  lcdReturnToReadyAt = millis() + 10000;
}

const char *stateName() {
  switch (state) {
    case State::Idle:
      return "พร้อมเริ่ม";
    case State::EnteringOtp:
      return "กำลังกรอก OTP";
    case State::WaitingOtpResult:
      return "กำลังรอผลตรวจ OTP";
    case State::ReadyForMeasurement:
      return "พร้อมรับค่าวัด";
    case State::WaitingMeasurementAck:
      return "กำลังรอผลบันทึกค่าวัด";
  }
  return "ไม่ทราบสถานะ";
}

const char *backendErrorThai(const String &errorCode) {
  if (errorCode == "INVALID_OTP") return "OTP ไม่ถูกต้อง หมดอายุ หรือถูกใช้แล้ว";
  if (errorCode == "RATE_LIMITED") return "ตรวจ OTP บ่อยเกินไป กรุณารอสักครู่";
  if (errorCode == "INVALID_SESSION") return "Session ไม่ถูกต้องหรือหมดอายุ";
  if (errorCode == "WEIGHT_OUT_OF_RANGE") return "น้ำหนักอยู่นอกช่วงที่กำหนด";
  if (errorCode == "HEIGHT_OUT_OF_RANGE") return "ส่วนสูงอยู่นอกช่วงที่กำหนด";
  if (errorCode == "DEVICE_MISMATCH") return "รหัสอุปกรณ์ไม่ตรงกับ Topic";
  if (errorCode == "MESSAGE_ID_CONFLICT") return "รหัสข้อความซ้ำกับอุปกรณ์อื่น";
  if (errorCode == "INVALID_PAYLOAD") return "รูปแบบข้อมูลไม่ถูกต้อง";
  if (errorCode == "INVALID_MODE") return "โหมดคิวไม่ถูกต้อง";
  if (errorCode == "RETAIN_NOT_ALLOWED") return "ห้ามส่งข้อมูลวัดแบบ Retain";
  if (errorCode == "INTERNAL_ERROR") return "Backend เกิดข้อผิดพลาดภายใน";
  return "ไม่ทราบสาเหตุ";
}

String responseErrorCode(JsonDocument &response) {
  String errorCode = response["error_code"] | "";
  if (errorCode.isEmpty()) {

    errorCode = String(response["code"] | "");
  }
  return errorCode;
}

String pendingMeasurementMode() {
  if (!pendingMeasurement.active || pendingMeasurement.payload.isEmpty()) {
    return "";
  }

  JsonDocument measurement;
  if (deserializeJson(measurement, pendingMeasurement.payload)) {
    return "";
  }
  return String(measurement["mode"] | "");
}

uint32_t measurementRetryDelayMs() {
  uint64_t delayMs = Config::Runtime::MEASUREMENT_RETRY_MS;
  const uint8_t shifts = pendingMeasurement.retryCount < 3
                             ? pendingMeasurement.retryCount
                             : 3;
  delayMs <<= shifts;
  if (delayMs > Config::Runtime::MEASUREMENT_RETRY_MAX_MS) {
    delayMs = Config::Runtime::MEASUREMENT_RETRY_MAX_MS;
  }
  return static_cast<uint32_t>(delayMs);
}

void clearPrintJobWait() {
  awaitingPrintJob = false;
  awaitingPrintMessageId = "";
  awaitingPrintQueueNumber = "";
  printJobWaitStartedAt = 0;
}

void startPrintJobWait(const String &messageId, const String &queueNumber) {
  if (messageId.isEmpty() || lastHandledPrintMessageId == messageId) {
    clearPrintJobWait();
    return;
  }
  awaitingPrintJob = true;
  awaitingPrintMessageId = messageId;
  awaitingPrintQueueNumber = queueNumber;
  printJobWaitStartedAt = millis();
  Serial.println("[พิมพ์] Backend ระบุ print_pending=true กำลังรอ /print");
}

void markPrintJobReceived(const String &messageId) {
  if (messageId.isEmpty()) {
    return;
  }
  lastHandledPrintMessageId = messageId;
  if (awaitingPrintJob && messageId == awaitingPrintMessageId) {
    clearPrintJobWait();
  }
}

bool isSixDigitOtp(const String &value) {
  if (value.length() != Config::Runtime::OTP_LENGTH) {
    return false;
  }

  for (size_t i = 0; i < value.length(); ++i) {
    if (!isdigit(static_cast<unsigned char>(value.charAt(i)))) {
      return false;
    }
  }
  return true;
}

bool measurementIsValid(float weightKg, float heightCm) {
  if (!isfinite(weightKg) ||
      weightKg < Config::LoadCell::MIN_WEIGHT_KG ||
      weightKg > Config::LoadCell::MAX_WEIGHT_KG) {
    Serial.printf("[วัด] น้ำหนักต้องอยู่ระหว่าง %.1f-%.1f กิโลกรัม\n",
                  Config::LoadCell::MIN_WEIGHT_KG,
                  Config::LoadCell::MAX_WEIGHT_KG);
    return false;
  }

  if (!isfinite(heightCm) ||
      heightCm < Config::HeightSensor::MIN_HEIGHT_CM ||
      heightCm > Config::HeightSensor::MAX_HEIGHT_CM) {
    Serial.printf("[วัด] ส่วนสูงต้องอยู่ระหว่าง %.1f-%.1f เซนติเมตร\n",
                  Config::HeightSensor::MIN_HEIGHT_CM,
                  Config::HeightSensor::MAX_HEIGHT_CM);
    return false;
  }
  return true;
}

bool parseMeasurementPair(String value, float &weightKg, float &heightCm) {
  value.replace(',', ' ');
  value.replace('/', ' ');
  return sscanf(value.c_str(), "%f %f", &weightKg, &heightCm) == 2 &&
         measurementIsValid(weightKg, heightCm);
}

bool clockIsReady() {
  return time(nullptr) > 1700000000;
}

void startClock() {
  if (clockIsReady()) {
    return;
  }
  lastClockAttemptAt = millis();
  configTime(0, 0, Config::Time::NTP_SERVER_1,
             Config::Time::NTP_SERVER_2, "time.cloudflare.com");
  Serial.println("[เวลา] เริ่มตั้งเวลาผ่าน NTP แล้ว");
}

String makeIsoUtcTime() {
  time_t now = time(nullptr);
  struct tm timeInfo = {};
  gmtime_r(&now, &timeInfo);

  char buffer[25] = {};
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeInfo);
  return String(buffer);
}

bool isLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

uint8_t daysInMonth(int year, int month) {
  static constexpr uint8_t DAYS[] = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && isLeapYear(year)) {
    return 29;
  }
  return DAYS[month - 1];
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra =
      yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return static_cast<int64_t>(era) * 146097 + dayOfEra - 719468;
}

bool parseIsoUtcTime(const String &value, time_t &epoch) {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!value.endsWith("Z") ||
      sscanf(value.c_str(),
             "%4d-%2d-%2dT%2d:%2d:%2d",
             &year,
             &month,
             &day,
             &hour,
             &minute,
             &second) != 6 ||
      year < 1970 || month < 1 || month > 12 || day < 1 ||
      day > daysInMonth(year, month) || hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 || second < 0 || second > 60) {
    return false;
  }

  const int64_t seconds =
      daysFromCivil(year, static_cast<unsigned>(month),
                    static_cast<unsigned>(day)) * 86400LL +
      hour * 3600LL + minute * 60LL + second;
  if (seconds <= 0) {
    return false;
  }
  epoch = static_cast<time_t>(seconds);
  return true;
}

String makeIdentifier(const char *prefix) {
  char randomHex[9] = {};
  snprintf(randomHex,
           sizeof(randomHex),
           "%08lX",
           static_cast<unsigned long>(esp_random()));
  return String(prefix) + "-" + Config::MQTT::DEVICE_ID + "-" +
         String(static_cast<uint32_t>(time(nullptr))) + "-" + randomHex;
}

void persistPendingMeasurement() {
  if (!preferencesReady) {
    return;
  }

  preferences.putString("pm_id", pendingMeasurement.messageId);
  preferences.putString("pm_topic", pendingMeasurement.topic);
  preferences.putString("pm_payload", pendingMeasurement.payload);
  preferences.putBool("pm_active", pendingMeasurement.active);
}

void clearPendingMeasurement() {
  pendingMeasurement = PendingMeasurement{};
  if (!preferencesReady) {
    return;
  }

  preferences.putBool("pm_active", false);
  preferences.remove("pm_id");
  preferences.remove("pm_topic");
  preferences.remove("pm_payload");
}

int findPrintHistory(const String &printJobId) {
  for (size_t i = 0; i < printHistoryCount; ++i) {
    if (printHistory[i].printJobId == printJobId) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void savePrintHistory() {
  if (!preferencesReady) {
    return;
  }

  String serialized;
  serialized.reserve(printHistoryCount * 96);
  for (size_t i = 0; i < printHistoryCount; ++i) {
    serialized += printHistory[i].printJobId;
    serialized += '\t';
    serialized += printHistory[i].status;
    serialized += '\t';
    serialized += printHistory[i].errorCode;
    serialized += '\n';
  }
  preferences.putString("print_hist", serialized);
}

void rememberPrintJob(const String &printJobId,
                      const String &status,
                      const String &errorCode) {
  int index = findPrintHistory(printJobId);
  if (index < 0) {
    if (printHistoryCount < PRINT_HISTORY_SIZE) {
      index = static_cast<int>(printHistoryCount++);
    } else {
      for (size_t i = 1; i < PRINT_HISTORY_SIZE; ++i) {
        printHistory[i - 1] = printHistory[i];
      }
      index = static_cast<int>(PRINT_HISTORY_SIZE - 1);
    }
  }

  printHistory[index].printJobId = printJobId;
  printHistory[index].status = status;
  printHistory[index].errorCode = errorCode;
  savePrintHistory();
}

void loadPrintHistory() {
  printHistoryCount = 0;
  if (!preferencesReady) {
    return;
  }

  const String serialized = preferences.getString("print_hist", "");
  int lineStart = 0;
  while (lineStart < static_cast<int>(serialized.length()) &&
         printHistoryCount < PRINT_HISTORY_SIZE) {
    int lineEnd = serialized.indexOf('\n', lineStart);
    if (lineEnd < 0) {
      lineEnd = serialized.length();
    }
    const String line = serialized.substring(lineStart, lineEnd);
    const int firstTab = line.indexOf('\t');
    const int secondTab = line.indexOf('\t', firstTab + 1);
    if (firstTab > 0 && secondTab > firstTab) {
      PrintHistoryEntry &entry = printHistory[printHistoryCount++];
      entry.printJobId = line.substring(0, firstTab);
      entry.status = line.substring(firstTab + 1, secondTab);
      entry.errorCode = line.substring(secondTab + 1);
    }
    lineStart = lineEnd + 1;
  }
}

void persistPendingPrintAck() {
  if (!preferencesReady) {
    return;
  }

  preferences.putString("pa_id", pendingPrintJobId);
  preferences.putString("pa_status", pendingPrintStatus);
  preferences.putString("pa_error", pendingPrintErrorCode);
  preferences.putBool("pa_active", pendingPrintAck);
}

void clearPendingPrintAck() {
  pendingPrintAck = false;
  pendingPrintJobId = "";
  pendingPrintStatus = "";
  pendingPrintErrorCode = "";
  if (!preferencesReady) {
    return;
  }

  preferences.putBool("pa_active", false);
  preferences.remove("pa_id");
  preferences.remove("pa_status");
  preferences.remove("pa_error");
}

void beginPersistence() {
  preferencesReady = preferences.begin("clinic-fw", false);
  if (!preferencesReady) {
    Serial.println("[NVS] เปิดพื้นที่เก็บสถานะไม่สำเร็จ ระบบจะเก็บเฉพาะใน RAM");
    return;
  }

  loadPrintHistory();

  if (preferences.getBool("pm_active", false)) {
    pendingMeasurement.active = true;
    pendingMeasurement.messageId = preferences.getString("pm_id", "");
    pendingMeasurement.topic = preferences.getString("pm_topic", "");
    pendingMeasurement.payload = preferences.getString("pm_payload", "");
    pendingMeasurement.lastSentAt = 0;
    pendingMeasurement.retryCount = 0;
    if (pendingMeasurement.messageId.isEmpty() ||
        pendingMeasurement.topic.isEmpty() ||
        pendingMeasurement.payload.isEmpty()) {
      clearPendingMeasurement();
    } else {
      state = State::WaitingMeasurementAck;
      Serial.println("[NVS] พบค่าวัดที่ยังไม่ได้ ACK จะส่งซ้ำหลังเชื่อม MQTT");
    }
  }

  if (preferences.getBool("pa_active", false)) {
    pendingPrintAck = true;
    pendingPrintJobId = preferences.getString("pa_id", "");
    pendingPrintStatus = preferences.getString("pa_status", "");
    pendingPrintErrorCode = preferences.getString("pa_error", "");
    if (pendingPrintJobId.isEmpty() || pendingPrintStatus.isEmpty()) {
      clearPendingPrintAck();
    } else {
      Serial.println("[NVS] พบ print ACK ที่ค้างอยู่ จะส่งซ้ำหลังเชื่อม MQTT");
    }
  }
}


void clearLocalFlow() {
  state = State::Idle;
  otpBuffer = "";
  measurementSessionId = "";
  measurementSessionExpiresAt = "";
  measurementSessionExpiresEpoch = 0;
  expectedOnlineQueueNumber = "";
  pendingOtpRequestId = "";
  otpRequestSentAt = 0;
  autoMeasurementAfterOtp = false;
  autoMeasurementReady = false;
  pendingWeightKg = 0.0f;
  pendingHeightCm = 0.0f;
}

void printStatus() {
  Serial.println();
  Serial.println("========== สถานะคิว A ==========");
  Serial.println(String("Wi-Fi   : ") +
                 (WiFi.status() == WL_CONNECTED ? "เชื่อมต่อแล้ว" : "ยังไม่เชื่อมต่อ"));
  Serial.println(String("MQTT    : ") +
                 (mqtt.connected() ? "เชื่อมต่อแล้ว" : "ยังไม่เชื่อมต่อ"));
  Serial.println("IP      : " + WiFi.localIP().toString());
  Serial.println("ขั้นตอน  : " + String(stateName()));
  Serial.println(String("Session : ") +
                 (measurementSessionId.isEmpty() ? "ยังไม่มี" : measurementSessionId));
  Serial.println(String("หมดอายุ : ") +
                 (measurementSessionExpiresAt.isEmpty()
                      ? "ยังไม่มี"
                      : measurementSessionExpiresAt + " (UTC)"));
  Serial.println(String("Pending : ") +
                 (pendingMeasurement.active ? pendingMeasurement.messageId : "ไม่มี"));
  Serial.println(String("รอพิมพ์ : ") +
                 (awaitingPrintJob
                      ? awaitingPrintQueueNumber + " / " + awaitingPrintMessageId
                      : "ไม่มี"));
  Serial.println("================================");
}

bool publishPayload(const String &topic, const String &payload) {
  if (!mqtt.connected()) {
    Serial.println("[MQTT] ยังไม่เชื่อมต่อ จึงยังส่งข้อความไม่ได้");
    return false;
  }

  Serial.println();
  Serial.println("========== ส่ง MQTT ==========");
  Serial.println("Topic    : " + topic);
  if (topic.endsWith("/otp-verify")) {
    Serial.println("ข้อมูล    : [ซ่อน OTP; ดู payload เต็มได้จาก MQTTX]");
  } else {
    Serial.println("ข้อมูล    : " + payload);
  }

  const bool published = mqtt.publish(topic,
                                      payload,
                                      false,
                                      Config::Runtime::MQTT_QOS);
  if (!published) {
    Serial.println("สถานะ    : ส่งไม่สำเร็จ");
  } else if (topic.endsWith("/otp-verify")) {
    Serial.println("สถานะ    : ส่งถึง Broker แล้ว กำลังรอ /otp-result จาก Backend");
  } else if (topic.endsWith("/measurements")) {
    Serial.println("สถานะ    : ส่งถึง Broker แล้ว กำลังรอ /measurement-ack จาก Backend");
  } else if (topic.endsWith("/print-ack")) {
    Serial.println("สถานะ    : ส่งผลการพิมพ์ถึง Broker แล้ว ไม่ต้องรอ ACK เพิ่ม");
  } else {
    Serial.println("สถานะ    : ส่งถึง Broker แล้ว");
  }
  Serial.println("=============================");
  return published;
}

bool publishJson(const String &topic, JsonDocument &document) {
  String payload;
  serializeJson(document, payload);
  return publishPayload(topic, payload);
}

bool publishPendingMeasurement(bool retrying) {
  if (!pendingMeasurement.active || !mqtt.connected()) {
    return false;
  }

  if (retrying) {
    if (pendingMeasurement.retryCount < UINT8_MAX) {
      ++pendingMeasurement.retryCount;
    }
    Serial.printf("[ค่าวัด] ยังไม่ได้ Backend ACK กำลังส่ง message_id เดิมซ้ำ ครั้งที่ %u\n",
                  pendingMeasurement.retryCount);
  }
  const bool published = publishPayload(pendingMeasurement.topic,
                                        pendingMeasurement.payload);

  pendingMeasurement.lastSentAt = millis();
  return published;
}

bool queueMeasurement(JsonDocument &measurement) {
  if (pendingMeasurement.active) {
    Serial.println("[ค่าวัด] ยังมีข้อความก่อนหน้ารอ ACK จึงยังไม่รับค่าวัดใหม่");
    return false;
  }

  pendingMeasurement.active = true;
  pendingMeasurement.messageId = String(measurement["message_id"] | "");
  if (pendingMeasurement.messageId.isEmpty()) {
    pendingMeasurement = PendingMeasurement{};
    Serial.println("[ค่าวัด] ไม่มี message_id จึงไม่จัดคิวส่ง");
    return false;
  }
  pendingMeasurement.topic = baseTopic + "/measurements";
  pendingMeasurement.payload = "";
  serializeJson(measurement, pendingMeasurement.payload);
  pendingMeasurement.lastSentAt = 0;
  pendingMeasurement.retryCount = 0;
  persistPendingMeasurement();
  state = State::WaitingMeasurementAck;

  if (!publishPendingMeasurement(false)) {
    Serial.println("[ค่าวัด] เก็บข้อความไว้แล้ว จะส่งอัตโนมัติเมื่อ MQTT พร้อม");
  }
  return true;
}

bool publishOtpVerify(const String &otp) {
  if (!isSixDigitOtp(otp)) {
    Serial.println("[คิว A] OTP ต้องเป็นตัวเลขให้ครบ 6 หลัก");
    return false;
  }

  if (pendingMeasurement.active) {
    Serial.println("[คิว A] ยังมีค่าวัดรอ ACK จึงยังเริ่ม OTP ใหม่ไม่ได้");
    return false;
  }
  if (state == State::WaitingOtpResult ||
      state == State::ReadyForMeasurement) {
    Serial.println("[คิว A] มีขั้นตอนเดิมอยู่ กรุณากดยกเลิกก่อนเริ่ม OTP ใหม่");
    return false;
  }

  pendingOtpRequestId = makeIdentifier("REQ");
  JsonDocument request;
  request["schema_version"] = "1.0";
  request["request_id"] = pendingOtpRequestId;
  request["device_id"] = Config::MQTT::DEVICE_ID;
  request["otp"] = otp;

  if (!publishJson(baseTopic + "/otp-verify", request)) {
    pendingOtpRequestId = "";
    showTemporaryScreen("NETWORK NOT READY", "OTP NOT SENT", "PLEASE TRY AGAIN");
    return false;
  }

  otpBuffer = otp;
  measurementSessionId = "";
  state = State::WaitingOtpResult;
  otpRequestSentAt = millis();
  lcdReturnToReadyAt = 0;
  LcdDevice::showScreen("CHECKING OTP...", "PLEASE WAIT",
                        "CONTACTING SERVER", "D:CANCEL");
  Serial.println("[คิว A] ส่ง OTP แล้ว กำลังรอผลจาก /otp-result");
  return true;
}

bool publishOnlineMeasurement(float weightKg, float heightCm) {
  if (measurementSessionId.isEmpty()) {
    Serial.println("[คิว A] ยังไม่มี measurement_session_id กรุณาตรวจ OTP ก่อน");
    return false;
  }
  if (measurementSessionExpiresEpoch > 0 &&
      time(nullptr) >= measurementSessionExpiresEpoch) {
    Serial.println("[คิว A] measurement session หมดอายุแล้ว กรุณาขอ OTP ใหม่");
    clearLocalFlow();
    return false;
  }
  if (!measurementIsValid(weightKg, heightCm)) {
    return false;
  }
  if (!clockIsReady()) {
    if (millis() - lastClockAttemptAt >= 30000UL) startClock();
    Serial.println("[เวลา] NTP ยังไม่พร้อม กรุณารอสักครู่แล้วส่งคำสั่ง วัด อีกครั้ง");
    return false;
  }

  JsonDocument measurement;
  measurement["schema_version"] = "1.0";
  measurement["message_id"] = makeIdentifier("MSG-A");
  measurement["device_id"] = Config::MQTT::DEVICE_ID;
  measurement["mode"] = "online";
  measurement["measurement_session_id"] = measurementSessionId;
  measurement["measured_at"] = makeIsoUtcTime();
  measurement["weight"] = weightKg;
  measurement["height"] = heightCm;

  if (!queueMeasurement(measurement)) {
    return false;
  }

  Serial.println("[คิว A] จัดคิวส่งน้ำหนักและส่วนสูงแล้ว (ไม่มี BMI) กำลังรอ ACK");
  LcdDevice::showScreen("ONLINE QUEUE A", "SENDING RESULT...",
                        "WAITING BACKEND", "PLEASE WAIT");
  return true;
}

bool publishWalkInMeasurement(float weightKg, float heightCm) {
  if (state != State::Idle || pendingMeasurement.active) {
    Serial.println("[คิว B] ระบบกำลังทำงานหรือมีค่าวัดรอ ACK จึงยังเริ่ม Walk-in ไม่ได้");
    return false;
  }
  if (!measurementIsValid(weightKg, heightCm)) {
    return false;
  }
  if (!clockIsReady()) {
    Serial.println("[เวลา] NTP ยังไม่พร้อม กรุณารอสักครู่แล้วลองใหม่");
    return false;
  }

  JsonDocument measurement;
  measurement["schema_version"] = "1.0";
  measurement["message_id"] = makeIdentifier("MSG-B");
  measurement["device_id"] = Config::MQTT::DEVICE_ID;
  measurement["mode"] = "walk_in";
  measurement["measured_at"] = makeIsoUtcTime();
  measurement["weight"] = weightKg;
  measurement["height"] = heightCm;

  if (!queueMeasurement(measurement)) {
    return false;
  }

  Serial.println("[คิว B] จัดคิวส่ง Walk-in แล้ว กำลังรอ Backend ออกเลขคิว");
  LcdDevice::showScreen("WALK-IN QUEUE B", "SENDING RESULT...",
                        "WAITING BACKEND", "PLEASE WAIT");
  return true;
}

void queuePrintAck(const String &printJobId,
                   const char *status,
                   const char *errorCode = "") {
  pendingPrintJobId = printJobId;
  pendingPrintStatus = status;
  pendingPrintErrorCode = errorCode;
  pendingPrintAck = true;
  persistPendingPrintAck();

  if (pendingPrintStatus == "printed") {
    Serial.println("[พิมพ์] พิมพ์บัตรสำเร็จ กำลังส่ง ACK=printed ไป Backend");
  } else {
    Serial.printf("[พิมพ์] พิมพ์ไม่สำเร็จ กำลังส่ง ACK=failed (%s)\n",
                  pendingPrintErrorCode.c_str());
  }
}

void finishPrintJob(String printJobId,
                     const char *status,
                     const char *errorCode = "") {
  const String finishedQueueNumber = activePrintQueueNumber;
  rememberPrintJob(printJobId, status, errorCode);
  printTransferPending = false;
  activePrintJobId = "";
  activePrintQueueNumber = "";
  printTransferStartedAt = 0;
  queuePrintAck(printJobId, status, errorCode);
  if (String(status) == "printed") {
    showTemporaryScreen("QUEUE COMPLETED",
                        "NUMBER: " + finishedQueueNumber,
                        "PRINTED OK", "TAKE YOUR TICKET");
  } else {
    showTemporaryScreen("PRINT FAILED",
                        "QUEUE: " + finishedQueueNumber,
                        String(errorCode).substring(0, 20),
                        "ASK STAFF FOR HELP");
  }
}

void handlePrintJob(JsonDocument &document) {
  const String printJobId = document["print_job_id"] | "";
  const String messageId = document["message_id"] | "";
  if (printJobId.isEmpty()) {
    Serial.println("[พิมพ์] ไม่มี print_job_id จึงไม่รับงานพิมพ์นี้");
    return;
  }

  if (printTransferPending && printJobId == activePrintJobId) {
    markPrintJobReceived(messageId);
    Serial.printf("[พิมพ์] งาน '%s' กำลังพิมพ์อยู่ จึงไม่พิมพ์ซ้ำ\n",
                  printJobId.c_str());
    return;
  }

  const int historyIndex = findPrintHistory(printJobId);
  if (historyIndex >= 0) {
    markPrintJobReceived(messageId);
    Serial.printf("[พิมพ์] ป้องกันงานซ้ำ พบ print_job_id เดิม '%s'\n",
                  printJobId.c_str());
    if (printTransferPending ||
        (pendingPrintAck && pendingPrintJobId != printJobId)) {
      Serial.println("[พิมพ์] ยังตอบงานซ้ำไม่ได้ Backend สามารถส่งซ้ำภายหลัง");
    } else if (!pendingPrintAck) {
      queuePrintAck(printJobId,
                    printHistory[historyIndex].status.c_str(),
                    printHistory[historyIndex].errorCode.c_str());
    }
    return;
  }

  if (printTransferPending || pendingPrintAck) {
    Serial.println("[พิมพ์] ยังมีงานก่อนหน้าที่กำลังพิมพ์หรือรอส่ง ACK จึงยังไม่รับงานใหม่");
    return;
  }

  const String schemaVersion = document["schema_version"] | "";
  const String queueNumber = document["queue_number"] | "";
  const float weightKg = document["weight"] | 0.0f;
  const float heightCm = document["height"] | 0.0f;
  const float bmi = document["bmi"] | 0.0f;
  const String measuredAt = document["measured_at"] | "";
  activePrintQueueNumber = queueNumber;

  if (schemaVersion != "1.0" || messageId.isEmpty() || queueNumber.isEmpty() ||
      measuredAt.isEmpty() || !isfinite(bmi) || bmi <= 0.0f ||
      !measurementIsValid(weightKg, heightCm)) {
    Serial.println("[พิมพ์] รูปแบบ Print Job ไม่ครบหรือค่าไม่ถูกต้อง");
    finishPrintJob(printJobId, "failed", "INVALID_PRINT_PAYLOAD");
    return;
  }

  markPrintJobReceived(messageId);

  Serial.println("[พิมพ์] ได้รับงานพิมพ์จาก Backend");
  Serial.println("--------------------------------");
  Serial.printf("หมายเลขคิว : %s\n", queueNumber.c_str());
  Serial.printf("น้ำหนัก     : %.1f กิโลกรัม\n", weightKg);
  Serial.printf("ส่วนสูง     : %.1f เซนติเมตร\n", heightCm);
  Serial.printf("BMI         : %.2f (รับจากระบบ ไม่ได้คำนวณเอง)\n", bmi);
  Serial.println("--------------------------------");
  LcdDevice::showScreen("QUEUE: " + queueNumber,
                        "PRINTING TICKET...",
                        "PLEASE WAIT", "DO NOT TURN OFF");

  rememberPrintJob(printJobId, "failed", "PRINT_INTERRUPTED");

  if (!Config::Features::ENABLE_USB_PRINTER || !UsbPrinter::isReady()) {
    finishPrintJob(printJobId, "failed", "PRINTER_NOT_CONNECTED");
    return;
  }
  if (UsbPrinter::isBusy()) {
    finishPrintJob(printJobId, "failed", "PRINTER_BUSY");
    return;
  }
  if (!UsbPrinter::printQueueTicket(queueNumber,
                                    weightKg,
                                    heightCm,
                                    bmi,
                                    measuredAt)) {
    finishPrintJob(printJobId, "failed", "PRINT_SUBMIT_FAILED");
    return;
  }

  activePrintJobId = printJobId;
  activePrintQueueNumber = queueNumber;
  printTransferPending = true;
  printTransferStartedAt = millis();
  Serial.println("[พิมพ์] ส่งข้อมูลบัตรเข้าเครื่องพิมพ์แล้ว กำลังรอผล USB transfer");
}

void onMqttMessage(String &topic, String &payload) {
  Serial.println();
  Serial.println("========== รับ MQTT ==========");
  Serial.println("Topic    : " + topic);
  Serial.println("ข้อมูล    : " + payload);
  Serial.println("=============================");

  JsonDocument response;
  const DeserializationError jsonError = deserializeJson(response, payload);
  if (jsonError) {
    Serial.println("[JSON] อ่านข้อมูลตอบกลับจาก Backend ไม่สำเร็จ");
    return;
  }

  const String status = response["status"] | "";

  if (topic == baseTopic + "/otp-result") {
    const String responseRequestId = response["request_id"] | "";
    if (state != State::WaitingOtpResult || pendingOtpRequestId.isEmpty() ||
        responseRequestId != pendingOtpRequestId) {
      Serial.printf("[OTP] ข้ามผลที่ไม่ตรงคำขอปัจจุบัน request_id='%s'\n",
                    responseRequestId.c_str());
      return;
    }
    pendingOtpRequestId = "";
    otpRequestSentAt = 0;

    if (status == "accepted") {
      measurementSessionId = String(response["measurement_session_id"] | "");
      if (measurementSessionId.isEmpty()) {
        Serial.println("[OTP] Backend ยืนยัน OTP แต่ไม่ได้ส่ง measurement_session_id");
        clearLocalFlow();
        return;
      }

      measurementSessionExpiresAt = String(response["expires_at"] | "");
      measurementSessionExpiresEpoch = 0;
      if (measurementSessionExpiresAt.isEmpty()) {
        Serial.println("[OTP] คำเตือน: Backend ไม่ได้ส่ง expires_at จะให้ Backend ตรวจอายุ Session");
      } else if (!parseIsoUtcTime(measurementSessionExpiresAt,
                                  measurementSessionExpiresEpoch)) {
        Serial.println("[OTP] คำเตือน: อ่าน expires_at ไม่สำเร็จ จะให้ Backend ตรวจอายุ Session");
      } else if (time(nullptr) >= measurementSessionExpiresEpoch) {
        Serial.println("[OTP] Backend ส่ง Session ที่หมดอายุแล้ว กรุณาขอ OTP ใหม่");
        clearLocalFlow();
        return;
      } else {
        Serial.println("[OTP] Session หมดอายุ: " +
                       measurementSessionExpiresAt + " UTC");
      }

      expectedOnlineQueueNumber = String(response["queue_number"] | "");
      if (!expectedOnlineQueueNumber.isEmpty() &&
          expectedOnlineQueueNumber.charAt(0) != 'A') {
        Serial.println("[OTP] Backend ส่ง queue_number ที่ไม่ใช่คิว A จึงยกเลิก Session");
        clearLocalFlow();
        return;
      }
      if (expectedOnlineQueueNumber.isEmpty()) {
        Serial.println("[OTP] คำเตือน: Backend ไม่ได้ส่ง queue_number จะตรวจจาก measurement ACK แทน");
      } else {
        Serial.println("[OTP] หมายเลขคิวออนไลน์: " + expectedOnlineQueueNumber);
      }

      state = State::ReadyForMeasurement;
      LcdDevice::showScreen("OTP ACCEPTED",
                            expectedOnlineQueueNumber.isEmpty()
                                ? "ONLINE QUEUE A"
                                : "QUEUE: " + expectedOnlineQueueNumber,
                            "STAND ON SCALE", "PRESS B TO MEASURE");
      Serial.println("[OTP] ยืนยันสำเร็จ พร้อมรับค่าวัดของคิวออนไลน์");
      if (autoMeasurementAfterOtp) {
        autoMeasurementReady = true;
        Serial.println("[อัตโนมัติ] OTP ผ่านแล้ว กำลังเตรียมส่งน้ำหนักและส่วนสูงจำลอง");
      } else if (Config::Features::ENABLE_LOAD_CELL &&
                 Config::Features::ENABLE_HEIGHT_SENSOR) {
        Serial.println("[ขั้นต่อไป] ยืนนิ่งบนเครื่อง แล้วกด B เพื่ออ่านน้ำหนัก/ส่วนสูงจริง");
        Serial.println("[ทดสอบก่อนคาลิเบรต] หรือพิมพ์ วัด 60 170 แล้วกด Enter");
      } else {
        Serial.println("[ขั้นต่อไป] พิมพ์ วัด 60 170 แล้วกด Enter");
      }
    } else {
      String errorCode = responseErrorCode(response);
      if (errorCode.isEmpty()) {
        errorCode = "UNKNOWN";
      }
      Serial.println("[OTP] ไม่ผ่าน: " + errorCode +
                     " (" + backendErrorThai(errorCode) + ")");
      Serial.println("[คิว A] ไม่มีการส่งค่าวัดและไม่มีการขอพิมพ์บัตร");
      clearLocalFlow();
      showTemporaryScreen("OTP REJECTED", errorCode,
                          "PRESS A TO RETRY", "OR B FOR WALK-IN");
    }
    return;
  }

  if (topic == baseTopic + "/measurement-ack") {
    const String ackMessageId = response["message_id"] | "";
    if (!pendingMeasurement.active || ackMessageId.isEmpty() ||
        ackMessageId != pendingMeasurement.messageId) {
      Serial.printf("[ACK] ข้าม ACK ที่ไม่ตรงค่าวัดปัจจุบัน message_id='%s'\n",
                    ackMessageId.c_str());
      return;
    }

    if (status == "accepted" || status == "duplicate") {
      const int64_t measurementId = response["measurement_id"].as<int64_t>();
      const String queueNumber = response["queue_number"] | "";
      const String measurementMode = pendingMeasurementMode();
      char expectedQueuePrefix = 0;
      if (measurementMode == "walk_in") {
        expectedQueuePrefix = 'B';
      } else if (measurementMode == "online") {
        expectedQueuePrefix = 'A';
      }

      if (measurementMode.isEmpty()) {
        Serial.println("[ACK] อ่าน mode ของค่าวัดที่ค้างไม่สำเร็จ จะเก็บข้อมูลไว้และรอ ACK ใหม่");
        return;
      }
      if (queueNumber.isEmpty()) {
        Serial.println("[ACK] Backend ตอบสำเร็จแต่ไม่มี queue_number จะเก็บข้อมูลไว้และรอ ACK ใหม่");
        return;
      }
      if (measurementId <= 0) {
        Serial.println("[ACK] Backend ตอบสำเร็จแต่ measurement_id ไม่ถูกต้อง จะเก็บข้อมูลไว้และรอ ACK ใหม่");
        return;
      }
      if (expectedQueuePrefix != 0 && queueNumber.charAt(0) != expectedQueuePrefix) {
        Serial.printf("[ACK] queue_number '%s' ไม่ตรงกับ mode '%s' จะเก็บข้อมูลไว้และรอ ACK ใหม่\n",
                      queueNumber.c_str(),
                      measurementMode.c_str());
        return;
      }
      if (measurementMode == "online" &&
          !expectedOnlineQueueNumber.isEmpty() &&
          queueNumber != expectedOnlineQueueNumber) {
        Serial.printf("[ACK] queue_number '%s' ไม่ตรงกับคิวจาก OTP '%s' จะเก็บข้อมูลไว้และรอ ACK ใหม่\n",
                      queueNumber.c_str(),
                      expectedOnlineQueueNumber.c_str());
        return;
      }

      Serial.println("[ACK] Backend บันทึกค่าวัดสำเร็จ");
      Serial.println("[ACK] สถานะ: " + status);
      Serial.println("[ACK] หมายเลขคิว: " + queueNumber);
      const bool printPending = response["print_pending"] | false;
      if (printPending) {
        startPrintJobWait(ackMessageId, queueNumber);
        LcdDevice::showScreen("SAVED SUCCESSFULLY",
                              "QUEUE: " + queueNumber,
                              "WAITING TO PRINT", "PLEASE WAIT");
      } else if (awaitingPrintJob && awaitingPrintMessageId == ackMessageId) {
        clearPrintJobWait();
        showTemporaryScreen("QUEUE COMPLETED", "NUMBER: " + queueNumber,
                            "SAVED OK", "NO PRINT REQUEST");
      } else {
        showTemporaryScreen("QUEUE COMPLETED", "NUMBER: " + queueNumber,
                            "SAVED OK", "NO PRINT REQUEST");
      }
      clearPendingMeasurement();
      clearLocalFlow();
      return;
    }

    if (status == "rejected") {
      const String errorCode = responseErrorCode(response);
      if (errorCode.isEmpty()) {
        Serial.println("[ACK] Backend ตอบ rejected แต่ไม่มี error_code จะเก็บข้อมูลไว้และรอ ACK ใหม่");
        return;
      }
      Serial.println("[ACK] Backend ปฏิเสธค่าวัด: " + errorCode +
                     " (" + backendErrorThai(errorCode) + ")");
      clearPendingMeasurement();
      clearLocalFlow();
      showTemporaryScreen("MEASURE REJECTED", errorCode,
                          "PLEASE TRY AGAIN", "OR ASK STAFF");
      return;
    }

    Serial.printf("[ACK] ไม่รู้จัก status='%s' จะเก็บข้อมูลไว้และรอ ACK ใหม่\n",
                  status.c_str());
    return;
  }

  if (topic == baseTopic + "/print") {
    handlePrintJob(response);
  }
}

void updatePrintTransfer() {
  if (!printTransferPending) {
    return;
  }

  const UsbPrinter::TransferResult result = UsbPrinter::takeTransferResult();
  if (result == UsbPrinter::TransferResult::Printed) {
    finishPrintJob(activePrintJobId, "printed");
    return;
  }
  if (result == UsbPrinter::TransferResult::Failed) {
    finishPrintJob(activePrintJobId, "failed", "PRINT_TRANSFER_FAILED");
    return;
  }

  if (!UsbPrinter::isReady() && !UsbPrinter::isBusy()) {
    finishPrintJob(activePrintJobId, "failed", "PRINTER_DISCONNECTED");
    return;
  }

  if (millis() - printTransferStartedAt >= Config::Printer::TRANSFER_TIMEOUT_MS) {
    finishPrintJob(activePrintJobId, "failed", "PRINT_TIMEOUT");
  }
}

void publishPendingPrintAck() {
  if (!pendingPrintAck || !mqtt.connected()) {
    return;
  }

  JsonDocument ack;
  ack["print_job_id"] = pendingPrintJobId;
  ack["device_id"] = Config::MQTT::DEVICE_ID;
  ack["status"] = pendingPrintStatus;
  if (pendingPrintStatus == "failed") {
    ack["error_code"] = pendingPrintErrorCode;
  }

  if (publishJson(baseTopic + "/print-ack", ack)) {
    clearPendingPrintAck();
  }
}

const char *wifiStatusText(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "SSID_NOT_FOUND";
    case WL_SCAN_COMPLETED:
      return "SCAN_COMPLETED";
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_CONNECT_FAILED:
      return "AUTH_OR_CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    default:
      return "UNKNOWN";
  }
}

void startWifi() {
  lastWifiAttemptAt = millis();
  if (Config::Features::REMOTE_HEIGHT_ENABLED &&
      !RemoteHeightLink::clinicStationStarted()) {
    clinicStaDeferredForHeight = true;
    Serial.println("[Wi-Fi] defer clinic STA until private C3 link is available");
    return;
  }
  Serial.printf("[Wi-Fi] กำลังเชื่อมต่อเครือข่าย %s...\n",
                Config::Network::WIFI_SSID);

  WiFi.mode(Config::Features::REMOTE_HEIGHT_ENABLED ? WIFI_AP_STA : WIFI_STA);
  WiFi.setAutoReconnect(!Config::Features::REMOTE_HEIGHT_ENABLED);
  WiFi.setSleep(false);
  WiFi.begin(Config::Network::WIFI_SSID, Config::Network::WIFI_PASSWORD);
}

void retryWifi() {
  lastWifiAttemptAt = millis();
  if (Config::Features::REMOTE_HEIGHT_ENABLED &&
      !RemoteHeightLink::clinicStationStarted()) {
    RemoteHeightLink::enableClinicStation();
    clinicStaDeferredForHeight = false;
    return;
  }
  const wl_status_t status = WiFi.status();
  Serial.printf("[Wi-Fi] ยังไม่เชื่อมต่อ: %s (%d), กำลัง reconnect...\n",
                wifiStatusText(status),
                static_cast<int>(status));
  WiFi.reconnect();
}

void connectMqtt() {
  lastMqttAttemptAt = millis();
  Serial.printf("[MQTT] กำลังเชื่อมต่อ %s:%u...\n",
                Config::MQTT::BROKER,
                Config::MQTT::PORT);

  bool connected = false;
  if (strlen(Config::MQTT::USERNAME) > 0) {
    connected = mqtt.connect(Config::MQTT::CLIENT_ID,
                             Config::MQTT::USERNAME,
                             Config::MQTT::PASSWORD);
  } else {
    connected = mqtt.connect(Config::MQTT::CLIENT_ID);
  }

  if (!connected) {
    Serial.printf("[MQTT] เชื่อมต่อไม่สำเร็จ: error=%d returnCode=%d\n",
                  static_cast<int>(mqtt.lastError()),
                  static_cast<int>(mqtt.returnCode()));
    char tlsError[128] = {};
    const int tlsErrorCode = networkClient.lastError(tlsError, sizeof(tlsError));
    if (tlsErrorCode != 0) {
      Serial.printf("[TLS] error=%d: %s\n", tlsErrorCode, tlsError);
    }
    return;
  }

  const bool otpSubscribed = mqtt.subscribe(
      baseTopic + "/otp-result", Config::Runtime::MQTT_QOS);
  const bool measurementSubscribed = mqtt.subscribe(
      baseTopic + "/measurement-ack", Config::Runtime::MQTT_QOS);
  const bool printSubscribed = mqtt.subscribe(
      baseTopic + "/print", Config::Runtime::MQTT_QOS);
  if (!otpSubscribed || !measurementSubscribed || !printSubscribed) {
    Serial.println("[MQTT] เชื่อมต่อแล้ว แต่ Subscribe บาง Topic ไม่สำเร็จ");
    mqtt.disconnect();
    return;
  }
  Serial.println("[MQTT] เชื่อมต่อ TLS สำเร็จและ Subscribe Topic ครบแล้ว");
  if (state == State::Idle && !pendingMeasurement.active &&
      lcdReturnToReadyAt == 0) {
    showReadyScreen();
  }
}

void updateConnections() {
  const uint32_t now = millis();
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;

  if (wifiConnected && !wifiWasConnected) {
    wifiWasConnected = true;
    Serial.println("[Wi-Fi] เชื่อมต่อสำเร็จ IP: " + WiFi.localIP().toString());
    startClock();
  } else if (!wifiConnected && wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println("[Wi-Fi] การเชื่อมต่อหลุด ระบบจะเชื่อมต่อใหม่อัตโนมัติ");
    if (state == State::Idle && !pendingMeasurement.active) showReadyScreen();
  }

  if (!wifiConnected) {

    const uint32_t retryInterval =
        Config::Features::REMOTE_HEIGHT_ENABLED
            ? 30000UL
            : Config::Runtime::WIFI_RETRY_MS;
    if (now - lastWifiAttemptAt >= retryInterval) {
      retryWifi();
    }
    return;
  }

  if (!clockIsReady()) {
    if (now - lastClockAttemptAt >= 30000UL) startClock();
    if (now - lastClockWaitLogAt >= 5000) {
      lastClockWaitLogAt = now;
      Serial.println("[เวลา] กำลังรอ NTP ก่อนเชื่อม MQTT/TLS...");
    }
    return;
  }

  if (!mqtt.connected() &&
      now - lastMqttAttemptAt >= Config::Runtime::MQTT_RETRY_MS) {
    connectMqtt();
  }
}

void cancelLocalFlow() {
  if (pendingMeasurement.active) {
    Serial.println("[ค่าวัด] ยกเลิกไม่ได้ เพราะส่งแล้วและกำลังรอ Backend ACK");
    return;
  }
  clearLocalFlow();
  lcdReturnToReadyAt = 0;
  showReadyScreen();
  Serial.println("[คิว A] ล้างขั้นตอนใน ESP32 และกลับหน้าเริ่มต้นแล้ว");
  Serial.println("[หมายเหตุ] คำสั่งนี้ไม่ได้ยกเลิกนัดหมายใน Backend");
}

bool startAutomaticOnline(const String &otp,
                          float weightKg,
                          float heightCm) {
  if (pendingMeasurement.active) {
    Serial.println("[อัตโนมัติ] ยังมีค่าวัดเดิมรอ ACK จึงยังเริ่มรายการใหม่ไม่ได้");
    return false;
  }
  if (!isSixDigitOtp(otp)) {
    Serial.println("[คิว A] OTP ต้องเป็นตัวเลขให้ครบ 6 หลัก");
    return false;
  }
  if (!measurementIsValid(weightKg, heightCm)) {
    return false;
  }

  clearLocalFlow();
  autoMeasurementAfterOtp = true;
  pendingWeightKg = weightKg;
  pendingHeightCm = heightCm;
  Serial.println("[อัตโนมัติ] ขั้นที่ 1/2 กำลังตรวจสอบ OTP");
  if (!publishOtpVerify(otp)) {
    clearLocalFlow();
    return false;
  }
  return true;
}
}

void begin() {
  clearLocalFlow();
  pendingMeasurement = PendingMeasurement{};
  printHistoryCount = 0;
  pendingPrintAck = false;
  pendingPrintJobId = "";
  pendingPrintStatus = "";
  pendingPrintErrorCode = "";
  printTransferPending = false;
  activePrintJobId = "";
  activePrintQueueNumber = "";
  printTransferStartedAt = 0;
  clearPrintJobWait();
  lastHandledPrintMessageId = "";

  beginPersistence();
  networkClient.setCACert(EMQX_CA_CERT);
  networkClient.setHandshakeTimeout(
      Config::Runtime::TLS_HANDSHAKE_TIMEOUT_SECONDS);
  mqtt.begin(Config::MQTT::BROKER, Config::MQTT::PORT, networkClient);
  mqtt.onMessage(onMqttMessage);
  mqtt.setKeepAlive(Config::Runtime::MQTT_KEEP_ALIVE_SECONDS);
  mqtt.setTimeout(Config::Runtime::MQTT_TIMEOUT_MS);
  mqtt.setCleanSession(true);


  clinicStaDeferredForHeight = false;
  startWifi();
  showReadyScreen();
}

void refreshDisplay() {
  if (state == State::Idle && !pendingMeasurement.active &&
      !printTransferPending && lcdReturnToReadyAt == 0) {
    showReadyScreen();
  }
}

void setMeasurementHardwareReady(bool loadCellReady, bool heightSensorReady) {
  loadCellHardwareReady = loadCellReady;
  heightSensorHardwareReady = heightSensorReady;
  refreshDisplay();
}

void handleKey(char key) {
  if (key == '\r' || key == '\n' || key == ' ' || key == '\t') {
    return;
  }
  if (key >= 'a' && key <= 'z') {
    key = static_cast<char>(key - 'a' + 'A');
  }

  if (key == 'D' || key == '*') {
    cancelLocalFlow();
    return;
  }

  if (state == State::Idle) {
    if (key != 'A') {
      Serial.printf("[คิว A] กรุณากด A ก่อน ระบบไม่รับปุ่ม '%c'\n", key);
      return;
    }
    otpBuffer = "";
    state = State::EnteringOtp;
    lcdReturnToReadyAt = 0;
    showOtpScreen();
    Serial.println("[คิว A] กรุณากรอก OTP 6 หลัก: C=ลบ, #=ยืนยัน, D/*=ยกเลิก");
    return;
  }

  if (state != State::EnteringOtp) {
    Serial.printf("[คิว A] ระบบกำลังทำงาน (%s) จึงไม่รับปุ่ม '%c'\n",
                  stateName(),
                  key);
    return;
  }

  if (key >= '0' && key <= '9') {
    if (otpBuffer.length() >= Config::Runtime::OTP_LENGTH) {
      Serial.println("[คิว A] OTP ครบ 6 หลักแล้ว กรุณากด # เพื่อยืนยัน");
      return;
    }
    otpBuffer += key;
    showOtpScreen();
    Serial.printf("[คิว A] กรอก OTP แล้ว %u/%u หลัก\n",
                  static_cast<unsigned int>(otpBuffer.length()),
                  Config::Runtime::OTP_LENGTH);
    return;
  }

  if (key == 'C') {
    if (!otpBuffer.isEmpty()) {
      otpBuffer.remove(otpBuffer.length() - 1);
    }
    showOtpScreen();
    Serial.printf("[คิว A] ลบตัวล่าสุดแล้ว เหลือ %u/%u หลัก\n",
                  static_cast<unsigned int>(otpBuffer.length()),
                  Config::Runtime::OTP_LENGTH);
    return;
  }

  if (key == '#') {
    if (!isSixDigitOtp(otpBuffer)) {
      LcdDevice::setLine(2, "OTP MUST BE 6 DIGITS");
      Serial.println("[คิว A] OTP ต้องเป็นตัวเลขให้ครบ 6 หลัก");
      return;
    }
    autoMeasurementAfterOtp = false;
    publishOtpVerify(otpBuffer);
    return;
  }

  Serial.printf("[คิว A] ไม่รองรับปุ่ม '%c'\n", key);
}


void update() {
  updateConnections();
  if (mqtt.connected()) {
    mqtt.loop();
  }

  const uint32_t now = millis();
  if (state == State::WaitingOtpResult && otpRequestSentAt != 0 &&
      now - otpRequestSentAt >= Config::Runtime::OTP_RESULT_TIMEOUT_MS) {
    Serial.println("[OTP] รอผลเกินเวลา กรุณาขอ OTP ใหม่แล้วลองอีกครั้ง");
    clearLocalFlow();
    showTemporaryScreen("OTP TIMEOUT", "PLEASE TRY AGAIN",
                        "PRESS A FOR ONLINE");
  }

  if (pendingMeasurement.active && mqtt.connected() &&
      (pendingMeasurement.lastSentAt == 0 ||
       now - pendingMeasurement.lastSentAt >=
           measurementRetryDelayMs())) {
    publishPendingMeasurement(pendingMeasurement.lastSentAt != 0);
  }

  if (autoMeasurementReady && mqtt.connected() && clockIsReady()) {
    autoMeasurementReady = false;
    autoMeasurementAfterOtp = false;
    Serial.println("[อัตโนมัติ] ขั้นที่ 2/2 กำลังส่งน้ำหนักและส่วนสูงจำลอง");
    if (!publishOnlineMeasurement(pendingWeightKg, pendingHeightCm)) {
      Serial.println("[อัตโนมัติ] ส่งค่าวัดไม่สำเร็จ กรุณาพิมพ์ วัด 60 170 เพื่อลองใหม่");
    }
  }

  updatePrintTransfer();
  publishPendingPrintAck();

  if (awaitingPrintJob && printJobWaitStartedAt != 0 &&
      now - printJobWaitStartedAt >=
          Config::Runtime::PRINT_JOB_WAIT_TIMEOUT_MS) {
    Serial.printf("[พิมพ์] คำเตือน: รอ /print ของคิว %s เกิน %lu วินาที กรุณาตรวจ Backend Print Outbox\n",
                  awaitingPrintQueueNumber.c_str(),
                  static_cast<unsigned long>(
                      Config::Runtime::PRINT_JOB_WAIT_TIMEOUT_MS / 1000));
    const String timedOutQueue = awaitingPrintQueueNumber;
    clearPrintJobWait();
    showTemporaryScreen("PRINT JOB TIMEOUT", "QUEUE: " + timedOutQueue,
                        "ASK STAFF FOR HELP");
  }

  if (lcdReturnToReadyAt != 0 &&
      static_cast<int32_t>(now - lcdReturnToReadyAt) >= 0) {
    lcdReturnToReadyAt = 0;
    if (state == State::Idle && !pendingMeasurement.active &&
        !printTransferPending) {
      showReadyScreen();
    }
  }
}

bool isActive() {
  return state != State::Idle || pendingMeasurement.active;
}

bool isNetworkReady() {
  return WiFi.status() == WL_CONNECTED && mqtt.connected();
}

bool isReadyForMeasurement() {
  return state == State::ReadyForMeasurement && !measurementSessionId.isEmpty();
}

bool submitMeasurement(float weightKg, float heightCm) {
  if (!isReadyForMeasurement()) {
    Serial.println("[คิว A] ยังไม่พร้อมรับค่าวัด กรุณาตรวจ OTP ให้ผ่านก่อน");
    return false;
  }
  return publishOnlineMeasurement(weightKg, heightCm);
}

bool submitWalkInMeasurement(float weightKg, float heightCm) {
  return publishWalkInMeasurement(weightKg, heightCm);
}

}
