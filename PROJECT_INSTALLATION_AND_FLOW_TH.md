# ESP32-S3 Clinic Modular

คู่มือนี้สรุปการติดตั้ง การต่ออุปกรณ์ การตั้งค่า การทำงานของ Firmware และ Flow ระหว่าง ESP32-S3, Backend และ EMQX สำหรับโปรเจกต์นี้

> เอกสารนี้อ้างอิงค่าจาก `config.h`, `platformio.ini` และโค้ดปัจจุบันของโปรเจกต์ หากค่าที่เขียนในเอกสารเดิมไม่ตรงกัน ให้ยึดค่าจากโค้ดปัจจุบันเป็นหลัก

## 1. ภาพรวมระบบ

ระบบแบ่งการทำงานออกเป็น 3 ส่วนหลัก

1. **ESP32-S3** อ่านปุ่ม, น้ำหนัก, ส่วนสูง, แสดงผล LCD และสั่งเครื่องพิมพ์
2. **EMQX Cloud** เป็น MQTT Broker ที่รับ-ส่งข้อความระหว่างอุปกรณ์กับ Backend
3. **Backend/Web** ตรวจ OTP, บันทึกค่าวัด, สร้างเลขคิว, คำนวณ BMI และสร้าง Print Job

ESP32 ไม่เชื่อม Backend โดยตรง แต่ทั้งสองฝั่งเชื่อมต่อ EMQX Broker เดียวกันผ่าน MQTT/TLS

```text
ผู้ใช้
  │
  ├─ Keypad A + OTP ──┐
  └─ Keypad B ────────┤
                      ▼
              ESP32-S3 Clinic
       ┌────────┼─────────┬─────────┐
       │        │         │         │
     LCD     HX711     VL53L1X   USB Printer
                      │
                      ▼
          Wi-Fi → MQTT/TLS → EMQX
                              │
                              ▼
                       Backend / Web
```

ระบบรองรับ 2 ประเภทคิว

| คิว | วิธีเริ่ม | การตรวจ OTP | ผลลัพธ์ |
|---|---|---|---|
| คิว A | กด `A` แล้วกรอก OTP | ต้องผ่าน | บันทึกคิวออนไลน์และสร้างเลขคิว A |
| คิว B | กด `B` ตอนระบบว่าง | ไม่ใช้ OTP | บันทึก Walk-in และสร้างเลขคิว B |

## 2. โครงสร้างไฟล์สำคัญ

| ไฟล์/โฟลเดอร์ | หน้าที่ |
|---|---|
| `platformio.ini` | Environment, board, library และ build flags |
| `include/config.h` | GPIO, I2C, calibration, feature flags และ MQTT settings |
| `include/secrets.example.h` | แบบฟอร์มสำหรับสร้าง `secrets.h` |
| `include/secrets.h` | Wi-Fi/MQTT password จริง ไฟล์นี้ถูก `.gitignore` |
| `include/emqx_ca.h` | CA certificate สำหรับ MQTT/TLS |
| `src/main.cpp` | เริ่มต้นอุปกรณ์และเรียก `update()` ใน loop |
| `src/devices/` | Driver ของ LCD, Keypad, HX711, VL53L1X และ Printer |
| `src/workflows/OnlineQueueWorkflow.cpp` | State machine, Wi-Fi, MQTT, OTP, Measurement, Retry และ Print |
| `src/tools/` | Firmware สำหรับสแกน I2C, คาลิเบรต และทดสอบอุปกรณ์เฉพาะตัว |
| `README_TH.md` | คู่มือเดิมของโปรเจกต์ |
| `MQTT_TEST_GUIDE_TH.md` | ขั้นตอนทดสอบ MQTT กับ Backend/MQTTX |

Backend ไม่ได้อยู่ในโฟลเดอร์ Firmware นี้ ต้องเตรียม Backend/Test environment แยกต่างหาก

## 3. สิ่งที่ต้องเตรียม

- ESP32-S3 DevKitC-1 หรือบอร์ดที่รองรับการตั้งค่า ESP32-S3 N16R8
- PlatformIO ใน VS Code หรือ PlatformIO CLI
- สาย USB สำหรับ Upload และ Serial Monitor
- Wi-Fi ที่ ESP32 ออก Internet ได้
- EMQX Cloud และบัญชี MQTT ที่ใช้กับอุปกรณ์
- Backend ที่ Subscribe Topic ของอุปกรณ์
- อุปกรณ์จริงตามตาราง Wiring ด้านล่าง หากต้องการทดสอบ Hardware

### ซอฟต์แวร์ที่ใช้

`platformio.ini` จะติดตั้ง Library หลักให้อัตโนมัติ เช่น MQTT, ArduinoJson, HX711, NeoPixel และ LiquidCrystal_I2C เมื่อ Build ครั้งแรก

## 4. ขั้นตอนติดตั้ง Firmware

### 4.1 เตรียมค่าลับ

คัดลอกไฟล์ตัวอย่าง:

```powershell
Copy-Item include/secrets.example.h include/secrets.h
```

แก้ไข `include/secrets.h`:

```cpp
namespace Secrets {
constexpr char WIFI_SSID[] = "ชื่อ Wi-Fi";
constexpr char WIFI_PASSWORD[] = "รหัส Wi-Fi";
constexpr char MQTT_PASSWORD[] = "รหัส MQTT";
}
```

ห้าม Commit หรือ Push `include/secrets.h` ขึ้น Git เพราะอาจมีรหัสผ่านจริง

### 4.2 ตรวจค่าการเชื่อมต่อ MQTT

ค่าปัจจุบันอยู่ใน `include/config.h`

```text
Broker   = hf391a51.ala.asia-southeast1.emqxsl.com
Port     = 8883
Username = scale_001
Device   = SCALE-001
Prefix   = clinic/v1
```

ระบบใช้ TLS และ CA ใน `include/emqx_ca.h` ไม่ควรเปลี่ยนเป็น `setInsecure()`

### 4.3 เลือก Environment

| Environment | การใช้งาน |
|---|---|
| `esp32-s3-hardware-test` | โหมดใช้งานกับอุปกรณ์จริงทั้งหมด และเป็นค่าเริ่มต้นของโปรเจกต์ |
| `esp32-s3-n16r8` | โหมดทดสอบผ่าน Serial โดยปิด LCD, Keypad, HX711, VL53L1X และ Printer |
| `esp32-s3-hardware-test-no-lcd` | ทดสอบอุปกรณ์จริงโดยปิด LCD |
| `esp32-s3-height-wide-test` | ทดสอบ Height Sensor ด้วย ROI 16x16 |
| `esp32-s3-walkin-retry-test` | ทดสอบ Retry/Deduplicate ของคิว B |
| `esp32-s3-online-retry-test` | ทดสอบ Retry/Deduplicate ของคิว A |
| `esp32-s3-i2c-scanner` | สแกน I2C เท่านั้น |
| `esp32-s3-keypad-direct-test` | ทดสอบ Keypad Matrix แบบ Direct GPIO |
| `esp32-s3-hx711-calibration` | คาลิเบรต Load Cell |
| `esp32-s3-vl53l1x-calibration` | คาลิเบรต VL53L1X |
| `esp32-s3-position-guide` | ช่วยตรวจท่ายืนและตำแหน่งศีรษะ |
| `esp32-s3-lcd-static-test` | ทดสอบ LCD แบบแยกส่วน |

### 4.4 Build และ Upload

โหมดอุปกรณ์จริง:

```powershell
pio run -e esp32-s3-hardware-test -t upload
pio device monitor -e esp32-s3-hardware-test
```

โหมดจำลองผ่าน Serial:

```powershell
pio run -e esp32-s3-n16r8 -t upload
pio device monitor -e esp32-s3-n16r8
```

ตั้ง Serial Monitor เป็น:

- Baud rate: `115200`
- Line Ending: `Newline`

ก่อน Upload ต้องปิด Serial Monitor เดิมก่อน เพื่อไม่ให้ COM Port ถูกใช้งานค้าง

## 5. การต่ออุปกรณ์

ปิดไฟ ESP32 และแหล่งจ่ายของอุปกรณ์ก่อนต่อหรือถอดสายทุกครั้ง ใช้ GND ร่วมกัน และตรวจสอบระดับแรงดันให้เหมาะกับ 3.3V

### 5.1 GPIO และ Bus ตามโค้ดปัจจุบัน

| อุปกรณ์/สัญญาณ | ESP32-S3 |
|---|---:|
| VL53L1X SDA (Wire0) | GPIO8 |
| VL53L1X SCL (Wire0) | GPIO9 |
| VL53L1X XSHUT | GPIO3 |
| LCD SDA (Wire1) | GPIO1 |
| LCD SCL (Wire1) | GPIO2 |
| LCD I2C Address | `0x27` |
| HX711 DT/DOUT | GPIO4 |
| HX711 SCK | GPIO5 |
| Status RGB LED | GPIO48 โดยทั่วไป |

LCD ถูกแยกไปอยู่บน I2C Controller อีกชุดที่ GPIO1/GPIO2 เพื่อไม่ให้การสื่อสารกับ VL53L1X รบกวนกัน

### 5.2 Keypad 4x4

Environment `esp32-s3-hardware-test` ตั้งค่า `CLINIC_KEYPAD_DIRECT_GPIO=1` ดังนั้นการต่อปัจจุบันเป็นแบบ Direct GPIO:

| กลุ่ม | GPIO |
|---|---|
| Row 1-4 | GPIO6, GPIO7, GPIO10, GPIO11 |
| Column 1-4 | GPIO12, GPIO13, GPIO14, GPIO15 |

หากต้องการใช้ PCF8574 ให้ปิด Direct GPIO flag และต่อ:

- SDA/SCL ตาม I2C Bus ของ Keypad
- Address ปกติ `0x20` เมื่อ A0/A1/A2 เป็น 0
- ห้ามตั้ง Address ชนกับ LCD `0x27`

### 5.3 HX711 และ Load Cell

- HX711 `DT/DOUT` → GPIO4
- HX711 `SCK` → GPIO5
- VCC → 3.3V ตามโมดูลที่ใช้งาน
- GND → GND
- ต่อ Load Cell เข้าขั้ว `E+`, `E-`, `A+`, `A-` ตามป้ายบนบอร์ด HX711

### 5.4 LCD และ VL53L1X

- LCD 20x4 I2C Address `0x27`, SDA=GPIO1, SCL=GPIO2
- VL53L1X ใช้ Address มาตรฐานของไลบรารี, SDA=GPIO8, SCL=GPIO9, XSHUT=GPIO3
- VL53L1X ใช้ไฟและระดับสัญญาณที่ปลอดภัยกับ 3.3V
- หาก LCD Backpack มี Pull-up ขึ้น 5V ต้องใช้ Level Shifter หรือย้าย Pull-up ให้เหมาะกับ 3.3V

### 5.5 USB Printer

โปรเจกต์ตรวจหาเครื่องพิมพ์ด้วย:

```text
VID = 0x0FE6
PID = 0x811E
```

ใช้ USB Host/OTG ของ ESP32-S3 และควรใช้แหล่งจ่ายของเครื่องพิมพ์เอง ห้ามดึงไฟมอเตอร์เครื่องพิมพ์จากขา 3.3V/5V ของ ESP32

## 6. ค่าที่ต้องคาลิเบรตก่อนใช้งานจริง

ค่าปัจจุบันใน `config.h` เป็นค่าของชุดติดตั้งปัจจุบัน แต่ควรตรวจสอบกับ Hardware จริงทุกครั้ง

```cpp
Config::LoadCell::CALIBRATION_FACTOR       // ปัจจุบัน 10716.723
Config::HeightSensor::SENSOR_MOUNT_HEIGHT_CM // ปัจจุบัน 197.3
Config::HeightSensor::HEIGHT_OFFSET_CM       // ปัจจุบัน 0.0
```

### 6.1 คาลิเบรต HX711

```powershell
pio run -e esp32-s3-hx711-calibration -t upload
pio device monitor -e esp32-s3-hx711-calibration
```

ใน Serial Monitor:

1. เอาของออกจากแท่นชั่ง แล้วส่ง `Z`
2. วางน้ำหนักอ้างอิงที่ทราบค่า เช่น 20 kg แล้วส่ง `W 20`
3. นำค่า `CALIBRATION_FACTOR` ที่ได้ไปใส่ใน `include/config.h`
4. Build และ Upload โหมดใช้งานจริงอีกครั้ง

### 6.2 ตั้งค่าความสูงและตรวจ VL53L1X

ตั้ง `SENSOR_MOUNT_HEIGHT_CM` เป็นระยะจากพื้นถึงตำแหน่งเซนเซอร์จริง จากนั้นใช้:

```powershell
pio run -e esp32-s3-vl53l1x-calibration -t upload
pio device monitor -e esp32-s3-vl53l1x-calibration
```

หรือใช้คำสั่งใน Firmware ปกติ:

```text
HEIGHTTEST
HEIGHTTEST12
HEIGHTTESTFIXED
HEIGHTTESTCAL 171
```

ผู้ถูกวัดควรยืนตรง อยู่กึ่งกลางเซนเซอร์ วางแขนข้างลำตัว และอยู่นิ่งระหว่างการอ่านค่า

## 7. ลำดับการเริ่มต้นระบบ

เมื่อเปิดเครื่อง `main.cpp` ทำงานตามลำดับนี้:

```text
Serial.begin(115200)
  ↓
เริ่ม VL53L1X
  ↓
เริ่ม LCD และแสดง Startup Screen
  ↓
เริ่ม Keypad
  ↓
เริ่ม HX711
  ↓
เริ่ม USB Host และรอ Printer
  ↓
เริ่ม Wi-Fi
  ↓
รอเวลา NTP
  ↓
เชื่อม MQTT/TLS และ Subscribe Topics
  ↓
แสดง CLINIC QUEUE READY
```

ระบบจะพยายาม Reconnect Wi-Fi และ MQTT อัตโนมัติเมื่อการเชื่อมต่อหลุด

## 8. Flow การทำงานคิว A: Online Queue

### 8.1 ผ่าน Keypad และวัดจาก Sensor จริง

```text
Idle
  ↓ กด A
EnteringOtp
  ↓ กรอก OTP 6 หลัก และกด #
ส่ง /otp-verify
  ↓
รอ /otp-result
  ├─ rejected → แสดงเหตุผล และกลับหน้าเริ่มต้น
  └─ accepted → ได้ measurement_session_id
                       ↓
                 ReadyForMeasurement
                       ↓ กด B
                 นับถอยหลัง 3 วินาที
                       ↓
                 อ่านน้ำหนักจาก HX711
                       ↓
                 อ่านระยะจาก VL53L1X และคำนวณส่วนสูง
                       ↓
                 ตรวจความเสถียรของน้ำหนัก/ส่วนสูง
                       ↓
                 ส่ง /measurements mode=online
                       ↓
                 รอ /measurement-ack
                       ↓
                 ได้เลขคิว A และสถานะบันทึก
                       ↓
                 รอ /print หาก Backend ระบุ print_pending=true
```

การอ่านจริงจะตรวจน้ำหนักก่อนและหลังอ่านส่วนสูง หากน้ำหนักเปลี่ยนมากเกินค่าที่กำหนด ระบบจะไม่ส่งคิว เพื่อป้องกันการวัดขณะผู้ใช้ขยับตัว

### 8.2 ทดสอบด้วยค่าจำลอง

```text
T 123456 60 170
```

คำสั่งนี้จะตรวจ OTP แล้วส่งน้ำหนัก 60 kg และส่วนสูง 170 cm อัตโนมัติ โดยไม่ต้องต่อ Sensor

แบบแยกขั้นตอน:

```text
A 123456
M 60 170
```

หรือใช้คำสั่งภาษาไทย:

```text
คิวเอ 123456
วัด 60 170
```

Firmware ไม่คำนวณหรือส่ง BMI ใน Measurement; Backend เป็นผู้คำนวณ BMI และส่งค่ากลับมาใน Print Job

## 9. Flow การทำงานคิว B: Walk-in

คิว B ไม่ต้องใช้ OTP และเริ่มได้เมื่อระบบอยู่สถานะ Idle

```text
Idle
  ↓ กด B หรือส่ง W 60 170
อ่าน/รับค่าน้ำหนักและส่วนสูง
  ↓
สร้าง message_id แบบ MSG-B
  ↓
ส่ง /measurements mode=walk_in
  ↓
รอ /measurement-ack
  ↓
ตรวจว่า queue_number ขึ้นต้นด้วย B
  ↓
แสดงและรอ Print Job
```

ตัวอย่าง:

```text
W 60 170
คิวบี 60 170
```

## 10. MQTT Topic และ Flow การรับ-ส่ง

Base Topic ปัจจุบันคือ:

```text
clinic/v1/devices/SCALE-001
```

| ทิศทาง | Topic | หน้าที่ |
|---|---|---|
| ESP32 → Backend | `/otp-verify` | ขอให้ Backend ตรวจ OTP |
| Backend → ESP32 | `/otp-result` | ตอบ OTP accepted/rejected และ Session |
| ESP32 → Backend | `/measurements` | ส่งน้ำหนักและส่วนสูง |
| Backend → ESP32 | `/measurement-ack` | ยืนยันการบันทึกและส่งเลขคิว |
| Backend → ESP32 | `/print` | ส่งข้อมูลบัตรคิวและ BMI ให้พิมพ์ |
| ESP32 → Backend | `/print-ack` | แจ้ง printed หรือ failed |

MQTTX สำหรับดูข้อความให้ Subscribe:

```text
clinic/v1/devices/SCALE-001/#
```

ควรใช้ Client ID ที่ไม่ซ้ำกับ Hardware และระหว่างทดสอบให้ MQTTX ดูข้อความอย่างเดียว ไม่ Publish แทรก

### 10.1 ข้อมูล OTP

ESP32 ส่งข้อมูลลักษณะนี้ไป `/otp-verify`:

```json
{
  "schema_version": "1.0",
  "request_id": "REQ-...",
  "device_id": "SCALE-001",
  "otp": "123456"
}
```

เมื่อผ่าน Backend ควรตอบ `/otp-result` พร้อมข้อมูลสำคัญ:

```json
{
  "request_id": "REQ-...",
  "status": "accepted",
  "measurement_session_id": "SESSION-...",
  "queue_number": "A001",
  "expires_at": "..."
}
```

ถ้า OTP ไม่ผ่าน จะเป็น `status=rejected` พร้อม `error_code` เช่น `INVALID_OTP` และ ESP32 จะไม่ส่ง Measurement ต่อ

### 10.2 ข้อมูล Measurement

คิว A:

```json
{
  "schema_version": "1.0",
  "message_id": "MSG-A-...",
  "device_id": "SCALE-001",
  "mode": "online",
  "measurement_session_id": "SESSION-...",
  "measured_at": "2026-01-01T00:00:00Z",
  "weight": 60.0,
  "height": 170.0
}
```

คิว B จะเหมือนกัน แต่ใช้ `mode=walk_in` และไม่มี `measurement_session_id`

Backend ตอบ `/measurement-ack` พร้อม `message_id` เดิม, `measurement_id`, `queue_number` และอาจมี `print_pending=true`

## 11. Flow การพิมพ์บัตรคิว

```text
Backend ส่ง /print
  ↓
ESP32 ตรวจ schema, print_job_id, queue, W/H/BMI
  ↓
ตรวจ Printer VID/PID และสถานะ Busy
  ├─ ไม่พร้อม → print-ack status=failed
  └─ พร้อม → ส่ง ESC/POS ผ่าน USB Host
                    ↓
              รอ USB Transfer
              ├─ สำเร็จ → print-ack status=printed
              └─ ล้มเหลว/Timeout → print-ack status=failed
```

Firmware เก็บประวัติ `print_job_id` ล่าสุด 16 รายการใน NVS เพื่อป้องกันการพิมพ์ซ้ำเมื่อ Backend ส่งงานเดิมซ้ำ

ถ้า Backend ตอบ `print_pending=true` แต่ไม่มี `/print` ภายใน 30 วินาที ให้ตรวจ Backend Print Outbox

## 12. Retry และป้องกันข้อมูลซ้ำ

เมื่อส่ง Measurement แล้วไม่ได้รับ ACK:

1. Firmware เก็บ Payload เดิมและ `message_id` เดิมไว้
2. ส่งซ้ำเมื่อ MQTT กลับมา หรือเมื่อครบเวลา Retry
3. ระยะ Retry เพิ่มเป็นประมาณ 10, 20, 40 และสูงสุด 60 วินาที
4. Backend ต้องใช้ `message_id` ทำ Deduplicate
5. ผลลัพธ์ที่ถูกต้องคือฐานข้อมูลมีเพียงหนึ่งรายการ แม้จะได้รับ Payload ซ้ำ

Pending Measurement และ Pending Print ACK ถูกเก็บใน NVS เพื่อให้ทำงานต่อได้หลัง ESP32 Restart

ทดสอบคิว B:

```powershell
pio run -e esp32-s3-walkin-retry-test -t upload
pio device monitor -e esp32-s3-walkin-retry-test
```

จากนั้นส่ง:

```text
WRETRY 66 174
```

ทดสอบคิว A:

```powershell
pio run -e esp32-s3-online-retry-test -t upload
pio device monitor -e esp32-s3-online-retry-test
```

จากนั้นส่ง:

```text
ARETRY 123456 60 170
```

เปลี่ยน `123456` เป็น OTP จริง และหลังทดสอบต้อง Upload กลับเป็น Environment ปกติ

## 13. คำสั่ง Serial ที่ใช้บ่อย

| คำสั่ง | หน้าที่ |
|---|---|
| `ช่วยเหลือ`, `HELP`, `?` | แสดงคำสั่งทั้งหมด |
| `สถานะ`, `STATUS` | แสดง Wi-Fi, MQTT, IP และ State |
| `A 123456` | ส่ง OTP คิว A |
| `M 60 170` หรือ `วัด 60 170` | ส่งค่าวัดจำลองหลัง OTP ผ่าน |
| `T 123456 60 170` | ตรวจ OTP และส่งค่าวัดจำลองอัตโนมัติ |
| `W 60 170` | ส่งค่าวัดจำลองคิว B |
| `A123456#` | จำลองการกด Keypad คิว A |
| `PRINTTEST` หรือ `พิมพ์ทดสอบ` | พิมพ์กระดาษทดสอบ |
| `C` | ลบเลข OTP ตัวล่าสุด |
| `D`, `*`, `ยกเลิก` | ยกเลิก Flow ใน ESP32 และกลับหน้าเริ่มต้น |
| `LCDRESET` | Reinitialize LCD |
| `HEIGHTTEST` | ทดสอบส่วนสูงโดยไม่สร้างคิว |
| `HEIGHTTESTCAL 171` | ทดสอบหลายรอบเทียบกับส่วนสูงอ้างอิง |

## 14. ขั้นตอนทดสอบระบบครบเส้นทาง

1. เปิด Backend/Test environment และให้ Backend Subscribe `clinic/v1/devices/+`
2. ตรวจ `include/secrets.h`
3. ต่อ ESP32 และ Upload `esp32-s3-hardware-test`
4. เปิด Serial Monitor ที่ 115200 และรอ Wi-Fi, NTP และ MQTT พร้อม
5. เปิด MQTTX และ Subscribe `clinic/v1/devices/SCALE-001/#`
6. สร้างผู้ป่วยและ OTP ใหม่จาก Test Patient Tool ของ Backend
7. ทดสอบค่าจำลองก่อนด้วย `T <OTP> 60 170`
8. ตรวจลำดับ MQTT: `otp-verify` → `otp-result` → `measurements` → `measurement-ack`
9. เมื่อค่าจำลองผ่านแล้ว จึงทดสอบ Sensor จริงด้วย `A` → OTP → `B`
10. ตรวจ Print Job และ `print-ack=printed`

## 15. Troubleshooting

| อาการ | จุดตรวจ |
|---|---|
| Build ไม่ผ่านเพราะ `secrets.h` | คัดลอก `secrets.example.h` เป็น `secrets.h` |
| Wi-Fi ไม่ต่อ | SSID/password, ระยะสัญญาณ และไฟเลี้ยง |
| MQTT ไม่ต่อ | Wi-Fi, NTP, Broker, port 8883, username/password และ CA |
| ไม่เห็น MQTTX | ใช้ Topic และ Client ID ถูกต้องหรือไม่ |
| เห็น `otp-verify` แต่ไม่มี `otp-result` | Backend ไม่ได้ Subscribe หรือไม่ตอบกลับ |
| OTP ผ่านแต่ไม่ส่ง Measurement | ตรวจ Session, Session หมดอายุ หรือยังมี Pending เดิม |
| HX711 ไม่พร้อม | VCC/GND, DT GPIO4, SCK GPIO5 และ Load Cell wiring |
| น้ำหนักไม่ตรง | คาลิเบรต `CALIBRATION_FACTOR` ใหม่ |
| VL53L1X อ่านไม่ได้ | SDA GPIO8, SCL GPIO9, XSHUT GPIO3, ไฟ 3.3V และตำแหน่งเป้าหมาย |
| LCD ไม่ขึ้น | SDA GPIO1, SCL GPIO2, Address 0x27 และระดับแรงดัน |
| Keypad ไม่ตอบสนอง | ตรวจว่าใช้ Direct GPIO ตาม Environment หรือ PCF8574 ตามโหมดที่เลือก |
| Printer ไม่พบ | แหล่งจ่ายภายนอก, USB OTG/Host และ VID/PID |
| ได้ ACK ซ้ำ | เป็นพฤติกรรม Retry ที่คาดไว้ ต้องตรวจว่า Backend Deduplicate ด้วย `message_id` |
| ได้ ACK แต่ไม่พิมพ์ | ตรวจ `/print`, Printer readiness, Print Outbox และ `print-ack` |

## 16. ข้อควรระวังในการ Push ขึ้น Git

ไฟล์ต่อไปนี้ถูก Ignore และไม่ควรบังคับเพิ่มเข้า Git:

```text
.pio/
.vscode/...
include/secrets.h
ESP32S3_Clinic_Modular_backup_*.zip
```

ควร Commit `include/secrets.example.h` และเอกสารนี้แทนไฟล์ที่มี Password จริง
