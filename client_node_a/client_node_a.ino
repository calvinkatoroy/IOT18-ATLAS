/*
 * ATLAS SLAVE - MQTT RFID + BLE Validator
 * Mode: 
 *   - DEFAULT: Scan RFID -> Validate BLE UUID -> Send to Master
 *   - REGISTER: Wait master signal -> Scan card -> Send UID to Master
 * 
 * Architecture: FreeRTOS with tasks, queues, and mutexes
 * Broker: HiveMQ
 */

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>

// ======================== PIN CONFIGURATION ========================
#define SS_PIN    21
#define RST_PIN   22
#define SCK_PIN   18
#define MOSI_PIN  23
#define MISO_PIN  19

// ======================== WIFI CONFIG ========================
const char* ssid = "CALNATH";
const char* password = "Calvin2304";

// ======================== MQTT CONFIG ========================
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_client_id = "ATLAS_Slave_001";

// MQTT Topics
#define TOPIC_MODE           "atlas/mode"          // Master -> Slave: register/default
#define TOPIC_REGISTER_DATA  "atlas/register"      // Master -> Slave: uid|npm|uuid
#define TOPIC_CARD_SCANNED   "atlas/card"          // Slave -> Master: card UID
#define TOPIC_ATTENDANCE     "atlas/attendance"    // Slave -> Master: uid|npm|rssi|valid

// ======================== BLE CONFIG ========================
#define BLE_SCAN_TIME 3
#define RSSI_THRESHOLD -80

// ======================== OBJECTS ========================
WiFiClient espClient;
PubSubClient mqttClient(espClient);
MFRC522 mfrc522(SS_PIN, RST_PIN);
Preferences prefs;
NimBLEScan* pBLEScan;

// ======================== GLOBAL STATE ========================
volatile bool registerMode = false;
String detectedBLEUUID = "";
int detectedBLERSSI = -100;

// ======================== RTOS HANDLES ========================
TaskHandle_t rfidTaskHandle = NULL;
TaskHandle_t validationTaskHandle = NULL;
TaskHandle_t mqttTaskHandle = NULL;
QueueHandle_t cardQueue = NULL;
SemaphoreHandle_t prefsMutex = NULL;
SemaphoreHandle_t bleMutex = NULL;
SemaphoreHandle_t mqttMutex = NULL;

// ======================== STRUCTURES ========================
struct CardData {
  String uid;
  String safeKey;  // UID without spaces
};

// ======================== FORWARD DECLARATIONS ========================
void rfidScanTask(void* param);
void validationProcessTask(void* param);
void mqttTask(void* param);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
void publishMQTT(const char* topic, const char* payload);
String getCardUID();

// ======================== BLE CALLBACK ========================
class BLECallback : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* device) {
    if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (device->haveServiceUUID()) {
        NimBLEUUID uuid = device->getServiceUUID();
        String uuidStr = uuid.toString().c_str();
        int rssi = device->getRSSI();
        
        if (rssi > detectedBLERSSI && rssi >= RSSI_THRESHOLD) {
          detectedBLEUUID = uuidStr;
          detectedBLERSSI = rssi;
          Serial.printf("[BLE] Found: %s, RSSI: %d\n", uuidStr.c_str(), rssi);
        }
      }
      xSemaphoreGive(bleMutex);
    }
  }
};

// ======================== MQTT CALLBACK ========================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  
  Serial.printf("[MQTT] Received from %s: %s\n", topic, msg.c_str());
  
  // Topic: atlas/mode (Master sends register/default)
  if (strcmp(topic, TOPIC_MODE) == 0) {
    if (msg == "register") {
      registerMode = true;
      Serial.println(">>> MODE: REGISTER <<<");
      Serial.println("Tap kartu untuk registrasi...");
    } else {
      registerMode = false;
      Serial.println(">>> MODE: DEFAULT <<<");
    }
  }
  
  // Topic: atlas/register (Master sends uid|npm|serviceUUID)
  else if (strcmp(topic, TOPIC_REGISTER_DATA) == 0) {
    Serial.printf("[DEBUG] Raw registration data: '%s'\n", msg.c_str());
    
    int idx1 = msg.indexOf('|');
    int idx2 = msg.indexOf('|', idx1 + 1);
    
    Serial.printf("[DEBUG] idx1=%d, idx2=%d, length=%d\n", idx1, idx2, msg.length());
    
    if (idx1 > 0 && idx2 > idx1) {
      String uid = msg.substring(0, idx1);
      String npm = msg.substring(idx1 + 1, idx2);
      String serviceUUID = msg.substring(idx2 + 1);
      
      Serial.println("[DEBUG] Parsed data:");
      Serial.printf("  UID : '%s' (len=%d)\n", uid.c_str(), uid.length());
      Serial.printf("  NPM : '%s' (len=%d)\n", npm.c_str(), npm.length());
      Serial.printf("  UUID: '%s' (len=%d)\n", serviceUUID.c_str(), serviceUUID.length());
      
      // Create safe key (UID without spaces)
      String safeKey = uid;
      safeKey.replace(" ", "");
      
      Serial.printf("[DEBUG] Safe key: '%s'\n", safeKey.c_str());
      
      // Store as: npm|serviceUUID
      String value = npm + "|" + serviceUUID;
      
      if (xSemaphoreTake(prefsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        prefs.putString(safeKey.c_str(), value);
        Serial.printf("[DEBUG] Stored to NVS: key='%s', value='%s'\n", safeKey.c_str(), value.c_str());
        xSemaphoreGive(prefsMutex);
      }
      
      Serial.println("\n✓ Registration data received and stored!");
      Serial.printf("  UID : %s\n", uid.c_str());
      Serial.printf("  NPM : %s\n", npm.c_str());
      Serial.printf("  UUID: %s\n\n", serviceUUID.c_str());
    } else {
      Serial.printf("✗ Invalid format! Expected 'uid|npm|uuid', got '%s'\n", msg.c_str());
    }
  }
}

// ======================== MQTT HELPER FUNCTIONS ========================
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT...");
    if (mqttClient.connect(mqtt_client_id)) {
      Serial.println(" connected!");
      mqttClient.subscribe(TOPIC_MODE);
      mqttClient.subscribe(TOPIC_REGISTER_DATA);
      Serial.println("✓ MQTT subscribed to topics");
    } else {
      Serial.printf(" failed, rc=%d. Retry in 5s\n", mqttClient.state());
      delay(5000);
    }
  }
}

void publishMQTT(const char* topic, const char* payload) {
  if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (mqttClient.connected()) {
      mqttClient.publish(topic, payload);
      Serial.printf("[MQTT] Published to %s: %s\n", topic, payload);
    }
    xSemaphoreGive(mqttMutex);
  }
}

// ======================== SETUP ========================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║     ATLAS SLAVE - RFID + BLE          ║");
  Serial.println("║     MQTT with RTOS                     ║");
  Serial.println("║     Broker: HiveMQ                     ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  // Init RFID
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  mfrc522.PCD_Init();
  mfrc522.PCD_SetAntennaGain(mfrc522.RxGain_max);
  Serial.println("✓ RFID initialized");
  
  // Init BLE
  NimBLEDevice::init("ATLAS_Slave");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new BLECallback(), false);
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  Serial.println("✓ BLE initialized");
  
  // Init WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✓ WiFi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  
  // Init MQTT
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(60);
  mqttClient.setSocketTimeout(30);
  
  reconnectMQTT();
  
  // Init Preferences
  prefs.begin("atlas_slave", false);
  Serial.println("✓ Preferences initialized");
  
  // Create RTOS primitives
  cardQueue = xQueueCreate(5, sizeof(CardData));
  prefsMutex = xSemaphoreCreateMutex();
  bleMutex = xSemaphoreCreateMutex();
  mqttMutex = xSemaphoreCreateMutex();
  
  if (!cardQueue || !prefsMutex || !bleMutex || !mqttMutex) {
    Serial.println("✗ RTOS init failed");
    while(1) delay(1000);
  }
  
  // Create tasks
  xTaskCreatePinnedToCore(rfidScanTask, "RFID", 4096, NULL, 2, &rfidTaskHandle, 0);
  xTaskCreatePinnedToCore(validationProcessTask, "Validation", 6144, NULL, 1, &validationTaskHandle, 1);
  xTaskCreatePinnedToCore(mqttTask, "MQTT", 4096, NULL, 2, &mqttTaskHandle, 0);
  
  Serial.println("✓ RTOS tasks created");
  Serial.println("\n>>> MODE: DEFAULT <<<");
  Serial.println("Ready. Waiting for card scan...\n");
}

// ======================== LOOP ========================
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// ======================== HELPER FUNCTIONS ========================
String getCardUID() {
  String uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uid += " 0";
    else uid += " ";
    uid += String(mfrc522.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  uid.trim();
  return uid;
}

// ======================== RTOS TASKS ========================

/**
 * Task: MQTT Management
 * Handles MQTT connection and loop
 */
void mqttTask(void* param) {
  Serial.println("[Task] MQTT started on core 0");
  
  while (1) {
    if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (!mqttClient.connected()) {
        reconnectMQTT();
      }
      mqttClient.loop();
      xSemaphoreGive(mqttMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

/**
 * Task: RFID Scanner
 * Continuously scans for RFID cards and pushes to queue
 */
void rfidScanTask(void* param) {
  Serial.println("[Task] RFID started on core 0");
  
  while (1) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      String uid = getCardUID();
      String safeKey = uid;
      safeKey.replace(" ", "");
      
      Serial.println("\n╔════════════════════════════════════════╗");
      Serial.println("║         KARTU TERDETEKSI               ║");
      Serial.println("╚════════════════════════════════════════╝");
      Serial.printf("UID: %s\n", uid.c_str());
      
      CardData card;
      card.uid = uid;
      card.safeKey = safeKey;
      
      if (xQueueSend(cardQueue, &card, pdMS_TO_TICKS(200)) != pdPASS) {
        Serial.println("✗ Queue full");
      }
      
      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      
      vTaskDelay(pdMS_TO_TICKS(1500));
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

/**
 * Task: Validation and Processing
 * Handles both register and default modes
 */
void validationProcessTask(void* param) {
  Serial.println("[Task] Validation started on core 1");
  CardData card;
  
  while (1) {
    if (xQueueReceive(cardQueue, &card, pdMS_TO_TICKS(200)) == pdPASS) {
      
      if (registerMode) {
        // ========== REGISTER MODE ==========
        Serial.println("\n[REGISTER] Sending UID to master...");
        publishMQTT(TOPIC_CARD_SCANNED, card.uid.c_str());
        Serial.println("✓ UID sent, waiting for master to complete registration");
        
      } else {
        // ========== DEFAULT MODE: VALIDATION ==========
        Serial.println("\n[VALIDATION] Checking database...");
        
        String storedData = "";
        if (xSemaphoreTake(prefsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          Serial.printf("[DEBUG] Looking for key: '%s'\n", card.safeKey.c_str());
          storedData = prefs.getString(card.safeKey.c_str(), "");
          Serial.printf("[DEBUG] Found data: '%s' (len=%d)\n", 
                        storedData.length() > 0 ? storedData.c_str() : "(empty)", 
                        storedData.length());
          xSemaphoreGive(prefsMutex);
        }
        
        if (storedData.length() == 0) {
          Serial.println("✗ Kartu tidak terdaftar");
          Serial.println("Minta admin untuk registrasi kartu ini.\n");
          continue;
        }
        
        // Parse: npm|serviceUUID
        int pipeIdx = storedData.indexOf('|');
        if (pipeIdx == -1) {
          Serial.println("✗ Data corrupt");
          continue;
        }
        
        String npm = storedData.substring(0, pipeIdx);
        String storedUUID = storedData.substring(pipeIdx + 1);
        
        Serial.printf("NPM: %s\n", npm.c_str());
        Serial.printf("Stored UUID: %s\n", storedUUID.c_str());
        
        // Scan BLE
        Serial.println("\n[BLE] Scanning...");
        detectedBLEUUID = "";
        detectedBLERSSI = -100;
        
        if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
          pBLEScan->start(BLE_SCAN_TIME, false);
          xSemaphoreGive(bleMutex);
        }
        
        pBLEScan->clearResults();
        
        // Validate
        Serial.println("\n╔════════════════════════════════════════╗");
        Serial.println("║         HASIL VALIDASI                 ║");
        Serial.println("╠════════════════════════════════════════╣");
        
        bool valid = false;
        
        if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
          if (detectedBLEUUID.length() > 0) {
            String detUpper = detectedBLEUUID;
            String storedUpper = storedUUID;
            detUpper.toUpperCase();
            storedUpper.toUpperCase();
            
            if (detUpper == storedUpper) {
              valid = true;
            }
          }
          xSemaphoreGive(bleMutex);
        }
        
        if (valid) {
          Serial.println("║ Status: ✓✓✓ VALID ✓✓✓                 ║");
          Serial.printf("║ RSSI  : %d dBm                     ║\n", detectedBLERSSI);
          Serial.println("║ ✓ Kartu: OK                            ║");
          Serial.println("║ ✓ BLE  : OK                            ║");
          Serial.println("║ ABSENSI DITERIMA                       ║");
          Serial.println("╚════════════════════════════════════════╝\n");
          
          // Send to master: uid|npm|rssi|valid
          char payload[200];
          snprintf(payload, sizeof(payload), "%s|%s|%d|1", 
                   card.uid.c_str(), npm.c_str(), detectedBLERSSI);
          publishMQTT(TOPIC_ATTENDANCE, payload);
          
        } else {
          Serial.println("║ Status: ✗✗✗ INVALID ✗✗✗               ║");
          Serial.println("║ ✓ Kartu: OK                            ║");
          Serial.println("║ ✗ BLE  : TIDAK COCOK                   ║");
          Serial.println("║ ABSENSI DITOLAK                        ║");
          Serial.println("╚════════════════════════════════════════╝\n");
          
          // Send invalid: uid|unknown|0|0
          char payload[200];
          snprintf(payload, sizeof(payload), "%s|Unknown|0|0", card.uid.c_str());
          publishMQTT(TOPIC_ATTENDANCE, payload);
        }
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
