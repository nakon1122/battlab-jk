// BattLab Cloud - firmware page: install over USB (ESP Web Tools), update over
// Bluetooth, downloads, and the installation guide. Loaded by index.html.
'use strict';
const FWB = {board: 's3', tab: 'install', art: null, q: ''};
const BOARDS = [['s3', '🔌', 'ESP32-S3', 'บอร์ดหลัก ไม่มีจอ'], ['cyd35', '📟', 'CYD 3.5"', 'ต่อ BMS ตรง'], ['cloudscreen', '🖥️', 'Cloud Screen', 'จอดูทุกแบต']];
const BSV = '7a1e0000-5b3c-4f6a-9d2e-b47710ba7700', BCH = n => '7a1e000' + n + '-5b3c-4f6a-9d2e-b47710ba7700';

// ------------------------------------------------------------------ guide
const GCAT = [
  ['board', '🧭', 'เลือกบอร์ด', 'S3 / CYD / Cloud Screen ใช้ทำอะไร ซื้อรุ่นไหน'],
  ['install', '💾', 'ติดตั้งครั้งแรก', 'ผ่านเว็บ (USB) หรือไฟล์ .bat'],
  ['wire', '🔧', 'ต่อสาย', 'RS485, ย้ำหัว RJ45, แบตหลายลูก, DALY'],
  ['setup', '📶', 'ตั้งค่าเริ่มต้น', 'Wi-Fi ผ่านบลูทูธ, จับคู่บอร์ดกับบัญชี'],
  ['bms', '🔋', 'เชื่อม BMS', 'JK · ANT · JBD · DALY ทั้งบลูทูธและสาย'],
  ['update', '⬆️', 'อัปเดตเฟิร์มแวร์', 'cloud · บลูทูธ · USB'],
  ['fix', '🩺', 'แก้ปัญหา', 'อาการที่พบบ่อยและวิธีแก้'],
  ['faq', '❓', 'คำถามที่พบบ่อย', 'ดูได้กี่ลูก, อินเวอร์เตอร์, ความปลอดภัย'],
];
const GART = [
  {id: 'which-board', c: 'board', t: 'บอร์ดแต่ละแบบใช้ทำอะไร', s: [
    ['ESP32-S3 (บอร์ดหลัก)', 'ไม่มีจอ อ่าน BMS ผ่านบลูทูธหรือสาย RS485 แล้วส่งขึ้น cloud · รองรับแบตหลายลูกบนสาย RS485 เส้นเดียว (สูงสุด 16 ลูก) · แนะนำรุ่น ESP32-S3 DevKitC N16R8'],
    ['CYD 3.5" (ต่อ BMS ตรง)', 'จอสัมผัส ESP32-3248S035 ต่อ BMS ผ่านบลูทูธ 1 ลูก แสดงผลบนจอและส่งขึ้น cloud · เหมาะติดรถหรือหน้าตู้แบต'],
    ['Cloud Screen', 'ใช้จอ CYD ตัวเดียวกัน แต่ดึงข้อมูลจาก cloud มาแสดง ดูได้ทุกแบตในบัญชี ไม่ต้องอยู่ใกล้ BMS · ตั้งค่า BMS ผ่านจอได้'],
    ['เลือกยังไง', 'แบตอยู่ไกลจาก Wi-Fi หรือมีหลายลูก → S3 + RS485 · อยากดูที่ตัวแบต → CYD · อยากดูทุกแบตจากในบ้าน → Cloud Screen']]},
  {id: 'buy-list', c: 'board', t: 'อุปกรณ์ที่ต้องซื้อ', s: [
    ['บอร์ด', 'ESP32-S3 DevKitC-1 (N16R8 หรือ N8R2) หรือ ESP32-3248S035 (CYD 3.5")'],
    ['สาย USB', 'สาย USB ที่ส่งข้อมูลได้ (ไม่ใช่สายชาร์จอย่างเดียว) สำหรับติดตั้งครั้งแรก'],
    ['สำหรับ RS485', 'โมดูล RS485-to-TTL แบบปรับทิศทางอัตโนมัติ ทำงาน 3.3V (MAX3485/SP3485) + สายแลน 1 เส้น'],
    ['ไฟเลี้ยง', 'USB 5V หรือ DC-DC step-down จากแบต (เช่น 48V → 5V)']]},
  {id: 'usb-web', c: 'install', t: 'ติดตั้งเฟิร์มแวร์ผ่านเว็บ (USB)', s: [
    ['เปิดด้วยคอมพิวเตอร์', 'ใช้ Chrome หรือ Edge บน Windows / Mac / Linux (มือถือติดตั้งผ่าน USB ไม่ได้)'],
    ['เลือกบอร์ด', 'หน้าเฟิร์มแวร์ → แท็บติดตั้ง/อัปเดต → เลือกบอร์ด'],
    ['เสียบสายแล้วกดติดตั้ง', 'กด "เชื่อมต่อบอร์ดและติดตั้ง" เลือกพอร์ต (COM / usbserial) → ติดตั้ง · ถ้าถาม "Erase device" เลือกล้างเฉพาะติดตั้งครั้งแรกหรืออยากเริ่มใหม่'],
    ['ถ้าเชื่อมต่อไม่ได้', 'กดปุ่ม BOOT ค้างไว้ แล้วกด RESET 1 ครั้ง ปล่อย BOOT แล้วลองใหม่ · Windows บางเครื่องต้องลงไดรเวอร์ CH340 / CP210x']]},
  {id: 'first-pc', c: 'setup', t: 'ตั้งค่าครั้งแรกด้วยคอมพิวเตอร์ (ไม่ต้องใช้มือถือ)', s: [
    ['เสียบสาย USB', 'ต่อบอร์ดเข้าคอมพิวเตอร์ เปิดเว็บนี้ด้วย Chrome หรือ Edge'],
    ['ติดตั้งเฟิร์มแวร์', 'ปุ่ม ⬇️ เฟิร์มแวร์ → เลือกบอร์ด → ผ่านสาย USB → เชื่อมต่อบอร์ดและติดตั้ง'],
    ['ตั้ง Wi-Fi บ้านทันที', 'ติดตั้งเสร็จ หน้าต่างจะถามชื่อและรหัส Wi-Fi บ้านต่อเลย (ปุ่ม Connect to Wi-Fi) บอร์ดจะต่อเน็ตเอง'],
    ['เพิ่มเข้าบัญชีในคลิกเดียว', 'ต่อ Wi-Fi ได้แล้วกด Visit device / ไปที่อุปกรณ์ เว็บจะเพิ่มบอร์ดเข้าบัญชีให้เอง (ต้องเข้าสู่ระบบ)'],
    ['บอร์ดที่ติดตั้งแล้ว', 'ใช้ปุ่ม "ตั้งค่าผ่าน USB" ที่หน้าแรกหรือหน้าอุปกรณ์ของฉัน ได้หน้าตั้งค่าเหมือนบลูทูธทุกอย่าง (Wi-Fi, BMS, RS485, จับคู่บัญชี)'],
    ['Windows หาบอร์ดไม่เจอ', 'จอ CYD ใช้ชิป USB CH340 ต้องลงไดรเวอร์ CH340 ครั้งแรก · บอร์ด S3 ใช้ USB ในตัว ไม่ต้องลงไดรเวอร์ (ใช้ช่อง USB ที่เขียนว่า USB ไม่ใช่ UART)'],
    ['คอมที่มีบลูทูธ', 'ใช้ปุ่ม "ตั้งค่าบอร์ดผ่านบลูทูธ" บน Chrome ของ Windows 10/11 หรือ Mac ได้เช่นกัน']]},
  {id: 'usb-bat', c: 'install', t: 'ติดตั้งด้วยไฟล์ .bat (Windows)', s: [
    ['ดาวน์โหลดชุดเต็ม', 'แท็บดาวน์โหลด → ดาวน์โหลดชุดเต็ม (.zip) แล้วแตกไฟล์'],
    ['ลง Python 3.11', 'ครั้งแรกเท่านั้น ไฟล์ .bat จะติดตั้ง esptool ให้เอง'],
    ['เลือกไฟล์', 'บอร์ดใหม่: FLASH_*_WINDOWS.bat (ล้างทั้งหมด) · อัปเดต: UPDATE_*_WINDOWS.bat (เก็บค่าเดิม)'],
    ['ใส่หมายเลขพอร์ต', 'ดูใน Device Manager เช่น COM5 แล้วรอจนขึ้น Hash of data verified']]},
  {id: 'rj45', c: 'wire', t: 'ย้ำหัว RJ45 สำหรับ JK-PB', img: 'guide/RS485_1_RJ45_CRIMP.jpg', s: [
    ['ใช้ 3 เส้น', 'ขา 1 (ขาว-ส้ม) = B · ขา 2 (ส้ม) = A · ขา 3 (ขาว-เขียว) = GND ตามมาตรฐาน T568B'],
    ['นับขา', 'หงายทองแดงขึ้น คลิปคว่ำลง ปลายเสียบชี้ออกจากตัว → ขา 1 อยู่ซ้ายสุด'],
    ['สาย T568A', 'B = ขาว-เขียว · A = เขียว · GND = ขาว-ส้ม · ยึดหมายเลขขาเป็นหลัก'],
    ['ทางลัด', 'ตัดสายแลนสำเร็จรูปครึ่งเส้น ใช้เฉพาะขา 1, 2, 3 แล้ววัดมิเตอร์ยืนยัน']]},
  {id: 'rs485-jk', c: 'wire', t: 'ต่อ JK BMS ผ่าน RS485 กับ ESP32-S3', img: 'guide/RS485_2_WIRING_ESP32S3.jpg', s: [
    ['ฝั่ง BMS → โมดูล', 'ขา 2 (A) → A+ · ขา 1 (B) → B− · ขา 3 (GND) → GND'],
    ['ฝั่งโมดูล → ESP32-S3', 'VCC → 3V3 · GND → GND · TXD → GPIO18 · RXD → GPIO17 (ห้ามจ่าย 5V)'],
    ['ตั้ง DIP', 'ลูกหลัก 0 · ลูกอื่น 1, 2, 3… ห้ามซ้ำ · เสียบช่อง RS485-2 (พอร์ตต่อขนาน) อย่าเสียบช่อง CAN'],
    ['เลือกบนเว็บ', 'ควบคุม → ช่องทางเชื่อม BMS → สาย RS485 → ยี่ห้อ อัตโนมัติ → บันทึก']]},
  {id: 'bank', c: 'wire', t: 'แบตหลายลูกบนสาย RS485 เส้นเดียว', s: [
    ['มีลูกหลัก (DIP 0)', 'บอร์ดจะ "ฟังอย่างเดียว" ได้ข้อมูลทุกลูก ลูกหลักยังคุย CAN กับอินเวอร์เตอร์ได้ปกติ'],
    ['ตั้งค่ารายลูก', 'กด "จับคู่อัตโนมัติ" ในการ์ดบลูทูธของแต่ละลูก (ปิดแอป JK ก่อน) แล้วเลือกลูกในหน้าควบคุม ตั้งได้ทุกลูกรวม #0'],
    ['ไม่มีลูกหลัก', 'ตั้ง DIP 1..15 ทุกลูก เลือกโหมด "บอร์ดถามเอง" + address ลูกแรก + จำนวนลูก → อ่านและตั้งค่าผ่านสายได้เลย'],
    ['ดูบนเว็บ', 'ภาพรวมแสดงยอดรวมทั้งแบงก์ + การ์ดทุกลูก · เซลล์และกราฟเลือกดูรายลูกได้']]},
  {id: 'daly-wire', c: 'wire', t: 'ต่อ DALY ผ่าน UART / RS485', s: [
    ['DALY รุ่นใหม่ (H/K/M/S)', 'ใช้พอร์ต RS485 ของ BMS ต่อ A/B/GND เข้าโมดูลแบบเดียวกับ JK · ตั้ง address ด้วยโปรแกรม DALY (ค่าเริ่มต้น 1)'],
    ['DALY รุ่นเก่า (A5)', 'ใช้พอร์ต UART ของ BMS (TX/RX/GND) ต่อตรงเข้า GPIO18/17 ได้ · GND ของ UART คือขั้วลบแบต ระวังกราวด์'],
    ['เลือกบนเว็บ', 'ควบคุม → ช่องทางเชื่อม BMS → สาย RS485 → ยี่ห้อ DALY → บันทึก'],
    ['ทำได้', 'อ่านค่าครบทุกเซลล์ + เปิด/ปิด MOS ชาร์จ/จ่าย · การตั้งพารามิเตอร์ DALY จะเพิ่มทีหลัง']]},
  {id: 'ble-setup', c: 'setup', t: 'ตั้ง Wi-Fi บ้านผ่านบลูทูธ', s: [
    ['เปิดเว็บบนมือถือ', 'Chrome บน Android / คอมพิวเตอร์ (iPhone ใช้แอป Bluefy)'],
    ['กด "ตั้งค่าบอร์ดผ่านบลูทูธ"', 'เลือกชื่อ BATTLAB-XXXXXX ของบอร์ดคุณ (6 หลักท้าย = MAC ของชิป ดูได้จากบรรทัด MAC: ตอนติดตั้ง หรือหน้า INFO ของจอ CYD)'],
    ['ใส่ Wi-Fi บ้าน', 'ค้นหา Wi-Fi → เลือก → ใส่รหัส → บอร์ดต่อเน็ตและขึ้น online ใน ~20 วินาที'],
    ['เพิ่มเข้าบัญชี', 'กด "เพิ่มเข้าบัญชีของฉัน" หรือใช้รหัสจับคู่ในหน้าอุปกรณ์']]},
  {id: 'pair', c: 'setup', t: 'จับคู่บอร์ดกับบัญชี', s: [
    ['หารหัสจับคู่', 'CYD: INFO → CLOUD · S3: หน้าเว็บ 192.168.4.1 หรือผ่านบลูทูธ · Cloud Screen: หน้าแรกของจอ'],
    ['กรอกบนเว็บ', 'หน้าอุปกรณ์ของฉัน → เพิ่มอุปกรณ์ → ใส่รหัส 8 ตัว'],
    ['ตั้งประเภทการใช้งาน', 'หน้าอุปกรณ์ → ตั้งค่า → การใช้งานและกรุ๊ป (โซลาร์, มอเตอร์ไซค์ EV ฯลฯ)']]},
  {id: 'bms-jk', c: 'bms', t: 'JK BMS ผ่านบลูทูธ', s: [
    ['ค้นหา', 'ควบคุม → การเชื่อมต่อ BMS → ค้นหา BMS → เลือก JK → เชื่อม (หรือกด เชื่อมอัตโนมัติ)'],
    ['ปิดแอป JK', 'BMS รับบลูทูธได้ทีละเครื่อง ถ้าแอปบนมือถือเชื่อมอยู่ บอร์ดจะเชื่อมไม่ได้'],
    ['ตั้งพารามิเตอร์', 'หน้าควบคุม → พารามิเตอร์ BMS · ทุกค่ามีขอบเขตปลอดภัย และถามยืนยันก่อนส่ง']]},
  {id: 'bms-other', c: 'bms', t: 'ANT / JBD / DALY ผ่านบลูทูธ', s: [
    ['ANT', 'อ่านได้ 47 ค่า · เขียนเป็นโหมดทดลองที่อ่านกลับมาตรวจทุกครั้ง'],
    ['JBD / Xiaoxiang', 'อ่าน + ตั้งได้ 22 ค่า เขียนลง EEPROM แล้วอ่านกลับตรวจ'],
    ['DALY', 'ชื่อบลูทูธขึ้นต้นด้วย DL- (รุ่นใหม่) หรือผ่านดองเกิล (รุ่นเก่า) · อ่านค่า + MOS ชาร์จ/จ่าย · ถ้าเปิดโหมด Wi-Fi ในแอป DALY บลูทูธจะดับ ต้องลบอุปกรณ์ในแอปก่อน']]},
  {id: 'upd-cloud', c: 'update', t: 'อัปเดตผ่าน cloud', s: [
    ['บอร์ดต้องออนไลน์', 'บอร์ดที่เป็น 2.4.0 ขึ้นไป และต่อ Wi-Fi บ้านแล้ว'],
    ['กดอัปเดต', 'หน้าอุปกรณ์ → ควบคุม → การ์ดเฟิร์มแวร์ → อัปเดต · บอร์ดดาวน์โหลดเอง ~1 นาที แล้วรีสตาร์ท'],
    ['ปลอดภัย', 'ถ้าเวอร์ชันใหม่รีสตาร์ทผิดปกติ 3 ครั้งติด บอร์ดกลับไปใช้เวอร์ชันเดิมเอง']]},
  {id: 'upd-ble', c: 'update', t: 'อัปเดตผ่านบลูทูธ', s: [
    ['ใช้ได้เมื่อไร', 'บอร์ดที่มี BATTLAB อยู่แล้ว (บอร์ดใหม่จากโรงงานต้องใช้ USB ครั้งแรก)'],
    ['บลูทูธล้วน', 'บอร์ด 2.8.0 ขึ้นไป: เว็บส่งไฟล์เข้าบอร์ดตรงๆ ไม่ต้องมี Wi-Fi ใช้เวลา 2–4 นาที'],
    ['ให้บอร์ดโหลดเอง', 'บอร์ด 2.4.0–2.7.0: เว็บสั่งผ่านบลูทูธให้บอร์ดโหลดผ่าน Wi-Fi บ้าน'],
    ['ระหว่างส่ง', 'วางมือถือใกล้บอร์ด อย่าปิดหน้าเว็บ · ถ้าขาดกลางทาง บอร์ดยังใช้เวอร์ชันเดิมได้ตามปกติ']]},
  {id: 'upd-usb', c: 'update', t: 'อัปเดตผ่าน USB', s: [
    ['เว็บ', 'แท็บติดตั้ง/อัปเดต → ผ่านสาย USB → ไม่เลือก Erase = เก็บค่าเดิม'],
    ['ไฟล์ .bat', 'ใช้ UPDATE_*_WINDOWS.bat จากชุดเต็ม (เก็บค่าเดิม)']]},
  {id: 'fix-rs485', c: 'fix', t: 'ขึ้น RS485_NO_ANSWER', img: 'guide/RS485_3_SETUP_OLD_JK.jpg', s: [
    ['A/B สลับ', 'สลับสาย A+ กับ B− ที่โมดูล (TX/RX สลับบอร์ดแก้ให้เอง)'],
    ['Address', 'เลขบนเว็บต้องตรงกับ DIP ของ BMS'],
    ['พอร์ตผิด', 'ใช้ RS485-2 (พอร์ตขนาน) ถ้าไม่ได้ลอง RS485-1 + ตั้งโปรโตคอล 001 ในแอป JK'],
    ['ไฟโมดูล', 'โมดูลต้องได้ 3.3V และต่อ GND ร่วม · สายยาวใส่ 120Ω คร่อม A/B']]},
  {id: 'fix-wifi', c: 'fix', t: 'สแกน Wi-Fi ไม่เจอ / ต่อไม่ติด', s: [
    ['ใช้ 2.4 GHz', 'ESP32 ใช้ได้เฉพาะ Wi-Fi 2.4 GHz ไม่รองรับ 5 GHz'],
    ['สแกนใหม่', 'เฟิร์มแวร์ 2.5.1 ขึ้นไปพักการเชื่อมต่อเดิมระหว่างสแกนแล้ว ถ้ายังไม่เจอ กดสแกนอีกครั้ง'],
    ['รหัสผิด', 'ถ้าต่อไม่ได้ภายใน 90 วินาที บอร์ดกลับไปใช้ Wi-Fi เดิมเอง']]},
  {id: 'fix-ble', c: 'fix', t: 'บอร์ดเชื่อม BMS ผ่านบลูทูธไม่ได้', s: [
    ['ปิดแอปมือถือ', 'แอป JK / DALY ที่เปิดค้างจะยึดบลูทูธของ BMS ไว้'],
    ['ระยะ', 'วางบอร์ดห่าง BMS ไม่เกินไม่กี่เมตร เปิด TX MAX ในหน้าควบคุม'],
    ['DALY บลูทูธดับ', 'ลบอุปกรณ์ในแอป DALY ที่ตั้งโหมด Wi-Fi/Cloud แล้วบลูทูธจะกลับมา'],
    ['เปลี่ยนเป็นสาย', 'JK-PB / DALY ที่มีพอร์ต RS485 ใช้สายแทนได้ เสถียรกว่า']]},
  {id: 'fix-match', c: 'fix', t: 'จับคู่บลูทูธกับแต่ละลูกไม่ครบ', s: [
    ['ปิดแอป JK', 'ลูกที่มือถือเชื่อมอยู่จะจับคู่ไม่ได้'],
    ['ลองใหม่', 'กด "จับคู่ใหม่" ได้ตลอด ลูกที่จับคู่แล้วจะถูกตรวจซ้ำ'],
    ['ระยะ', 'ทุกลูกต้องอยู่ในระยะบลูทูธของบอร์ด']]},
  {id: 'faq-count', c: 'faq', t: 'บอร์ดเดียวดูแบตได้กี่ลูก', s: [
    ['RS485', 'สูงสุด 16 ลูก (DIP 0–15) ถ้ามีลูกหลัก · 15 ลูกถ้าบอร์ดถามเอง'],
    ['บลูทูธ', '1 ลูกต่อบอร์ด (ใช้ RS485 ถ้าต้องการหลายลูก)'],
    ['จอ Cloud Screen', 'แสดงได้ทุกแบตในบัญชี หน้า CELLS เลือกได้ 8 ลูกแรกของแบงก์']]},
  {id: 'faq-inverter', c: 'faq', t: 'ใช้ร่วมกับอินเวอร์เตอร์ได้ไหม', s: [
    ['ได้', 'บอร์ดแค่ฟังสาย RS485 ลูกหลักยังส่งข้อมูลให้อินเวอร์เตอร์ทาง CAN ตามปกติ'],
    ['ตั้งค่า', 'การตั้งค่ารายลูกใช้บลูทูธ ไม่รบกวนสายสื่อสารกับอินเวอร์เตอร์']]},
  {id: 'faq-safe', c: 'faq', t: 'ความปลอดภัยของการตั้งค่า', s: [
    ['ขอบเขต', 'ทุกค่ามีช่วงที่ปลอดภัย ค่านอกช่วงจะถูกปฏิเสธ'],
    ['ยืนยัน', 'ทุกครั้งที่ตั้งค่ามีหน้าต่างยืนยัน และบอร์ดอ่านค่ากลับมาตรวจ'],
    ['ข้อมูล', 'ข้อมูลแบตเก็บในบัญชีของคุณเท่านั้น ผู้อื่นไม่เห็นจนกว่าคุณจะแชร์']]},
];

// --------------------------------------------------------------- rendering
function fwShow(tab, art) {
  FWB.tab = tab || 'install';
  FWB.art = art || null;
  show('vFw');
  $('btnBack').classList.remove('hide');
  renderFw();
}
function fwBoard() { return MAN && MAN.boards && MAN.boards[FWB.board]; }
function renderFw() {
  const e = $('vFw');
  const seg = '<div class="seg">' + [['install', '⚙️ ติดตั้ง/อัปเดต'], ['dl', '⬇️ ดาวน์โหลด'], ['guide', '📘 คู่มือ']]
    .map(t => '<span class="' + (FWB.tab === t[0] ? 'on' : '') + '" data-ft="' + t[0] + '">' + t[1] + '</span>').join('') + '</div>';
  let h = '<div class="big" style="margin-bottom:6px">เฟิร์มแวร์</div>' + seg + '<div id="fwBody">';
  if (FWB.tab === 'install') h += fwInstallHtml();
  else if (FWB.tab === 'dl') h += fwDlHtml();
  else h += FWB.art ? fwArtHtml(FWB.art) : fwGuideHtml();
  e.innerHTML = h + '</div>';
  e.querySelectorAll('[data-ft]').forEach(x => x.onclick = () => go('fw/' + x.dataset.ft));
  e.querySelectorAll('[data-fb]').forEach(x => x.onclick = () => { FWB.board = x.dataset.fb; renderFw(); });
  e.querySelectorAll('[data-art]').forEach(x => x.onclick = () => go('fw/guide/' + x.dataset.art));
  e.querySelectorAll('[data-gc]').forEach(x => x.onclick = () => { FWB.q = ''; const s = $('fwQ'); FWB.cat = x.dataset.gc; renderFw(); });
  const q = $('fwQ');
  if (q) { q.oninput = () => { FWB.q = q.value; FWB.cat = null; const p = q.selectionStart; renderFw(); const n = $('fwQ'); n.focus(); n.setSelectionRange(p, p); }; }
  const b = $('fwBle'); if (b) b.onclick = fwBleFind;
  const st = $('fwBleStart'); if (st) st.onclick = fwBleStart;
  const us = $('fwUsbSetup'); if (us) us.onclick = () => serialConnect();
  e.querySelectorAll('[data-fm]').forEach(x => x.onclick = () => { fwBle.mode = x.dataset.fm; renderFw(); });
  enterAnim('vFw');
  if (FWB.tab === 'install') fwLoadWebTools();
}
function fwBoardsHtml() {
  const B = fwBoard();
  return '<div class="card"><h3 style="margin:0">1. เลือกบอร์ด</h3><div class="bd">' + BOARDS.map(b => '<div class="' + (FWB.board === b[0] ? 'on' : '') + '" data-fb="' + b[0] + '"><i>' + b[1] + '</i><b>' + b[2] + '</b>' + b[3] + '</div>').join('') + '</div>' +
    '<div class="small muted" style="margin-top:8px">' + (B ? 'เวอร์ชันล่าสุด <b style="color:var(--text)">' + esc(B.version) + '</b>' + (MAN.notes ? ' · ' + esc(MAN.notes) : '') : 'กำลังโหลดรายการเฟิร์มแวร์…') + '</div></div>';
}
function fwInstallHtml() {
  const B = fwBoard(), cs = FWB.board === 'cloudscreen';
  let h = fwBoardsHtml() + '<div class="card"><h3 style="margin:0">2. เลือกวิธีติดตั้ง</h3>';
  h += '<div class="meth"><h4>🔵 ผ่านบลูทูธ <span class="pill on" style="font-size:11px">ง่ายสุด</span></h4><div class="small muted">' + (cs ? 'จอ Cloud Screen ไม่มีบลูทูธ ใช้ปุ่ม UPDATE บนจอ (SETUP → PAIRING) หรือปุ่ม ⬆ ในหน้าอุปกรณ์ของฉัน หรือ USB ด้านล่าง' : '<b style="color:var(--text)">เฉพาะบอร์ดที่ติดตั้ง BATTLAB แล้ว</b> · ทำจากมือถือได้ ไม่ต้องต่อสาย') + '</div>' +
    (cs ? '' : fwBleHtml()) + '</div>';
  h += '<div class="meth"><h4>🔌 ผ่านสาย USB</h4><div class="small muted"><b style="color:var(--text)">บอร์ดใหม่ต้องใช้วิธีนี้ครั้งแรก</b> · Chrome / Edge บนคอมพิวเตอร์ · ถ้าถาม "Erase device" ให้เลือกล้างเฉพาะติดตั้งครั้งแรก (ไม่ล้าง = เก็บค่าที่ตั้งไว้)</div>' +
    (B && B.webtools ? '<esp-web-install-button manifest="' + esc(FW_BASE + B.webtools) + '"><button slot="activate">เชื่อมต่อบอร์ดและติดตั้ง</button><span slot="unsupported" class="small" style="color:var(--warn)">เบราว์เซอร์นี้ติดตั้งผ่าน USB ไม่ได้ ใช้ Chrome หรือ Edge บนคอมพิวเตอร์</span><span slot="not-allowed" class="small" style="color:var(--warn)">ต้องเปิดผ่าน https</span></esp-web-install-button>'
      : '<div class="small muted">ชุดติดตั้งของเวอร์ชันนี้ยังไม่พร้อม ใช้ไฟล์ .bat ในแท็บดาวน์โหลด</div>') +
    '<div class="small muted" style="margin-top:8px">เชื่อมต่อไม่ได้: กด BOOT ค้าง แล้วกด RESET 1 ครั้ง ปล่อย BOOT แล้วลองใหม่ · ใช้สาย USB ที่ส่งข้อมูลได้</div></div>';
  h += '<div class="meth"><h4>🖥️ ตั้งค่าบอร์ดผ่าน USB</h4><div class="small muted">บอร์ดที่ติดตั้ง BATTLAB 2.8.1 แล้ว · ตั้ง Wi-Fi บ้าน เพิ่มเข้าบัญชี เชื่อม BMS ได้จากคอมพิวเตอร์ ไม่ต้องใช้มือถือ' + (cs ? ' · จอ Cloud Screen: ใช้ปุ่มติดตั้งด้านบน แล้วกด Connect to Wi-Fi' : '') + '</div>' + (cs ? '' : '<button class="soft" id="fwUsbSetup" style="margin-top:6px">เชื่อมต่อบอร์ดผ่าน USB</button>') + '</div>';
  h += '<div class="small muted" style="margin-top:10px">ติดตั้งผ่าน USB เสร็จ หน้าต่างจะถาม Wi-Fi บ้านต่อทันที (Connect to Wi-Fi) แล้วกด Visit device เพื่อเพิ่มเข้าบัญชีในคลิกเดียว · <a href="#fw/guide/first-pc">ดูขั้นตอน</a></div>';
  h += '<div class="tiny muted" style="margin-top:10px">iPhone: ใช้แอป Bluefy สำหรับบลูทูธ · ติดตั้งผ่าน USB ต้องใช้คอมพิวเตอร์</div></div>';
  return h;
}
let wtLoaded = false;
function fwLoadWebTools() {
  if (wtLoaded || !document.querySelector('esp-web-install-button')) return;
  wtLoaded = true;
  const s = document.createElement('script');
  s.type = 'module';
  s.src = 'https://unpkg.com/esp-web-tools@10.0.1/dist/web/install-button.js?module';
  document.head.appendChild(s);
}
// ---------------------------------------------------------- Bluetooth OTA
const fwBle = {dev: null, cfg: null, mode: 'ble', st: '', pct: 0, msg: '', busy: false, done: false};
function fwBleHtml() {
  const f = fwBle, B = f.cfg ? MAN && MAN.boards && MAN.boards[f.cfg.board || FWB.board] : fwBoard();
  if (!f.dev) return '<button id="fwBle" style="margin-top:6px">ค้นหาบอร์ดผ่านบลูทูธ</button>' + (f.msg ? '<div class="small" style="color:var(--warn);margin-top:6px">' + esc(f.msg) + '</div>' : '');
  const cur = (f.cfg && f.cfg.fw) || '?', canBle = !!(f.cfg && f.cfg.bleota), newer = B && verNum(B.version) > verNum(cur);
  let h = '<div class="info" style="margin-top:10px"><div><span>บอร์ด</span><b>' + esc(f.dev.name || 'BATTLAB') + '</b></div><div><span>เวอร์ชันตอนนี้</span><b>' + esc(cur) + '</b></div><div><span>อัปเดตเป็น</span><b style="color:var(--good)">' + (B ? esc(B.version) : '--') + '</b></div></div>';
  h += '<div class="small muted" style="margin:10px 0 6px">วิธีส่งไฟล์</div><div class="chips" style="flex-wrap:wrap;overflow:visible">' +
    (canBle ? '<span class="chip' + (f.mode === 'ble' ? ' on' : '') + '" data-fm="ble">บลูทูธล้วน · ไม่ต้องมี Wi-Fi</span>' : '') +
    '<span class="chip' + (f.mode === 'wifi' || !canBle ? ' on' : '') + '" data-fm="wifi">ให้บอร์ดโหลดเองผ่าน Wi-Fi</span></div>';
  if (f.st) {
    const L = 2 * Math.PI * 50;
    h += '<div style="display:flex;align-items:center;gap:16px;margin-top:12px;position:relative"><svg width="110" height="110" viewBox="0 0 120 120" style="transform:rotate(-90deg)"><circle cx="60" cy="60" r="50" fill="none" stroke="var(--line)" stroke-width="10"/><circle cx="60" cy="60" r="50" fill="none" stroke="' + (f.done ? 'var(--good)' : 'var(--accent)') + '" stroke-width="10" stroke-linecap="round" stroke-dasharray="' + (L * f.pct / 100) + ' 999" style="transition:stroke-dasharray .4s;filter:drop-shadow(0 0 6px var(--accent))"/></svg>' +
      '<div><b style="font-size:24px">' + (f.done ? '✓' : f.pct + '%') + '</b><div class="small muted">' + esc(f.st) + '</div></div><canvas id="fwConf" style="position:absolute;inset:-20px;pointer-events:none;width:calc(100% + 40px);height:150px"></canvas></div>';
  }
  if (f.msg) h += '<div class="small" style="color:' + (f.done ? 'var(--good)' : 'var(--warn)') + ';margin-top:8px">' + esc(f.msg) + '</div>';
  h += '<div class="row wrap" style="margin-top:10px"><button id="fwBleStart" ' + (f.busy || !B ? 'disabled' : '') + '>' + (f.busy ? 'กำลังอัปเดต…' : newer ? 'อัปเดตเป็น ' + esc(B ? B.version : '') : 'ติดตั้งซ้ำ ' + esc(B ? B.version : '')) + '</button><button class="ghost" id="fwBle" ' + (f.busy ? 'disabled' : '') + '>เลือกบอร์ดอื่น</button></div>';
  h += '<div class="small" style="color:var(--warn);margin-top:8px">วางมือถือใกล้บอร์ด และอย่าปิดหน้านี้จนเสร็จ · ถ้าขาดกลางทาง บอร์ดยังใช้เวอร์ชันเดิมได้ตามปกติ</div>';
  return h;
}
async function fwBleFind() {
  const f = fwBle;
  f.msg = '';
  if (!navigator.bluetooth) { f.msg = 'เบราว์เซอร์นี้ใช้บลูทูธไม่ได้ ใช้ Chrome บน Android / คอมพิวเตอร์ หรือแอป Bluefy บน iPhone'; renderFw(); return; }
  try {
    const dev = await navigator.bluetooth.requestDevice({filters: [{services: [BSV]}], optionalServices: [BSV]});
    const gatt = await dev.gatt.connect();
    const svc = await gatt.getPrimaryService(BSV);
    f.dev = dev; f.svc = svc; f.st = ''; f.pct = 0; f.done = false;
    const v = await (await svc.getCharacteristic(BCH(2))).readValue();
    f.cfg = JSON.parse(new TextDecoder().decode(v));
    if (f.cfg.board && MAN && MAN.boards[f.cfg.board]) FWB.board = f.cfg.board;
    f.mode = f.cfg.bleota ? 'ble' : 'wifi';
    dev.addEventListener('gattserverdisconnected', () => { if (f.busy && !f.done) { f.busy = false; f.msg = 'บลูทูธหลุด ลองใหม่อีกครั้ง (บอร์ดยังใช้เวอร์ชันเดิม)'; renderFw(); } });
  } catch (e) { if (!/cancel/i.test(e.message)) f.msg = 'เชื่อมต่อไม่สำเร็จ: ' + e.message; }
  renderFw();
}
async function fwSha(buf) { const h = await crypto.subtle.digest('SHA-256', buf); return [...new Uint8Array(h)].map(b => b.toString(16).padStart(2, '0')).join(''); }
async function fwBleStart() {
  const f = fwBle, B = MAN && MAN.boards[FWB.board];
  if (!f.svc || !B) return;
  if (!confirm('อัปเดตบอร์ด ' + (f.dev.name || '') + ' เป็น ' + B.version + '?\nระหว่างอัปเดตบอร์ดจะหยุดอ่าน BMS ชั่วคราว แล้วรีสตาร์ทเอง')) return;
  f.busy = true; f.done = false; f.msg = ''; f.pct = 0;
  const set = (st, pct) => { f.st = st; if (pct != null) f.pct = pct; renderFw(); };
  try {
    if (f.mode === 'wifi' || !f.cfg.bleota) {
      if (!f.cfg.conn) throw new Error('บอร์ดยังไม่ได้ต่อ Wi-Fi บ้าน · ตั้ง Wi-Fi ก่อน หรือใช้ USB');
      const cmd = await f.svc.getCharacteristic(BCH(6));
      await cmd.writeValueWithResponse(new TextEncoder().encode(JSON.stringify({id: Date.now() % 100000, cmd: 'ota', args: {url: FW_BASE + B.url}})));
      set('ส่งคำสั่งแล้ว บอร์ดกำลังดาวน์โหลดผ่าน Wi-Fi (~1 นาที)', 100);
      f.done = true; f.busy = false; f.msg = 'บอร์ดจะรีสตาร์ทเองเมื่อเสร็จ ดูความคืบหน้าบนจอ CYD หรือหน้าอุปกรณ์';
      renderFw(); return;
    }
    set('กำลังดาวน์โหลดไฟล์', 0);
    const buf = await (await fetch(FW_BASE + B.url, {cache: 'no-store'})).arrayBuffer();
    const sha = await fwSha(buf);
    if (B.sha256 && sha !== B.sha256) throw new Error('ไฟล์ที่ดาวน์โหลดไม่ตรงกับ SHA-256 ในรายการ');
    const ctl = await f.svc.getCharacteristic(BCH(8)), dat = await f.svc.getCharacteristic(BCH(9));
    let last = null;
    await ctl.startNotifications();
    ctl.addEventListener('characteristicvaluechanged', ev => { try { last = JSON.parse(new TextDecoder().decode(ev.target.value)); } catch (e) {} });
    const wait = async (want, ms) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { if (last && last.st === 'error') throw new Error('บอร์ด: ' + last.msg); if (last && want.includes(last.st)) return last; await new Promise(r => setTimeout(r, 100)); } throw new Error('บอร์ดไม่ตอบ'); };
    last = null;
    await ctl.writeValueWithResponse(new TextEncoder().encode(JSON.stringify({begin: buf.byteLength, sha})));
    await wait(['ready'], 15000);
    const u8 = new Uint8Array(buf), N = 240, t0 = Date.now();
    let fast = !!dat.writeValueWithoutResponse;
    for (let off = 0, k = 0; off < u8.length; off += N, k++) {
      const chunk = u8.slice(off, off + N);
      try {
        if (fast && k % 6 !== 5) await dat.writeValueWithoutResponse(chunk);
        else await dat.writeValueWithResponse(chunk);
      } catch (e) {
        if (k === 0 && fast) { fast = false; await dat.writeValueWithResponse(chunk); } // phone refused the fast mode: slower but safe
        else throw e;
      }
      if (last && last.st === 'error') throw new Error('บอร์ด: ' + last.msg);
      if (k % 20 === 0) {
        const pct = Math.floor(off * 100 / u8.length), sec = (Date.now() - t0) / 1000, rate = off / 1024 / Math.max(.5, sec), left = (u8.length - off) / 1024 / Math.max(.1, rate);
        set('ส่งผ่านบลูทูธ ' + Math.round(off / 1024) + ' / ' + Math.round(u8.length / 1024) + ' KB · ' + rate.toFixed(1) + ' KB/s · เหลือ ~' + Math.ceil(left / 60) + ' นาที', pct);
      }
    }
    set('บอร์ดกำลังตรวจไฟล์', 99);
    last = null;
    await ctl.writeValueWithResponse(new TextEncoder().encode(JSON.stringify({end: 1})));
    await wait(['done'], 20000);
    f.done = true; f.busy = false; f.msg = 'อัปเดตสำเร็จ บอร์ดกำลังรีสตาร์ท (~20 วินาที)';
    set('เสร็จแล้ว', 100);
    fwConfetti();
  } catch (e) {
    f.busy = false;
    f.msg = 'อัปเดตไม่สำเร็จ: ' + e.message + ' · บอร์ดยังใช้เวอร์ชันเดิม ลองใหม่ได้';
    renderFw();
  }
}
function fwConfetti() {
  const cv = $('fwConf'); if (!cv) return;
  const r = cv.getBoundingClientRect(), x = cv.getContext('2d'); cv.width = r.width * 2; cv.height = r.height * 2; x.scale(2, 2);
  const C = ['#36c4a8', '#e8a040', '#4f94e8', '#40c878'];
  const P = Array.from({length: 70}, () => ({x: 60, y: 70, vx: (Math.random() - .3) * 8, vy: -Math.random() * 7 - 2, c: C[Math.random() * 4 | 0], a: Math.random() * 6}));
  let n = 0;
  (function f() { x.clearRect(0, 0, r.width, r.height); P.forEach(p => { p.vy += .25; p.x += p.vx; p.y += p.vy; p.a += .2; x.save(); x.translate(p.x, p.y); x.rotate(p.a); x.fillStyle = p.c; x.fillRect(-3, -2, 6, 4); x.restore(); }); if (++n < 80) requestAnimationFrame(f); else x.clearRect(0, 0, r.width, r.height); })();
}
// ---------------------------------------------------------------- downloads
function fwDlHtml() {
  if (!MAN) return '<div class="card"><i class="sk" style="width:50%"></i><i class="sk"></i><i class="sk" style="width:70%"></i></div>';
  const H = (MAN.history && MAN.history.length ? MAN.history : [{version: MAN.boards.s3.version, date: (MAN.updated || '').slice(0, 10), notes: MAN.notes, zip: MAN.zip, boards: Object.fromEntries(Object.entries(MAN.boards).map(([k, v]) => [k, {app: v.url, install: v.install && v.install.url}]))}]);
  const sel = H.find(x => x.version === FWB.dlv) || H[0];
  let h = '<div class="card"><div class="chips" style="flex-wrap:wrap;overflow:visible">' + H.map((x, i) => '<span class="chip' + (x === sel ? ' on' : '') + '" data-dv="' + esc(x.version) + '">' + esc(x.version) + (i ? '' : ' ล่าสุด') + '</span>').join('') + '</div>' +
    '<div class="small muted" style="margin-top:8px">' + esc(sel.date || '') + (sel.notes ? ' · ' + esc(sel.notes) : '') + '</div>' +
    (sel.zip ? '<a class="dlbig" href="' + esc(FW_BASE + sel.zip.url) + '" download>⬇️ ดาวน์โหลดชุดเต็ม (.zip) · ' + Math.round(sel.zip.size / 1048576) + ' MB</a><div class="tiny muted" style="margin-top:6px">มีไฟล์ .bat สำหรับ Windows ภาพหน้าจอ และคู่มือ</div>' : '<div class="tiny muted" style="margin-top:8px">เวอร์ชันนี้ไม่มีชุดเต็มบนเว็บ ดาวน์โหลดไฟล์ .bin ด้านล่าง</div>') + '</div>';
  BOARDS.forEach(b => {
    const f = sel.boards && sel.boards[b[0]]; if (!f) return;
    h += '<div class="card"><h3 style="margin:0 0 4px">' + b[1] + ' ' + b[2] + '</h3>' +
      (f.install ? '<div class="fw"><div class="l"><b>ติดตั้งใหม่ (merged)</b><span>เขียนที่ 0x0 · ล้างค่าเดิม</span></div><a href="' + esc(FW_BASE + f.install) + '" download>.bin</a></div>' : '') +
      '<div class="fw"><div class="l"><b>อัปเดต (app)</b><span>เก็บค่าเดิม · เขียนที่ 0x10000</span></div><a href="' + esc(FW_BASE + f.app) + '" download>.bin</a></div></div>';
  });
  return h + '<div class="tiny muted" style="margin:4px 4px 10px">เก็บไว้ 3 เวอร์ชันล่าสุด · ไฟล์เก่ากว่านั้นดูได้ใน GitHub</div>';
}
document.addEventListener('click', e => { const c = e.target.closest('[data-dv]'); if (c) { FWB.dlv = c.dataset.dv; renderFw(); } });
// ------------------------------------------------------------------- guide
function fwGuideHtml() {
  const q = (FWB.q || '').trim().toLowerCase();
  let h = '<input class="srch" id="fwQ" placeholder="🔍 ค้นหา เช่น RS485, Wi-Fi, DALY" value="' + esc(FWB.q || '') + '">';
  if (q || FWB.cat) {
    const L = GART.filter(a => FWB.cat ? a.c === FWB.cat : (a.t + ' ' + a.s.map(x => x.join(' ')).join(' ')).toLowerCase().includes(q));
    const cat = GCAT.find(c => c[0] === FWB.cat);
    h += '<div class="card"><div class="row"><h3 style="margin:0">' + (cat ? cat[1] + ' ' + cat[2] : 'ผลการค้นหา') + '</h3><button class="ghost nf" data-gc="">ทุกหมวด</button></div>' +
      (L.length ? L.map(a => '<div class="devrow" data-art="' + a.id + '" style="cursor:pointer"><div class="l"><b>' + esc(a.t) + '</b><span class="tiny muted">' + esc(a.s[0][1]).slice(0, 80) + '…</span></div><span class="muted">›</span></div>').join('') : '<div class="small muted">ไม่พบหัวข้อ ลองคำอื่น</div>') + '</div>';
    return h;
  }
  h += '<div class="cat">' + GCAT.map((c, i) => '<div data-gc="' + c[0] + '" style="cursor:pointer"><i>' + c[1] + '</i><b>' + (i + 1) + '. ' + c[2] + '</b><span>' + c[3] + ' · ' + GART.filter(a => a.c === c[0]).length + ' หัวข้อ</span></div>').join('') + '</div>' +
    '<div class="tiny muted" style="margin-top:10px">เปิดได้โดยไม่ต้องล็อกอิน</div>';
  return h;
}
function fwArtHtml(id) {
  const a = GART.find(x => x.id === id);
  if (!a) return fwGuideHtml();
  const cat = GCAT.find(c => c[0] === a.c), sib = GART.filter(x => x.c === a.c), k = sib.indexOf(a);
  let h = '<div class="crumb"><a href="#fw/guide">คู่มือ</a> › ' + esc(cat[2]) + '</div><h2 style="margin:0 0 8px;font-size:21px">' + esc(a.t) + '</h2>';
  if (a.img) h += '<div class="card" style="padding:8px"><a href="' + esc(a.img) + '" target="_blank" rel="noopener"><img src="' + esc(a.img) + '" alt="' + esc(a.t) + '" style="width:100%;border-radius:10px;display:block"></a><div class="tiny muted" style="margin-top:4px">แตะรูปเพื่อขยาย</div></div>';
  h += a.s.map((s, i) => '<div class="stp"><em>' + (i + 1) + '</em><div><b>' + esc(s[0]) + '</b><div class="small muted">' + esc(s[1]) + '</div></div></div>').join('');
  h += '<div class="btnrow">' + (k > 0 ? '<button class="ghost" data-art="' + sib[k - 1].id + '">‹ ' + esc(sib[k - 1].t) + '</button>' : '<span></span>') + (k < sib.length - 1 ? '<button class="ghost" data-art="' + sib[k + 1].id + '">' + esc(sib[k + 1].t) + ' ›</button>' : '<span></span>') + '</div>';
  return h;
}
