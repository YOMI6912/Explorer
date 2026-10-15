#include <Arduino.h>
#include <esp_system.h>

#include "remote_config.h"
#include "sensor_reader.h"
#include "wireless_link.h"

namespace {
uint32_t bootId = 0;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  const esp_reset_reason_t resetReason = esp_reset_reason();
  Serial.printf("[BOOT] reset reason=%d\n", static_cast<int>(resetReason));
  bootId = esp_random();

  WirelessLink::begin();
  Serial.printf("[SENSOR] SDA=%u SCL=%u XSHUT=%u address=0x29\n",
                RemoteConfig::SDA_PIN, RemoteConfig::SCL_PIN,
                RemoteConfig::XSHUT_PIN);
  const bool sensorReady = SensorReader::begin();
  Serial.printf("[SENSOR] init=%s; wall_calibration=%s; head_profile=ENABLED\n",
                sensorReady ? "OK" : "FAILED",
                RemoteConfig::CALIBRATION_VALID ? "VALID" : "NOT SET");
}

void loop() {
  WirelessLink::poll();
  WirelessLink::Request request;
  if (WirelessLink::takeRequest(request)) {
    const HeightLink::Response reply =
        SensorReader::measureHead(request.id, bootId);
    WirelessLink::sendReply(request.mac, reply);
  }
  delay(2);
}
