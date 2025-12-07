#include <painlessMesh.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>

// ═══════════════════════════════════════════════════════════════
//                        CONFIGURATION
// ═══════════════════════════════════════════════════════════════

// Mesh Configuration
#define MESH_PREFIX     "ATLAS_MESH"
#define MESH_PASSWORD   "atlas2024"
#define MESH_PORT       5555

// WiFi Configuration
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// Blynk Configuration
#define BLYNK_AUTH      "YOUR_BLYNK_AUTH_TOKEN"

// Blynk Virtual Pins
#define VPIN_REGISTRATION_TOGGLE  V0   // Switch untuk enable/disable registration
#define VPIN_LAST_ATTENDANCE      V1   // Display last attendance
#define VPIN_FRAUD_ALERT          V2   // LED for fraud alert
#define VPIN_TOTAL_STUDENTS       V3   // Value display
#define VPIN_TOTAL_SCANS          V4   // Value display
#define VPIN_SUCCESS_SCANS        V5   // Value display
#define VPIN_FRAUD_ATTEMPTS       V6   // Value display
#define VPIN_ATTENDANCE_LOG       V7   // Terminal widget
#define VPIN_REGISTRATION_LOG     V8   // Terminal widget
#define VPIN_NODE_STATUS          V9   // LED for node connection

// ═══════════════════════════════════════════════════════════════
//                        GLOBAL OBJECTS
// ═══════════════════════════════════════════════════════════════

painlessMesh mesh;
BlynkTimer timer;
WidgetTerminal terminal(VPIN_ATTENDANCE_LOG);
WidgetTerminal regTerminal(VPIN_REGISTRATION_LOG);

// ═══════════════════════════════════════════════════════════════
//                        GLOBAL VARIABLES
// ═══════════════════════════════════════════════════════════════

bool registrationEnabled = false;
unsigned long totalStudents = 0;
unsigned long totalScans = 0;
unsigned long successScans = 0;
unsigned long fraudAttempts = 0;

String lastAttendanceName = "-";
String lastAttendanceNPM = "-";
String lastAttendanceStatus = "-";

// ═══════════════════════════════════════════════════════════════
//                        MESH CALLBACKS
// ═══════════════════════════════════════════════════════════════

void receivedCallback(uint32_t from, String &msg) {
  Serial.printf("[MESH] Received from %u: %s\n", from, msg.c_str());
  
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, msg);
  
  if (error) {
    Serial.println("[MESH] JSON parse failed!");
    return;
  }
  
  String msgType = doc["type"];
  
  if (msgType == "ATTENDANCE") {
    // Attendance log received
    String npm = doc["npm"];
    String nama = doc["nama"];
    bool success = doc["success"];
    int rssi = doc["rssi"];
    String node = doc["node"];
    
    lastAttendanceNPM = npm;
    lastAttendanceName = nama;
    lastAttendanceStatus = success ? "HADIR" : "FRAUD";
    
    // Update Blynk
    Blynk.virtualWrite(VPIN_LAST_ATTENDANCE, 
                       nama + " (" + npm + ") - " + lastAttendanceStatus);
    
    // Log to terminal
    terminal.print("[");
    terminal.print(node);
    terminal.print("] ");
    terminal.print(nama);
    terminal.print(" (");
    terminal.print(npm);
    terminal.print(") - ");
    
    if (success) {
      terminal.print("✓ HADIR");
      terminal.print(" | RSSI: ");
      terminal.println(rssi);
      Blynk.virtualWrite(VPIN_FRAUD_ALERT, 0); // Turn off fraud LED
    } else {
      terminal.println("✗ FRAUD ALERT!");
      Blynk.virtualWrite(VPIN_FRAUD_ALERT, 255); // Turn on fraud LED
      Blynk.notify("FRAUD ALERT: " + nama + " (" + npm + ") - Phone not detected!");
      
      // Turn off fraud LED after 5 seconds
      timer.setTimeout(5000, []() {
        Blynk.virtualWrite(VPIN_FRAUD_ALERT, 0);
      });
    }
    
    terminal.flush();
    
  } else if (msgType == "NEW_REGISTRATION") {
    // New registration notification
    String npm = doc["npm"];
    String nama = doc["nama"];
    String node = doc["node"];
    
    regTerminal.print("[");
    regTerminal.print(node);
    regTerminal.print("] NEW: ");
    regTerminal.print(nama);
    regTerminal.print(" (");
    regTerminal.print(npm);
    regTerminal.println(")");
    regTerminal.flush();
    
    Blynk.notify("New Registration: " + nama + " (" + npm + ")");
    
  } else if (msgType == "STATS") {
    // Statistics update
    totalStudents = doc["total_students"];
    totalScans = doc["total_scans"];
    successScans = doc["success_scans"];
    fraudAttempts = doc["fraud_attempts"];
    
    // Update Blynk
    Blynk.virtualWrite(VPIN_TOTAL_STUDENTS, totalStudents);
    Blynk.virtualWrite(VPIN_TOTAL_SCANS, totalScans);
    Blynk.virtualWrite(VPIN_SUCCESS_SCANS, successScans);
    Blynk.virtualWrite(VPIN_FRAUD_ATTEMPTS, fraudAttempts);
  }
}

void newConnectionCallback(uint32_t nodeId) {
  Serial.printf("[MESH] New Connection, nodeId = %u\n", nodeId);
  Blynk.virtualWrite(VPIN_NODE_STATUS, 255); // Turn on node connected LED
}

void changedConnectionCallback() {
  Serial.println("[MESH] Connections changed");
  
  // Check if any nodes are connected
  if (mesh.getNodeList().size() > 0) {
    Blynk.virtualWrite(VPIN_NODE_STATUS, 255);
  } else {
    Blynk.virtualWrite(VPIN_NODE_STATUS, 0);
  }
}

void nodeTimeAdjustedCallback(int32_t offset) {
  Serial.printf("[MESH] Time adjusted. Offset = %d\n", offset);
}

// ═══════════════════════════════════════════════════════════════
//                        BLYNK CALLBACKS
// ═══════════════════════════════════════════════════════════════

BLYNK_WRITE(VPIN_REGISTRATION_TOGGLE) {
  registrationEnabled = param.asInt();
  
  Serial.printf("[BLYNK] Registration mode: %s\n", 
                registrationEnabled ? "ENABLED" : "DISABLED");
  
  // Send command to all client nodes
  StaticJsonDocument<256> doc;
  doc["type"] = "ENABLE_REGISTER";
  doc["enabled"] = registrationEnabled;
  
  String msg;
  serializeJson(doc, msg);
  
  mesh.sendBroadcast(msg);
  
  if (registrationEnabled) {
    Blynk.notify("Registration mode ENABLED on all nodes");
    regTerminal.println(">>> REGISTRATION MODE: ON");
  } else {
    Blynk.notify("Registration mode DISABLED");
    regTerminal.println(">>> REGISTRATION MODE: OFF");
  }
  regTerminal.flush();
}

BLYNK_WRITE(VPIN_ATTENDANCE_LOG) {
  // Clear terminal if command is "clear"
  String input = param.asStr();
  if (input == "clear") {
    terminal.clear();
    terminal.println("Log cleared.");
    terminal.flush();
  }
}

BLYNK_WRITE(VPIN_REGISTRATION_LOG) {
  // Clear terminal if command is "clear"
  String input = param.asStr();
  if (input == "clear") {
    regTerminal.clear();
    regTerminal.println("Log cleared.");
    regTerminal.flush();
  }
}

BLYNK_CONNECTED() {
  Serial.println("[BLYNK] Connected to server");
  
  // Initial display
  Blynk.virtualWrite(VPIN_LAST_ATTENDANCE, "Waiting for attendance...");
  Blynk.virtualWrite(VPIN_FRAUD_ALERT, 0);
  
  terminal.clear();
  terminal.println("=== ATLAS Gateway Started ===");
  terminal.println("Waiting for attendance logs...");
  terminal.flush();
  
  regTerminal.clear();
  regTerminal.println("=== Registration Log ===");
  regTerminal.println("Toggle switch to enable registration mode");
  regTerminal.flush();
}

// ═══════════════════════════════════════════════════════════════
//                        SETUP FUNCTION
// ═══════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  while (!Serial);
  
  printHeader();
  
  // Initialize WiFi
  Serial.println("\n[WiFi] Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\n✓ WiFi connected");
  Serial.print("  IP: ");
  Serial.println(WiFi.localIP());
  
  // Initialize Blynk
  Serial.println("\n[BLYNK] Connecting to Blynk...");
  Blynk.config(BLYNK_AUTH);
  Blynk.connect();
  
  while (!Blynk.connected()) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\n✓ Blynk connected");
  
  // Initialize Mesh
  Serial.println("\n[MESH] Initializing Mesh Network...");
  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT);
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);
  
  Serial.println("✓ Mesh Network initialized");
  Serial.printf("  Node ID: %u\n", mesh.getNodeId());
  
  // Setup timer
  timer.setInterval(1000L, []() {
    // Keep Blynk alive
  });
  
  Serial.println("\n[SYSTEM] Gateway Ready!");
  Serial.println("  - Monitoring mesh network");
  Serial.println("  - Connected to Blynk dashboard");
  Serial.println("  - Ready to receive attendance logs\n");
}

// ═══════════════════════════════════════════════════════════════
//                        MAIN LOOP
// ═══════════════════════════════════════════════════════════════

void loop() {
  mesh.update();
  Blynk.run();
  timer.run();
}

// ═══════════════════════════════════════════════════════════════
//                      UTILITY FUNCTIONS
// ═══════════════════════════════════════════════════════════════

void printHeader() {
  Serial.println("\n╔════════════════════════════════════════════════════════╗");
  Serial.println("║           PROJECT ATLAS - GATEWAY NODE                ║");
  Serial.println("║              Rangkaian B (Meja Dosen)                  ║");
  Serial.println("╚════════════════════════════════════════════════════════╝\n");
}
```

---

## **SETUP BLYNK DASHBOARD:**

### **1. Buat Project Baru di Blynk:**
- Download **Blynk IoT** app (Android/iOS)
- Create new template
- Copy **Auth Token**

### **2. Tambahkan Widgets:**

**Virtual Pin V0 - Switch:**
- Name: "Registration Mode"
- Mode: Switch
- DataStream: V0

**Virtual Pin V1 - Label:**
- Name: "Last Attendance"
- DataStream: V1

**Virtual Pin V2 - LED:**
- Name: "Fraud Alert"
- Color: Red
- DataStream: V2

**Virtual Pin V3-V6 - Value Display:**
- V3: "Total Students"
- V4: "Total Scans"
- V5: "Success"
- V6: "Fraud Attempts"

**Virtual Pin V7 - Terminal:**
- Name: "Attendance Log"
- DataStream: V7

**Virtual Pin V8 - Terminal:**
- Name: "Registration Log"
- DataStream: V8

**Virtual Pin V9 - LED:**
- Name: "Node Connected"
- Color: Green
- DataStream: V9

---

## **LIBRARY YANG DIPERLUKAN:**

**Rangkaian A:**
```
- MFRC522
- Adafruit GFX
- Adafruit SSD1306
- painlessMesh
- ArduinoJson
```

**Rangkaian B:**
```
- painlessMesh
- ArduinoJson
- Blynk (legacy or new)