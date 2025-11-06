#include <WiFi.h>
#include <WebServer.h>

// ===== Wi-Fi Credentials =====
const char* ssid     = "Test";       // 🔹 your hotspot SSID
const char* password = "12345678";   // 🔹 your hotspot password

WebServer server(80);

// ===== Local State =====
volatile bool chargingActive = false;
volatile unsigned long chargingEndTime = 0;
String uartLine;

// --- HTTP Handlers ---
void handleRoot() {
  server.send(200, "text/plain", "ESP32-CAM Charging Station Ready (Hotspot Mode)");
}

void handleStart() {
  if (!server.hasArg("time")) {
    server.send(400, "text/plain", "Missing 'time' parameter.");
    return;
  }
  int minutes = server.arg("time").toInt();
  if (minutes <= 0) {
    server.send(400, "text/plain", "Invalid 'time' value. Must be > 0.");
    return;
  }
  unsigned long durationMs = (unsigned long)minutes * 60UL * 1000UL;
  chargingEndTime = millis() + durationMs;
  chargingActive  = true;

  Serial1.print("<CMD:START:");
  Serial1.print(minutes);
  Serial1.println(">");
  server.send(200, "text/plain", "Charging started for " + String(minutes) + " minutes. Waiting for door to close...");
}

void handleStop() {
  if (!chargingActive) {
    server.send(200, "text/plain", "Already idle.");
    return;
  }
  chargingActive = false;
  Serial1.println("<CMD:STOP>");
  server.send(200, "text/plain", "Charging stopped manually.");
}

void handleUnlockDoor() {
  if (!chargingActive) {
    server.send(400, "text/plain", "No active charging session. Cannot unlock door.");
    return;
  }
  Serial1.println("<CMD:UNLOCK_DOOR>");
  server.send(200, "text/plain", "Door unlock command sent. You can open the door now.");
}

void handleStatus() {
  if (!chargingActive) {
    server.send(200, "text/plain", "Idle – No active charging");
    return;
  }
  long remaining = (long)(chargingEndTime - millis()) / 1000L;
  String resp = "Charging... ";
  if (remaining > 0)
    resp += String(remaining / 60) + " min " + String(remaining % 60) + " sec remaining";
  else
    resp += "OVERTIME – " + String(abs(remaining) / 60) + " min over";
  server.send(200, "text/plain", resp);
}

// --- UART handler (feedback from Arduino) ---
void processUartLine(const String& lineIn) {
  String line = lineIn;
  line.trim();
  if (!line.length()) return;

  Serial.println("[Arduino] " + line);

  if (line.startsWith("ACK:STARTED:")) {
    int minutes = line.substring(12).toInt();
    if (minutes > 0) {
      chargingActive = true;
      chargingEndTime = millis() + (unsigned long)minutes * 60UL * 1000UL;
    }
  } else if (line == "DOOR_LOCKED") {
    Serial.println("✅ Door is now locked. Charging in progress.");
  } else if (line == "CHARGING_STOPPED") {
    chargingActive = false;
    Serial.println("⏹️ Charging stopped by Arduino.");
  }
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, 3, 1);     // RX=3, TX=1
  Serial1.println("<CMD:READY>");
  Serial.println("ESP32 Ready. Connecting to Wi-Fi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("✅ Connected! IP = ");
    Serial.println(WiFi.localIP());
    Serial1.print("<CMD:IP:");
    Serial1.print(WiFi.localIP());
    Serial1.println(">");
  } else {
    Serial.println("\n❌ Wi-Fi connection failed.");
  }

  server.on("/", handleRoot);
  server.on("/start", handleStart);
  server.on("/stop", handleStop);
  server.on("/unlock", handleUnlockDoor);
  server.on("/status", handleStatus);
  server.begin();

  Serial.println("HTTP Server ready!");
  Serial.println("Endpoints:");
  Serial.println("  GET /start?time=X  - Start charging for X minutes");
  Serial.println("  GET /stop          - Stop charging");
  Serial.println("  GET /unlock        - Unlock door during active charging");
  Serial.println("  GET /status        - Check charging status");
}

// --- LOOP ---
void loop() {
  server.handleClient();

  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\r' || c == '\n') {
      if (uartLine.length()) {
        // 🔹 Respond to IP request
        if (uartLine.indexOf("<CMD:REQ_IP") >= 0) {
          String ip = WiFi.localIP().toString();
          Serial1.print("<CMD:IP:");
          Serial1.print(ip);
          Serial1.println(">");
          Serial.print("📤 Sent IP to Arduino: ");
          Serial.println(ip);
        } else {
          processUartLine(uartLine);
        }
        uartLine = "";
      }
    } else {
      uartLine += c;
      if (uartLine.length() > 120) uartLine = "";
    }
  }
}