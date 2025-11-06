#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SoftwareSerial.h>
#include <Servo.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);
SoftwareSerial esp32Serial(4, 7);  // RX=4, TX=7
Servo doorServo;

// Pins
const int coinPin = 2;
const int relayPin = 8;
const int servoPin = 9;
const int irSensorPin = 3;

// Vars
volatile int pulseCount = 0;
volatile unsigned long lastPulseTime = 0;
const unsigned long pulseTimeout = 400;
unsigned int totalAmount = 0;
unsigned long chargeStartTime = 0;
bool charging = false;
bool pendingCharging = false;
unsigned int pendingMinutes = 0;
String serialBuffer = "";

// Door states
enum DoorState {
  DOOR_LOCKED,
  DOOR_UNLOCKED,
  DOOR_WAITING_TO_LOCK
};
DoorState doorState = DOOR_LOCKED;

// --- Helpers ---
int getCoinValue(int pulses) {
  if (pulses == 1) return 1;
  if (pulses == 2) return 2;
  if (pulses == 5) return 5;
  if (pulses == 10) return 10;
  return 0;
}

void displayElapsedTime(unsigned long msElapsed) {
  unsigned int sec = (msElapsed / 1000) % 60;
  unsigned int min = (msElapsed / 60000);
  lcd.setCursor(0, 0);
  lcd.print("Time: ");
  if (min < 10) lcd.print('0');
  lcd.print(min);
  lcd.print(':');
  if (sec < 10) lcd.print('0');
  lcd.print(sec);
  lcd.print("   ");
  lcd.setCursor(0, 1);
  lcd.print("Total Rs:");
  lcd.print(totalAmount);
  lcd.print("   ");
}

// --- Door control ---
void lockDoor() { 
  doorServo.write(0); 
  doorState = DOOR_LOCKED;
}

void unlockDoor() { 
  doorServo.write(180); 
  doorState = DOOR_UNLOCKED;
}

void stopCharging() {
  charging = false;
  digitalWrite(relayPin, HIGH);
  pendingCharging = false;
  pendingMinutes = 0;
  esp32Serial.println("CHARGING_STOPPED");
  unlockDoor();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Stopped.");
  delay(1000);
}

void startChargingFromApp(int minutes) {
  charging = true;
  totalAmount = minutes;
  chargeStartTime = millis();
  digitalWrite(relayPin, LOW);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("App OK ");
  lcd.setCursor(0, 1);
  lcd.print("Time ");
  lcd.print(minutes);
  lcd.print("min");
  esp32Serial.print("ACK:STARTED:");
  esp32Serial.println(minutes);
}

// --- ESP32 commands ---
void processESP32Command(String cmd) {
  cmd.trim();
  if (!cmd.startsWith("<CMD:") || !cmd.endsWith(">")) return;
  cmd = cmd.substring(5, cmd.length() - 1);

  if (cmd == "READY") {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("ESP Ready");
    delay(500);
    esp32Serial.println("<CMD:REQ_IP>");
  } else if (cmd.startsWith("IP:")) {
    String ip = cmd.substring(3);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("ESP32 IP:");
    lcd.setCursor(0, 1);
    lcd.print(ip);
    Serial.print("ESP32 IP: ");
    Serial.println(ip);
  } else if (cmd.startsWith("START:")) {
    int mins = cmd.substring(6).toInt();
    pendingCharging = (mins > 0);
    if (pendingCharging) {
      pendingMinutes = mins;
      charging = false;
      digitalWrite(relayPin, HIGH);
      doorState = DOOR_WAITING_TO_LOCK;
      unlockDoor();
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Close door...");
    } else {
      esp32Serial.println("ERR:INVALID_TIME");
    }
  } else if (cmd == "STOP") {
    stopCharging();
  } else if (cmd == "UNLOCK_DOOR") {
    // App wants to unlock door while charging is active
    if (charging) {
      unlockDoor();
      doorState = DOOR_WAITING_TO_LOCK;
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Door unlocked");
      lcd.setCursor(0, 1);
      lcd.print("by app");
      delay(1000);
      esp32Serial.println("DOOR_UNLOCKED");
    }
  }
}

// --- ISR ---
void pulseISR() {
  static unsigned long last = 0;
  unsigned long now = millis();
  if (now - last > 5) {
    pulseCount++;
    lastPulseTime = now;
    last = now;
  }
}

// --- SETUP ---
void setup() {
  Serial.begin(9600);
  esp32Serial.begin(9600);
  lcd.init(); lcd.backlight();
  lcd.print("Initializing...");
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH);
  pinMode(coinPin, INPUT_PULLUP);
  pinMode(irSensorPin, INPUT);
  doorServo.attach(servoPin);
  lockDoor();
  attachInterrupt(digitalPinToInterrupt(coinPin), pulseISR, FALLING);
  lcd.clear();
  lcd.print("System Ready");
}

// --- LOOP ---
void loop() {
  while (esp32Serial.available()) {
    char c = esp32Serial.read();
    if (c == '\r' || c == '\n') {
      if (serialBuffer.length()) {
        processESP32Command(serialBuffer);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
      if (serialBuffer.length() > 120) serialBuffer = "";
    }
  }

  unsigned long now = millis();
  if ((now - lastPulseTime) > pulseTimeout && pulseCount > 0) {
    noInterrupts();
    int p = pulseCount; pulseCount = 0;
    interrupts();
    int coinVal = getCoinValue(p);
    if (coinVal > 0) totalAmount += coinVal;
  }

  // Handle door locking when waiting
  if (doorState == DOOR_WAITING_TO_LOCK) {
    // Read IR sensor (assuming LOW = blocked/closed, HIGH = open)
    // Adjust based on your IR sensor logic
    bool doorClosed = (digitalRead(irSensorPin) == LOW);
    
    if (doorClosed) {
      lockDoor();
      esp32Serial.println("DOOR_LOCKED");
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Door locked!");
      delay(1000);
      if (pendingCharging) {
        startChargingFromApp(pendingMinutes);
        pendingCharging = false;
        pendingMinutes = 0;
      }
    } else {
      // Keep waiting, show message
      static unsigned long lastBlink = 0;
      if (now - lastBlink > 500) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Waiting for");
        lcd.setCursor(0, 1);
        lcd.print("door close...");
        lastBlink = now;
      }
    }
  }

  static unsigned long lastUpdate = 0;
  if (charging && doorState == DOOR_LOCKED && (now - lastUpdate >= 1000)) {
    displayElapsedTime(now - chargeStartTime);
    lastUpdate = now;
  }
}