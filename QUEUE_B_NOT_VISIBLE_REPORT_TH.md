# รายงานปัญหาคิว B ไม่แสดงในระบบคลินิก

วันที่รายงาน: 11 ตุลาคม 2026  
อุปกรณ์: ESP32-S3 เครื่องชั่ง `SCALE-001`  
หัวข้อ: วัดแบบ Walk-in สำเร็จ แต่คิว B ไม่แสดงในหน้าเว็บคลินิก

## 1. อาการที่พบ

- คิว A แสดงในระบบคลินิกตามปกติ
- เมื่อวัดผู้รับบริการแบบ Walk-in ระบบออกคิว B เช่น `B003` แต่ไม่พบคิวในหน้าเว็บ
- เครื่องพิมพ์พิมพ์บัตรได้สำเร็จ
- หากต่อ Serial Monitor จะเห็นการตอบกลับจาก Backend แต่เมื่อถอดสาย Serial ระบบยังทำงานวัดและพิมพ์ได้

## 2. หลักฐานจาก Log

อุปกรณ์วัดและส่งข้อมูลสำเร็จ:

```text
[REMOTE] TCP reply ... status=0 ...
[REMOTE] Accepted height 183.7 cm over WPA2/TCP
```

อุปกรณ์ส่งผลวัดเข้า MQTT สำเร็จ:

```text
Topic: clinic/v1/devices/SCALE-001/measurements
mode: walk_in
weight: 62.15667
height: 183.7
```

Backend ตอบรับและออกคิวแล้ว:

```text
status: accepted
measurement_id: 384
queue_number: B003
print_pending: true
```

Backend ส่งงานพิมพ์กลับมา และเครื่องพิมพ์สำเร็จ:

```text
[PRINTER] Print success: 237 bytes
status: printed
```

## 3. ข้อสรุป

ปัญหาไม่ได้อยู่ที่เซนเซอร์, TCP ระหว่าง ESP32-C3 กับ ESP32-S3, MQTT ฝั่งอุปกรณ์ หรือเครื่องพิมพ์ เพราะ Backend ตอบรับข้อมูล ออกเลขคิว B003 และส่งงานพิมพ์สำเร็จแล้ว

จุดที่มีปัญหาอยู่ระหว่าง Backend ที่รับ MQTT กับ API/หน้าเว็บที่แสดงคิว หรือหน้าเว็บกำลังเปิดคนละชุด Backend/ฐานข้อมูล

## 4. สาเหตุที่ตรวจพบ

### 4.1 คิว A และคิว B มาจากคนละขั้นตอน

- คิว A มาจากการนัดหมายและมี `appointment_id`
- คิว B เป็น `walk_in` และอาจไม่มี `appointment_id`
- หน้า “รายการนัดหมาย” จะแสดงเฉพาะรายการนัดหมาย จึงไม่ใช่หน้าที่ใช้ตรวจคิว B
- คิว B ต้องตรวจจากหน้า Admin Dashboard ส่วน “คิววันนี้” และเลือกตัวกรอง `ทั้งหมด` หรือ `Walk-in / B`

### 4.2 พบคอนฟิก MQTT คนละระบบ

คอนฟิก ESP32 ใช้ EMQX Cloud:

```text
hf391a51.ala.asia-southeast1.emqxsl.com:8883
```

แต่ Backend local ในไฟล์ `.env` ตั้งให้ใช้ MQTT ภายในเครื่อง:

```text
MQTT_URL=mqtt://127.0.0.1:1883
```

ถ้า ESP32 ส่งเข้า EMQX Cloud แต่หน้าเว็บอ่านฐานข้อมูลของ Backend local จะเกิดผลดังนี้:

```text
ESP32 -> EMQX Cloud -> Backend ชุดหนึ่ง -> ฐานข้อมูลชุดหนึ่ง
หน้าเว็บ -> API/ฐานข้อมูลอีกชุดหนึ่ง
```

ผลคือเครื่องอาจได้รับ ACK และพิมพ์ได้ แต่คิว B ไม่ปรากฏในหน้าเว็บที่กำลังเปิดอยู่

## 5. วิธีตรวจสอบทันที

1. เปิด Admin Dashboard ไม่ใช่หน้า “รายการนัดหมาย”
2. เลือกวันที่เป็นวันนี้
3. เปิดเมนูกรองคิว แล้วเลือก `ทั้งหมด` หรือ `Walk-in / B`
4. กด `Ctrl+F5`
5. เปิด Developer Tools > Network แล้วตรวจคำขอ `GET /api/today`
6. ค้นหา `B003` ใน Response

การแปลผล:

| ผลตรวจ `/api/today` | สรุป |
|---|---|
| มี `B003` แต่หน้าไม่แสดง | หน้าเว็บใช้ตัวกรองผิด, cache เก่า หรือกำลังใช้ build เก่า |
| ไม่มี `B003` แต่ MQTT log มี `accepted` | API กับ MQTT bridge ใช้คนละ Backend/ฐานข้อมูล หรือ query ฝั่ง API ตัดคิว B |
| ไม่มี ACK จาก Backend | ตรวจ MQTT URL, username/password, TLS และการเชื่อมต่อ Broker |

## 6. แนวทางแก้ไขถาวร

ให้ Backend ที่หน้าเว็บใช้งานเชื่อม Broker และฐานข้อมูลชุดเดียวกับที่ ESP32 ใช้งาน:

```text
MQTT_URL=mqtts://hf391a51.ala.asia-southeast1.emqxsl.com:8883
MQTT_USERNAME=<บัญชี MQTT ที่ได้รับอนุญาต>
MQTT_PASSWORD=<รหัส MQTT>
MQTT_TOPIC_PREFIX=clinic/v1
MQTT_REJECT_UNAUTHORIZED=true
```

จากนั้นให้ทำตามลำดับนี้:

1. ใช้ซอร์ส Backend และ Frontend ชุดเดียวกันทั้งระบบ
2. ตรวจว่า Backend มี `routes/queue.js`, `services/hardwareMeasurementService.js` และ `frontend`
3. ตรวจ API `GET /api/today` ให้คืนคิวทั้ง prefix `A` และ `B`
4. ใช้ `LEFT JOIN` กับข้อมูลนัดหมาย เพื่อไม่ตัด Walk-in ที่ไม่มี `appointment_id`
5. ให้ Dashboard มีตัวกรอง `ทั้งหมด`, `A` และ `B`
6. เปิดการ refresh คิวทุก 5 วินาที หรือใช้ WebSocket/SSE
7. ปิด MQTT bridge ตัวเก่าที่ใช้คนละฐานข้อมูล เพื่อป้องกันการออกคิวหรือพิมพ์ซ้ำ
8. ทดสอบใหม่ด้วย Walk-in หนึ่งรายการ และยืนยันว่าเลขคิว, measurement และ print status อยู่ในฐานข้อมูลเดียวกัน

## 7. ข้อควรระวัง

ห้ามเปิด MQTT bridge สองตัวที่ subscribe topic เดียวกันพร้อมกัน หากทั้งสองตัวประมวลผลข้อความเดียวกัน อาจเกิดคิวซ้ำ, ACK ซ้ำ หรือพิมพ์บัตรซ้ำได้

ห้ามใส่ค่า `MQTT_PASSWORD`, `DATABASE_URL` หรือ JWT secret ลงในรายงาน, Git หรือภาพหน้าจอ

## 8. สถานะปัจจุบัน

| ส่วนประกอบ | สถานะ |
|---|---|
| ESP32-C3 วัดส่วนสูง | ผ่าน |
| ESP32-S3 รับข้อมูล TCP | ผ่าน |
| HX711 น้ำหนัก | ผ่าน แต่ต้องวางน้ำหนักให้นิ่ง |
| MQTT publish | ผ่าน |
| Backend ACK | ผ่าน |
| ออกเลขคิว B | ผ่านจาก Log (`B003`) |
| USB Printer | ผ่าน |
| แสดงคิว B บนหน้าเว็บ | ยังไม่ผ่าน |
| การใช้ Broker/ฐานข้อมูลชุดเดียวกัน | ต้องตรวจและแก้ให้ตรงกัน |
