# คู่มือทดสอบ Firmware กับเว็บผ่าน MQTT

ESP32 และ Backend ไม่ได้เชื่อมกันโดยตรง ทั้งสองฝั่งเชื่อม EMQX Broker เดียวกัน
ตาม Topic ใน `MQTT_HARDWARE_CONTRACT_TH.md` ส่วน MQTTX ใช้เป็นตัวดูข้อความเท่านั้น

## 1. เตรียมก่อนทดสอบ

1. ให้ผู้ดูแลเว็บเปิด Backend และยืนยันว่า Subscribe `clinic/v1/devices/+` แล้ว
2. Upload Firmware และเปิด Serial Monitor ที่ `115200`, Line Ending=`Newline`
3. รอข้อความต่อไปนี้:

```text
[Wi-Fi] เชื่อมต่อสำเร็จ
[MQTT] เชื่อมต่อ TLS สำเร็จและ Subscribe Topic ครบแล้ว
```

4. เปิด MQTTX ด้วย Client ID ที่ไม่ซ้ำกับฮาร์ดแวร์ เช่น
   `mqttx-scale-001-test` และ Subscribe แบบ QoS 1:

```text
clinic/v1/devices/SCALE-001/#
```

ระหว่างทดสอบบอร์ด ให้ MQTTX ดูข้อความอย่างเดียวและไม่ Publish ข้อมูลแทรก

## 2. ทดสอบเส้นทางรับ-ส่งด้วย OTP ผิด

ส่งใน Serial Monitor:

```text
A 000000
```

ต้องเห็น `/otp-verify` จาก ESP และ `/otp-result` จาก Backend โดยผลเป็น
`INVALID_OTP` จากนั้น ESP ต้องไม่ส่ง `/measurements` กรณีนี้ถือว่าการรับ-ส่ง MQTT
ทำงาน แต่ OTP ไม่ผ่านตามที่ตั้งใจทดสอบ

## 3. ทดสอบคิว A ด้วยค่าจำลอง

ขอ OTP ใหม่สำหรับ `SCALE-001` จากผู้ดูแลเว็บ แล้วส่ง:

```text
T <OTPจริง> 60 170
```

ตัวอย่าง:

```text
T 654321 60 170
```

MQTTX ต้องเห็นตามลำดับ:

1. `.../otp-verify` จาก ESP
2. `.../otp-result` สถานะ `accepted` จาก Backend
3. `.../measurements` จาก ESP มี `mode=online`, `weight=60`, `height=170`
4. `.../measurement-ack` จาก Backend โดย `message_id` ต้องตรงกัน

Serial ต้องแสดง `[OTP] ยืนยันสำเร็จ` และ `[ACK] Backend บันทึกค่าวัดสำเร็จ`
ฝั่งเว็บต้องเห็นน้ำหนัก 60 kg, ส่วนสูง 170 cm และหมายเลขคิว A

## 4. ทดสอบคิว B แบบ Walk-in

ส่งใน Serial Monitor โดยไม่ต้องใช้ OTP:

```text
W 60 170
```

MQTTX ต้องเห็น `/measurements` ที่มี `mode=walk_in` และไม่มี
`measurement_session_id` จากนั้น Backend ต้องตอบ `/measurement-ack` พร้อม
หมายเลขคิว B และหน้าเว็บต้องแสดงข้อมูลเพียงหนึ่งรายการ

## 5. ทดสอบ reconnect และป้องกันข้อมูลซ้ำ

1. ส่งค่าวัดแล้วปิด Wi-Fi/Hotspot ชั่วคราวก่อน Backend ACK
2. เปิด Wi-Fi กลับ
3. ESP ต้อง reconnect และส่ง payload เดิมด้วย `message_id` เดิม
4. Backend อาจตอบ `accepted` หรือ `duplicate` แต่ฐานข้อมูลต้องมีเพียงหนึ่งรายการ

Firmware เก็บ Measurement และ print ACK ที่ค้างใน NVS จึงยังส่งต่อได้หลังรีสตาร์ต

## ทดสอบ ACK สูญหายและ Backend deduplicate โดยไม่ต่ออุปกรณ์

Environment ทดสอบนี้เปิดคำสั่งจำลองเฉพาะกิจ โดยจะจงใจข้าม
`measurement-ack` แรกหนึ่งครั้ง จากนั้น Firmware ส่ง payload และ `message_id`
เดิมซ้ำหลัง 10 วินาที:

```text
pio run -e esp32-s3-walkin-retry-test -t upload
pio device monitor -e esp32-s3-walkin-retry-test
```

เมื่อ MQTT พร้อม ให้ส่งเพียงครั้งเดียว:

```text
WRETRY 66 174
```

ผลที่ผ่านต้องได้ `accepted` ครั้งแรกซึ่ง Firmware จำลองว่าหาย แล้วได้
`duplicate` พร้อม `queue_number` เดิมในครั้งถัดไป หน้าเว็บต้องเพิ่มเพียงหนึ่งคิว
หลังทดสอบให้อัปโหลด Environment `esp32-s3-n16r8` กลับเพื่อปิดคำสั่งจำลองนี้
ส่วน `print_job_id` ที่จัดการแล้วจะถูกเก็บไว้ 16 รายการล่าสุดเพื่อกันพิมพ์ซ้ำ

## ทดสอบ ACK สูญหายสำหรับคิว A

ใช้ OTP จริงที่เพิ่งสร้างและยังไม่เคยใช้ แล้วอัปโหลด Environment ทดสอบ:

```text
pio run -e esp32-s3-online-retry-test -t upload
pio device monitor -e esp32-s3-online-retry-test
```

ส่งคำสั่งโดยเปลี่ยน OTP เป็นค่าจริง:

```text
ARETRY <OTPจริง> 60 170
```

ผลที่ผ่านต้องได้ `accepted` สำหรับคิว A ครั้งแรกซึ่ง Firmware จำลองว่าหาย
จากนั้น Firmware ต้องส่ง payload และ `message_id` เดิมซ้ำภายใน 10 วินาที
และ Backend ต้องตอบ `duplicate` พร้อม `measurement_id` และ `queue_number`
เดิม หน้าเว็บต้องมีคิว A เพียงรายการเดียว หลังทดสอบให้อัปโหลด
Environment `esp32-s3-n16r8` กลับทุกครั้ง

Firmware จะตรวจเพิ่มว่า `queue_number` ใน `/measurement-ack` ตรงกับคิวที่ได้จาก
`/otp-result` และจะไม่ล้าง Pending หาก `measurement_id` ไม่ถูกต้อง นอกจากนี้
การส่ง Measurement ซ้ำจะหน่วงแบบ 10, 20, 40 และสูงสุด 60 วินาที เพื่อลดภาระ
Backend หากระบบปลายทางหยุดทำงานเป็นเวลานาน หาก ACK ระบุ `print_pending=true`
แต่ไม่มี `/print` ภายใน 30 วินาที Serial จะแจ้งให้ตรวจ Backend Print Outbox

## 6. ทดสอบเซนเซอร์จริง

ทำหลังค่าจำลองผ่านแล้วเท่านั้น ต้องตั้ง `CALIBRATION_FACTOR`, เปิด tare และตั้ง
`SENSOR_MOUNT_HEIGHT_CM` ก่อน จากนั้น:

- ตอนว่างกด `B` เพื่อวัด Walk-in
- คิวออนไลน์กด `A`, กรอก OTP, กด `#`, รอ accepted แล้วกด `B`

## แยกตำแหน่งปัญหา

| อาการ | จุดที่ตรวจ |
|---|---|
| MQTT ไม่เชื่อม | Wi-Fi, เวลา NTP, CA, Host/Port, Username/Password |
| MQTTX ไม่เห็นข้อความจาก ESP | Topic, Publish หรือสถานะ MQTT ของ ESP |
| เห็น `otp-verify` แต่ไม่มี `otp-result` | Backend ไม่ตอบหรือไม่ได้ Subscribe |
| MQTTX เห็น response แต่ ESP ไม่ทำงานต่อ | `request_id`/`message_id` ไม่ตรงหรือ parser ฝั่ง ESP |
| ได้ ACK แต่หน้าเว็บไม่แสดง | Backend, ฐานข้อมูล หรือ Frontend |
