#define BLYNK_TEMPLATE_ID "TMPL6VPh0o-Dm"
#define BLYNK_TEMPLATE_NAME "ATLAS"
#define BLYNK_AUTH_TOKEN "kXflwxH-ISh8FD4j2aTDssEML_PqumqY"

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <BlynkSimpleEsp32.h>
#include <Preferences.h>

// WiFi
const char* ssid = "Alga";
const char* password = "bonifasius1103";

// MQTT HiveMQ Broker
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_client_id = "ATLAS_Master_001";

// MQTT Topics
#define TOPIC_MODE           "atlas/mode"          // Master -> Slave: register/default
#define TOPIC_REGISTER_DATA  "atlas/register"      // Master -> Slave: uid|npm|uuid
#define TOPIC_CARD_SCANNED   "atlas/card"          // Slave -> Master: card UID
#define TOPIC_ATTENDANCE     "atlas/attendance"    // Slave -> Master: uid|npm|rssi|valid

// State
volatile bool registerMode = false;
String pendingNPM = "";
String pendingServiceUUID = "";
String pendingCardUID = "";  // Store scanned UID until NPM/UUID ready
volatile bool waitingForCard = false;
int attendanceCount = 0;

// RTOS
TaskHandle_t mqttTaskHandle = NULL;
QueueHandle_t attendanceQueue = NULL;
QueueHandle_t registrationQueue = NULL;
QueueHandle_t commandQueue = NULL;

struct AttendanceData {
  String uid;
  String npm;
  int rssi;
  bool valid;
};

struct RegistrationData {
  char uid[30];
  char npm[20];
  char serviceUUID[50];
};

struct CommandData {
  uint8_t type;  // 0=mode signal, 1=registration
  bool modeValue;
  char uid[30];
  char npm[20];
  char serviceUUID[50];
};

// Objects
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Preferences prefs;
WidgetTerminal terminal(V3);
SemaphoreHandle_t mqttMutex = NULL;

// Forward declarations
void mqttManagementTask(void* param);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
void publishMQTT(const char* topic, const char* payload);

// MQTT Callback
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  
  Serial.printf("[MQTT] Received from %s: %s\n", topic, msg.c_str());
  
  // Topic: atlas/card (Slave sends card UID)
  if (strcmp(topic, TOPIC_CARD_SCANNED) == 0) {
    if (registerMode) {
      Serial.printf("[REGISTER] Card UID received: %s\n", msg.c_str());
      
      // Store card UID
      pendingCardUID = msg;
      Serial.printf("[DEBUG] Stored pendingCardUID: '%s'\n", pendingCardUID.c_str());
      Serial.printf("[DEBUG] Current pendingNPM: '%s'\n", pendingNPM.c_str());
      Serial.printf("[DEBUG] Current pendingServiceUUID: '%s'\n", pendingServiceUUID.c_str());
      
      // Check if NPM and UUID already entered
      if (pendingNPM.length() > 0 && pendingServiceUUID.length() > 0) {
        // Complete registration immediately
        Serial.println("[DEBUG] All data ready, creating registration...");
        RegistrationData reg;
        
        pendingCardUID.toCharArray(reg.uid, sizeof(reg.uid));
        pendingNPM.toCharArray(reg.npm, sizeof(reg.npm));
        pendingServiceUUID.toCharArray(reg.serviceUUID, sizeof(reg.serviceUUID));
        
        Serial.println("[DEBUG] RegistrationData created:");
        Serial.printf("  reg.uid: '%s'\n", reg.uid);
        Serial.printf("  reg.npm: '%s'\n", reg.npm);
        Serial.printf("  reg.serviceUUID: '%s'\n", reg.serviceUUID);
        
        xQueueSend(registrationQueue, &reg, 0);
        
        pendingNPM = "";
        pendingServiceUUID = "";
        pendingCardUID = "";
        waitingForCard = false;
      } else {
        // Store UID and wait for NPM/UUID
        Serial.println("[REGISTER] Card stored. Waiting for NPM and UUID...");
      }
    }
  }
  
  // Topic: atlas/attendance (Slave sends uid|npm|rssi|valid)
  else if (strcmp(topic, TOPIC_ATTENDANCE) == 0) {
    int idx1 = msg.indexOf('|');
    int idx2 = msg.indexOf('|', idx1 + 1);
    int idx3 = msg.indexOf('|', idx2 + 1);
    
    if (idx1 > 0 && idx2 > idx1 && idx3 > idx2) {
      AttendanceData att;
      att.uid = msg.substring(0, idx1);
      att.npm = msg.substring(idx1 + 1, idx2);
      att.rssi = msg.substring(idx2 + 1, idx3).toInt();
      att.valid = (msg.substring(idx3 + 1) == "1");
      xQueueSend(attendanceQueue, &att, 0);
    }
  }
}

// Blynk Callbacks
BLYNK_WRITE(V0) {
  registerMode = (param.asInt() == 1);
  
  CommandData cmd;
  cmd.type = 0;
  cmd.modeValue = registerMode;
  xQueueSend(commandQueue, &cmd, 0);
  
  if (registerMode) {
    Serial.println(">>> MODE: REGISTER <<<");
    terminal.println(">>> MODE: REGISTER");
    terminal.println("Enter NPM and UUID, then tap card");
    terminal.flush();
    
    // Allow card scan anytime in register mode
    if (pendingNPM.length() > 0) pendingServiceUUID = "00000000-0000-0000-0000-00" + pendingNPM;
    if (pendingNPM.length() > 0 && pendingServiceUUID.length() > 0) {
      waitingForCard = true;
      terminal.println("Ready! Tap card now...");
      terminal.flush();
    }
  } else {
    Serial.println(">>> MODE: DEFAULT <<<");
    terminal.println(">>> MODE: DEFAULT");
    terminal.flush();
    waitingForCard = false;
  }
}

BLYNK_WRITE(V1) {
  pendingNPM = param.asStr();
  Serial.printf("[Blynk] NPM input: %s\n", pendingNPM.c_str());
  Serial.printf("[DEBUG] pendingCardUID: '%s'\n", pendingCardUID.c_str());
  Serial.printf("[DEBUG] pendingServiceUUID: '%s'\n", pendingServiceUUID.c_str());
  
  if (registerMode) {
    // Check if we have all data (including scanned card)
    if (pendingCardUID.length() > 0 && pendingServiceUUID.length() > 0) {
      // Complete registration now
      Serial.println("[DEBUG] All data ready (from NPM input), creating registration...");
      RegistrationData reg;
      
      pendingCardUID.toCharArray(reg.uid, sizeof(reg.uid));
      pendingNPM.toCharArray(reg.npm, sizeof(reg.npm));
      pendingServiceUUID.toCharArray(reg.serviceUUID, sizeof(reg.serviceUUID));
      
      Serial.println("[DEBUG] RegistrationData created:");
      Serial.printf("  reg.uid: '%s'\n", reg.uid);
      Serial.printf("  reg.npm: '%s'\n", reg.npm);
      Serial.printf("  reg.serviceUUID: '%s'\n", reg.serviceUUID);
      
      xQueueSend(registrationQueue, &reg, 0);
      
      pendingNPM = "";
      pendingServiceUUID = "";
      pendingCardUID = "";
      waitingForCard = false;
      
      terminal.println("✓ Registration processing...");
      terminal.flush();
    } else if (pendingServiceUUID.length() > 0) {
      waitingForCard = true;
      terminal.println("Ready! Tap card on slave...");
      terminal.flush();
    }
  }
}

BLYNK_WRITE(V3) {
  String cmd = param.asStr();
  if (cmd == "clear" || cmd == "clr") {
    terminal.clear();
  }
}

// Setup
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║   ATLAS MASTER - Blynk + MQTT         ║");
  Serial.println("║   Broker: HiveMQ                       ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  // WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✓ WiFi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  
  // MQTT
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(60);
  mqttClient.setSocketTimeout(30);
  
  reconnectMQTT();
  
  // Subscribe to topics
  mqttClient.subscribe(TOPIC_CARD_SCANNED);
  mqttClient.subscribe(TOPIC_ATTENDANCE);
  Serial.println("✓ MQTT subscribed to topics");
  
  // Blynk
  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect();
  Serial.println("✓ Blynk connected");
  
  // Preferences
  prefs.begin("atlas_master", false);
  attendanceCount = prefs.getInt("att_count", 0);
  Serial.println("✓ Preferences initialized");
  
  // RTOS
  attendanceQueue = xQueueCreate(10, sizeof(AttendanceData));
  registrationQueue = xQueueCreate(5, sizeof(RegistrationData));
  commandQueue = xQueueCreate(5, sizeof(CommandData));
  mqttMutex = xSemaphoreCreateMutex();
  
  if (!attendanceQueue || !registrationQueue || !commandQueue || !mqttMutex) {
    Serial.println("✗ RTOS init failed");
    while(1) delay(1000);
  }
  
  xTaskCreatePinnedToCore(mqttManagementTask, "MQTT", 8192, NULL, 2, &mqttTaskHandle, 0);
  
  Serial.println("✓ RTOS tasks created\n");
  Serial.println(">>> MODE: DEFAULT <<<");
  Serial.println("Ready.\n");
  
  Blynk.virtualWrite(V4, attendanceCount);
  terminal.println("System Ready");
  terminal.flush();
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// MQTT Helper Functions
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT...");
    if (mqttClient.connect(mqtt_client_id)) {
      Serial.println(" connected!");
      Serial.printf("Broker: %s:%d\n", mqtt_server, mqtt_port);
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
    } else {
      Serial.println("[MQTT] Not connected, reconnecting...");
      reconnectMQTT();
    }
    xSemaphoreGive(mqttMutex);
  }
}

// RTOS Task
void mqttManagementTask(void* param) {
  Serial.println("[Task] MQTT+Blynk started on core 0");
  
  AttendanceData att;
  RegistrationData reg;
  CommandData cmd;
  
  while (1) {
    // MQTT loop
    if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (!mqttClient.connected()) {
        reconnectMQTT();
        mqttClient.subscribe(TOPIC_CARD_SCANNED);
        mqttClient.subscribe(TOPIC_ATTENDANCE);
      }
      mqttClient.loop();
      xSemaphoreGive(mqttMutex);
    }
    
    // Run Blynk
    Blynk.run();
    
    // Process commands (MQTT publish)
    if (xQueueReceive(commandQueue, &cmd, 0) == pdPASS) {
      if (cmd.type == 0) {
        // Send mode signal
        const char* mode = cmd.modeValue ? "register" : "default";
        publishMQTT(TOPIC_MODE, mode);
        Serial.printf("[MQTT] Mode signal sent: %s\n", mode);
      } else if (cmd.type == 1) {
        // Send registration data
        Serial.println("[DEBUG] Preparing registration MQTT message:");
        Serial.printf("  cmd.uid: '%s' (len=%d)\n", cmd.uid, strlen(cmd.uid));
        Serial.printf("  cmd.npm: '%s' (len=%d)\n", cmd.npm, strlen(cmd.npm));
        Serial.printf("  cmd.serviceUUID: '%s' (len=%d)\n", cmd.serviceUUID, strlen(cmd.serviceUUID));
        
        char payload[200];
        snprintf(payload, sizeof(payload), "%s|%s|%s", 
                 cmd.uid, cmd.npm, cmd.serviceUUID);
        
        Serial.printf("[DEBUG] Formatted payload: '%s' (len=%d)\n", payload, strlen(payload));
        publishMQTT(TOPIC_REGISTER_DATA, payload);
        Serial.println("[MQTT] Registration data sent");
      }
    }
    
    // Process attendance
    if (xQueueReceive(attendanceQueue, &att, 0) == pdPASS) {
      if (att.valid) {
        attendanceCount++;
        prefs.putInt("att_count", attendanceCount);
        
        Serial.printf("✓ Attendance #%d: %s (RSSI: %d)\n", 
                      attendanceCount, att.npm.c_str(), att.rssi);
        
        Blynk.virtualWrite(V4, attendanceCount);
        Blynk.virtualWrite(V5, att.npm + " - ✓ Valid");
        
        String log = "✓ #" + String(attendanceCount) + ": " + att.npm + 
                     " (RSSI:" + String(att.rssi) + ")";
        terminal.println(log);
        terminal.flush();
      } else {
        Serial.printf("✗ Invalid: %s\n", att.uid.c_str());
        
        Blynk.virtualWrite(V5, att.uid + " - ✗ Invalid (BLE mismatch)");
        terminal.println("✗ Invalid: " + att.uid + " (BLE mismatch)");
        terminal.flush();
      }
    }
    
    // Process registration
    if (xQueueReceive(registrationQueue, &reg, 0) == pdPASS) {
      Serial.println("\n[REGISTER] Processing:");
      Serial.printf("  UID : %s\n", reg.uid);
      Serial.printf("  NPM : %s\n", reg.npm);
      Serial.printf("  UUID: %s\n", reg.serviceUUID);
      
      // Store locally
      String safeKey = String(reg.uid);
      safeKey.replace(" ", "");
      String value = String(reg.npm) + "|" + String(reg.serviceUUID);
      prefs.putString(safeKey.c_str(), value);
      
      // Send to slave: uid|npm|serviceUUID
      CommandData regCmd;
      regCmd.type = 1;
      strncpy(regCmd.uid, reg.uid, sizeof(regCmd.uid) - 1);
      regCmd.uid[sizeof(regCmd.uid) - 1] = '\0';
      strncpy(regCmd.npm, reg.npm, sizeof(regCmd.npm) - 1);
      regCmd.npm[sizeof(regCmd.npm) - 1] = '\0';
      strncpy(regCmd.serviceUUID, reg.serviceUUID, sizeof(regCmd.serviceUUID) - 1);
      regCmd.serviceUUID[sizeof(regCmd.serviceUUID) - 1] = '\0';
      
      Serial.println("[DEBUG] Before queue send:");
      Serial.printf("  regCmd.uid: '%s'\n", regCmd.uid);
      Serial.printf("  regCmd.npm: '%s'\n", regCmd.npm);
      Serial.printf("  regCmd.serviceUUID: '%s'\n", regCmd.serviceUUID);
      
      xQueueSend(commandQueue, &regCmd, 0);
      
      Serial.println("✓ Registration complete\n");
      
      Blynk.virtualWrite(V5, "Registered: " + String(reg.npm));
      terminal.println("✓ Registered: " + String(reg.npm));
      terminal.println("  UID: " + String(reg.uid));
      terminal.flush();
      
      // Exit register mode
      registerMode = false;
      waitingForCard = false;
      Blynk.virtualWrite(V0, 0);
      
      CommandData modeCmd;
      modeCmd.type = 0;
      modeCmd.modeValue = false;
      xQueueSend(commandQueue, &modeCmd, 0);
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
