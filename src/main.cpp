// Slave
#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <time.h>

#define SS_PIN    21
#define RST_PIN   22
#define SCK_PIN   18
#define MOSI_PIN  23
#define MISO_PIN  19

const char* ssid = "calvin";
const char* password = "calvin2304";

const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_client_id = "ATLAS_Slave_001";

#define TOPIC_MODE           "atlas/mode"         
#define TOPIC_REGISTER_DATA  "atlas/register"     
#define TOPIC_CARD_SCANNED   "atlas/card"         
#define TOPIC_ATTENDANCE     "atlas/attendance"   
#define TOPIC_COMMAND        "atlas/command"      

#define BLE_SCAN_TIME 3
#define RSSI_THRESHOLD -100

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600; 
const int daylightOffset_sec = 0;

WiFiClient espClient;
PubSubClient mqttClient(espClient);
MFRC522 mfrc522(SS_PIN, RST_PIN);
Preferences prefs;
NimBLEScan* pBLEScan;

volatile bool registerMode = false;
String detectedBLEUUID = "";
int detectedBLERSSI = -100;

TaskHandle_t rfidTaskHandle = NULL;
TaskHandle_t validationTaskHandle = NULL;
TaskHandle_t mqttTaskHandle = NULL;
QueueHandle_t cardQueue = NULL;
SemaphoreHandle_t prefsMutex = NULL;
SemaphoreHandle_t bleMutex = NULL;
SemaphoreHandle_t mqttMutex = NULL;

struct CardData {
  String uid;
  String safeKey;  
};

void rfidScanTask(void* param);
void validationProcessTask(void* param);
void mqttTask(void* param);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
void publishMQTT(const char* topic, const char* payload);
String getCardUID();
String getTimestamp();

class BLECallback : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* device) {
    if (device->haveServiceUUID()) {
      int rssi = device->getRSSI();
      if (rssi >= RSSI_THRESHOLD && rssi > detectedBLERSSI) {
        if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          detectedBLEUUID = device->getServiceUUID().toString().c_str();
          detectedBLERSSI = rssi;
          xSemaphoreGive(bleMutex);
        }
      }
    }
  }
};

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  
  if (strcmp(topic, TOPIC_MODE) == 0) {
    registerMode = (msg == "register");
    Serial.printf(">>> MODE: %s <<<\n", registerMode ? "REGISTER" : "DEFAULT");
  }
  else if (strcmp(topic, TOPIC_REGISTER_DATA) == 0) {
    int idx1 = msg.indexOf('|');
    int idx2 = msg.indexOf('|', idx1 + 1);
    
    if (idx1 > 0 && idx2 > idx1) {
      String uid = msg.substring(0, idx1);
      String npm = msg.substring(idx1 + 1, idx2);
      String serviceUUID = msg.substring(idx2 + 1);
      
      String safeKey = uid;
      safeKey.replace(" ", "");
      String value = npm + "|" + serviceUUID;
      
      if (xSemaphoreTake(prefsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        prefs.putString(safeKey.c_str(), value);
        xSemaphoreGive(prefsMutex);
      }
      
      Serial.printf("Registered: %s -> %s\n", uid.c_str(), npm.c_str());
    }
  }
  else if (strcmp(topic, TOPIC_COMMAND) == 0) {
    if (msg == "clear_all") {
      if (xSemaphoreTake(prefsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        prefs.clear();
        xSemaphoreGive(prefsMutex);
      }
      Serial.println("Database cleared by master");
    }
  }
}

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT...");
    if (mqttClient.connect(mqtt_client_id)) {
      Serial.println(" connected!");
      mqttClient.subscribe(TOPIC_MODE);
      mqttClient.subscribe(TOPIC_REGISTER_DATA);
      mqttClient.subscribe(TOPIC_COMMAND);
      Serial.println("MQTT subscribed to topics");
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
    }
    xSemaphoreGive(mqttMutex);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== ATLAS SLAVE - RFID + BLE ===\n");
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  mfrc522.PCD_Init();
  mfrc522.PCD_SetAntennaGain(mfrc522.RxGain_max);
  
  NimBLEDevice::init("ATLAS_Slave");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new BLECallback(), false);
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("NTP synced");
  
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  reconnectMQTT();
  
  prefs.begin("atlas_slave", false);
  
  cardQueue = xQueueCreate(5, sizeof(CardData));
  prefsMutex = xSemaphoreCreateMutex();
  bleMutex = xSemaphoreCreateMutex();
  mqttMutex = xSemaphoreCreateMutex();
  
  xTaskCreatePinnedToCore(rfidScanTask, "RFID", 4096, NULL, 2, &rfidTaskHandle, 0);
  xTaskCreatePinnedToCore(validationProcessTask, "Validation", 6144, NULL, 1, &validationTaskHandle, 1);
  xTaskCreatePinnedToCore(mqttTask, "MQTT", 4096, NULL, 2, &mqttTaskHandle, 0);
  
  Serial.println("Ready\n");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}

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

String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "N/A";
  }
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}

void mqttTask(void* param) {
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

void rfidScanTask(void* param) {
  while (1) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      String uid = getCardUID();
      String safeKey = uid;
      safeKey.replace(" ", "");
      
      Serial.printf("\n[CARD] %s\n", uid.c_str());
      
      CardData card;
      card.uid = uid;
      card.safeKey = safeKey;
      xQueueSend(cardQueue, &card, pdMS_TO_TICKS(200));
      
      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      vTaskDelay(pdMS_TO_TICKS(1500));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void validationProcessTask(void* param) {
  CardData card;
  
  while (1) {
    if (xQueueReceive(cardQueue, &card, pdMS_TO_TICKS(200)) == pdPASS) {
      
      if (registerMode) {
        publishMQTT(TOPIC_CARD_SCANNED, card.uid.c_str());
        Serial.println("Sent to master for registration");
        
      } else {
        String storedData = "";
        if (xSemaphoreTake(prefsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          storedData = prefs.getString(card.safeKey.c_str(), "");
          xSemaphoreGive(prefsMutex);
        }
        
        if (storedData.length() == 0) {
          Serial.println("Card not registered\n");
          continue;
        }
        
        int pipeIdx = storedData.indexOf('|');
        String npm = storedData.substring(0, pipeIdx);
        String storedUUID = storedData.substring(pipeIdx + 1);
        
        Serial.printf("[BLE] Scanning %ds...\n", BLE_SCAN_TIME);
        detectedBLEUUID = "";
        detectedBLERSSI = -100;
        
        pBLEScan->start(BLE_SCAN_TIME, false);
        vTaskDelay(pdMS_TO_TICKS(200));
        
        String finalUUID = "";
        int finalRSSI = -100;
        if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
          finalUUID = detectedBLEUUID;
          finalRSSI = detectedBLERSSI;
          xSemaphoreGive(bleMutex);
        }
        
        pBLEScan->clearResults();
        
        bool valid = false;
        if (finalUUID.length() > 0) {
          String detUpper = finalUUID;
          String storedUpper = storedUUID;
          detUpper.toUpperCase();
          storedUpper.toUpperCase();
          valid = (detUpper == storedUpper);
        }
        
        if (valid) {
          String counterKey = "cnt_" + npm;
          int scanNum = 0;
          if (xSemaphoreTake(prefsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            scanNum = prefs.getInt(counterKey.c_str(), 0) + 1;
            prefs.putInt(counterKey.c_str(), scanNum);
            xSemaphoreGive(prefsMutex);
          }
          
          String status = (scanNum % 2 == 1) ? "MASUK" : "KELUAR";
          String timestamp = getTimestamp();
          Serial.printf("VALID #%d [%s] - %s | NPM: %s | RSSI: %d dBm\n\n", 
                        scanNum, status.c_str(), timestamp.c_str(), npm.c_str(), finalRSSI);
          char payload[256];
          snprintf(payload, sizeof(payload), "%s|%s|%d|1|%s|%d|%s", 
                   card.uid.c_str(), npm.c_str(), finalRSSI, timestamp.c_str(), 
                   scanNum, status.c_str());
          publishMQTT(TOPIC_ATTENDANCE, payload);
        } else {
          String timestamp = getTimestamp();
          Serial.printf("INVALID - %s | BLE mismatch\n\n", timestamp.c_str());
          char payload[256];
          snprintf(payload, sizeof(payload), "%s|Unknown|0|0|%s|0|INVALID", 
                   card.uid.c_str(), timestamp.c_str());
          publishMQTT(TOPIC_ATTENDANCE, payload);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}