/*
 * ATLAS - Proof of Concept (Simple Version) - FIXED
 * ESP32 + RFID + BLE Beacon Scanner
 * Database: Preferences.h
 */

#include <SPI.h>
#include <MFRC522.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Preferences.h>

// ======================== PIN DEFINITIONS ========================
#define SS_PIN    21
#define RST_PIN   22
#define SCK_PIN   18
#define MOSI_PIN  23
#define MISO_PIN  19

// ======================== OBJECTS ========================
MFRC522 mfrc522(SS_PIN, RST_PIN);
BLEScan* pBLEScan;
Preferences preferences;

// ======================== CONFIG ========================
#define BLE_SCAN_TIME 3        // Detik scan BLE
#define RSSI_THRESHOLD -75     // RSSI minimum
const char* PREF_NAMESPACE = "atlas_db";

// ======================== GLOBAL VARS ========================
String detectedBeaconUUID = "";
int detectedRSSI = -100;

// ======================== BLE CALLBACK ========================
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.haveServiceUUID()) {
      BLEUUID serviceUUID = advertisedDevice.getServiceUUID();
      String uuidStr = serviceUUID.toString().c_str();
      int rssi = advertisedDevice.getRSSI();
      
      Serial.print("  [BLE] UUID: ");
      Serial.print(uuidStr);
      Serial.print(" | RSSI: ");
      Serial.println(rssi);
      
      // Update jika RSSI lebih kuat
      if (rssi > detectedRSSI && rssi > RSSI_THRESHOLD) {
        detectedBeaconUUID = uuidStr;
        detectedRSSI = rssi;
      }
    }
  }
};

// ======================== SETUP ========================
void setup() {
  Serial.begin(115200);
  while (!Serial);
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║   ATLAS - Proof of Concept (Simple)   ║");
  Serial.println("║   RFID + BLE Beacon Dual-Factor Auth  ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  // Init Preferences
  preferences.begin(PREF_NAMESPACE, false);
  Serial.println("✓ Preferences initialized");
  
  // Init RFID
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  mfrc522.PCD_Init();
  Serial.println("✓ RFID RC522 initialized");
  
  // Init BLE
  BLEDevice::init("ATLAS");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  Serial.println("✓ BLE Scanner initialized");
  
  Serial.println("\n════════════════════════════════════════");
  Serial.println("Commands:");
  Serial.println("  REGISTER - Daftarkan kartu baru");
  Serial.println("  TABLE    - Lihat database");
  Serial.println("  CLEAR    - Hapus database");
  Serial.println("════════════════════════════════════════\n");
  
  int count = preferences.getInt("count", 0);
  Serial.print("Kartu terdaftar: ");
  Serial.println(count);
  Serial.println("\nSilakan tap kartu atau ketik command...\n");
}

// ======================== LOOP ========================
void loop() {
  // Cek command
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toUpperCase();
    
    if (cmd == "TABLE") {
      displayDatabase();
    } else if (cmd == "CLEAR") {
      clearDatabase();
    } else if (cmd == "REGISTER") {
      registerMode();
    }
  }
  
  // Cek RFID
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String cardUID = getCardUID();
    
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║         KARTU TERDETEKSI!              ║");
    Serial.println("╚════════════════════════════════════════╝");
    Serial.print("UID: ");
    Serial.println(cardUID);
    
    // Cek apakah kartu terdaftar
    String safeKey = cardUID;
    safeKey.replace(" ", "");
    
    if (preferences.isKey(safeKey.c_str())) {
      // Kartu terdaftar - mulai validasi
      processAttendance(cardUID, safeKey);
    } else {
      // Kartu belum terdaftar
      Serial.println("\n✗ Kartu tidak terdaftar!");
      Serial.println("Ketik 'REGISTER' untuk mendaftar.\n");
    }
    
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    delay(2000);
  }
  
  delay(100);
}

// ======================== FUNCTIONS ========================

String getCardUID() {
  String content = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) content += " 0";
    else content += " ";
    content += String(mfrc522.uid.uidByte[i], HEX);
  }
  content.toUpperCase();
  content.trim();
  return content;
}

void processAttendance(String cardUID, String safeKey) {
  // Ambil data dari database
  String value = preferences.getString(safeKey.c_str(), "");
  
  if (value.length() == 0) return;
  
  // Parse: npm|nama|beaconUUID
  int idx1 = value.indexOf('|');
  int idx2 = value.indexOf('|', idx1 + 1);
  
  String npm = value.substring(0, idx1);
  String nama = value.substring(idx1 + 1, idx2);
  String beaconUUID = value.substring(idx2 + 1);
  
  Serial.println("\n--- Data Mahasiswa ---");
  Serial.print("NPM  : ");
  Serial.println(npm);
  Serial.print("Nama : ");
  Serial.println(nama);
  Serial.print("UUID : ");
  Serial.println(beaconUUID);
  
  // STEP 1: Scan BLE
  Serial.println("\n[SCANNING BLE BEACON...]");
  detectedBeaconUUID = "";
  detectedRSSI = -100;
  
  // FIX: Gunakan pointer, bukan object
  BLEScanResults* foundDevices = pBLEScan->start(BLE_SCAN_TIME, false);
  Serial.print("Total devices found: ");
  Serial.println(foundDevices->getCount());
  pBLEScan->clearResults();
  
  // STEP 2: Validasi
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║         HASIL VALIDASI                 ║");
  Serial.println("╠════════════════════════════════════════╣");
  
  bool beaconFound = false;
  
  // Cek apakah UUID cocok
  if (detectedBeaconUUID.length() > 0) {
    detectedBeaconUUID.toUpperCase();
    beaconUUID.toUpperCase();
    
    // Cek apakah UUID mengandung NPM atau cocok dengan registered UUID
    if (detectedBeaconUUID.indexOf(npm) >= 0 || detectedBeaconUUID == beaconUUID) {
      beaconFound = true;
    }
  }
  
  if (beaconFound) {
    // ✓✓✓ SUKSES ✓✓✓
    Serial.println("║ Status : ✓✓✓ HADIR ✓✓✓                ║");
    Serial.print("║ RSSI   : ");
    Serial.print(detectedRSSI);
    Serial.println(" dBm                      ║");
    Serial.println("║                                        ║");
    Serial.println("║ ✓ Kartu: Terdeteksi                    ║");
    Serial.println("║ ✓ HP   : Terdeteksi                    ║");
    Serial.println("║                                        ║");
    Serial.println("║ ABSENSI DITERIMA!                      ║");
    Serial.println("╚════════════════════════════════════════╝\n");
    
    // Update scan count
    updateScanCount(safeKey, value);
    
  } else {
    // ✗✗✗ FRAUD ALERT ✗✗✗
    Serial.println("║ Status : ✗✗✗ FRAUD ALERT ✗✗✗          ║");
    Serial.println("║                                        ║");
    Serial.println("║ ✓ Kartu: Terdeteksi                    ║");
    Serial.println("║ ✗ HP   : TIDAK TERDETEKSI!             ║");
    Serial.println("║                                        ║");
    Serial.println("║ KEMUNGKINAN: TITIP ABSEN               ║");
    Serial.println("║ ABSENSI DITOLAK!                       ║");
    Serial.println("╚════════════════════════════════════════╝\n");
  }
}

void updateScanCount(String safeKey, String value) {
  // Format: npm|nama|beaconUUID|scanCount
  int lastIdx = value.lastIndexOf('|');
  String baseValue = value;
  int scanCount = 1;
  
  if (lastIdx > 0 && value.substring(lastIdx + 1).toInt() > 0) {
    // Ada scan count
    scanCount = value.substring(lastIdx + 1).toInt() + 1;
    baseValue = value.substring(0, lastIdx);
  }
  
  String newValue = baseValue + "|" + String(scanCount);
  preferences.putString(safeKey.c_str(), newValue);
}

void registerMode() {
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║         MODE REGISTRASI                ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.println("Tap kartu yang ingin didaftarkan...\n");
  
  // Wait for card
  unsigned long timeout = millis() + 30000;
  while (millis() < timeout) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      String cardUID = getCardUID();
      String safeKey = cardUID;
      safeKey.replace(" ", "");
      
      Serial.print("Kartu terdeteksi: ");
      Serial.println(cardUID);
      
      // Input NPM
      Serial.print("\nMasukkan NPM: ");
      String npm = "";
      while (npm.length() == 0) {
        if (Serial.available() > 0) {
          npm = Serial.readStringUntil('\n');
          npm.trim();
        }
        delay(100);
      }
      Serial.println(npm);
      
      // Input Nama
      Serial.print("Masukkan Nama: ");
      String nama = "";
      while (nama.length() == 0) {
        if (Serial.available() > 0) {
          nama = Serial.readStringUntil('\n');
          nama.trim();
        }
        delay(100);
      }
      Serial.println(nama);
      
      // Input Beacon UUID
      Serial.print("Masukkan Beacon UUID (format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx): ");
      String beaconUUID = "";
      while (beaconUUID.length() == 0) {
        if (Serial.available() > 0) {
          beaconUUID = Serial.readStringUntil('\n');
          beaconUUID.trim();
        }
        delay(100);
      }
      Serial.println(beaconUUID);
      
      // Simpan ke database
      String value = npm + "|" + nama + "|" + beaconUUID + "|0";
      preferences.putString(safeKey.c_str(), value);
      
      // Update counter
      int count = preferences.getInt("count", 0);
      count++;
      preferences.putInt("count", count);
      
      // Update UID list
      String uidList = preferences.getString("uid_list", "");
      if (uidList.length() > 0) uidList += ";";
      uidList += safeKey;
      preferences.putString("uid_list", uidList);
      
      Serial.println("\n✓ Registrasi berhasil!\n");
      
      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      return;
    }
    delay(100);
  }
  
  Serial.println("\n✗ Timeout! Registrasi dibatalkan.\n");
}

void displayDatabase() {
  int count = preferences.getInt("count", 0);
  
  Serial.println("\n╔════════════════════════════════════════════════════════════╗");
  Serial.println("║                    DATABASE ATLAS                         ║");
  Serial.println("╠════════════════════════════════════════════════════════════╣");
  Serial.print("║ Total: ");
  Serial.print(count);
  Serial.println(" kartu                                             ║");
  Serial.println("╠════════════════════════════════════════════════════════════╣");
  
  if (count == 0) {
    Serial.println("║ Database kosong.                                          ║");
    Serial.println("╚════════════════════════════════════════════════════════════╝\n");
    return;
  }
  
  Serial.println("║                                                            ║");
  Serial.println("║ No | UID          | NPM        | Nama        | Scan Count ║");
  Serial.println("╟────┼──────────────┼────────────┼─────────────┼────────────╢");
  
  String uidList = preferences.getString("uid_list", "");
  
  if (uidList.length() > 0) {
    int cardNum = 1;
    int startIdx = 0;
    
    while (startIdx < uidList.length()) {
      int endIdx = uidList.indexOf(';', startIdx);
      if (endIdx == -1) endIdx = uidList.length();
      
      String safeKey = uidList.substring(startIdx, endIdx);
      
      if (safeKey.length() > 0) {
        String value = preferences.getString(safeKey.c_str(), "");
        
        if (value.length() > 0) {
          // Parse
          int idx1 = value.indexOf('|');
          int idx2 = value.indexOf('|', idx1 + 1);
          int idx3 = value.indexOf('|', idx2 + 1);
          
          String npm = value.substring(0, idx1);
          String nama = value.substring(idx1 + 1, idx2);
          String scanCount = "0";
          
          if (idx3 > 0) {
            scanCount = value.substring(idx3 + 1);
          }
          
          // Format UID
          String displayUID = "";
          for (int i = 0; i < safeKey.length() && i < 12; i += 2) {
            if (i > 0) displayUID += " ";
            displayUID += safeKey.substring(i, i + 2);
          }
          
          // Truncate jika terlalu panjang
          if (npm.length() > 10) npm = npm.substring(0, 10);
          if (nama.length() > 11) nama = nama.substring(0, 11);
          
          Serial.print("║ ");
          if (cardNum < 10) Serial.print(" ");
          Serial.print(cardNum);
          Serial.print(" | ");
          Serial.print(displayUID);
          for (int i = displayUID.length(); i < 12; i++) Serial.print(" ");
          Serial.print(" | ");
          Serial.print(npm);
          for (int i = npm.length(); i < 10; i++) Serial.print(" ");
          Serial.print(" | ");
          Serial.print(nama);
          for (int i = nama.length(); i < 11; i++) Serial.print(" ");
          Serial.print(" | ");
          Serial.print(scanCount);
          for (int i = scanCount.length(); i < 10; i++) Serial.print(" ");
          Serial.println(" ║");
          
          cardNum++;
        }
      }
      
      startIdx = endIdx + 1;
    }
  }
  
  Serial.println("╚════════════════════════════════════════════════════════════╝\n");
}

void clearDatabase() {
  Serial.print("\nHapus database? (Y/N): ");
  
  String confirm = "";
  unsigned long timeout = millis() + 10000;
  
  while (confirm.length() == 0 && millis() < timeout) {
    if (Serial.available() > 0) {
      confirm = Serial.readStringUntil('\n');
      confirm.trim();
      confirm.toUpperCase();
    }
    delay(100);
  }
  
  if (confirm.length() > 0) Serial.println(confirm);
  
  if (confirm == "Y" || confirm == "YES") {
    preferences.clear();
    Serial.println("\n✓ Database dihapus!\n");
  } else {
    Serial.println("\n✗ Batal.\n");
  }
}