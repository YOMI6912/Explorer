#include "devices/RemoteHeightLink.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <math.h>
#include <string.h>

#include "config.h"
#include "height_link_protocol.h"

namespace RemoteHeightLink {
namespace {

WiFiServer heightServer(Config::RemoteHeight::TCP_PORT, 2);
WiFiClient heightClient;
WiFiUDP discoveryUdp;
constexpr uint16_t DISCOVERY_PORT = 45832;
constexpr char DISCOVERY_BEACON[] = "CLINIC-HEIGHT-S3-V1";
constexpr char DISCOVERY_QUERY[] = "CLINIC-HEIGHT-C3-DISCOVER";
uint32_t lastBeaconAt = 0;
bool serverReady = false;
uint32_t nextRequestId = 0;
bool eventRegistered = false;
volatile bool apStopped = false;
volatile bool stationLeft = false;
uint32_t lastApRestartAt = 0;
uint32_t lastStatusLogAt = 0;
uint32_t tcpWaitingSince = 0;
bool initialChannelScanDone = false;
bool clinicStaStarted = false;

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    const uint8_t *mac = info.wifi_ap_staconnected.mac;
    Serial.printf("[REMOTE] AP station joined %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    stationLeft = true;
    const uint8_t *mac = info.wifi_ap_stadisconnected.mac;
    Serial.printf("[REMOTE] AP station left %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else if (event == ARDUINO_EVENT_WIFI_AP_STOP) {
    apStopped = true;
    Serial.println("[REMOTE] S3 AP stopped; restart scheduled");
  } else if (event == ARDUINO_EVENT_WIFI_AP_START) {

    apStopped = false;
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("[REMOTE] Clinic STA disconnected reason=%u\n",
                  static_cast<unsigned>(info.wifi_sta_disconnected.reason));
  }
}

bool validate(const HeightLink::Response &reply, float &heightCm) {
  if (reply.status != HeightLink::OK || reply.calibration_valid != 1 ||
      reply.i2c_retries != 0 ||
      reply.valid_frames < Config::RemoteHeight::MIN_VALID_FRAMES ||
      reply.span_mm > Config::RemoteHeight::MAX_SPAN_MM ||
      reply.raw_distance_mm < Config::RemoteHeight::MIN_RAW_MM ||
      reply.raw_distance_mm > Config::RemoteHeight::MAX_RAW_MM) return false;
  // แปลงระยะที่แก้ไขแล้วเป็นส่วนสูง
  const float reportedHeight = Config::RemoteHeight::MOUNT_HEIGHT_CM -
                               reply.corrected_distance_mm / 10.0f;
  const float expectedHeight = 201.44257f -
                               0.11148977f * reply.raw_distance_mm;
  if (!isfinite(reportedHeight) ||
      fabsf(reportedHeight - expectedHeight) > 0.16f) return false;
  heightCm = reportedHeight;
  return true;
}

void acceptClient() {
  if (!serverReady) return;
  WiFiClient candidate = heightServer.available();
  if (!candidate) {

    if (heightClient.fd() >= 0 && !heightClient.connected() &&
        heightClient.available() == 0) {
      Serial.println("[REMOTE] C3 TCP disconnected; waiting for reconnect");
      heightClient.stop();
    }
    return;
  }
  if (heightClient.fd() >= 0) {
    Serial.println("[REMOTE] Replacing previous TCP client with new connection");
    heightClient.stop();
  }
  heightClient = candidate;
  heightClient.setNoDelay(true);
  int keepAlive = 1;
  int keepIdleSeconds = 15;
  int keepIntervalSeconds = 5;
  int keepCount = 3;
  heightClient.setSocketOption(SOL_SOCKET, SO_KEEPALIVE, &keepAlive,
                               sizeof(keepAlive));
  heightClient.setOption(TCP_KEEPIDLE, &keepIdleSeconds);
  heightClient.setOption(TCP_KEEPINTVL, &keepIntervalSeconds);
  heightClient.setOption(TCP_KEEPCNT, &keepCount);
  Serial.printf("[REMOTE] C3 TCP connected from %s\n",
                heightClient.remoteIP().toString().c_str());
}

void drainClientHeartbeat() {
  if (!heightClient || !heightClient.connected()) return;
  while (heightClient.available() > 0) {
    heightClient.read();
  }
}

struct ClinicResumeGuard {
  bool active = false;
  ~ClinicResumeGuard() {
    if (active) {
      WiFi.begin(Config::Network::WIFI_SSID, Config::Network::WIFI_PASSWORD);
      Serial.println("[REMOTE] Height exchange finished; resuming clinic STA for MQTT/outbox");
    }
  }
};
}

bool begin() {
  if (serverReady) return true;
  if (!eventRegistered) {
    WiFi.onEvent(onWiFiEvent);
    eventRegistered = true;
  }

  uint8_t apChannel = Config::RemoteHeight::AP_CHANNEL;
  if (WiFi.status() == WL_CONNECTED) {
    apChannel = WiFi.channel();
  } else if (!initialChannelScanDone) {
    initialChannelScanDone = true;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    const int count = WiFi.scanNetworks(false, true);
    for (int i = 0; i < count; ++i) {
      if (WiFi.SSID(i) == Config::Network::WIFI_SSID &&
          WiFi.channel(i) > 0) {
        apChannel = static_cast<uint8_t>(WiFi.channel(i));
        Serial.printf("[REMOTE] Clinic hotspot found on channel %u; matching height AP\n",
                      static_cast<unsigned>(apChannel));
        break;
      }
    }
    WiFi.scanDelete();
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
  const IPAddress apAddress(192, 168, 4, 1);
  const IPAddress netmask(255, 255, 255, 0);
  if (!WiFi.softAPConfig(apAddress, apAddress, netmask) ||
      !WiFi.softAP(Config::RemoteHeight::AP_SSID,
                   Config::RemoteHeight::AP_PASSWORD,
                   apChannel, false, 4)) {
    Serial.println("[REMOTE] Private height access point start failed");
    return false;
  }
  heightServer.begin();
  if (!heightServer) {
    Serial.printf("[REMOTE] TCP listen failed on port %u; retrying\n",
                  Config::RemoteHeight::TCP_PORT);
    heightServer.end();
    return false;
  }
  discoveryUdp.begin(DISCOVERY_PORT);
  heightServer.setNoDelay(true);
  nextRequestId = esp_random();
  if (nextRequestId == 0) nextRequestId = 1;
  serverReady = true;
  apStopped = false;
  tcpWaitingSince = 0;
  Serial.printf("[REMOTE] HEIGHT-AP/TCP server ready SSID=%s IP=%s channel=%u AP_MAC=%s port=%u protocol=v%u\n",
                Config::RemoteHeight::AP_SSID,
                WiFi.softAPIP().toString().c_str(),
                static_cast<unsigned>(WiFi.channel()),
                WiFi.softAPmacAddress().c_str(),
                Config::RemoteHeight::TCP_PORT, HeightLink::VERSION);
  return true;
}

bool isReady() { return serverReady; }

bool isConnected() {
  return serverReady && heightClient && heightClient.connected();
}

uint8_t stationCount() {
  return serverReady ? WiFi.softAPgetStationNum() : 0;
}

bool clinicStationStarted() { return clinicStaStarted; }

void enableClinicStation() {
  if (clinicStaStarted) return;
  clinicStaStarted = true;

  WiFi.enableSTA(true);

  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  WiFi.begin(Config::Network::WIFI_SSID, Config::Network::WIFI_PASSWORD);
  Serial.println("[REMOTE] Clinic STA enabled after private AP/TCP startup");
}

void update() {
  if (!serverReady) {
    const uint32_t now = millis();
    if (now - lastApRestartAt >= 3000) {
      lastApRestartAt = now;
      begin();
    }
    return;
  }
  if (apStopped || (serverReady && WiFi.softAPIP() != IPAddress(192, 168, 4, 1))) {
    const uint32_t now = millis();
    if (now - lastApRestartAt >= 3000) {
      lastApRestartAt = now;
      if (heightClient) heightClient.stop();
      heightServer.end();
      serverReady = false;
      apStopped = false;
      Serial.println("[REMOTE] Restarting S3 AP and TCP server");
      begin();
    }
    return;
  }
  if (stationLeft) {
    stationLeft = false;
    if (heightClient.fd() >= 0 && !heightClient.connected()) {
      Serial.println("[REMOTE] Closing dropped C3 TCP client");
      heightClient.stop();
    }
  }
  acceptClient();
  drainClientHeartbeat();
  const uint32_t now = millis();

  if (WiFi.softAPgetStationNum() > 0 && !isConnected()) {
    if (tcpWaitingSince == 0) tcpWaitingSince = now;
    if (now - tcpWaitingSince >= 25000UL) {
      tcpWaitingSince = now;
      if (heightClient.fd() >= 0) heightClient.stop();

      esp_wifi_deauth_sta(0);
      Serial.println("[REMOTE] Private AP station without TCP; cleared for reassociation");
      heightServer.end();
      heightServer.begin();
      heightServer.setNoDelay(true);
      Serial.printf("[REMOTE] TCP listener reopened on port %u: %s\n",
                    Config::RemoteHeight::TCP_PORT,
                    heightServer ? "READY" : "FAILED");
      if (!heightServer) serverReady = false;
    }
  } else {
    tcpWaitingSince = 0;
  }
  if (WiFi.status() == WL_CONNECTED) {
    if (now - lastBeaconAt >= 2000) {
      lastBeaconAt = now;
      const uint32_t broadcastAddress =
          static_cast<uint32_t>(WiFi.localIP()) |
          ~static_cast<uint32_t>(WiFi.subnetMask());
      discoveryUdp.beginPacket(IPAddress(broadcastAddress), DISCOVERY_PORT);
      discoveryUdp.write(reinterpret_cast<const uint8_t *>(DISCOVERY_BEACON),
                         sizeof(DISCOVERY_BEACON) - 1);
      discoveryUdp.endPacket();
    }
    const int packetSize = discoveryUdp.parsePacket();
    if (packetSize > 0) {
      char packet[40] = {};
      const int received = discoveryUdp.read(packet, sizeof(packet) - 1);
      if (received > 0 && strcmp(packet, DISCOVERY_QUERY) == 0) {
        discoveryUdp.beginPacket(discoveryUdp.remoteIP(),
                                 discoveryUdp.remotePort());
        discoveryUdp.write(reinterpret_cast<const uint8_t *>(DISCOVERY_BEACON),
                           sizeof(DISCOVERY_BEACON) - 1);
        discoveryUdp.endPacket();
      }
    }
  }
  if (now - lastStatusLogAt >= 15000) {
    lastStatusLogAt = now;
    Serial.printf("[REMOTE] STATUS S3_AP=%s stations=%u C3_TCP=%s clinic_STA=%s channel=%u\n",
                  serverReady ? "READY" : "DOWN",
                  WiFi.softAPgetStationNum(),
                  isConnected() ? "CONNECTED" : "WAITING",
                  WiFi.status() == WL_CONNECTED ? "CONNECTED" : "WAITING",
                  static_cast<unsigned>(WiFi.channel()));
  }
}

bool requestHeightCm(float &heightCm) {
  heightCm = 0.0f;
  if (!serverReady && !begin()) return false;
  ClinicResumeGuard clinicResume;
  uint32_t clientWaitMs = Config::RemoteHeight::CLIENT_WAIT_MS;

  if (!isConnected() ||
      (WiFi.status() == WL_CONNECTED && WiFi.channel() != Config::RemoteHeight::AP_CHANNEL)) {
    clinicResume.active = clinicStaStarted;
    WiFi.setAutoReconnect(false);
    if (clinicStaStarted) {
      WiFi.disconnect(false, false);
      delay(200);
    }
    if (heightClient.fd() >= 0) heightClient.stop();
    esp_wifi_deauth_sta(0);
    if (!WiFi.softAP(Config::RemoteHeight::AP_SSID,
                    Config::RemoteHeight::AP_PASSWORD,
                    Config::RemoteHeight::AP_CHANNEL, false, 4)) {
      Serial.println("[REMOTE] Private measurement AP recovery failed");
      return false;
    }
    const esp_err_t channelResult = esp_wifi_set_channel(
        Config::RemoteHeight::AP_CHANNEL, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[REMOTE] Recovering measurement TCP on channel %u; channel result=%d\n",
                  Config::RemoteHeight::AP_CHANNEL, static_cast<int>(channelResult));
    heightServer.end();
    heightServer.begin();
    heightServer.setNoDelay(true);
    if (!heightServer || channelResult != ESP_OK) return false;
    tcpWaitingSince = 0;
    clientWaitMs = 35000UL;
  }
  const uint32_t deadline = millis() + clientWaitMs;
  while (!(heightClient && heightClient.connected()) &&
         static_cast<int32_t>(deadline - millis()) > 0) {
    acceptClient();
    delay(5);
  }
  if (!(heightClient && heightClient.connected())) {
    Serial.println("[REMOTE] C3 is not connected to private TCP link; no print");
    return false;
  }
  while (heightClient.available() > 0) heightClient.read();

  ++nextRequestId;
  if (nextRequestId == 0) ++nextRequestId;
  const HeightLink::Request request = {
      HeightLink::MAGIC, HeightLink::VERSION, HeightLink::REQUEST,
      0, nextRequestId};
  if (heightClient.write(reinterpret_cast<const uint8_t *>(&request),
                         sizeof(request)) != sizeof(request)) {
    Serial.println("[REMOTE] TCP request write failed; no print");
    heightClient.stop();
    return false;
  }
  Serial.printf("[REMOTE] TCP request id=%lu sent; waiting for C3\n",
                static_cast<unsigned long>(nextRequestId));

  uint8_t bytes[sizeof(HeightLink::Response)] = {};
  size_t received = 0;
  const uint32_t started = millis();
  while (millis() - started < Config::RemoteHeight::RESPONSE_TIMEOUT_MS) {

    while (heightClient.available() > 0 && received < sizeof(bytes)) {
      const int value = heightClient.read();
      if (value >= 0) bytes[received++] = static_cast<uint8_t>(value);
    }
    if (received == sizeof(bytes)) {
      HeightLink::Response reply;
      memcpy(&reply, bytes, sizeof(reply));
      if (reply.magic != HeightLink::MAGIC ||
          reply.version != HeightLink::VERSION ||
          reply.type != HeightLink::RESPONSE ||
          reply.request_id != nextRequestId) {
        Serial.println("[REMOTE] TCP reply identity mismatch; connection reset");
        heightClient.stop();
        return false;
      }
      Serial.printf("[REMOTE] TCP reply id=%lu status=%u raw=%u corrected=%u valid=%u retries=%u span=%u cal=%u\n",
                    static_cast<unsigned long>(reply.request_id), reply.status,
                    reply.raw_distance_mm, reply.corrected_distance_mm,
                    reply.valid_frames, reply.i2c_retries, reply.span_mm,
                    reply.calibration_valid);
      if (!validate(reply, heightCm)) {
        Serial.println("[REMOTE] Rejected uncalibrated or low-quality height; no print");
        return false;
      }
      Serial.printf("[REMOTE] Accepted height %.1f cm over local AP/TCP\n", heightCm);
      return true;
    }
    if (!heightClient.connected()) {
      Serial.printf("[REMOTE] C3 TCP disconnected after %u/%u reply bytes; reconnecting\n",
                    static_cast<unsigned>(received),
                    static_cast<unsigned>(sizeof(bytes)));
      heightClient.stop();
      return false;
    }
    delay(2);
  }
  Serial.println("[REMOTE] TCP timeout waiting for calibrated C3 result; no print");
  heightClient.stop();
  return false;
}
}
