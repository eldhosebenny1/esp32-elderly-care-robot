#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>
#include <ESP32Servo.h>

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

WebServer server(80);
Preferences preferences;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo medicineServo;

#define IN1 26
#define IN2 27
#define IN3 18
#define IN4 19

#define LEFT_IR 34
#define RIGHT_IR 35
#define BLACK_IS_LOW 1

#define TRIG_PIN 16
#define ECHO_PIN 17

#define BUZZER_PIN 25
#define LED_PIN 23
#define MED_SERVO_PIN 13

const int MED_SERVO_CLOSED = 0;
const int MED_SERVO_OPEN_ANGLES[3] = {45, 90, 135};
const unsigned long MEDICINE_OPEN_MS = 5000;

const int MED_HOURS[3] = {8, 13, 20};
const int MED_MINUTES[3] = {0, 0, 0};
const char* MED_NAMES[3] = {"MORNING", "NOON", "NIGHT"};
const char* MED_KEYS[3] = {"morning", "noon", "night"};

const unsigned long ROOM_WAIT_MS = 30000;

const unsigned long MARKER_CONFIRM_MS = 600;
const unsigned long IGNORE_MARKER_MS = 1500;
const unsigned long TURN_180_MS = 750;
const float OBSTACLE_DISTANCE_CM = 20.0;

const float LOW_BPM_LIMIT = 50.0;
const float HIGH_BPM_LIMIT = 120.0;
const float LOW_TEMP_LIMIT = 35.0;
const float HIGH_TEMP_LIMIT = 38.0;

enum RobotMode {
  IDLE,
  OUTBOUND,
  WAITING_AT_ROOM,
  TURNING_FOR_RETURN,
  RETURNING
};

RobotMode mode = IDLE;

float lastBPM = 0.0;
float lastTemperature = -99.0;
bool latestFall = false;
bool healthAlertActive = false;

String healthStatus = "WAITING FOR REPORT";
String tripReason = "";
String lastEvent = "System ready";

bool obstacleBlocked = false;
int pendingMedicineSlot = -1;
bool medicineDoorOpen = false;
unsigned long medicineOpenUntil = 0;

unsigned long modeStartMs = 0;
unsigned long markerStartMs = 0;
unsigned long ignoreMarkerUntil = 0;
unsigned long lastLCDUpdate = 0;
unsigned long lastScheduleCheck = 0;
unsigned long alarmUntil = 0;

void stopCar();
void forwardCar();
void reverseCar();
void turnRight();
void steerLeft();
void steerRight();

bool isBlack(int pin);
float getDistance();

void startTrip(String reason);
void arriveAtRoom();
void finishTrip();
void runLineFollower();
void runRoomWait();
void runTurnaround();

void checkMedicineSchedule();
void openMedicine(int slot);
void closeMedicine();

void handleRoot();
void handleData();
void handleReport();
void handleMedicineRequest();

void updateHealthStatus();
void updateLCD();
void printLCDLine(int row, String text);

String robotStateText();
String getClockText();

void stopCar() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void forwardCar() {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);

  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void reverseCar() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);

  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

void turnRight() {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);

  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

void steerLeft() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);

  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void steerRight() {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);

  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

bool isBlack(int pin) {
#if BLACK_IS_LOW
  return digitalRead(pin) == LOW;
#else
  return digitalRead(pin) == HIGH;
#endif
}

float getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 25000);

  if (duration == 0) {
    return 999.0;
  }

  return duration * 0.0343 / 2.0;
}

void startTrip(String reason) {
  if (mode != IDLE) {
    if (reason == "HEALTH ALERT") {
      tripReason = reason;
      lastEvent = "Health alert while travelling";

      digitalWrite(LED_PIN, HIGH);
      digitalWrite(BUZZER_PIN, HIGH);
      alarmUntil = millis() + 5000;
    }

    return;
  }

  tripReason = reason;
  mode = OUTBOUND;

  modeStartMs = millis();
  markerStartMs = 0;
  ignoreMarkerUntil = millis() + IGNORE_MARKER_MS;

  stopCar();

  lastEvent = "Trip started: " + reason;

  Serial.println();
  Serial.println("================================");
  Serial.println("ROBOT TRIP STARTED");
  Serial.println(tripReason);
  Serial.println("Following line to room...");
  Serial.println("================================");

  if (reason == "HEALTH ALERT") {
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    alarmUntil = millis() + 5000;
  }
}

void arriveAtRoom() {
  stopCar();

  mode = WAITING_AT_ROOM;
  modeStartMs = millis();
  markerStartMs = 0;

  digitalWrite(LED_PIN, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);

  lastEvent = "Arrived at room: " + tripReason;

  if (pendingMedicineSlot >= 0 && pendingMedicineSlot < 3) {
    openMedicine(pendingMedicineSlot);
  }

  Serial.println();
  Serial.println("================================");
  Serial.println("ARRIVED AT ROOM");
  Serial.println(tripReason);

  if (pendingMedicineSlot >= 0 && pendingMedicineSlot < 3) {
    Serial.print("Medicine: ");
    Serial.println(MED_NAMES[pendingMedicineSlot]);
    Serial.println("Medicine compartment opened.");
  } else {
    Serial.println("Health assistance visit");
  }

  Serial.println("Waiting 30 seconds...");
  Serial.println("================================");
}

void runTurnaround() {
  turnRight();

  if (millis() - modeStartMs >= TURN_180_MS) {
    stopCar();

    mode = RETURNING;
    modeStartMs = millis();

    ignoreMarkerUntil = millis() + IGNORE_MARKER_MS;
    markerStartMs = 0;

    digitalWrite(BUZZER_PIN, LOW);

    lastEvent = "Returning to base";

    Serial.println("Returning to base...");
  }
}

void runRoomWait() {
  stopCar();

  unsigned long elapsed = millis() - modeStartMs;

  if (medicineDoorOpen && millis() >= medicineOpenUntil) {
    closeMedicine();
  }

  if (elapsed < 5000) {
    digitalWrite(BUZZER_PIN, ((millis() / 300) % 2) ? HIGH : LOW);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }

  if (elapsed >= ROOM_WAIT_MS) {
    closeMedicine();
    mode = TURNING_FOR_RETURN;
    modeStartMs = millis();
    lastEvent = "Handover wait finished";

    Serial.println("Room wait completed.");
    Serial.println("Turning around...");
  }
}

void runLineFollower() {
  if (mode != OUTBOUND && mode != RETURNING) {
    return;
  }

  float distance = getDistance();

  if (distance < OBSTACLE_DISTANCE_CM) {
    stopCar();

    obstacleBlocked = true;
    lastEvent = "Obstacle detected - robot stopped";

    digitalWrite(LED_PIN, HIGH);

    Serial.println("OBSTACLE! Robot stopped.");

    return;
  }

  obstacleBlocked = false;

  bool leftBlack = isBlack(LEFT_IR);
  bool rightBlack = isBlack(RIGHT_IR);

  bool bothBlack = leftBlack && rightBlack;

  if (millis() < ignoreMarkerUntil) {
    markerStartMs = 0;
  } else if (bothBlack) {
    if (markerStartMs == 0) {
      markerStartMs = millis();
    }

    if (millis() - markerStartMs >= MARKER_CONFIRM_MS) {
      markerStartMs = 0;

      stopCar();

      if (mode == OUTBOUND) {
        arriveAtRoom();
      } else if (mode == RETURNING) {
        finishTrip();
      }

      return;
    }

    forwardCar();
    return;
  } else {
    markerStartMs = 0;
  }

  if (leftBlack && !rightBlack) {
    steerLeft();
  } else if (!leftBlack && rightBlack) {
    steerRight();
  } else {
    forwardCar();
  }
}

void finishTrip() {
  stopCar();
  closeMedicine();
  pendingMedicineSlot = -1;

  mode = IDLE;
  modeStartMs = millis();

  tripReason = "";

  obstacleBlocked = false;
  markerStartMs = 0;

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  lastEvent = "Robot returned to base";

  Serial.println();
  Serial.println("================================");
  Serial.println("TRIP COMPLETED");
  Serial.println("Robot returned to base.");
  Serial.println("================================");
}

void openMedicine(int slot) {
  if (slot < 0 || slot >= 3) {
    return;
  }

  medicineServo.write(MED_SERVO_OPEN_ANGLES[slot]);
  medicineDoorOpen = true;
  medicineOpenUntil = millis() + MEDICINE_OPEN_MS;

  lastEvent = "Medicine opened: " + String(MED_NAMES[slot]);

  Serial.print("Medicine servo opened: ");
  Serial.println(MED_NAMES[slot]);
  Serial.print("Servo angle: ");
  Serial.println(MED_SERVO_OPEN_ANGLES[slot]);
}

void closeMedicine() {
  if (!medicineDoorOpen) {
    return;
  }

  medicineServo.write(MED_SERVO_CLOSED);
  medicineDoorOpen = false;
  medicineOpenUntil = 0;

  Serial.println("Medicine servo closed.");
}

void checkMedicineSchedule() {
  if (millis() - lastScheduleCheck < 1000) {
    return;
  }

  lastScheduleCheck = millis();

  struct tm timeInfo;

  if (!getLocalTime(&timeInfo, 10)) {
    return;
  }

  if (mode != IDLE) {
    return;
  }

  int dateKey = (timeInfo.tm_year + 1900) * 1000 + timeInfo.tm_yday;

  int nowMinutes = timeInfo.tm_hour * 60 + timeInfo.tm_min;

  int dueSlot = -1;

  for (int i = 0; i < 3; i++) {
    int scheduledMinutes = MED_HOURS[i] * 60 + MED_MINUTES[i];

    int lastDate = preferences.getInt(MED_KEYS[i], -1);

    if (nowMinutes >= scheduledMinutes && lastDate != dateKey) {
      dueSlot = i;
    }
  }

  if (dueSlot < 0) {
    return;
  }

  for (int i = 0; i < dueSlot; i++) {
    int scheduledMinutes = MED_HOURS[i] * 60 + MED_MINUTES[i];

    int lastDate = preferences.getInt(MED_KEYS[i], -1);

    if (nowMinutes >= scheduledMinutes && lastDate != dateKey) {
      preferences.putInt(MED_KEYS[i], dateKey);
    }
  }

  preferences.putInt(MED_KEYS[dueSlot], dateKey);

  String reason = "MEDICINE - ";
  reason += MED_NAMES[dueSlot];

  Serial.println();
  Serial.print("Scheduled medicine visit: ");
  Serial.println(reason);

  pendingMedicineSlot = dueSlot;
  startTrip(reason);
}

String robotStateText() {
  if (obstacleBlocked) {
    return "OBSTACLE STOP";
  }

  switch (mode) {
    case IDLE:
      return "AT BASE";

    case OUTBOUND:
      return "GOING TO ROOM";

    case WAITING_AT_ROOM:
      return "AT ROOM - WAIT";

    case TURNING_FOR_RETURN:
      return "TURNING AROUND";

    case RETURNING:
      return "RETURNING HOME";
  }

  return "UNKNOWN";
}

String getClockText() {
  struct tm timeInfo;

  if (!getLocalTime(&timeInfo, 5)) {
    return "Time unavailable";
  }

  char buffer[24];

  strftime(buffer, sizeof(buffer), "%d-%m-%Y %H:%M:%S", &timeInfo);

  return String(buffer);
}

void handleRoot() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Elderly Care Robot</title>
<style>
body {
  font-family: Arial, sans-serif;
  max-width: 700px;
  margin: 20px auto;
  padding: 15px;
  background: #f1f4f7;
  color: #20252a;
}
h1 { font-size: 25px; }
.card {
  background: white;
  border-radius: 12px;
  padding: 16px;
  margin: 12px 0;
  box-shadow: 0 2px 6px #0001;
}
.value { font-size: 23px; font-weight: bold; }
button {
  padding: 12px 18px;
  font-size: 16px;
  border: 0;
  border-radius: 8px;
  background: #1769aa;
  color: white;
}
small { color: #555; }
</style>
</head>
<body>
<h1>Elderly Care Robot</h1>
<p>Live health report and robot status</p>

<div class="card">
  <small>Heart rate</small>
  <div class="value" id="bpm">--</div>
</div>

<div class="card">
  <small>Temperature</small>
  <div class="value" id="temp">--</div>
</div>

<div class="card">
  <small>Health status</small>
  <div class="value" id="health">Waiting for report</div>
  <p>Fall detected: <b id="fall">--</b></p>
</div>

<div class="card">
  <small>Robot status</small>
  <div class="value" id="robot">Loading...</div>
  <p>Trip reason: <b id="reason">--</b></p>
  <p>Last event: <b id="event">--</b></p>
</div>

<div class="card">
  <small>Current time</small>
  <div id="clock">--</div>
</div>

<div class="card">
  <h3>Medicine schedule</h3>
  <p>Morning: 08:00</p>
  <p>Noon: 13:00</p>
  <p>Night: 20:00</p>
  <button onclick="testTrip()">Test medicine visit</button>
  <p id="response"></p>
</div>

<script>
async function refreshData() {
  try {
    const response = await fetch('/data');
    const d = await response.json();

    document.getElementById('bpm').textContent =
      d.bpm > 0 ? d.bpm.toFixed(1) + ' BPM' : '--';

    document.getElementById('temp').textContent =
      d.temp > -90 ? d.temp.toFixed(1) + ' °C' : '--';

    document.getElementById('health').textContent = d.health;
    document.getElementById('fall').textContent = d.fall ? 'YES' : 'NO';

    document.getElementById('robot').textContent = d.robot;
    document.getElementById('reason').textContent = d.reason;
    document.getElementById('event').textContent = d.event;
    document.getElementById('clock').textContent = d.time;
  } catch (e) {
    document.getElementById('response').textContent =
      'Dashboard connection lost.';
  }
}

async function testTrip() {
  document.getElementById('response').textContent =
    'Requesting medicine visit...';

  try {
    const r = await fetch('/medicine', { method: 'POST' });
    document.getElementById('response').textContent = await r.text();
  } catch (e) {
    document.getElementById('response').textContent = 'Request failed.';
  }

  refreshData();
}

refreshData();
setInterval(refreshData, 1500);
</script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", page);
}

void handleData() {
  String json = "{";

  json += "\"bpm\":" + String(lastBPM, 1) + ",";
  json += "\"temp\":" + String(lastTemperature, 2) + ",";

  json += "\"fall\":";
  json += latestFall ? "true," : "false,";

  json += "\"health\":\"" + healthStatus + "\",";
  json += "\"robot\":\"" + robotStateText() + "\",";
  json += "\"reason\":\"" + tripReason + "\",";
  json += "\"event\":\"" + lastEvent + "\",";
  json += "\"time\":\"" + getClockText() + "\"";

  json += "}";

  server.send(200, "application/json", json);
}

void updateHealthStatus() {
  if (latestFall) {
    healthStatus = "FALL DETECTED";
  } else if (lastTemperature >= HIGH_TEMP_LIMIT) {
    healthStatus = "HIGH TEMPERATURE WARNING";
  } else if (lastTemperature > -90 && lastTemperature < LOW_TEMP_LIMIT) {
    healthStatus = "LOW TEMPERATURE WARNING";
  } else if (lastBPM > 0 && lastBPM < LOW_BPM_LIMIT) {
    healthStatus = "LOW BPM WARNING";
  } else if (lastBPM > HIGH_BPM_LIMIT) {
    healthStatus = "HIGH BPM WARNING";
  } else {
    healthStatus = "NORMAL";
  }
}

void handleReport() {
  if (server.hasArg("bpm")) {
    float value = server.arg("bpm").toFloat();

    if (value >= 0 && value <= 250) {
      lastBPM = value;
    }
  }

  if (server.hasArg("temp")) {
    float value = server.arg("temp").toFloat();

    if (value >= -55 && value <= 125) {
      lastTemperature = value;
    }
  }

  if (server.hasArg("fall")) {
    String value = server.arg("fall");
    value.toLowerCase();

    latestFall = value == "1" || value == "true" || value == "yes";
  }

  updateHealthStatus();

  bool alertNow =
    latestFall ||
    (lastBPM > 0 && lastBPM < LOW_BPM_LIMIT) ||
    (lastBPM > HIGH_BPM_LIMIT) ||
    (lastTemperature >= HIGH_TEMP_LIMIT) ||
    (lastTemperature > -90 && lastTemperature < LOW_TEMP_LIMIT);

  bool previousAlert = healthAlertActive;

  healthAlertActive = alertNow;

  if (alertNow && !previousAlert) {
    lastEvent = "Health alert: " + healthStatus;

    Serial.println();
    Serial.println("HEALTH ALERT RECEIVED");
    Serial.println(healthStatus);

    if (mode == IDLE) {
      startTrip("HEALTH ALERT");
    } else {
      tripReason = "HEALTH ALERT";
    }
  }

  if (!alertNow) {
    lastEvent = "Health report received";
  }

  server.send(200, "text/plain", alertNow ? "HEALTH ALERT RECEIVED" : "REPORT RECEIVED");

  Serial.print("BPM: ");
  Serial.println(lastBPM);

  Serial.print("Temperature: ");
  Serial.println(lastTemperature);

  Serial.print("Health status: ");
  Serial.println(healthStatus);
}

void handleMedicineRequest() {
  if (mode != IDLE) {
    server.send(409, "text/plain", "Robot is already busy.");
    return;
  }

  pendingMedicineSlot = 0;
  startTrip("MEDICINE - MANUAL TEST");

  server.send(200, "text/plain", "Medicine visit started.");
}

void printLCDLine(int row, String text) {
  if (text.length() > 16) {
    text = text.substring(0, 16);
  }

  lcd.setCursor(0, row);
  lcd.print(text);

  for (int i = text.length(); i < 16; i++) {
    lcd.print(' ');
  }
}

void updateLCD() {
  if (millis() - lastLCDUpdate < 700) {
    return;
  }

  lastLCDUpdate = millis();

  printLCDLine(0, robotStateText());

  String line2 = "B:";

  if (lastBPM > 0) {
    line2 += String(lastBPM, 0);
  } else {
    line2 += "--";
  }

  line2 += " T:";

  if (lastTemperature > -90) {
    line2 += String(lastTemperature, 1);
    line2 += "C";
  } else {
    line2 += "--";
  }

  printLCDLine(1, line2);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  stopCar();

  pinMode(LEFT_IR, INPUT);
  pinMode(RIGHT_IR, INPUT);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  digitalWrite(TRIG_PIN, LOW);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  medicineServo.setPeriodHertz(50);
  medicineServo.attach(MED_SERVO_PIN, 500, 2400);
  medicineServo.write(MED_SERVO_CLOSED);

  Wire.begin(21, 22);

  lcd.init();
  lcd.backlight();
  lcd.clear();

  printLCDLine(0, "ELDERLY ROBOT");
  printLCDLine(1, "Starting...");

  preferences.begin("medschedule", false);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.println();
  Serial.println("Connecting to Wi-Fi...");

  unsigned long wifiStart = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected!");

    Serial.print("Robot website IP: http://");
    Serial.println(WiFi.localIP());

    configTime(19800, 0, "pool.ntp.org", "time.google.com");
  } else {
    Serial.println("Wi-Fi connection failed.");
    Serial.println("Website and scheduled visits need Wi-Fi.");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);

  server.on("/report", HTTP_GET, handleReport);
  server.on("/report", HTTP_POST, handleReport);

  server.on("/medicine", HTTP_GET, handleMedicineRequest);
  server.on("/medicine", HTTP_POST, handleMedicineRequest);

  server.begin();

  Serial.println("Web server started.");
  Serial.println("Robot ready.");
}

void loop() {
  server.handleClient();

  checkMedicineSchedule();

  if (mode == OUTBOUND || mode == RETURNING) {
    runLineFollower();
  } else if (mode == WAITING_AT_ROOM) {
    runRoomWait();
  } else if (mode == TURNING_FOR_RETURN) {
    runTurnaround();
  }

  if (alarmUntil > 0 && millis() >= alarmUntil && mode != WAITING_AT_ROOM) {
    digitalWrite(BUZZER_PIN, LOW);
    alarmUntil = 0;
  }

  updateLCD();

  delay(5);
}
