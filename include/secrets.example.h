#pragma once

// คัดลอกไฟล์นี้เป็น include/secrets.h แล้วใส่ค่าจริง
// ห้าม commit include/secrets.h ขึ้น source control
namespace Secrets {
constexpr char WIFI_SSID[] = "YOUR_WIFI_SSID";
constexpr char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";
constexpr char MQTT_PASSWORD[] = "YOUR_MQTT_PASSWORD";
}  // namespace Secrets
