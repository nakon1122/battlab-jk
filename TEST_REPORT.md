# BATTLAB 1.2 AUTH DEV • 31 ตุลาคม 2026

- Full PlatformIO build: PASS (test/build-final.log), espressif32 6.9.0 / Arduino 2.0.17 / NimBLE 1.4.2 / ArduinoJson 6.21.5
- RAM 51,596 / 327,680 bytes; Flash 1,015,405 / 3,342,336 bytes
- Native C++ tests ASAN/UBSAN: PASS รหัสสองช่อง, leading zero, ไม่ยอมรับรหัสผิด/ว่าง/ยาวเกิน/ASCII ไม่ถูก/เฟรมผิดชนิด/CRC ผิด
- Native C++ GATT selection: PASS TX/RX แยก FFE1, ช่องเดียว, ขาด TX, UUID อื่นและ nullptr
- Protocol regression: PASS 602 split cases และชุดตรวจ JK02 เดิม
- jsdom: PASS ช่องรหัสสองช่อง, POST ถูก schema, ล้าง input, ไม่บันทึกรหัสใน localStorage, ปฏิเสธรหัสว่าง และ 10 เมนู/live/stale/graphs เดิม
- esptool image_info: checksum และ SHA256 valid; merged app ที่ 0x10000 ตรงกับ app-only ทุก byte

ยังไม่ทดสอบ: บูตจริง, BLE กับ JK แต่ละรุ่น/เฟรม Device Info, ยืนยันว่ารหัสในแอปของรุ่นผู้ใช้เป็น app gate หรือ SMP, รหัสถูก/ผิดจริง, reconnect และความถูกต้องของ telemetry
พารามิเตอร์ยังอ่านอย่างเดียว ไม่มีการทดสอบ/เปิดใช้คำสั่งเขียน

ครั้งนี้ใช้ compiler และ framework ที่ตรวจ hash ผ่านแล้ว แหล่ง mirror แรกบางรายการ hash ผิดจึงถูกปฏิเสธ แล้วใช้ mirror สำรองที่ผ่าน validation ไม่ข้ามการตรวจ checksum
