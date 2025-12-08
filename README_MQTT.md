# ATLAS - MQTT Version

## 🔄 Perubahan dari ESP-NOW ke MQTT

### Broker: HiveMQ
- Server: `broker.hivemq.com`
- Port: `1883`
- No authentication required

### MQTT Topics

| Topic | Direction | Format | Description |
|-------|-----------|--------|-------------|
| `atlas/mode` | Master → Slave | `register` / `default` | Mode signal |
| `atlas/register` | Master → Slave | `uid\|npm\|serviceUUID` | Registration data |
| `atlas/card` | Slave → Master | `uid` | Scanned card UID |
| `atlas/attendance` | Slave → Master | `uid\|npm\|rssi\|valid` | Attendance result (valid: 1/0) |

## 📋 Cara Upload

### Master (`main.cpp`)
```bash
pio run --target upload
```

### Slave (`main2.txt`)
1. Rename `main.cpp` menjadi `main_backup.cpp`
2. Rename `main2.txt` menjadi `main.cpp`
3. Upload:
```bash
pio run --target upload
```

## 🔧 Konfigurasi WiFi

Ubah di kedua file (Master & Slave):
```cpp
const char* ssid = "Alga";
const char* password = "bonifasius1103";
```

## 📊 Flow Registrasi

1. **Blynk**: User input NPM + UUID → toggle switch ON
2. **Master**: Publish `register` ke `atlas/mode`
3. **Slave**: Receive mode → wait for card
4. **Slave**: Card scanned → publish UID ke `atlas/card`
5. **Master**: Receive UID → combine with NPM+UUID
6. **Master**: Publish `uid|npm|uuid` ke `atlas/register`
7. **Slave**: Store to Preferences → done

## 📊 Flow Absensi

1. **Slave**: Card scanned → check Preferences
2. **Slave**: BLE scan → validate UUID
3. **Slave**: Publish `uid|npm|rssi|1` ke `atlas/attendance` (jika valid)
4. **Master**: Update counter + Blynk display

## 🎯 Keuntungan MQTT vs ESP-NOW

✅ Tidak perlu MAC address (lebih mudah setup)  
✅ Bisa monitoring via MQTT client (MQTT Explorer, etc)  
✅ Lebih stabil dengan WiFi router  
✅ Bisa tambah device lebih banyak  
✅ Debug lebih mudah dengan topic subscription  

## 🐛 Troubleshooting

**MQTT tidak connect:**
- Cek WiFi credentials
- Cek koneksi internet (HiveMQ butuh internet)
- Ganti broker: `broker.emqx.io` atau `test.mosquitto.org`

**Slave tidak terima mode:**
- Cek subscription di slave: `atlas/mode`
- Monitor dengan MQTT Explorer

**Master tidak terima data:**
- Cek subscription di master: `atlas/card`, `atlas/attendance`
- Pastikan payload format benar
