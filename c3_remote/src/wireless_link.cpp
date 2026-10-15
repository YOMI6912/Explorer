#include "wireless_link.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <string.h>

#include "remote_config.h"
#include "clinic_network_local.h"

namespace WirelessLink {
namespace {
WiFiClient tcpClient;
WiFiUDP discoveryUdp;
constexpr uint16_t DISCOVERY_PORT = 45832;
constexpr char DISCOVERY_BEACON[] = "CLINIC-HEIGHT-S3-V1";
constexpr char DISCOVERY_QUERY[] = "CLINIC-HEIGHT-C3-DISCOVER";
IPAddress serverIp(0, 0, 0, 0);
bool discoveryStarted = false;
uint32_t lastDiscoveryQueryAt = 0;
Request pendingRequest = {};
bool requestPending = false;
bool linkStarted = false;
uint32_t lastConnectAttemptAt = 0;
uint32_t lastTcpAttemptAt = 0;
uint32_t lastStatusLogAt = 0;
uint32_t associationNotBeforeAt = 0;
uint8_t consecutiveTcpFailures = 0;
bool tcpWasConnected = false;
volatile bool stationDropped = false;
bool everGotIp = false;
bool associationResetInProgress = false;
bool savedProfileCleared = false;


bool useClinicHotspot = false;
uint32_t clinicConnectedAt = 0;
constexpr uint32_t TCP_RETRY_MS = 2000;
constexpr uint32_t TCP_HEARTBEAT_MS = 5000;
uint8_t requestBytes[sizeof(HeightLink::Request)] = {};
size_t requestBytesRead = 0;
uint32_t lastHeartbeatAt = 0;
uint8_t discoveredBssid[6] = {};
int32_t discoveredChannel = 0;
bool targetDiscovered = false;

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    if (!associationResetInProgress) stationDropped = true;
    Serial.printf("[LINK] WiFi disconnected reason=%u\n",
                  static_cast<unsigned>(info.wifi_sta_disconnected.reason));
  } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.println("[LINK] WiFi association accepted");
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    everGotIp = true;
    stationDropped = false;
    clinicConnectedAt = useClinicHotspot ? millis() : 0;
    serverIp = useClinicHotspot ? IPAddress(0, 0, 0, 0)
                                : IPAddress(192, 168, 4, 1);
    const uint8_t *connectedBssid = WiFi.BSSID();
    if (connectedBssid != nullptr) {
      memcpy(discoveredBssid, connectedBssid, sizeof(discoveredBssid));
      discoveredChannel = WiFi.channel();
      targetDiscovered = discoveredChannel > 0;
    }
    Serial.printf("[LINK] C3 received IP=%s\n",
                  WiFi.localIP().toString().c_str());
  }
}

void applyRadioProfile() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  esp_wifi_set_protocol(WIFI_IF_STA,
                        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  Serial.println("[LINK] Radio profile: b/g/n, 8.5 dBm, sleep OFF");
}
void resetStationRadio() {


  associationResetInProgress = true;



  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_STA);
  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);
  WiFi.setSleep(false);
  applyRadioProfile();


  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  delay(50);
  associationResetInProgress = false;
}

void scanForTarget() {
  targetDiscovered = false;
  discoveredChannel = 0;
  const int found = WiFi.scanNetworks(false, true);
  Serial.printf("[LINK] Completed WiFi scan: networks=%d\n", found);
  for (int i = 0; i < found; ++i) {
    if (WiFi.SSID(i) != RemoteConfig::LINK_SSID) continue;
    const uint8_t *bssid = WiFi.BSSID(i);
    if (bssid == nullptr) continue;
    memcpy(discoveredBssid, bssid, sizeof(discoveredBssid));
    discoveredChannel = WiFi.channel(i);
    targetDiscovered = true;
    Serial.printf("[LINK] Target AP found RSSI=%d dBm channel=%ld auth=%d BSSID=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  WiFi.RSSI(i), static_cast<long>(discoveredChannel),
                  WiFi.encryptionType(i), discoveredBssid[0],
                  discoveredBssid[1], discoveredBssid[2], discoveredBssid[3],
                  discoveredBssid[4], discoveredBssid[5]);
    break;
  }
  if (!targetDiscovered) {
    Serial.println("[LINK] Target AP absent from completed scan");
  }
  WiFi.scanDelete();
}

void startAssociation() {
  WiFi.mode(WIFI_STA);
  if (useClinicHotspot) {
    WiFi.begin(ClinicNetwork::WIFI_SSID, ClinicNetwork::WIFI_PASSWORD);
    Serial.println("[LINK] Joining clinic hotspot for direct S3 TCP");
  } else {
    scanForTarget();
    if (targetDiscovered) {
      WiFi.begin(RemoteConfig::LINK_SSID, RemoteConfig::LINK_PASSWORD,
                 discoveredChannel, discoveredBssid);
    } else {
      WiFi.begin(RemoteConfig::LINK_SSID, RemoteConfig::LINK_PASSWORD);
    }
    Serial.println("[LINK] Joining S3 private AP for direct TCP");
  }
  lastConnectAttemptAt = millis();
  Serial.println("[LINK] Waiting up to 15 seconds for WPA2 association");
}

void pollDiscovery(uint32_t now) {
  if (!discoveryStarted) {
    discoveryStarted = discoveryUdp.begin(DISCOVERY_PORT) != 0;
    if (!discoveryStarted) return;
    Serial.println("[LINK] Listening for S3 clinic-network discovery");
  }
  const int packetSize = discoveryUdp.parsePacket();
  if (packetSize > 0) {
    char packet[40] = {};
    const int received = discoveryUdp.read(packet, sizeof(packet) - 1);
    if (received > 0 && strcmp(packet, DISCOVERY_BEACON) == 0) {
      const IPAddress discoveredIp = discoveryUdp.remoteIP();
      if (serverIp != discoveredIp) {
        serverIp = discoveredIp;
        if (tcpClient) tcpClient.stop();
        Serial.printf("[LINK] S3 discovered at %s:%u\n",
                      serverIp.toString().c_str(), RemoteConfig::LINK_TCP_PORT);
      }
    }
  }
  if (now - lastDiscoveryQueryAt >= 2000) {
    lastDiscoveryQueryAt = now;
    const uint32_t broadcastAddress =
        static_cast<uint32_t>(WiFi.localIP()) |
        ~static_cast<uint32_t>(WiFi.subnetMask());
    discoveryUdp.beginPacket(IPAddress(broadcastAddress), DISCOVERY_PORT);
    discoveryUdp.write(reinterpret_cast<const uint8_t *>(DISCOVERY_QUERY),
                       sizeof(DISCOVERY_QUERY) - 1);
    discoveryUdp.endPacket();
  }
}

void connectIfNeeded() {
  const uint32_t now = millis();


  if (associationNotBeforeAt != 0 &&
      static_cast<int32_t>(now - associationNotBeforeAt) < 0) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (discoveryStarted) {
      discoveryUdp.stop();
      discoveryStarted = false;
    }
    serverIp = IPAddress(0, 0, 0, 0);
    if (tcpClient) tcpClient.stop();
    if (tcpWasConnected) {
      Serial.println("[LINK] TCP closed because C3 WiFi disconnected");
      tcpWasConnected = false;
    }
    requestBytesRead = 0;
    if (lastConnectAttemptAt != 0 &&
        now - lastConnectAttemptAt < RemoteConfig::LINK_RETRY_MS) return;
    if (lastConnectAttemptAt != 0) {
      Serial.printf("[LINK] WiFi association timed out; status=%d\n",
                    static_cast<int>(WiFi.status()));


      WiFi.disconnect(false, false);
      delay(150);
      useClinicHotspot = false;
    }
    stationDropped = false;
    startAssociation();
    return;
  }
  if (stationDropped) {
    stationDropped = false;
    if (tcpClient) tcpClient.stop();
    tcpWasConnected = false;
    requestBytesRead = 0;
    Serial.println("[LINK] Reopening TCP after WiFi association changed");
  }
  if (!useClinicHotspot && serverIp == IPAddress(0, 0, 0, 0)) {
    serverIp = IPAddress(192, 168, 4, 1);
  }


  if (useClinicHotspot && serverIp == IPAddress(0, 0, 0, 0)) {
    if (clinicConnectedAt != 0 && now - clinicConnectedAt >= 15000UL) {
      Serial.println("[LINK] S3 discovery unavailable on hotspot; trying private AP");
      WiFi.disconnect(false, false);
      useClinicHotspot = false;
      lastConnectAttemptAt = 0;
    }
    return;
  }
  if (tcpClient.connected()) {
    return;
  }
  if (tcpWasConnected) {
    Serial.println("[LINK] TCP disconnected; reconnecting automatically");
    tcpWasConnected = false;
  }
  if (tcpClient) tcpClient.stop();
  requestBytesRead = 0;
  if (lastTcpAttemptAt != 0 && now - lastTcpAttemptAt < TCP_RETRY_MS) return;
  lastTcpAttemptAt = now;
  if (tcpClient.connect(serverIp, RemoteConfig::LINK_TCP_PORT, 1000)) {
    tcpClient.setNoDelay(true);
    int keepAlive = 1;
    int keepIdleSeconds = 15;
    int keepIntervalSeconds = 5;
    int keepCount = 3;
    tcpClient.setSocketOption(SOL_SOCKET, SO_KEEPALIVE, &keepAlive,
                              sizeof(keepAlive));
    tcpClient.setOption(TCP_KEEPIDLE, &keepIdleSeconds);
    tcpClient.setOption(TCP_KEEPINTVL, &keepIntervalSeconds);
    tcpClient.setOption(TCP_KEEPCNT, &keepCount);
    tcpWasConnected = true;
    lastHeartbeatAt = millis();
    consecutiveTcpFailures = 0;
    Serial.printf("[LINK] TCP connected to S3 %s:%u, C3 IP=%s\n",
                  serverIp.toString().c_str(), RemoteConfig::LINK_TCP_PORT,
                  WiFi.localIP().toString().c_str());
  } else {
    tcpClient.stop();
    ++consecutiveTcpFailures;
    Serial.printf("[LINK] TCP connection to S3 failed (%u/3); retry in 2 seconds\n",
                  consecutiveTcpFailures);
    if (consecutiveTcpFailures >= 3) {
      consecutiveTcpFailures = 0;
      if (useClinicHotspot) {
        Serial.println("[LINK] Clinic TCP unreachable; trying private AP");
        WiFi.disconnect(false, false);
        useClinicHotspot = false;
        lastConnectAttemptAt = 0;
      } else {
        Serial.println("[LINK] TCP unreachable; renewing private AP association");



        WiFi.disconnect(false, false);
        serverIp = IPAddress(0, 0, 0, 0);
        lastConnectAttemptAt = 0;
        tcpWasConnected = false;
        requestBytesRead = 0;
        associationNotBeforeAt = millis() + 1000UL;
      }
    }
  }
}
}

bool begin() {
  if (linkStarted) return true;
  WiFi.onEvent(onWiFiEvent);
  resetStationRadio();
  associationNotBeforeAt = millis() + 20000UL;
  Serial.println("[LINK] Waiting 20 seconds for S3 clinic WiFi and AP channel to settle");
  linkStarted = true;
  Serial.printf("[LINK] Joining S3 private AP %s; TCP port=%u\n",
                RemoteConfig::LINK_SSID, RemoteConfig::LINK_TCP_PORT);
  return true;
}

void poll() {
  if (!linkStarted) return;
  connectIfNeeded();
  if (WiFi.status() == WL_CONNECTED) {
    pollDiscovery(millis());
  }
  if (millis() - lastStatusLogAt >= 15000) {
    lastStatusLogAt = millis();
    Serial.printf("[LINK] STATUS C3_WIFI=%s S3_TCP=%s IP=%s\n",
                  WiFi.status() == WL_CONNECTED ? "CONNECTED" : "WAITING",
                  tcpClient.connected() ? "CONNECTED" : "WAITING",
                  WiFi.localIP().toString().c_str());
  }
  if (!tcpClient.connected()) return;
  while (tcpClient.available() > 0 && !requestPending) {
    const int value = tcpClient.read();
    if (value < 0) break;
    requestBytes[requestBytesRead++] = static_cast<uint8_t>(value);
    if (requestBytesRead == sizeof(requestBytes)) {
      HeightLink::Request packet;
      memcpy(&packet, requestBytes, sizeof(packet));
      requestBytesRead = 0;
      if (packet.magic == HeightLink::MAGIC &&
          packet.version == HeightLink::VERSION &&
          packet.type == HeightLink::HEARTBEAT && packet.request_id == 0) {
        continue;
      } else if (packet.magic == HeightLink::MAGIC &&
          packet.version == HeightLink::VERSION &&
          packet.type == HeightLink::REQUEST && packet.request_id != 0) {
        memset(pendingRequest.mac, 0, sizeof(pendingRequest.mac));
        pendingRequest.id = packet.request_id;
        requestPending = true;
        Serial.printf("[LINK] TCP measurement request id=%lu\n",
                      static_cast<unsigned long>(packet.request_id));
      } else {
        Serial.println("[LINK] Invalid TCP request; resetting connection");
        tcpClient.stop();
        return;
      }
    }
  }
}

bool takeRequest(Request &out) {
  if (!requestPending) return false;
  out = pendingRequest;
  requestPending = false;
  return true;
}

bool sendReply(const uint8_t *, const HeightLink::Response &reply) {
  if (WiFi.status() != WL_CONNECTED || !tcpClient.connected()) {
    Serial.println("[LINK] Cannot return height: TCP disconnected");
    return false;
  }
  const size_t written = tcpClient.write(
      reinterpret_cast<const uint8_t *>(&reply), sizeof(reply));
  if (written != sizeof(reply)) {
    Serial.println("[LINK] Incomplete TCP height reply; connection reset");
    tcpClient.stop();
    return false;
  }
  Serial.printf("[LINK] TCP reply id=%lu status=%u raw=%u corrected=%u cal=%u retries=%u bytes=%u\n",
                static_cast<unsigned long>(reply.request_id), reply.status,
                reply.raw_distance_mm, reply.corrected_distance_mm,
                reply.calibration_valid, reply.i2c_retries,
                static_cast<unsigned>(written));
  return true;
}
}
