# BATTLAB 1.2 AUTH DEV • ESP32-S3

รุ่นสำหรับบอร์ด S3-N16R8 ใช้ Flash 8MB แรก ไม่พึ่ง PSRAM
Wi-Fi BATTLAB-JK ไม่มีรหัส เข้า http://192.168.4.1

## วิธีใช้รหัส
เปิดเมนูเชื่อมต่อ JK ใส่รหัส Bluetooth ที่ใช้ในแอป JK ของ BMS ตัวนั้น
ใส่รหัสแก้ไขพารามิเตอร์ได้อีกช่อง (ไม่จำเป็นสำหรับอ่านข้อมูล)
กด “ใช้รหัสและเชื่อมใหม่” แล้วเลือก BMS ที่ถูกตัว/Auto Connect
รหัสเก็บใน RAM จนรีสตาร์ตเท่านั้น ไม่เก็บ NVS/เบราว์เซอร์ และไม่ส่งกลับใน API
กดรับรหัสแล้วไม่ได้แปลว่าผ่าน ต้องรอ VERIFIED จากข้อมูลอุปกรณ์
เปลี่ยน BMS/การเชื่อมต่อ จะตรวจรหัสจากข้อมูลอุปกรณ์ใหม่ทุกครั้ง
บน Wi-Fi แบบเปิด ข้อมูล HTTP รวมถึงรหัสที่กรอกไม่มีการเข้ารหัสระหว่างมือถือและบอร์ด

## ขอบเขตการยืนยัน
สำหรับตระกูล JK ที่รหัสอยู่ในเฟรม Device Info type 0x03:
- ตรวจ checksum/header/type ก่อนใช้ข้อมูล
- ตรวจรหัสเชื่อมต่อที่ offset 62 และรหัสพารามิเตอร์ที่ offset 118 (แต่ละช่อง 16 bytes ASCII)
- รหัสไม่ตรงไม่ปล่อยข้อมูล live ผ่านเว็บ และไม่ขอเฟรม telemetry
- อ่าน Device Info ก่อนตรวจรหัส เพราะรหัสของรุ่นเหล่านี้ตรวจภายในแอป ไม่ได้เป็นคำสั่ง login ไป BMS
- ไม่มีการเดารหัส/เปลี่ยนรหัส BMS
- หากข้อมูลรหัสว่าง/ไม่ใช่รูปแบบที่รองรับ จะขึ้น UNSUPPORTED_INFO ไม่อ้างว่ารหัสผิด
- ไม่รองรับการกรอก SMP PIN/Pairing แบบระบบปฏิบัติการในรุ่นนี้

## พารามิเตอร์ยังอ่านอย่างเดียว
VERIFIED_READ_ONLY = รหัสตรงกับ Device Info แต่ยังไม่ใช่สิทธิ์เขียน BMS
ไม่มี endpoint เขียนพารามิเตอร์ ไม่มีคำสั่งแก้ OVP/UVP หรือสั่ง MOS
การเพิ่มช่องรหัสไม่ได้หมายความว่าส่วนเขียนค่าและอ่านกลับผ่านการทดสอบแล้ว

## แก้ช่อง BLE
ค้นหา FFE1 ทุกตัวใน service FFE0 แล้วเลือก characteristic ที่เขียนได้เป็น TX
และ characteristic ที่ notify ได้เป็น RX แยกกัน รองรับทั้งกรณีรวมช่องเดียวและแยกช่อง
รายงาน write_handle/notify_handle/ble_error ในตรวจสอบระบบ
ไม่บังคับ handle ตายตัว 0x03/0x05

## API
POST /api/auth JSON: {"bluetooth_code":"รหัสจริง", "parameter_code":"รหัสจริงหรือว่าง"}
GET /api/state คืน connection_auth, parameter_auth, parameter_write_supported=false
ไม่มีรหัส/expected-code ใน response

## คอมไพล์
ใช้ PlatformIO espressif32 6.9.0, Arduino 2.0.17, NimBLE 1.4.2, ArduinoJson 6.21.5
pio run
pio run -t upload --upload-port COMที่ถูกต้อง
ไม่ใช้ไฟล์ .bin RC2 เดิมเพื่อรับฟีเจอร์นี้ ต้องสร้างจาก source นี้

## หลักฐานการอ้างอิง
https://github.com/syssi/esphome-jk-bms/blob/main/components/jk_bms_ble/jk_bms_ble.cpp
ตรวจ offset Device passcode 62, Setup passcode 118 และการแยก notify handle
https://github.com/chinawrj/BMSTracker/blob/main/data/JK_BMS_BLE_Protocol_Analysis.md
ส่วน 8 อธิบาย local app verification และส่วน 9 อธิบาย characteristic ที่แยกช่อง
ข้อมูลนี้ไม่ยืนยันว่าฮาร์ดแวร์ JK ทุกเวอร์ชันใช้วิธีเดียวกัน

## ทดสอบกับฮาร์ดแวร์จริงที่ยังต้องทำ
ยืนยันรุ่น BMS/HW/SW, ข้อมูล Device Info และค่าในแอป JK ตรงกัน
ทดสอบรหัสถูก/ผิดแยกสองช่อง, รีสตาร์ตล้างรหัส, เปลี่ยน BMS
ทดสอบ FFE1 แบบรวม/แยก, Wi-Fi+BLE, reconnect และค่ากระแส/เซลล์
