// Master
#define BLYNK_TEMPLATE_ID "TMPL6VPh0o-Dm"
#define BLYNK_TEMPLATE_NAME "ATLAS"
#define BLYNK_AUTH_TOKEN "kXflwxH-ISh8FD4j2aTDssEML_PqumqY"

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <BlynkSimpleEsp32.h>
#include <Preferences.h>
#include <map>

const char* ssid = "calvin";
const char* password = "calvin2304";

const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_client_id = "ATLAS_Master_001";

#define TOPIC_MODE           "atlas/mode"          
#define TOPIC_REGISTER_DATA  "atlas/register"      
#define TOPIC_CARD_SCANNED   "atlas/card"          
#define TOPIC_ATTENDANCE     "atlas/attendance"    

volatile bool registerMode = false;
String pendingNPM = "";
String pendingServiceUUID = "";
String pendingCardUID = "";
volatile bool waitingForCard = false;
int attendanceCount = 0;
int inClassCount = 0;

struct StudentStatus {
  String npm;
  String lastStatus;
  String lastTime;
  unsigned long enterMillis;
};
std::map<String, StudentStatus> activeStudents;

struct InvalidAttempt {
  String time;
  String uid;
  String reason;
};
InvalidAttempt invalidLog[5];
int invalidIndex = 0;

TaskHandle_t mqttTaskHandle = NULL;
QueueHandle_t attendanceQueue = NULL;
QueueHandle_t registrationQueue = NULL;
QueueHandle_t commandQueue = NULL;

struct AttendanceData {
  String uid;
  String npm;
  int rssi;
  bool valid;
  String timestamp;
  int scanNum;
  String status;
};

struct RegistrationData {
  char uid[30];
  char npm[20];
  char serviceUUID[50];
};

struct CommandData {
  uint8_t type; 
  bool modeValue;
  char uid[30];
  char npm[20];
  char serviceUUID[50];
};

WiFiClient espClient;
PubSubClient mqttClient(espClient);
Preferences prefs;
WidgetTerminal terminal(V3);
WidgetTerminal invalidTerminal(V7);
WidgetTerminal studentList(V8);
SemaphoreHandle_t mqttMutex = NULL;

void mqttManagementTask(void* param);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
void publishMQTT(const char* topic, const char* payload);
String calculateDuration(unsigned long startMillis);
void updateStudentList();

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  
  if (strcmp(topic, TOPIC_CARD_SCANNED) == 0) {
    if (registerMode) {
      pendingCardUID = msg;
      
      if (pendingNPM.length() > 0 && pendingServiceUUID.length() > 0) {
        RegistrationData reg;
        pendingCardUID.toCharArray(reg.uid, sizeof(reg.uid));
        pendingNPM.toCharArray(reg.npm, sizeof(reg.npm));
        pendingServiceUUID.toCharArray(reg.serviceUUID, sizeof(reg.serviceUUID));
        xQueueSend(registrationQueue, &reg, 0);
        
        pendingNPM = "";
        pendingServiceUUID = "";
        pendingCardUID = "";
        waitingForCard = false;
      }
    }
  }
  else if (strcmp(topic, TOPIC_ATTENDANCE) == 0) {
    int idx1 = msg.indexOf('|');
    int idx2 = msg.indexOf('|', idx1 + 1);
    int idx3 = msg.indexOf('|', idx2 + 1);
    int idx4 = msg.indexOf('|', idx3 + 1);
    int idx5 = msg.indexOf('|', idx4 + 1);
    int idx6 = msg.indexOf('|', idx5 + 1);
    
    if (idx1 > 0 && idx6 > 0) {
      AttendanceData att;
      att.uid = msg.substring(0, idx1);
      att.npm = msg.substring(idx1 + 1, idx2);
      att.rssi = msg.substring(idx2 + 1, idx3).toInt();
      att.valid = (msg.substring(idx3 + 1, idx4) == "1");
      att.timestamp = msg.substring(idx4 + 1, idx5);
      att.scanNum = msg.substring(idx5 + 1, idx6).toInt();
      att.status = msg.substring(idx6 + 1);
      xQueueSend(attendanceQueue, &att, 0);
    }
  }
}

BLYNK_WRITE(V0) {
  registerMode = (param.asInt() == 1);
  
  CommandData cmd;
  cmd.type = 0;
  cmd.modeValue = registerMode;
  xQueueSend(commandQueue, &cmd, 0);
  
  terminal.println(registerMode ? ">>> REGISTER MODE" : ">>> DEFAULT MODE");
  if (registerMode) terminal.println("Enter NPM, then tap card");
  terminal.flush();
  
  if (!registerMode) waitingForCard = false;
}

BLYNK_WRITE(V1) {
  pendingNPM = param.asStr();
  
  if (pendingNPM.length() > 0) {
    String paddedNPM = pendingNPM;
    while (paddedNPM.length() < 12) {
      paddedNPM = "0" + paddedNPM;
    }
    pendingServiceUUID = "00000000-0000-0000-0000-" + paddedNPM;
  }
  
  if (registerMode && pendingCardUID.length() > 0 && pendingServiceUUID.length() > 0) {
    RegistrationData reg;
    pendingCardUID.toCharArray(reg.uid, sizeof(reg.uid));
    pendingNPM.toCharArray(reg.npm, sizeof(reg.npm));
    pendingServiceUUID.toCharArray(reg.serviceUUID, sizeof(reg.serviceUUID));
    xQueueSend(registrationQueue, &reg, 0);
    
    pendingNPM = "";
    pendingServiceUUID = "";
    pendingCardUID = "";
    waitingForCard = false;
    
    terminal.println("  Registration processing...");
    terminal.flush();
  } else if (pendingServiceUUID.length() > 0) {
    waitingForCard = true;
    terminal.println("Ready! Tap card on slave...");
    terminal.flush();
  }
}

BLYNK_WRITE(V3) {
  String cmd = param.asStr();
  if (cmd == "clear" || cmd == "clr") {
    terminal.clear();
  }
}

BLYNK_WRITE(V9) {
  if (param.asInt() == 1) {
    attendanceCount = 0;
    prefs.putInt("att_count", 0);
    Blynk.virtualWrite(V4, 0);
    terminal.println("  Counter cleared");
    terminal.flush();
  }
}

BLYNK_WRITE(V10) {
  if (param.asInt() == 1) {
    prefs.clear();
    activeStudents.clear();
    inClassCount = 0;
    attendanceCount = 0;
    
    studentList.clear();
    studentList.println("Database cleared");
    studentList.flush();
    
    terminal.println("  Database cleared");
    terminal.flush();
    
    Blynk.virtualWrite(V4, 0);
    Blynk.virtualWrite(V6, 0);
    
    publishMQTT("atlas/command", "clear_all");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== ATLAS MASTER - Blynk + MQTT ===\n");
  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n  WiFi connected");
  
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  reconnectMQTT();
  mqttClient.subscribe(TOPIC_CARD_SCANNED);
  mqttClient.subscribe(TOPIC_ATTENDANCE);
  
  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect();
  
  prefs.begin("atlas_master", false);
  attendanceCount = prefs.getInt("att_count", 0);
  
  attendanceQueue = xQueueCreate(10, sizeof(AttendanceData));
  registrationQueue = xQueueCreate(5, sizeof(RegistrationData));
  commandQueue = xQueueCreate(5, sizeof(CommandData));
  mqttMutex = xSemaphoreCreateMutex();
  
  xTaskCreatePinnedToCore(mqttManagementTask, "MQTT", 8192, NULL, 2, &mqttTaskHandle, 0);
  
  Serial.println("  Ready\n");
  
  Blynk.virtualWrite(V4, attendanceCount);
  Blynk.virtualWrite(V6, inClassCount);
  Blynk.virtualWrite(V11, "0m");
  
  terminal.println("System Ready");
  terminal.flush();
  
  invalidTerminal.println("No invalid attempts yet");
  invalidTerminal.flush();
  
  studentList.println("No students in class");
  studentList.flush();
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}

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
    if (!mqttClient.connected()) reconnectMQTT();
    mqttClient.publish(topic, payload);
    xSemaphoreGive(mqttMutex);
  }
}

String calculateDuration(unsigned long startMillis) {
  if (startMillis == 0) return "-";
  unsigned long duration = millis() - startMillis;
  unsigned long hours = duration / 3600000;
  unsigned long minutes = (duration % 3600000) / 60000;
  
  if (hours > 0) {
    return String(hours) + "h " + String(minutes) + "m";
  } else {
    return String(minutes) + "m";
  }
}

void updateStudentList() {
  studentList.clear();
  
  if (activeStudents.empty()) {
    studentList.println("No students in class");
  } else {
    for (auto& pair : activeStudents) {
      StudentStatus& s = pair.second;
      String icon = (s.lastStatus == "MASUK") ? " " : " ";
      String dur = (s.lastStatus == "MASUK") ? calculateDuration(s.enterMillis) : "-";
      
      studentList.print(icon + " " + s.npm + " | ");
      studentList.print(s.lastStatus + " | ");
      studentList.print(s.lastTime + " | ");
      studentList.println(dur);
    }
  }
  
  studentList.flush();
}

void mqttManagementTask(void* param) {
  AttendanceData att;
  RegistrationData reg;
  CommandData cmd;
  
  while (1) {
    if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (!mqttClient.connected()) {
        reconnectMQTT();
        mqttClient.subscribe(TOPIC_CARD_SCANNED);
        mqttClient.subscribe(TOPIC_ATTENDANCE);
      }
      mqttClient.loop();
      xSemaphoreGive(mqttMutex);
    }
    
    Blynk.run();
    
    if (xQueueReceive(commandQueue, &cmd, 0) == pdPASS) {
      if (cmd.type == 0) {
        publishMQTT(TOPIC_MODE, cmd.modeValue ? "register" : "default");
      } else if (cmd.type == 1) {
        char payload[200];
        snprintf(payload, sizeof(payload), "%s|%s|%s", cmd.uid, cmd.npm, cmd.serviceUUID);
        publishMQTT(TOPIC_REGISTER_DATA, payload);
      }
    }
    
    if (xQueueReceive(attendanceQueue, &att, 0) == pdPASS) {
      if (att.valid) {
        attendanceCount++;
        prefs.putInt("att_count", attendanceCount);
        
        if (att.status == "MASUK") {
          inClassCount++;
        } else if (att.status == "KELUAR" && inClassCount > 0) {
          inClassCount--;
        }
        
        StudentStatus& student = activeStudents[att.npm];
        student.npm = att.npm;
        student.lastStatus = att.status;
        student.lastTime = att.timestamp.substring(11, 19);
        
        if (att.status == "MASUK") {
          student.enterMillis = millis();
        } else {
          student.enterMillis = 0;
        }
        
        unsigned long totalDuration = 0;
        int activeCount = 0;
        for (auto& pair : activeStudents) {
          if (pair.second.lastStatus == "MASUK" && pair.second.enterMillis > 0) {
            totalDuration += (millis() - pair.second.enterMillis);
            activeCount++;
          }
        }
        String avgDur = activeCount > 0 ? calculateDuration(millis() - (totalDuration / activeCount)) : "0m";
        
        Serial.printf("  #%d [%s] %s | %s | RSSI:%d\n", 
                      att.scanNum, att.status.c_str(), att.npm.c_str(), 
                      att.timestamp.c_str(), att.rssi);
        
        Blynk.virtualWrite(V4, attendanceCount);
        Blynk.virtualWrite(V5, att.npm + " -   " + att.status);
        Blynk.virtualWrite(V6, inClassCount);
        Blynk.virtualWrite(V11, avgDur);
        
        String log = "  #" + String(att.scanNum) + " [" + att.status + "] " + 
                     att.npm + " | " + att.timestamp;
        terminal.println(log);
        terminal.flush();
        
        updateStudentList();
        
      } else {
        String timeOnly = att.timestamp.substring(11, 19);
        String reason = "BLE mismatch";
        
        invalidLog[invalidIndex].time = timeOnly;
        invalidLog[invalidIndex].uid = att.uid;
        invalidLog[invalidIndex].reason = reason;
        invalidIndex = (invalidIndex + 1) % 5;
        
        invalidTerminal.clear();
        for (int i = 0; i < 5; i++) {
          int idx = (invalidIndex + i) % 5;
          if (invalidLog[idx].time.length() > 0) {
            invalidTerminal.println("  " + invalidLog[idx].time + " - " + 
                                   invalidLog[idx].uid + " (" + 
                                   invalidLog[idx].reason + ")");
          }
        }
        invalidTerminal.flush();
        
        Blynk.virtualWrite(V5, att.uid + " - Invalid");
        terminal.println("Invalid: " + att.uid);
        terminal.flush();
      }
    }
    
    if (xQueueReceive(registrationQueue, &reg, 0) == pdPASS) {
      String safeKey = String(reg.uid);
      safeKey.replace(" ", "");
      String value = String(reg.npm) + "|" + String(reg.serviceUUID);
      prefs.putString(safeKey.c_str(), value);
      
      CommandData regCmd;
      regCmd.type = 1;
      strncpy(regCmd.uid, reg.uid, sizeof(regCmd.uid) - 1);
      strncpy(regCmd.npm, reg.npm, sizeof(regCmd.npm) - 1);
      strncpy(regCmd.serviceUUID, reg.serviceUUID, sizeof(regCmd.serviceUUID) - 1);
      regCmd.uid[sizeof(regCmd.uid) - 1] = '\0';
      regCmd.npm[sizeof(regCmd.npm) - 1] = '\0';
      regCmd.serviceUUID[sizeof(regCmd.serviceUUID) - 1] = '\0';
      xQueueSend(commandQueue, &regCmd, 0);
      
      Serial.printf("Registered: %s -> %s\n", reg.uid, reg.npm);
      
      Blynk.virtualWrite(V5, "Registered: " + String(reg.npm));
      terminal.println("Registered: " + String(reg.npm));
      terminal.flush();
      
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