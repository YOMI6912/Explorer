# รายงานรวมโปรเจคเครื่องชั่งคลินิก ESP32-S3 + ESP32-C3

วันที่จัดทำ: 10 ตุลาคม 2026  
สถานะ: ระบบวัดส่วนสูงผ่านลิงก์ไร้สายยังใช้งานจริงไม่ได้ แต่ระบบ AP ของ S3 และการอ่านเซนเซอร์ของ C3 ยังทำงานแยกส่วนได้

เอกสารนี้รวบรวมโครงสร้างโปรเจค การต่อสาย เฟิร์มแวร์ การคาลิเบรต การทดสอบ ปัญหาที่พบ และสถานะล่าสุดจาก Serial Monitor

---

## 1. ภาพรวมระบบ

ระบบถูกแยกเป็น 2 บอร์ด:

| บอร์ด | หน้าที่ |
|---|---|
| ESP32-S3 | เครื่องคลินิกหลัก: LCD, keypad, HX711, เครื่องพิมพ์ และ TCP server |
| ESP32-C3 | อ่าน VL53L1X ที่อยู่ใกล้เซนเซอร์ แล้วส่งผลส่วนสูงกลับ S3 |

โฟลว์ที่ตั้งใจไว้:

```text
คนขึ้นแท่น
    -> S3 อ่านน้ำหนัก
    -> S3 ส่ง TCP request ไป C3
    -> C3 อ่าน VL53L1X หลายรอบและตรวจความเสถียร
    -> C3 ส่งผลดิบ/ผลคาลิเบรตกลับ S3
    -> S3 ตรวจคุณภาพ
    -> ผ่าน: คำนวณ BMI และพิมพ์ใบเสร็จ
    -> ไม่ผ่าน: ขึ้น MEASURE FAILED และไม่พิมพ์
```

ระบบออกแบบให้ปฏิเสธค่าที่น่าสงสัย ไม่พิมพ์ค่าที่อาจผิด แม้ผลคือใช้งานยากขึ้นเมื่อเซนเซอร์หรือลิงก์ไม่พร้อม

---

## 2. โครงสร้างไฟล์

### 2.1 โปรเจค S3

โฟลเดอร์หลัก:

```text
C:\Users\ACER\Downloads\ESP32S3_Clinic_Modular_v1\ESP32S3_Clinic_Modular
```

ไฟล์สำคัญ:

| ไฟล์ | หน้าที่ |
|---|---|
| `platformio.ini` | environment และ build flags ของ S3 |
| `include/config.h` | pin, ค่าเซนเซอร์, AP, timeout, เกณฑ์ยอมรับผล |
| `src/main.cpp` | ลำดับเริ่มระบบ, keypad B, workflow และ loop |
| `src/devices/RemoteHeightLink.cpp` | Wi-Fi AP และ TCP server สำหรับ C3 |
| `src/devices/VL53L1XDevice.cpp` | ไดรเวอร์ VL53 ฝั่ง S3/โหมดท้องถิ่น |
| `src/devices/HX711Device.cpp` | อ่านน้ำหนักและ tare |
| `src/devices/LcdDevice.cpp` | LCD 20x4 |
| `src/devices/KeypadDevice.cpp` | keypad 4x4 |
| `src/devices/UsbPrinter.cpp` | USB Host และเครื่องพิมพ์ |
| `src/workflows/OnlineQueueWorkflow.cpp` | MQTT/queue/online workflow เดิม |
| `include/height_link_protocol.h` | packet TCP ระหว่าง S3/C3 |
| `lib/VL53L1X_safe/` | ไลบรารี VL53 ที่มี guard และการ recovery |

### 2.2 โปรเจค C3

โฟลเดอร์แยก:

```text
C:\Users\ACER\Downloads\ESP32_C3_VL53_Remote
```

ไฟล์สำคัญ:

| ไฟล์ | หน้าที่ |
|---|---|
| `platformio.ini` | environment `esp32-c3-vl53-remote` |
| `include/remote_config.h` | pin, Wi-Fi, calibration และเกณฑ์อ่านส่วนสูง |
| `src/main.cpp` | คำสั่ง serial, measurement และตอบ request จาก S3 |
| `src/sensor_reader.cpp` | อ่าน VL53, ROI, filtering และสูตรคาลิเบรต |
| `src/wireless_link.cpp` | Wi-Fi station และ TCP client |
| `include/height_link_protocol.h` | packet แบบเดียวกับฝั่ง S3 |

โปรเจค C3 อยู่นอกโฟลเดอร์ S3 ต้องเปิดโฟลเดอร์แยกใน VS Code/PlatformIO

---

## 3. การต่อสายปัจจุบัน

### 3.1 ESP32-S3

| อุปกรณ์ | ขา |
|---|---|
| VL53L1X SDA | GPIO8 |
| VL53L1X SCL | GPIO9 |
| VL53L1X XSHUT | GPIO3 |
| VL53L1X VCC | 3.3V |
| VL53L1X GND | GND |
| LCD SDA | GPIO1 |
| LCD SCL | GPIO2 |
| LCD address | `0x27` |
| HX711 DT | GPIO4 |
| HX711 SCK | GPIO5 |
| Keypad rows | GPIO6, GPIO7, GPIO10, GPIO11 |
| Keypad columns | GPIO12, GPIO13, GPIO14, GPIO15 |
| Keypad mode | direct GPIO 4x4 |
| USB printer | VID `0x0FE6`, PID `0x811E` |

### 3.2 ESP32-C3

| อุปกรณ์ | ขา |
|---|---|
| VL53L1X SDA | GPIO4 |
| VL53L1X SCL | GPIO5 |
| VL53L1X XSHUT | GPIO3 |
| VL53L1X VCC | 3.3V |
| VL53L1X GND | GND |
| I2C clock | 50 kHz |
| ROI ผนัง/ระยะสั้น | 4x4, center SPAD 28 |
| ROI ศีรษะ | 12x12, centered |

ข้อควรระวัง:

- ต้องมี GND ร่วมระหว่าง C3 กับ VL53
- SDA/SCL ต้องมี pull-up ไป 3.3V โดยทั่วไป 4.7 kΩ ต่อเส้น
- เคยเพิ่ม capacitor 100 nF และ 10 µF ระหว่าง VCC-GND ของโมดูลแล้ว
- ห้ามป้อน 5V เข้าขา I2C หรือขา logic ของเซนเซอร์
- ปลอกทึบของ VL53 ช่วยลดแสงรบกวนได้ แต่ปากปลอกต้องไม่บังช่องมอง

---

## 4. ค่าเฟิร์มแวร์ที่สำคัญ

### 4.1 S3 environment ปัจจุบัน

ใช้:

```text
esp32-s3-hardware-test
```

เปิดใช้งาน:

```text
CLINIC_ENABLE_LCD=1
CLINIC_ENABLE_KEYPAD=1
CLINIC_ENABLE_LOAD_CELL=1
CLINIC_ENABLE_HEIGHT_SENSOR=1
CLINIC_ENABLE_USB_PRINTER=1
CLINIC_REMOTE_HEIGHT=1
```

ค่าที่เกี่ยวข้อง:

```text
CLINIC_OFFLINE_ONLY=1
CLINIC_B_DISTANCE_TEST=0
AP SSID       = Clinic-Height-Link
AP IP         = 192.168.4.1
TCP port      = 45831
AP channel    = 1
mount height  = 197.0 cm (ฝั่ง remote protocol)
```

`CLINIC_OFFLINE_ONLY=1` หมายความว่า workflow MQTT/queue ออนไลน์เดิมถูกปิด ระบบใช้การวัดในเครื่องและเครื่องพิมพ์โดยตรง แม้ AP/TCP สำหรับ C3 จะเปิดอยู่

### 4.2 C3 profile

```text
SSID          = Clinic-Height-Link
TCP server    = 192.168.4.1:45831
I2C           = 50 kHz
wall ROI      = 4x4, center 28
head ROI      = 12x12, short mode
head confirms = 3 independent reads
max spread    = 5 mm สำหรับ HEADAUTO
raw head      = 155–430 mm
```

เฟิร์มแวร์ C3 รุ่นล่าสุดที่อัปโหลดมีการแก้ดังนี้:

- ไม่ล็อก BSSID เดิมของ S3
- สแกนหา SSID และอ่าน BSSID/channel จริง
- reset วิทยุ Wi-Fi ก่อน association
- รอ handshake สูงสุด 15 วินาที
- ไม่สแกนแทรกระหว่าง handshake
- แสดง Wi-Fi disconnect reason

---

## 5. โปรโตคอล S3-C3

ใช้ TCP แบบ binary protocol รุ่น 2:

```text
Request  = 12 bytes
Response = 30 bytes
MAGIC    = 0x31474848
VERSION  = 2
REQUEST  = 1
RESPONSE = 2
```

สถานะ response:

| status | ความหมาย |
|---:|---|
| 0 | OK |
| 1 | SENSOR_OFFLINE |
| 2 | I2C_ERROR |
| 3 | NO_STABLE_TARGET |
| 4 | OUTSIDE_CALIBRATION |

S3 จะรับผลเฉพาะเมื่อ:

- status เป็น OK
- calibration_valid เป็น 1
- retries เป็น 0
- valid frames ถึงเกณฑ์
- span ไม่เกิน 8 mm
- raw distance อยู่ในช่วง 155–430 mm
- สูตรคาดการณ์ภายในคลาดเคลื่อนไม่เกิน 0.16 cm

หากไม่ผ่านจะไม่พิมพ์ใบเสร็จ

---

## 6. งานแก้เสถียรภาพ VL53L1X ที่ทำแล้ว

### 6.1 I2C และ hardware

- ตรวจขา GPIO หลายชุดแล้วพบว่าชุดที่พบ address ได้คือ SDA=GPIO8, SCL=GPIO9 บนชุดเดิม
- เพิ่ม pull-up 4.7 kΩ ให้ SDA/SCL
- เพิ่ม capacitor 100 nF + 10 µF ที่ VCC-GND เซนเซอร์
- ตรวจ VCC ขณะวัด ได้ประมาณ 3.24 V และไม่พบการตกชัดเจน
- ใช้ XSHUT GPIO3 เพื่อ reset โมดูล
- ลด I2C clock จาก 50 kHz เป็น 25 kHz แล้ว แต่ยังพบ init/ranging fail จึงไม่ได้แก้ปัญหาหลัก
- พบว่าปลอกทึบล้อมเซนเซอร์ช่วยให้ค่าดีขึ้น โดยต้องไม่บังปากเลนส์

### 6.2 software filtering/recovery

- เปลี่ยนเป็น single-shot เพื่อหลีกเลี่ยงเฟรมค้างจาก continuous stream
- รอ data-ready และมี timeout ทุกครั้ง
- ทิ้งเฟรมเมื่อ I2C error
- reset XSHUT เมื่อ init/ranging ล้มเหลว
- ต้องมีหลายเฟรมอยู่ใน cluster เดียวกันก่อนรับผล
- reject เมื่อ I2C retry มากกว่า 3
- reject เมื่อ sample spread มากเกินกำหนด
- แยก diagnostic ROI ออกจาก ROI ที่ใช้กับงานศีรษะ
- เพิ่ม HEADAUTO/HEADSERIES สำหรับตรวจ repeatability โดยไม่ส่งผลไปพิมพ์

ผลที่ได้คือซอฟต์แวร์ป้องกันค่าผิดได้ดีขึ้น แต่ไม่สามารถแก้ optical target ผิด, สายยาว, bus noise หรือวิทยุเสียได้

---

## 7. ผลการทดสอบระยะและคาลิเบรต

### 7.1 ผล wall target ที่มีบันทึก

| ระยะจริง | ผลที่อ่าน/คำนวณ | ข้อสังเกต |
|---:|---:|---|
| 26.3 cm | 31.2 cm | error ประมาณ +4.9 cm |
| 39.3 cm | 45.4 cm | error ประมาณ +6.1 cm |
| 50 cm | ประมาณ 47.5–48.6 cm ในเฟรมที่นิ่ง | บางรอบ retry สูงหรือ reject |
| 200 mm | raw ประมาณ 179 mm | cluster เสถียร แต่มี offset |
| 300 mm | raw ประมาณ 216 mm ในชุดแรก | ผิดมากและไม่ควรใช้คาลิเบรตชุดนั้น |
| 300 mm | raw 276–278 mm ใน ROI 16x16 | ดีขึ้นเมื่อมีเป้า/ปลอกเหมาะสม |
| 400 mm | ROI 4x4 raw 362 mm | cluster span 4 mm แต่มี bias |
| 400 mm | ROI 12x12 raw 390 mm | เสถียรกว่า ROI 4x4 ในบางตำแหน่ง |

สูตร wall ที่อยู่ใน C3 ปัจจุบันเป็นสูตรเชิงประจักษ์สำหรับ ROI 4x4 center 28:

```text
corrected_distance_mm = raw_mm * 1.06824 - 21.32
valid raw range       = 250–450 mm
```

สูตรนี้ไม่ควรนำไปใช้กับศีรษะหรือช่วงระยะที่อยู่นอกชุดทดสอบ

### 7.2 ปัญหา optical/ROI

ผล DIAGROIMAP ที่เป้า 400 mm แสดงว่า center SPAD แต่ละตำแหน่งให้ค่าต่างกันมาก เช่นประมาณ 359–417 mm ในการวัดคนละรอบ แปลว่าแนวแสง/ตำแหน่งเป้ามีผลสูง ไม่ใช่ noise แบบสุ่มอย่างเดียว

จึงไม่ควรคาลิเบรตจากจุดเดียวหรือจากค่าที่ cluster แคบแต่ชี้ไปยังไหล่/พื้นหลัง

---

## 8. ผลคาลิเบรตส่วนสูงคนจริง

เงื่อนไขที่ใช้กับชุดล่าสุด:

- วัดจากหน้าเลนส์เซนเซอร์ถึงพื้นแท่นจริงประมาณ 197 cm
- ผู้ทดสอบใช้แผ่นวัดศีรษะหนา 4 mm แบบเดียวกัน
- ไม่มีการหัก 1.5 cm เพิ่ม เพราะระยะ 197 cm วัดจากหน้าเลนส์แล้ว
- ใช้ Short mode และ ROI 12x12 สำหรับ profile ศีรษะ
- สูตรรวมที่ fit จากข้อมูลคนสูง 158, 171 และ 183.2 cm คือ:

```text
height_cm = 201.44257 - (0.11148977 * raw_distance_mm)
```

สูตรนี้รวมผลของแผ่น 4 mm แล้ว ห้ามลบความหนาแผ่นซ้ำ

ข้อจำกัด:

- ได้จากผู้ทดสอบจำนวนน้อยและท่าทางจริงไม่เหมือนกันทุกครั้ง
- ค่าบางรอบจับหน้าผาก/ไหล่/พื้นหลัง ทำให้ raw เปลี่ยนหลายเซนติเมตร
- ความเสถียรของกลุ่มตัวอย่างไม่ได้รับประกันความถูกต้องเชิง absolute
- ยังไม่สามารถรับประกันความแม่น ±0.5 cm
- ต้องคงตำแหน่งเซนเซอร์ ความสูง 197 cm ปลอก และแผ่นวัดให้เหมือนชุดคาลิเบรต

ตัวอย่าง HEADAUTO ที่เคยได้:

```text
คนสูง 171: raw median 270 mm -> ประมาณ 174.2 cm (trial)
คนสูง 158: raw median 404 mm -> ประมาณ 160.8 cm (trial)
คนสูง 171: raw median 295 mm -> ประมาณ 171.7 cm (trial)
คนสูง 171: raw median 274 mm -> ประมาณ 173.8 cm (trial)
```

ค่าที่แกว่งเหล่านี้เป็นเหตุผลที่ระบบต้องปฏิเสธผลที่ไม่ตรงกัน และยังไม่ควรเรียกว่าคาลิเบรตเสร็จสมบูรณ์

---

## 9. คำสั่ง Serial ที่ใช้บ่อยบน C3

เปิด COM13 ที่ 115200 baud แล้วใช้คำสั่งเหล่านี้:

| คำสั่ง | หน้าที่ |
|---|---|
| `?` | แสดง help |
| `TARGET 300` | ตั้งเป้าหมาย 300 mm และล้าง sample |
| `SAMPLE` | อ่านและเก็บ 1 sample |
| `SUMMARY` | สรุปค่าเฉลี่ย/min/max |
| `CLEAR` | ล้าง sample โดยคง target |
| `MEASURE` | อ่านหนึ่งครั้งโดยไม่เก็บ |
| `DIAG` | แสดงทุก raw frame และเหตุผล reject |
| `DIAGCONT` | ทดสอบ continuous 16 เฟรม |
| `DIAGROI4` | ทดสอบ ROI 4x4 |
| `DIAGROI12` | ทดสอบ ROI 12x12 |
| `DIAGROI14` | ทดสอบ ROI 14x14 |
| `DIAGROI16` | ทดสอบ ROI 16x16 |
| `DIAGROIMAP` | สแกนตำแหน่ง center SPAD 3x3 |
| `HEADAUTO` | ยืนยัน raw 3 รอบภายใน 5 mm; ไม่ส่งไร้สาย/ไม่พิมพ์ |
| `HEADSERIES` | ทำ HEADAUTO 5 รอบ; ไม่ส่งไร้สาย/ไม่พิมพ์ |
| `HEADMEASURE` | อ่าน profile ศีรษะสำหรับทดสอบ serial |
| `HEADTEST` | อ่านศีรษะ 5 รอบแบบทดลอง |
| `HEADTEST15` | หน่วง 15 วินาทีก่อน HEADTEST |

การทดสอบ head ต้องยืนนิ่ง ใช้แผ่นแบบเดิม และไม่ขยับเซนเซอร์

---

## 10. คำสั่งทดสอบ S3

เปิด COM19 ที่ 115200 baud:

```text
REMOTEHEIGHT
```

คำสั่งนี้ควรใช้ตรวจ TCP ก่อนกด B และไม่ควรพิมพ์ใบเสร็จ หากไม่มี C3 จะรายงาน:

```text
[REMOTE] C3 is not connected to private TCP link; no print
[REMOTEHEIGHT] FAILED
```

เมื่อระบบพร้อมควรเห็น:

```text
[REMOTE] C3 TCP connected from 192.168.4.x
[REMOTE] TCP request id=... sent; waiting for C3
[REMOTE] Accepted height ... cm over WPA2/TCP
```

จึงค่อยให้คนขึ้นแท่นและกด `B`

---

## 11. คำสั่งเปิด Serial Monitor แยก 2 หน้าต่าง

เนื่องจาก `pio` ไม่ได้อยู่ใน PATH ให้ใช้ path เต็ม:

### C3 / COM13

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" device monitor -p COM13 -b 115200
```

### S3 / COM19

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" device monitor -p COM19 -b 115200
```

ห้ามพิมพ์ `&&` หน้าคำสั่งใน PowerShell ต้องใช้ `&` เพียงตัวเดียว

ตรวจพอร์ตด้วย:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" device list
```

---

## 12. สถานะลิงก์ไร้สายล่าสุด

### สิ่งที่ยืนยันแล้ว

1. S3 แสดง:

```text
[REMOTE] WPA2/TCP server ready
SSID=Clinic-Height-Link IP=192.168.4.1 port=45831 protocol=v2
```

2. โทรศัพท์เชื่อม AP ได้จริงและได้รับ:

```text
192.168.4.2
```

จึงยืนยันว่า AP ของ S3, รหัส WPA2 และ subnet ทำงานได้

3. C3 สแกนพบ AP จริงด้วยสัญญาณแรงมาก:

```text
[LINK] Target AP found RSSI=-26 ถึง -32 dBm channel=1 auth=3
BSSID=DE:DA:0C:78:D1:90
```

### สิ่งที่ยังล้มเหลว

C3 ไม่สามารถทำ WPA2 association ให้สำเร็จ:

```text
[LINK] WiFi disconnected reason=2
[LINK] WiFi disconnected reason=201
[LINK] WiFi disconnected reason=202
[LINK] WPA2 association timed out; WiFi status=6
```

ความหมายโดยย่อ:

| reason | ความหมายโดยใช้งาน |
|---:|---|
| 2 | authentication หมดเวลา/ไม่ได้รับการตอบกลับ |
| 201 | ไม่พบ AP ในรอบนั้น |
| 202 | authentication ล้มเหลว |

C3 ไม่เคยได้:

```text
[LINK] WPA2 association accepted by S3 AP
[LINK] C3 received IP=192.168.4.x
[LINK] TCP connected to S3
```

ดังนั้น S3 ไม่ได้ทำงานผิดที่ TCP layer เพราะ C3 ยังไม่ผ่าน Wi-Fi layer ก่อน

### ข้อสรุปปัจจุบัน

มีความเป็นไปได้สูงว่าภาคส่ง Wi-Fi, เสาอากาศ, RF switch หรือไฟเลี้ยงขณะส่งของ C3 มีปัญหา เพราะ:

- C3 รับสัญญาณและ scan ได้แรงมาก
- โทรศัพท์เชื่อม S3 ได้
- C3 ล้มเหลวซ้ำที่ authentication/การส่งกลับ
- เคยมีอาการ ESP-NOW เดิมที่ C3 รับ request ได้ แต่ส่ง response กลับไม่สำเร็จ

---

## 13. ปัญหาอื่นที่พบล่าสุด

### 13.1 LCD S3 ไม่พบ address 0x27

S3 log ล่าสุดมี:

```text
[LCD] ERROR: no device at 0x27
```

นี่เป็นปัญหา LCD/I2C แยกจาก Wi-Fi link ต้องตรวจ:

- SDA LCD GPIO1
- SCL LCD GPIO2
- VCC/GND
- backpack address อาจไม่ใช่ 0x27
- pull-up และสายหลวม

ไม่ใช่สาเหตุโดยตรงของ `HEIGHT LINK FAILED`

### 13.2 VL53L1X และ I2C เดิม

เคยพบ:

```text
requestFrom(): i2cRead returned Error -1
init failed
ranging configuration failed
ranging did not start
```

แม้เพิ่ม pull-up, capacitor, reset XSHUT, single-shot และลด clock แล้ว อาการยังเกิดเป็นบางครั้ง จึงต้องคง policy ปฏิเสธผลเมื่อ retry มากหรือ quality ไม่ผ่าน

### 13.3 ค่า sensor ถูกแต่ target ผิด

บางชุดมี cluster แคบมาก แต่ค่านั้นมาจากไหล่ พื้นหลัง หรือขอบปลอก ไม่ใช่ศีรษะ ทำให้ค่าดูนิ่งแต่ส่วนสูงผิดหลายเซนติเมตร การเพิ่ม median อย่างเดียวแก้ไม่ได้ ต้องจัดแนว optical/เป้า/ROI ให้ถูก

---

## 14. สิ่งที่ทำไปแล้วตามลำดับ

1. แยกการอ่าน VL53 ออกจาก S3 ไป C3
2. สร้าง C3 PlatformIO project และ profile การอ่านแบบ Short/Long
3. ต่อ pull-up SDA/SCL และ capacitor ที่เซนเซอร์
4. ตรวจ pin mapping และยืนยัน address 0x29 บน GPIO8/GPIO9 ของชุดเดิม
5. เพิ่ม XSHUT recovery และ I2C retry policy
6. เปลี่ยนเป็น single-shot และรอ data-ready
7. เพิ่ม cluster/median/span/retry rejection
8. ทดลอง ROI 4x4, 8x8, 12x12, 14x14 และ 16x16
9. ทดลอง continuous diagnostic เพื่อแยกปัญหาจาก single-shot
10. ใช้ปลอกทึบล้อมเซนเซอร์เพื่อลดแสงรบกวน
11. เก็บค่าคนจริง 158, 171 และ 183.2 cm พร้อมแผ่น 4 mm
12. สร้างสูตรศีรษะรวมจากข้อมูลคนจริง
13. สร้าง AP WPA2/TCP บน S3
14. เปลี่ยน C3 จาก ESP-NOW มาเป็น WPA2/TCP
15. แก้ C3 ไม่ให้ล็อก BSSID เดิม
16. แก้ C3 ไม่ restart association ทุก 2 วินาที
17. เพิ่มการ reset วิทยุ, scan จริง, dynamic BSSID/channel และ Wi-Fi reason log
18. อัปโหลด C3 หลายรอบและตรวจ log
19. ยืนยันว่าโทรศัพท์ได้ IP 192.168.4.2 จาก S3 AP
20. ตรวจล่าสุดแล้วยังไม่เกิด TCP connection ระหว่าง C3/S3

---

## 15. แผนแก้ปัญหาที่แนะนำ

### ทางเลือก A: เปลี่ยน C3 (เร็วและมีโอกาสจบสูงสุด)

1. ใช้ ESP32-C3 ตัวอื่น
2. ต่อ VL53 ตาม pin เดิม
3. อัปโหลดเฟิร์มแวร์ C3 ล่าสุด
4. เปิด COM13 และดูว่ามี `WPA2 association accepted` หรือไม่
5. ถ้าเชื่อมแล้วจึงทดสอบ `REMOTEHEIGHT` บน S3

### ทางเลือก B: ทดสอบ C3 โดยไม่ต่อ VL53

1. ถอด VCC ของ VL53 จาก C3 ชั่วคราว
2. จ่ายไฟ C3 ด้วย USB อย่างเดียว
3. รีเซต S3 และ C3
4. ถ้ายัง reason=2/202 ให้ถือว่าเป็นปัญหา C3/RF/เสาอากาศหรือไฟเลี้ยง ไม่ใช่ I2C sensor

### ทางเลือก C: ใช้สาย UART แทน Wi-Fi

สำหรับระยะประมาณ 1.5–2 m สามารถทดสอบ UART 3.3V ที่ baud ต่ำ เช่น 9600 ได้:

```text
C3 TX -> S3 RX
C3 RX -> S3 TX
C3 GND -> S3 GND
```

ห้ามต่อ 5V logic และควรใช้สายคู่บิด/มี GND ร่วม การใช้ UART ตรงเหมาะสำหรับทดสอบ แต่ถ้าต้องการความทนทานสูงควรใช้ RS485 หรือ differential extender

### ทางเลือก D: ทดสอบ C3 กับ hotspot โทรศัพท์

ถ้าต้องการยืนยันฮาร์ดแวร์โดยไม่พึ่ง S3 ให้ตั้ง hotspot 2.4 GHz แล้วเปลี่ยน SSID/password ใน C3 ชั่วคราว:

- เชื่อมได้: ปัญหาอยู่ที่ความเข้ากันได้ S3 AP/เฟิร์มแวร์
- เชื่อมไม่ได้และยัง auth fail: C3 ภาคส่ง/เสาอากาศมีปัญหาสูงมาก

---

## 16. สิ่งที่ยังห้ามสรุป

- ยังห้ามถือว่าคาลิเบรตส่วนสูงเสร็จสมบูรณ์ เพราะลิงก์ C3-S3 ยังไม่ผ่าน
- ยังห้ามรับประกันความแม่น ±0.5 cm
- ยังห้ามพิมพ์ใบเสร็จจากค่าที่ไม่ได้ผ่าน TCP validation
- ยังห้ามแก้ด้วย offset เพิ่มแบบสุ่ม เพราะจะซ่อนปัญหา target/optical/I2C
- ยังห้ามสรุปว่า LCD เป็นสาเหตุของ Wi-Fi failure

---

## 17. เกณฑ์ถือว่าพร้อมใช้งาน

ต้องผ่านครบทุกข้อ:

1. S3 พบ LCD ที่ address ถูกต้อง หรือระบบยืนยันได้ว่า LCD ไม่อยู่ในเส้นทางที่ใช้งาน
2. C3 อ่าน VL53 ได้ต่อเนื่องโดย `i2c_retries=0`
3. C3 และ S3 แสดง `WPA2 association accepted`
4. C3 ได้ IP `192.168.4.x`
5. S3 แสดง `C3 TCP connected`
6. `REMOTEHEIGHT` ผ่านอย่างน้อย 10 รอบ
7. คนมาตรฐาน 158, 171 และ 183.2 cm ให้ผลอยู่ใน tolerance ที่กำหนด
8. น้ำหนักนิ่งและ tare ถูกต้อง
9. กด B แล้วพิมพ์เฉพาะเมื่อ height link ผ่าน
10. ทดสอบกรณีลิงก์หลุด ต้องขึ้น `MEASURE FAILED` และไม่พิมพ์

---

## สรุปสั้นที่สุด

ส่วนอ่าน VL53 และ logic ป้องกันค่าผิดถูกพัฒนาไปมากแล้ว แต่ระบบที่ใช้งานจริงยังติดที่ C3 ไม่ผ่าน WPA2 authentication แม้จะมองเห็น S3 AP แรงมาก ขณะเดียวกันโทรศัพท์เชื่อม S3 ได้ปกติ จึงควรหยุดคาลิเบรตและแก้/เปลี่ยน C3 หรือเปลี่ยนเป็น UART/RS485 ก่อน เมื่อได้ `TCP connected` แล้วจึงกลับไปทดสอบส่วนสูงและเครื่องพิมพ์ต่อ
