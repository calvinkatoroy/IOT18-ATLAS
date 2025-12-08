# ATLAS - Dual-Factor Attendance System
## ESP-NOW Master-Slave dengan RTOS

Sistem absensi dengan validasi ganda: RFID Card + BLE Service UUID menggunakan ESP32, ESP-NOW, dan Blynk.

---

## 📋 Arsitektur Sistem

### **SLAVE (RFID + BLE Scanner)**
- Membaca kartu RFID
- Scan BLE beacon untuk validasi Service UUID
- Komunikasi dengan Master via ESP-NOW
- Mode:
  - **DEFAULT**: Validasi otomatis (RFID + BLE)
  - **REGISTER**: Kirim UID kartu ke Master

### **MASTER (Controller + Blynk)**
- Menerima data absensi dari Slave
- Interface Blynk untuk monitoring dan registrasi
- Kontrol mode Slave (DEFAULT/REGISTER)
- Menggabungkan NPM + Service UUID dengan UID kartu

---

## 🔧 Hardware Requirements

### Slave
- ESP32 DevKit
- MFRC522 RFID Reader
- Koneksi:
  - SS: GPIO 21
  - RST: GPIO 22
  - SCK: GPIO 18
  - MOSI: GPIO 23
  - MISO: GPIO 19

### Master
- ESP32 DevKit
- Koneksi WiFi

---

## 📦 Library Dependencies

Tambahkan di `platformio.ini`:

```ini
[env:slave]
platform = espressif32
board = esp32doit-devkit-v1
framework = arduino
lib_deps = 
    miguelbalboa/MFRC522
    h2zero/NimBLE-Arduino
    bblanchon/ArduinoJson
monitor_speed = 115200
build_src_filter = +<slave_main.cpp>

[env:master]
platform = espressif32
board = esp32doit-devkit-v1
framework = arduino
lib_deps = 
    blynkkk/Blynk
    bblanchon/ArduinoJson
monitor_speed = 115200
build_src_filter = +<master_main.cpp>
```

---

## 🚀 Setup Instructions

### 1. **Konfigurasi MAC Address**

#### Slave (`slave_main.cpp` line 27):
```cpp
// Ganti dengan MAC Address ESP32 Master
uint8_t masterMAC[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
```

#### Master (`master_main.cpp` line 30):
```cpp
// Ganti dengan MAC Address ESP32 Slave
uint8_t slaveMAC[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
```

**Cara mendapatkan MAC Address:**
```cpp
Serial.println(WiFi.macAddress());
```

### 2. **Konfigurasi WiFi (Master)**

Edit `master_main.cpp` line 18-19:
```cpp
const char* ssid = "NamaWiFiAnda";
const char* password = "PasswordWiFi";
```

### 3. **Konfigurasi Blynk (Master)**

Edit `master_main.cpp` line 10-12:
```cpp
#define BLYNK_TEMPLATE_ID "YourTemplateID"
#define BLYNK_TEMPLATE_NAME "ATLAS Master"
#define BLYNK_AUTH_TOKEN "YourAuthToken"
```

**Blynk Setup:**
- Buat template baru di Blynk Console
- Tambahkan widget:
  - **V0**: Switch (Register Mode)
  - **V1**: Text Input (NPM)
  - **V2**: Text Input (Service UUID)
  - **V3**: Terminal (Log)
  - **V4**: Value Display (Attendance Counter)
  - **V5**: Label (Last Attendance Info)

---

## 📱 Cara Penggunaan

### **Mode DEFAULT (Validasi Absensi)**

1. Pastikan switch Register Mode (V0) di Blynk = OFF
2. Slave akan otomatis scan kartu
3. Jika kartu terdaftar:
   - Scan BLE beacon
   - Validasi Service UUID
   - Kirim hasil ke Master
4. Master tampilkan di Blynk

**Output Slave:**
```
╔════════════════════════════════════════╗
║         KARTU TERDETEKSI               ║
╚════════════════════════════════════════╝
UID: A1 B2 C3 D4

[VALIDATION] Checking database...
NPM: 2306242350
Stored UUID: 00000000-0000-0000-0000-002306242350

[BLE] Scanning...
[BLE] Found: 00000000-0000-0000-0000-002306242350, RSSI: -65

╔════════════════════════════════════════╗
║         HASIL VALIDASI                 ║
╠════════════════════════════════════════╣
║ Status: ✓✓✓ VALID ✓✓✓                 ║
║ RSSI  : -65 dBm                        ║
║ ✓ Kartu: OK                            ║
║ ✓ BLE  : OK                            ║
║ ABSENSI DITERIMA                       ║
╚════════════════════════════════════════╝
```

### **Mode REGISTER (Registrasi Kartu Baru)**

1. Di Blynk:
   - Switch Register Mode (V0) = ON
   - Input NPM di V1 (contoh: `2306242350`)
   - Input Service UUID di V2 (contoh: `00000000-0000-0000-0000-002306242350`)
2. Tap kartu di Slave
3. Slave kirim UID ke Master
4. Master gabungkan NPM + UUID + UID
5. Master kirim data lengkap ke Slave
6. Slave simpan di Preferences
7. Mode otomatis kembali ke DEFAULT

**Flow:**
```
Blynk (NPM + UUID) → Master → (sinyal register) → Slave
                                                      ↓
                                                  Tap Kartu
                                                      ↓
Master ← (UID kartu) ← Slave
   ↓
Gabungkan data (NPM|UUID)
   ↓
Master → (data lengkap) → Slave → Simpan ke Preferences
```

---

## 🗂️ Format Data Storage (Preferences)

### Slave
- **Namespace**: `atlas_slave`
- **Key**: UID tanpa spasi (contoh: `A1B2C3D4`)
- **Value**: `npm|serviceUUID`
  - Contoh: `2306242350|00000000-0000-0000-0000-002306242350`

### Master (Backup)
- **Namespace**: `atlas_master`
- **Key**: UID tanpa spasi
- **Value**: `npm|serviceUUID`
- **Extra**: `att_count` untuk counter absensi

---

## 🔄 ESP-NOW Message Protocol

### Message Types
```cpp
0x01  MSG_REGISTER_MODE     Master → Slave: Masuk mode register
0x02  MSG_DEFAULT_MODE      Master → Slave: Keluar mode register
0x10  MSG_CARD_SCANNED      Slave → Master: UID kartu (saat register)
0x20  MSG_ATTENDANCE_OK     Slave → Master: Absensi valid
0x21  MSG_ATTENDANCE_FAIL   Slave → Master: Absensi ditolak
0x30  MSG_REGISTER_DATA     Master → Slave: Data registrasi lengkap
```

### Message Structure
```cpp
struct ESPNowMessage {
  uint8_t type;      // Message type (0x01 - 0x30)
  char payload[200]; // Data payload
};
```

---

## 🧵 RTOS Architecture

### **Slave Tasks**
1. **rfidScanTask** (Core 0, Priority 2)
   - Scan RFID reader
   - Push card data ke queue
   
2. **validationProcessTask** (Core 1, Priority 1)
   - Process card queue
   - Handle register/validation mode
   - BLE scanning
   - ESP-NOW communication

### **Master Tasks**
1. **blynkManagementTask** (Core 0, Priority 2)
   - Blynk.run()
   - Process attendance queue
   - Process registration queue
   - Update Blynk widgets
   
2. **espnowReceiveTask** (Core 1, Priority 1)
   - Handle ESP-NOW callbacks
   - Lightweight monitoring task

### **Synchronization**
- **Mutexes**: 
  - `prefsMutex`: Protect Preferences access
  - `bleMutex`: Protect BLE scan state
  - `wifiMutex`: Protect WiFi/Blynk operations
  - `dataMutex`: Protect shared data
- **Queues**:
  - `cardQueue`: RFID scan → validation
  - `attendanceQueue`: ESP-NOW → Blynk
  - `registrationQueue`: ESP-NOW → Blynk

---

## 🐛 Troubleshooting

### Slave tidak menerima sinyal dari Master
- Cek MAC Address benar
- Pastikan kedua ESP32 di channel WiFi sama
- Coba `esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)` di kedua device

### BLE UUID tidak terdeteksi
- Pastikan smartphone broadcast BLE beacon
- Cek format UUID sesuai (36 karakter dengan dash)
- Turunkan `RSSI_THRESHOLD` jika sinyal lemah
- Tambah `BLE_SCAN_TIME` untuk scan lebih lama

### Master tidak connect ke Blynk
- Cek WiFi credentials
- Pastikan `BLYNK_AUTH_TOKEN` benar
- Cek koneksi internet
- Restart ESP32 Master

### Data tidak tersimpan
- Cek Preferences namespace sama dengan kode
- Pastikan mutex digunakan sebelum akses Preferences
- Monitor Serial untuk error messages

---

## 📊 Performance Notes

- **Memory**: 
  - Slave: ~120KB program, ~30KB dynamic
  - Master: ~180KB program (dengan Blynk), ~40KB dynamic
- **Power**: WiFi power save dinonaktifkan untuk ESP-NOW reliability
- **Latency**: 
  - RFID scan → validation: ~3-4 detik (termasuk BLE scan)
  - ESP-NOW transmission: <50ms
  - Blynk update: ~100-200ms

---

## 📝 Notes

- Gunakan NimBLE (bukan ESP32 BLE Arduino) untuk memory efficiency
- ESP-NOW dan WiFi bisa jalan bersamaan (Master)
- Slave bisa ditambah jika diperlukan (multi-slave to single master)
- Data Preferences persisten meski ESP32 restart

---

## 🔐 Security Considerations

- ESP-NOW tanpa enkripsi (untuk enkripsi, set `peerInfo.encrypt = true` dan tambahkan PMK/LMK)
- Service UUID sebagai identifier kedua (dual-factor)
- Validasi RSSI untuk deteksi proximity

---

## 📞 Support

Jika ada masalah, cek:
1. Serial Monitor output (115200 baud)
2. Blynk Terminal widget (V3)
3. MAC Address konfigurasi
4. Library versions compatibility

---

**Created**: December 2025  
**Platform**: ESP32 + PlatformIO + Arduino Framework  
**RTOS**: FreeRTOS (native ESP-IDF)
