/*
 * ═══════════════════════════════════════════════════════════════
 *              PROJECT ATLAS - RANGKAIAN A (CLIENT NODE)
 *                    Node Pintu Kelas dengan Mesh
 * ═══════════════════════════════════════════════════════════════
 * 
 * Hardware:
 * - ESP32 DevKit V1
 * - MFRC522 RFID Reader
 * - SSD1306 OLED Display (128x64)
 * - Active Buzzer
 * - LED (Green/Red)
 * 
 * Features:
 * - RFID Attendance & Registration
 * - BLE Beacon Scanning (Anti-cheat)
 * - Local Database (Preferences.h)
 * - Mesh Network Communication
 * - No WiFi Connection (Only Mesh)
 * - Remote Registration Control from Gateway
 * 
 * ═══════════════════════════════════════════════════════════════
 */

#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <painlessMesh.h>
#include <ArduinoJson.h>
#include <vector>

// ═══════════════════════════════════════════════════════════════
//                        PIN DEFINITIONS
// ═══════════════════════════════════════════════════════════════

// RFID Pins
#define SS_PIN      21
#define RST_PIN     22
#define SCK_PIN     18
#define MOSI_PIN    23
#define MISO_PIN    19

// Output Pins
#define LED_GREEN   2
#define LED_RED     4
#define BUZZER      5

// I2C Pins (OLED)
#define SDA_PIN     21
#define SCL_PIN     22

// OLED Config
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDRESS  0x3C

// ═══════════════════════════════════════════════════════════════
//                        MESH CONFIGURATION
// ═══════════════════════════════════════════════════════════════

#define MESH_PREFIX     "ATLAS_MESH"
#define MESH_PASSWORD   "atlas2024"
#define MESH_PORT       5555

// ═══════════════════════════════════════════════════════════════
//                        CONFIGURATION
// ═══════════════════════════════════════════════════════════════

#define BLE_SCAN_TIME       5
#define BLE_SCAN_QUICK      3
#define RSSI_THRESHOLD      -75
#define NODE_NAME          "CLIENT_NODE_1"

const char* PREF_NAMESPACE = "atlas_db";

// ═══════════════════════════════════════════════════════════════
//                        GLOBAL OBJECTS
// ═══════════════════════════════════════════════════════════════

MFRC522 mfrc522(SS_PIN, RST_PIN);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
BLEScan* pBLEScan;
Preferences preferences;
painlessMesh mesh;

// ═══════════════════════════════════════════════════════════════
//                        DATA STRUCTURES
// ═══════════════════════════════════════════════════════════════

struct BLEDeviceInfo {
  String uuid;
  String name;
  int rssi;
};

enum SystemState {
  STATE_STANDBY,
  STATE_CARD_DETECTED,
  STATE_SCANNING_BLE,
  STATE_SUCCESS,
  STATE_FRAUD,
  STATE_ERROR,
  STATE_REGISTRATION
};

// ═══════════════════════════════════════════════════════════════
//                        GLOBAL VARIABLES
// ═══════════════════════════════════════════════════════════════

String detectedBeaconUUID = "";
int detectedRSSI = -100;
std::vector<BLEDeviceInfo> scannedDevices;
bool isRegistrationMode = false;
bool registrationEnabled = false; // Controlled by Gateway

volatile SystemState currentState = STATE_STANDBY;
String currentCardUID = "";
String currentNPM = "";
String currentNama = "";

unsigned long totalScans = 0;
unsigned long successScans = 0;
unsigned long fraudAttempts = 0;

// FreeRTOS
TaskHandle_t Task_RFID_Handle;
TaskHandle_t Task_Display_Handle;
SemaphoreHandle_t xDisplayMutex;

// ═══════════════════════════════════════════════════════════════
//                        BLE CALLBACK
// ═══════════════════════════════════════════════════════════════

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.haveServiceUUID()) {
      BLEUUID serviceUUID = advertisedDevice.getServiceUUID();
      String uuidStr = serviceUUID.toString().c_str();
      int rssi = advertisedDevice.getRSSI();
      String name = advertisedDevice.haveName() ? advertisedDevice.getName().c_str() : "Unknown";
      
      if (isRegistrationMode) {
        BLEDeviceInfo device;
        device.uuid = uuidStr;
        device.name = name;
        device.rssi = rssi;
        scannedDevices.push_back(device);
        
        Serial.print("  [");
        Serial.print(scannedDevices.size());
        Serial.print("] ");
        Serial.print(name);
        Serial.print(" | RSSI: ");
        Serial.println(rssi);
      } else {
        if (rssi > detectedRSSI && rssi > RSSI_THRESHOLD) {
          detectedBeaconUUID = uuidStr;
          detectedRSSI = rssi;
        }
      }
    }
  }
};

// ═══════════════════════════════════════════════════════════════
//                        MESH CALLBACKS
// ═══════════════════════════════════════════════════════════════

void receivedCallback(uint32_t from, String &msg) {
  Serial.printf("[MESH] Received from %u: %s\n", from, msg.c_str());
  
  // Parse JSON message
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, msg);
  
  if (error) {
    Serial.println("[MESH] JSON parse failed!");
    return;
  }
  
  String msgType = doc["type"];
  
  if (msgType == "ENABLE_REGISTER") {
    registrationEnabled = doc["enabled"];
    Serial.printf("[MESH] Registration mode: %s\n", registrationEnabled ? "ENABLED" : "DISABLED");
    
    if (registrationEnabled) {
      displayMessage("REGISTRATION", "MODE ENABLED", "Tap card to", "register");
      successBeep();
    } else {
      displayMessage("REGISTRATION", "MODE DISABLED", "", "");
      errorBeep();
      delay(2000);
      currentState = STATE_STANDBY;
    }
  }
}

void newConnectionCallback(uint32_t nodeId) {
  Serial.printf("[MESH] New Connection, nodeId = %u\n", nodeId);
}

void changedConnectionCallback() {
  Serial.println("[MESH] Connections changed");
}

void nodeTimeAdjustedCallback(int32_t offset) {
  Serial.printf("[MESH] Time adjusted. Offset = %d\n", offset);
}

// ═══════════════════════════════════════════════════════════════
//                        MESH SEND FUNCTIONS
// ═══════════════════════════════════════════════════════════════

void sendAttendanceLog(String npm, String nama, bool success, int rssi) {
  StaticJsonDocument<512> doc;
  doc["type"] = "ATTENDANCE";
  doc["node"] = NODE_NAME;
  doc["npm"] = npm;
  doc["nama"] = nama;
  doc["success"] = success;
  doc["rssi"] = rssi;
  doc["timestamp"] = mesh.getNodeTime();
  
  String msg;
  serializeJson(doc, msg);
  
  mesh.sendBroadcast(msg);
  Serial.println("[MESH] Attendance log sent");
}

void sendRegistrationComplete(String npm, String nama) {
  StaticJsonDocument<512> doc;
  doc["type"] = "NEW_REGISTRATION";
  doc["node"] = NODE_NAME;
  doc["npm"] = npm;
  doc["nama"] = nama;
  doc["timestamp"] = mesh.getNodeTime();
  
  String msg;
  serializeJson(doc, msg);
  
  mesh.sendBroadcast(msg);
  Serial.println("[MESH] Registration complete notification sent");
}

void sendSystemStats() {
  StaticJsonDocument<512> doc;
  doc["type"] = "STATS";
  doc["node"] = NODE_NAME;
  doc["total_students"] = preferences.getInt("count", 0);
  doc["total_scans"] = totalScans;
  doc["success_scans"] = successScans;
  doc["fraud_attempts"] = fraudAttempts;
  
  String msg;
  serializeJson(doc, msg);
  
  mesh.sendBroadcast(msg);
  Serial.println("[MESH] System stats sent");
}

// ═══════════════════════════════════════════════════════════════
//                        SETUP FUNCTION
// ═══════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  while (!Serial);
  
  printHeader();
  
  // Initialize Pins
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, LOW);
  digitalWrite(BUZZER, LOW);
  
  // Initialize OLED
  Wire.begin(SDA_PIN, SCL_PIN);
  if(!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println(F("✗ OLED init failed!"));
    while(1);
  }
  display.clearDisplay();
  displayBoot();
  Serial.println("✓ OLED Display initialized");
  
  // Initialize Preferences
  preferences.begin(PREF_NAMESPACE, false);
  Serial.println("✓ Preferences initialized");
  
  // Initialize RFID
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  mfrc522.PCD_Init();
  delay(100);
  Serial.println("✓ RFID RC522 initialized");
  
  // Initialize BLE
  BLEDevice::init("ATLAS_Node");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  Serial.println("✓ BLE Scanner initialized");
  
  // Initialize Mesh
  Serial.println("\n[MESH] Initializing Mesh Network...");
  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT);
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);
  Serial.println("✓ Mesh Network initialized");
  Serial.printf("  Node ID: %u\n", mesh.getNodeId());
  
  // Create Semaphores
  xDisplayMutex = xSemaphoreCreateMutex();
  
  // Create FreeRTOS Tasks
  Serial.println("\n[FreeRTOS] Creating tasks...");
  
  xTaskCreatePinnedToCore(
    Task_RFID,
    "RFID_Scanner",
    4096,
    NULL,
    3,
    &Task_RFID_Handle,
    0
  );
  Serial.println("  ✓ Task_RFID created");
  
  xTaskCreatePinnedToCore(
    Task_Display,
    "Display_Update",
    4096,
    NULL,
    1,
    &Task_Display_Handle,
    0
  );
  Serial.println("  ✓ Task_Display created");
  
  printStatistics();
  Serial.println("\n[SYSTEM] Ready. Waiting for card or mesh command...\n");
  
  currentState = STATE_STANDBY;
  startupBeep();
}

// ═══════════════════════════════════════════════════════════════
//                        MAIN LOOP
// ═══════════════════════════════════════════════════════════════

void loop() {
  mesh.update();
  
  // Send stats every 30 seconds
  static unsigned long lastStatsTime = 0;
  if (millis() - lastStatsTime > 30000) {
    sendSystemStats();
    lastStatsTime = millis();
  }
  
  delay(10);
}

// ═══════════════════════════════════════════════════════════════
//                    FREERTOS TASK: RFID SCANNER
// ═══════════════════════════════════════════════════════════════

void Task_RFID(void *pvParameters) {
  Serial.println("[Task_RFID] Started on Core " + String(xPortGetCoreID()));
  
  for(;;) {
    if (currentState == STATE_STANDBY || (currentState == STATE_REGISTRATION && registrationEnabled)) {
      
      if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
        
        String cardUID = getCardUID();
        currentCardUID = cardUID;
        
        Serial.println("\n╔════════════════════════════════════════╗");
        Serial.println("║      KARTU TERDETEKSI!                ║");
        Serial.println("╚════════════════════════════════════════╝");
        Serial.print("UID: ");
        Serial.println(cardUID);
        
        String safeKey = cardUID;
        safeKey.replace(" ", "");
        
        // Check if in registration mode
        if (registrationEnabled) {
          currentState = STATE_REGISTRATION;
          registerCard(cardUID, safeKey);
        } else {
          // Normal attendance mode
          if (preferences.isKey(safeKey.c_str())) {
            currentState = STATE_CARD_DETECTED;
            processAttendance(cardUID, safeKey);
          } else {
            Serial.println("✗ Kartu tidak terdaftar!");
            Serial.println("  Minta dosen untuk enable registration mode.\n");
            currentState = STATE_ERROR;
            displayMessage("ERROR", "Card Not", "Registered", "");
            errorBeep();
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            currentState = STATE_STANDBY;
          }
        }
        
        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();
        
        vTaskDelay(2000 / portTICK_PERIOD_MS);
      }
    }
    
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// ═══════════════════════════════════════════════════════════════
//                  FREERTOS TASK: DISPLAY UPDATE
// ═══════════════════════════════════════════════════════════════

void Task_Display(void *pvParameters) {
  Serial.println("[Task_Display] Started on Core " + String(xPortGetCoreID()));
  
  for(;;) {
    if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY) == pdTRUE) {
      
      switch(currentState) {
        case STATE_STANDBY:
          displayStandby();
          break;
        case STATE_CARD_DETECTED:
          displayCardDetected();
          break;
        case STATE_SCANNING_BLE:
          displayScanning();
          break;
        case STATE_SUCCESS:
          displaySuccess();
          break;
        case STATE_FRAUD:
          displayFraud();
          break;
        case STATE_REGISTRATION:
          // Handled in registerCard function
          break;
      }
      
      xSemaphoreGive(xDisplayMutex);
    }
    
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// ═══════════════════════════════════════════════════════════════
//                      CORE FUNCTIONS
// ═══════════════════════════════════════════════════════════════

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
  totalScans++;
  
  String value = preferences.getString(safeKey.c_str(), "");
  if (value.length() == 0) return;
  
  int idx1 = value.indexOf('|');
  int idx2 = value.indexOf('|', idx1 + 1);
  int idx3 = value.indexOf('|', idx2 + 1);
  
  String npm = value.substring(0, idx1);
  String nama = value.substring(idx1 + 1, idx2);
  String beaconUUID = value.substring(idx2 + 1, idx3);
  
  currentNPM = npm;
  currentNama = nama;
  
  Serial.println("\n--- Data Mahasiswa ---");
  Serial.print("NPM  : ");
  Serial.println(npm);
  Serial.print("Nama : ");
  Serial.println(nama);
  
  // Scan BLE
  Serial.println("\n[BLE] Scanning for phone signal...");
  currentState = STATE_SCANNING_BLE;
  
  isRegistrationMode = false;
  detectedBeaconUUID = "";
  detectedRSSI = -100;
  
  BLEScanResults* foundDevices = pBLEScan->start(BLE_SCAN_QUICK, false);
  pBLEScan->clearResults();
  
  // Validate
  bool beaconFound = false;
  
  if (detectedBeaconUUID.length() > 0) {
    detectedBeaconUUID.toUpperCase();
    beaconUUID.toUpperCase();
    
    if (detectedBeaconUUID.indexOf(npm) >= 0 || detectedBeaconUUID == beaconUUID) {
      beaconFound = true;
    }
  }
  
  if (beaconFound) {
    // SUCCESS
    successScans++;
    currentState = STATE_SUCCESS;
    
    Serial.println("✓✓✓ HADIR ✓✓✓");
    Serial.print("RSSI: ");
    Serial.print(detectedRSSI);
    Serial.println(" dBm");
    
    updateScanCount(safeKey, value);
    sendAttendanceLog(npm, nama, true, detectedRSSI);
    successBeep();
    
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    
  } else {
    // FRAUD
    fraudAttempts++;
    currentState = STATE_FRAUD;
    
    Serial.println("✗✗✗ FRAUD ALERT ✗✗✗");
    Serial.println("Phone not detected!");
    
    sendAttendanceLog(npm, nama, false, -100);
    fraudBeep();
    
    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
  
  currentState = STATE_STANDBY;
}

void registerCard(String cardUID, String safeKey) {
  Serial.println("\n[REGISTRATION] Starting registration...");
  
  displayMessage("STEP 1", "Enter NPM", "via Serial", "");
  
  Serial.print("Enter NPM: ");
  String npm = waitForSerialInput(60000);
  if (npm.length() == 0) {
    Serial.println("\nTimeout!");
    currentState = STATE_STANDBY;
    return;
  }
  Serial.println(npm);
  
  displayMessage("STEP 2", "Enter Name", "via Serial", "");
  
  Serial.print("Enter Name: ");
  String nama = waitForSerialInput(60000);
  if (nama.length() == 0) {
    Serial.println("\nTimeout!");
    currentState = STATE_STANDBY;
    return;
  }
  Serial.println(nama);
  
  // Scan BLE
  displayMessage("STEP 3", "Scanning", "BLE Devices", "...");
  
  Serial.println("\n[BLE] Scanning for devices...");
  scannedDevices.clear();
  isRegistrationMode = true;
  
  BLEScanResults* foundDevices = pBLEScan->start(BLE_SCAN_TIME, false);
  pBLEScan->clearResults();
  isRegistrationMode = false;
  
  if (scannedDevices.size() == 0) {
    Serial.println("No devices found!");
    displayMessage("ERROR", "No BLE", "Devices", "Found");
    errorBeep();
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    currentState = STATE_STANDBY;
    return;
  }
  
  // Display list
  displayBLEListSerial();
  
  displayMessage("STEP 4", "Select Device", "via Serial", "");
  
  Serial.print("Select device (1-");
  Serial.print(scannedDevices.size());
  Serial.print("): ");
  
  String choice = waitForSerialInput(30000);
  if (choice.length() == 0) {
    Serial.println("\nTimeout!");
    currentState = STATE_STANDBY;
    return;
  }
  Serial.println(choice);
  
  int selectedIndex = choice.toInt() - 1;
  
  if (selectedIndex < 0 || selectedIndex >= scannedDevices.size()) {
    Serial.println("Invalid choice!");
    currentState = STATE_STANDBY;
    return;
  }
  
  // Save
  String selectedUUID = scannedDevices[selectedIndex].uuid;
  String value = npm + "|" + nama + "|" + selectedUUID + "|0";
  preferences.putString(safeKey.c_str(), value);
  
  int count = preferences.getInt("count", 0);
  count++;
  preferences.putInt("count", count);
  
  String uidList = preferences.getString("uid_list", "");
  if (uidList.length() > 0) uidList += ";";
  uidList += safeKey;
  preferences.putString("uid_list", uidList);
  
  Serial.println("\n✓✓✓ REGISTRATION SUCCESS ✓✓✓");
  Serial.print("NPM : ");
  Serial.println(npm);
  Serial.print("Name: ");
  Serial.println(nama);
  
  displayMessage("SUCCESS!", npm, nama, "Registered");
  sendRegistrationComplete(npm, nama);
  successBeep();
  
  vTaskDelay(3000 / portTICK_PERIOD_MS);
  currentState = STATE_STANDBY;
}

String waitForSerialInput(unsigned long timeout) {
  String input = "";
  unsigned long startTime = millis();
  
  while (input.length() == 0 && (millis() - startTime) < timeout) {
    if (Serial.available() > 0) {
      input = Serial.readStringUntil('\n');
      input.trim();
    }
    delay(100);
  }
  
  return input;
}

void updateScanCount(String safeKey, String value) {
  int idx3 = value.lastIndexOf('|');
  String baseValue = value.substring(0, idx3);
  int scanCount = value.substring(idx3 + 1).toInt() + 1;
  
  String newValue = baseValue + "|" + String(scanCount);
  preferences.putString(safeKey.c_str(), newValue);
}

void displayBLEListSerial() {
  Serial.println("\n╔════════════════════════════════════════════════════════╗");
  Serial.println("║           BLE DEVICES DETECTED                        ║");
  Serial.println("╠════════════════════════════════════════════════════════╣");
  
  for (int i = 0; i < scannedDevices.size(); i++) {
    Serial.print("║ [");
    Serial.print(i + 1);
    Serial.print("] ");
    Serial.print(scannedDevices[i].name);
    Serial.print(" | RSSI: ");
    Serial.print(scannedDevices[i].rssi);
    Serial.println(" dBm");
  }
  
  Serial.println("╚════════════════════════════════════════════════════════╝");
}

// ═══════════════════════════════════════════════════════════════
//                      DISPLAY FUNCTIONS
// ═══════════════════════════════════════════════════════════════

void displayBoot() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(25, 10);
  display.println("ATLAS");
  display.setTextSize(1);
  display.setCursor(15, 35);
  display.println("CLIENT NODE");
  display.setCursor(10, 50);
  display.print("ID: ");
  display.println(NODE_NAME);
  display.display();
  delay(2000);
}

void displayStandby() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(20, 15);
  display.println("READY");
  display.setTextSize(1);
  display.setCursor(10, 40);
  
  if (registrationEnabled) {
    display.println("REG MODE: ON");
  } else {
    display.println("Tap Your Card");
  }
  
  int count = preferences.getInt("count", 0);
  display.setCursor(0, 55);
  display.print("DB:");
  display.print(count);
  display.print(" S:");
  display.print(successScans);
  display.print(" F:");
  display.print(fraudAttempts);
  
  display.display();
}

void displayCardDetected() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 5);
  display.println("Card Detected!");
  display.setCursor(0, 20);
  display.print("UID: ");
  
  String shortUID = currentCardUID;
  if (shortUID.length() > 15) shortUID = shortUID.substring(0, 15);
  display.println(shortUID);
  
  display.setCursor(0, 35);
  display.println("Validating...");
  display.display();
}

void displayScanning() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(5, 15);
  display.println("Scanning for");
  display.setCursor(5, 30);
  display.println("Phone Signal...");
  
  static int dotCount = 0;
  display.setCursor(5, 45);
  for (int i = 0; i < dotCount; i++) {
    display.print(".");
  }
  dotCount = (dotCount + 1) % 4;
  
  display.display();
}

void displaySuccess() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(5, 5);
  display.println("WELCOME");
  
  display.setTextSize(1);
  display.setCursor(5, 30);
  String shortNama = currentNama;
  if (shortNama.length() > 21) shortNama = shortNama.substring(0, 21);
  display.println(shortNama);
  
  display.setCursor(5, 45);
  display.println("ATTENDANCE OK");
  
  display.setCursor(5, 55);
  display.print("RSSI: ");
  display.print(detectedRSSI);
  display.println(" dBm");
  
  display.display();
}

void displayFraud() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(15, 10);
  display.println("FRAUD!");
  
  display.setTextSize(1);
  display.setCursor(0, 35);
  display.println("Phone Not Detected");
  display.setCursor(0, 50);
  display.println("Access DENIED!");
  
  display.display();
}

void displayMessage(String line1, String line2, String line3, String line4) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 10);
  display.println(line1);
  display.setCursor(0, 25);
  display.println(line2);
  display.setCursor(0, 40);
  display.println(line3);
  display.setCursor(0, 55);
  display.println(line4);
  display.display();
}

// ═══════════════════════════════════════════════════════════════
//                      SOUND FUNCTIONS
// ═══════════════════════════════════════════════════════════════

void startupBeep() {
  tone(BUZZER, 1000, 100);
  delay(150);
  tone(BUZZER, 1500, 100);
}

void successBeep() {
  digitalWrite(LED_GREEN, HIGH);
  tone(BUZZER, 2000, 100);
  delay(150);
  tone(BUZZER, 2500, 100);
  delay(500);
  digitalWrite(LED_GREEN, LOW);
}

void fraudBeep() {
  for (int i = 0; i < 5; i++) {
    digitalWrite(LED_RED, HIGH);
    tone(BUZZER, 500, 200);
    delay(200);
    digitalWrite(LED_RED, LOW);
    noTone(BUZZER);
    delay(200);
  }
}

void errorBeep() {
  digitalWrite(LED_RED, HIGH);
  tone(BUZZER, 300, 500);
  delay(600);
  digitalWrite(LED_RED, LOW);
}

// ═══════════════════════════════════════════════════════════════
//                      UTILITY FUNCTIONS
// ═══════════════════════════════════════════════════════════════

void printHeader() {
  Serial.println("\n╔════════════════════════════════════════════════════════╗");
  Serial.println("║           PROJECT ATLAS - CLIENT NODE                 ║");
  Serial.println("║              Rangkaian A (Pintu Kelas)                 ║");
  Serial.println("╚════════════════════════════════════════════════════════╝\n");
}

void printStatistics() {
  int count = preferences.getInt("count", 0);
  
  Serial.println("\n╔════════════════════════════════════════════════════════╗");
  Serial.println("║              SYSTEM STATISTICS                         ║");
  Serial.println("╠════════════════════════════════════════════════════════╣");
  Serial.print("║  Registered Students : ");
  Serial.println(count);
  Serial.print("║  Total Scans        : ");
  Serial.println(totalScans);
  Serial.print("║  Success            : ");
  Serial.println(successScans);
  Serial.print("║  Fraud Attempts     : ");
  Serial.println(fraudAttempts);
  Serial.println("╚════════════════════════════════════════════════════════╝");
}