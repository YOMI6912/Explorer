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
// Listen on both the clinic STA address and the local AP address. C3 can
// reach the server through the clinic network without joining the S3 AP.
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
  }
}

bool validate(const HeightLink::Response &reply, float &heightCm) {
  if (reply.status != HeightLink::OK || reply.calibration_valid != 1 ||
      reply.i2c_retries != 0 ||
      reply.valid_frames < Config::RemoteHeight::MIN_VALID_FRAMES ||
      reply.span_mm > Config::RemoteHeight::MAX_SPAN_MM ||
      reply.raw_distance_mm < Config::RemoteHeight::MIN_RAW_MM ||
      reply.raw_distance_mm > Config::RemoteHeight::MAX_RAW_MM) return false;
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
    // WiFiClient::operator bool() is the same as connected(), so using
    // `heightClient && !heightClient.connected()` can never be true. Check the
    // file descriptor instead, otherwise a closed client occupies the only
    // stored slot forever and the C3 cannot reconnect.
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
}  // namespace

bool begin() {
  if (serverReady) return true;
  if (!eventRegistered) {
    WiFi.onEvent(onWiFiEvent);
    eventRegistered = true;
  }
  // Bring up the private AP alone first. The clinic STA is enabled only after
  // C3 associates, avoiding an AP+STA handshake race on the shared radio.
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  // Clear the previous soft-AP security state before applying the matching
  // WPA2 profile used by the C3 station.
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);
  const IPAddress apAddress(192, 168, 4, 1);
  const IPAddress netmask(255, 255, 255, 0);
  if (!WiFi.softAPConfig(apAddress, apAddress, netmask) ||
      !WiFi.softAP(Config::RemoteHeight::AP_SSID,
                   Config::RemoteHeight::AP_PASSWORD,
                   Config::RemoteHeight::AP_CHANNEL, false, 4)) {
    Serial.println("[REMOTE] Private height access point start failed");
    return false;
  }
  // Force the exact WPA2 profile after the Arduino helper so an older AP
  // profile in NVS cannot leave a stale passphrase/auth mode active.
  wifi_config_t apConfig = {};
  if (esp_wifi_get_config(WIFI_IF_AP, &apConfig) == ESP_OK) {
    apConfig.ap.authmode = WIFI_AUTH_WPA2_PSK;
    apConfig.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
    memset(apConfig.ap.password, 0, sizeof(apConfig.ap.password));
    strncpy(reinterpret_cast<char *>(apConfig.ap.password),
            Config::RemoteHeight::AP_PASSWORD,
            sizeof(apConfig.ap.password) - 1);
    if (esp_wifi_set_config(WIFI_IF_AP, &apConfig) != ESP_OK) {
      Serial.println("[REMOTE] Direct WPA2 AP credential apply failed");
      WiFi.softAPdisconnect(true);
      return false;
    }
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
  const uint32_t now = millis();
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
  const uint32_t deadline = millis() + Config::RemoteHeight::CLIENT_WAIT_MS;
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
    // Drain queued bytes before checking connected(). The ESP32 TCP stack can
    // report the peer as closed while the complete final reply is still in the
    // receive buffer.
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
}  // namespace RemoteHeightLink
