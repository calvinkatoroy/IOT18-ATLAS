# ATLAS (Anti-cheat Tracking & Location Attendance System)

**Group 18 - IoT & Real-Time Systems Project**

## Group Members

| Name | NPM | Role |
| :--- | :--- | :--- |
| **Aliya Rizqiningrum Salamun** | 2306161813 | Concept Developer, Report Paper, PPT Maker |
| **Bonifasius Raditya Pandu H.** | 2306242350 | Hardware Designer, Code Writer, Circuit Maker |
| **Calvin Wirathama Katoroy** | 2306242395 | Team Leader, Adv. Conceptor, Hardware/Code |
| **Zhafira Zahra Alfarisy** | 2306250636 | Concept Developer, Report Paper, PPT Maker, README |

---

## i. Introduction

Masalah utama yang melatarbelakangi proyek ini adalah kerentanan sistem presensi manual terhadap praktik "titip absen" (*proxy attendance*), di mana mahasiswa menandatangani daftar hadir untuk teman yang tidak hadir. Praktik ini menyebabkan ketidakakuratan data kehadiran, ketidakadilan bagi mahasiswa yang rajin, dan penurunan integritas akademik.

Solusi presensi elektronik sederhana (hanya RFID) masih memiliki celah karena kartu dapat dipinjamkan. Oleh karena itu, **ATLAS** hadir sebagai solusi berbasis IoT dengan arsitektur *client-server* yang menerapkan mekanisme Dual-Factor Authentication. Sistem ini menggabungkan:
1.  **Physical Authentication:** Validasi kepemilikan kartu RFID (KTM).
2.  **Digital Proximity Detection:** Validasi keberadaan fisik melalui pemindaian sinyal BLE (*Bluetooth Low Energy*) dari smartphone mahasiswa secara bersamaan.

Sistem ini dirancang untuk memastikan kehadiran fisik mahasiswa secara valid dan mengirimkan data secara *real-time* ke dashboard monitoring dosen.

## ii. Implementation

Pengembangan ATLAS dibagi menjadi perancangan perangkat keras dan perangkat lunak yang terintegrasi.

### Hardware Design
Sistem menggunakan arsitektur terpusat (*Centralized Architecture*) dengan topologi Star, terdiri dari dua node utama:
* **Client Node (Slave):** Berfungsi sebagai unit *scanning* di ruang kelas. Menggunakan **ESP32 DevKit V1** yang terhubung dengan **RFID Reader RC522** via SPI.
* **Admin Node (Master):** Berfungsi sebagai *gateway* dan koordinator. Menggunakan **ESP32 DevKit V1** untuk menerima data dari Client dan meneruskannya ke Cloud (Blynk).

**Wiring RFID ke ESP32 (SPI Interface):**
* **SDA (SS):** GPIO 21
* **SCK:** GPIO 18
* **MOSI:** GPIO 23
* **MISO:** GPIO 19
* **RST:** GPIO 22

### Software Development
Perangkat lunak dikembangkan menggunakan **PlatformIO** dengan framework Arduino dan berbasis **FreeRTOS** untuk manajemen *multitasking*.

**Library Dependencies:**
* `PubSubClient`: Untuk protokol MQTT.
* `NimBLE-Arduino`: Untuk scanning BLE yang efisien memori.
* `MFRC522`: Driver modul RFID.
* `BlynkSimpleEsp32`: Koneksi ke aplikasi Blynk IoT.
* `Preferences`: Penyimpanan database lokal (NVS) pada flash memory.

**Logic Alur Program:**
1.  **Task Management:** Program dibagi menjadi `rfidScanTask` (baca kartu), `validationProcessTask` (logika verifikasi), dan `mqttTask` (komunikasi jaringan).
2.  **Synchronization:** Menggunakan *Queue* untuk pengiriman data UID antar-task dan *Mutex Semaphore* untuk mencegah *Race Condition* saat akses memori/MQTT.
3.  **Communication:** Seluruh pertukaran data antara Client dan Admin menggunakan protokol MQTT via WiFi.

## iii. Testing and Evaluation

### Testing Scenarios
Pengujian dilakukan dalam tiga tahapan utama:
1.  **Konektivitas & Inisialisasi:** Memastikan Client dan Admin Node terhubung ke WiFi dan Broker HiveMQ (ditandai log "Connected").
2.  **Remote Registration:** Menguji fitur pendaftaran kartu baru yang dikontrol dari aplikasi Blynk Admin, di mana data UID dari Client dikirim ke Admin untuk digabungkan dengan NPM, lalu dikembalikan ke Client untuk disimpan.
3.  **Validasi Presensi:**
    * **Kondisi Valid:** Kartu ditempelkan + Sinyal BLE UUID yang sesuai terdeteksi dalam radius < 1 meter.
    * **Kondisi Fraud:** Kartu ditempelkan tanpa adanya sinyal BLE (smartphone mati/jauh).

### Result & Evaluation
* **Berhasil:** Sistem mampu membedakan presensi sah dan percobaan kecurangan. Pada kondisi valid, status "VALID" dan RSSI dikirim ke dashboard. Pada kondisi *fraud*, sistem menolak akses dan mengirim notifikasi "Invalid/BLE Mismatch".
* **Kinerja:** FreeRTOS berhasil menangani proses *scanning* BLE yang berat secara paralel dengan koneksi MQTT tanpa *blocking*.
* **Limitasi:** Belum adanya feedback visual langsung (LCD/LED) pada Client Node dan keterbatasan skalabilitas karena database masih tersimpan lokal (Preferences).

## iv. Conclusion

Proyek ATLAS berhasil diimplementasikan sebagai solusi IoT untuk mengatasi masalah *proxy attendance*. Mekanisme verifikasi ganda (RFID + BLE) terbukti efektif menjamin kehadiran fisik mahasiswa dan mencegah titip absen.

Penggunaan arsitektur *client-server* dengan protokol MQTT dan FreeRTOS menjamin komunikasi data yang stabil, *real-time*, dan responsif antara ruang kelas dan dashboard dosen. Seluruh *acceptance criteria* telah terpenuhi, termasuk fitur *fraud alert* otomatis. Untuk pengembangan selanjutnya, lebih baik ditambah visual feedback interface pada node dan penggunaan database cloud centralized untuk better scalability.

## v. References

1.  Digilab DTE, "Module 1 Introduction to SMP with RTOS," Internet of Things.
2.  Digilab DTE, "Module 2 Task Management," Internet of Things.
3.  Digilab DTE, "Module 3 Memory Management & Queue," Internet of Things.
4.  Digilab DTE, "Module 4 Deadlock & Synchronization," Internet of Things.
5.  Digilab DTE, "Module 6 Bluetooth & BLE," Internet of Things.
6.  Digilab DTE, "Module 7 MQTT, HTTP & Wi-Fi," Internet of Things.
7.  Digilab DTE, "Module 9 - IoT Platforms: Blynk and Node-RED," Internet of Things.
