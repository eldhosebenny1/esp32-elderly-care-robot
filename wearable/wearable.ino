#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <MAX30105.h>
#include <math.h>

const char* WIFI_NAME = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

const char* ROBOT_IP = "192.168.137.85";

#define TEMP_PIN 4

#define MPU_ADDR       0x68
#define MPU_PWR        0x6B
#define MPU_ACCEL_CFG  0x1C
#define MPU_ACCEL_X    0x3B

MAX30105 pulseSensor;

OneWire oneWire(TEMP_PIN);
DallasTemperature tempSensor(&oneWire);

bool pulseSensorAvailable = false;
bool mpuAvailable = false;
bool tempSensorAvailable = false;

const bool DEMO_RANDOM_BPM = true;

float stableBPM = 0.0;
float instantBPM = 0.0;

byte validBeats = 0;

unsigned long lastDemoBpmMs = 0;
const unsigned long DEMO_BPM_INTERVAL_MS = 3500;

const float LOW_BPM = 50.0;
const float HIGH_BPM = 120.0;

long lastIRValue = 0;
const long IR_THRESHOLD = 10000;

bool validPulseSignal = false;

float temperatureC = -127.0;

bool tempConversionPending = false;
unsigned long tempRequestMs = 0;
unsigned long lastTemperatureCycleMs = 0;

const float FEVER_TEMP = 38.0;
const float LOW_TEMP = 35.0;

const float FREE_FALL_G = 0.60;
const float IMPACT_G = 2.50;
const float STRONG_IMPACT_G = 3.50;

const float STILL_MIN_G = 0.70;
const float STILL_MAX_G = 1.30;

const unsigned long FALL_CONFIRM_TIME = 2000;
const unsigned long FREE_FALL_TIMEOUT = 1500;
const unsigned long FALL_TIMEOUT_MS = 8000;

bool freeFallDetected = false;
bool fallCandidate = false;
bool fallConfirmed = false;
bool impactDetected = false;

unsigned long freeFallTime = 0;
unsigned long impactTime = 0;
unsigned long stillStartMs = 0;
unsigned long lastAccelPrintMs = 0;

float currentG = -1.0;

unsigned long lastReportMs = 0;
unsigned long lastWiFiTryMs = 0;

const unsigned long REPORT_INTERVAL_MS = 2000;

bool wifiWasConnected = false;

String healthStatus = "STARTING";

bool readMPUAccel(float &x, float &y, float &z) {
  if (!mpuAvailable) {
    return false;
  }

  Wire.beginTransmission(MPU_ADDR);
  Wire.write(MPU_ACCEL_X);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)6) != 6) {
    return false;
  }

  int16_t rawX = (int16_t)(((uint16_t)Wire.read() << 8) | Wire.read());
  int16_t rawY = (int16_t)(((uint16_t)Wire.read() << 8) | Wire.read());
  int16_t rawZ = (int16_t)(((uint16_t)Wire.read() << 8) | Wire.read());

  x = rawX / 4096.0;
  y = rawY / 4096.0;
  z = rawZ / 4096.0;

  return true;
}

void updateDemoBPM() {
  if (!DEMO_RANDOM_BPM) {
    return;
  }

  unsigned long now = millis();

  if (lastDemoBpmMs != 0 && now - lastDemoBpmMs < DEMO_BPM_INTERVAL_MS) {
    return;
  }

  lastDemoBpmMs = now;

  int choice = random(0, 12);

  if (choice == 0) {
    stableBPM = random(42, 50);
  } else if (choice == 1) {
    stableBPM = random(121, 136);
  } else {
    stableBPM = random(65, 101);
  }

  instantBPM = stableBPM;

  validBeats = 5;

  Serial.println();
  Serial.println("***** SIMULATED EXHIBITION BPM *****");
  Serial.printf("Demo BPM value: %.1f\n", stableBPM);
  Serial.println("This value is RANDOM, not a real measurement.");
  
}

void readPulseSignal() {
  if (!pulseSensorAvailable) {
    lastIRValue = 0;
    validPulseSignal = false;
    return;
  }

  lastIRValue = pulseSensor.getIR();
  validPulseSignal = (lastIRValue >= IR_THRESHOLD);
}

void readTemperature() {
  if (!tempSensorAvailable) {
    temperatureC = -127.0;
    return;
  }

  unsigned long now = millis();

  if (!tempConversionPending) {
    if (now - lastTemperatureCycleMs >= 2000) {
      tempSensor.requestTemperatures();

      tempRequestMs = now;
      tempConversionPending = true;
    }

    return;
  }

  if (now - tempRequestMs >= 800) {
    float t = tempSensor.getTempCByIndex(0);

    if (t != DEVICE_DISCONNECTED_C && t >= -55.0 && t <= 125.0) {
      temperatureC = t;
    } else {
      temperatureC = -127.0;
      Serial.println("DS18B20 ERROR: Invalid reading");
    }

    tempConversionPending = false;
    lastTemperatureCycleMs = now;
  }
}

void checkFall() {
  if (fallConfirmed || !mpuAvailable) {
    return;
  }

  float x, y, z;

  if (!readMPUAccel(x, y, z)) {
    return;
  }

  currentG = sqrt(x * x + y * y + z * z);

  unsigned long now = millis();

  if (now - lastAccelPrintMs >= 500) {
    lastAccelPrintMs = now;

    Serial.printf("MPU | X: %.2f G | Y: %.2f G | Z: %.2f G | TOTAL: %.2f G\n",
                  x, y, z, currentG);
  }

  if (currentG < FREE_FALL_G) {
    if (!freeFallDetected) {
      Serial.println("FREE FALL DETECTED");
    }

    freeFallDetected = true;
    freeFallTime = now;
  }

  if (freeFallDetected && now - freeFallTime > FREE_FALL_TIMEOUT) {
    freeFallDetected = false;
  }

  if (!fallCandidate) {
    bool impactAfterFreeFall = freeFallDetected && currentG >= IMPACT_G;

    bool veryStrongImpact = currentG >= STRONG_IMPACT_G;

    if (impactAfterFreeFall || veryStrongImpact) {
      fallCandidate = true;
      impactDetected = true;

      impactTime = now;
      stillStartMs = 0;

      freeFallDetected = false;

      Serial.println();
      
      Serial.println("       !!! IMPACT DETECTED !!!");
      Serial.printf("       Acceleration: %.2f G\n", currentG);
      Serial.println("       Checking subsequent movement");
      
    }
  }

  if (fallCandidate) {
    bool isStill = currentG >= STILL_MIN_G && currentG <= STILL_MAX_G;

    if (isStill) {
      if (stillStartMs == 0) {
        stillStartMs = now;
      }

      if (now - stillStartMs >= FALL_CONFIRM_TIME &&
          now - impactTime >= FALL_CONFIRM_TIME) {
        fallCandidate = false;
        fallConfirmed = true;

        Serial.println();
        
        Serial.println("          FALL CONFIRMED");
       
        Serial.println("Please check the person.");
      }
    } else {
      stillStartMs = 0;
    }

    if (now - impactTime > FALL_TIMEOUT_MS && !fallConfirmed) {
      fallCandidate = false;
      stillStartMs = 0;

      Serial.println("Fall candidate cancelled/expired.");
    }
  }
}

String getHealthStatus() {
  if (fallConfirmed) {
    return "FALL";
  }

  if (stableBPM > 0) {
    if (stableBPM < LOW_BPM) {
      return "LOW_BPM";
    }

    if (stableBPM > HIGH_BPM) {
      return "HIGH_BPM";
    }
  }

  if (temperatureC == -127.0) {
    return "TEMP_SENSOR_ERROR";
  }

  if (temperatureC >= FEVER_TEMP) {
    return "HIGH_TEMP";
  }

  if (temperatureC < LOW_TEMP) {
    return "LOW_TEMP";
  }

  return "NORMAL";
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;

      Serial.println();
      Serial.println("WEARABLE WIFI CONNECTED");

      Serial.print("Wi-Fi network: ");
      Serial.println(WiFi.SSID());

      Serial.print("Wearable IP: ");
      Serial.println(WiFi.localIP());

      Serial.print("Configured robot IP: ");
      Serial.println(ROBOT_IP);
    }

    return;
  }

  if (wifiWasConnected) {
    Serial.println("Wearable Wi-Fi disconnected");
    wifiWasConnected = false;
  }

  if (millis() - lastWiFiTryMs >= 10000) {
    lastWiFiTryMs = millis();

    Serial.println("Reconnecting to laptop Wi-Fi...");
    WiFi.begin(WIFI_NAME, WIFI_PASS);
  }
}

void sendReport() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("HTTP REPORT SKIPPED: Wi-Fi disconnected");
    return;
  }

  healthStatus = getHealthStatus();

  String url = "http://";
  url += ROBOT_IP;
  url += "/report?bpm=";
  url += String(stableBPM, 1);

  url += "&temp=";
  url += String(temperatureC, 2);

  url += "&fall=";
  url += (fallConfirmed ? "1" : "0");

  url += "&pulsevalid=";
  url += (stableBPM > 0 ? "1" : "0");

  url += "&bpmmode=SIMULATED";

  url += "&impact=";
  url += (impactDetected ? "1" : "0");

  url += "&g=";
  url += String(currentG, 2);

  url += "&status=";
  url += healthStatus;

  Serial.println();
  Serial.println("----- SENDING REPORT TO ROBOT -----");
  Serial.print("Request URL: ");
  Serial.println(url);

  WiFiClient client;
  HTTPClient http;

  http.setConnectTimeout(2500);
  http.setTimeout(2500);

  if (!http.begin(client, url)) {
    Serial.println("HTTP ERROR: Could not initialize request");
    return;
  }

  int code = http.GET();

  Serial.print("HTTP result code: ");
  Serial.println(code);

  if (code < 0) {
    Serial.print("Error details: ");
    Serial.println(http.errorToString(code));

    Serial.println("Check:");
    Serial.println("1. The robot's current IP address.");
    Serial.println("2. The robot's web server is running.");
    Serial.println("3. server.handleClient() is in the robot loop.");
    Serial.println("4. Both boards can communicate on this Wi-Fi.");
  } else {
    Serial.print("Robot response: ");
    Serial.println(http.getString());

    if (code >= 200 && code < 300) {
      impactDetected = false;
    }
  }

  http.end();

  Serial.println("-----------------------------------");
}

void printReport() {
  healthStatus = getHealthStatus();

  Serial.println();
 
  Serial.println("        WEARABLE HEALTH REPORT");
  

  Serial.printf("BPM MODE              : %s\n",
                DEMO_RANDOM_BPM ? "RANDOM SIMULATION" : "SENSOR");

  Serial.printf("Displayed BPM         : %.1f\n", stableBPM);

  Serial.println("NOTE                  : BPM is simulated, NOT a real reading.");

  Serial.printf("MAX30102 Raw IR       : %ld\n", lastIRValue);

  Serial.printf("MAX30102 Signal       : %s\n",
                validPulseSignal ? "STRONG ENOUGH" : "WEAK / NOT DETECTED");

  Serial.printf("Temperature           : %.2f C\n", temperatureC);

  if (currentG >= 0) {
    Serial.printf("Acceleration          : %.2f G\n", currentG);
  } else {
    Serial.println("Acceleration          : UNAVAILABLE");
  }

  Serial.printf("Impact this report    : %s\n", impactDetected ? "YES" : "NO");

  Serial.printf("Fall candidate        : %s\n", fallCandidate ? "YES" : "NO");

  Serial.printf("Fall confirmed        : %s\n", fallConfirmed ? "YES" : "NO");

  Serial.printf("Wearable Wi-Fi        : %s\n",
                WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");

  Serial.print("Robot IP              : ");
  Serial.println(ROBOT_IP);

  Serial.print("Health status         : ");
  Serial.println(healthStatus);

  
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  randomSeed(esp_random());

  Serial.println();
  Serial.println("ELDERLY CARE WEARABLE");
  Serial.println("Exhibition demonstration version");
  Serial.println("Random BPM simulation is ENABLED.");

  Wire.begin(21, 22);

  Serial.println("Checking MAX30102...");

  pulseSensorAvailable = pulseSensor.begin(Wire, I2C_SPEED_FAST);

  if (pulseSensorAvailable) {
    pulseSensor.setup(0x1F, 8, 2, 100, 411, 4096);
    pulseSensor.setPulseAmplitudeIR(0x1F);
    pulseSensor.setPulseAmplitudeRed(0x0A);
    pulseSensor.setPulseAmplitudeGreen(0);

    Serial.println("MAX30102 detected; raw IR debugging enabled.");
  } else {
    Serial.println("MAX30102 not detected. Random demo BPM will still work.");
  }

  Serial.println("Checking MPU6050...");

  Wire.beginTransmission(MPU_ADDR);
  mpuAvailable = (Wire.endTransmission() == 0);

  if (mpuAvailable) {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(MPU_PWR);
    Wire.write(0x00);
    bool wakeOK = (Wire.endTransmission() == 0);

    Wire.beginTransmission(MPU_ADDR);
    Wire.write(MPU_ACCEL_CFG);
    Wire.write(0x10);
    bool configOK = (Wire.endTransmission() == 0);

    mpuAvailable = wakeOK && configOK;
  }

  Serial.println(mpuAvailable ? "MPU6050 ready." : "MPU6050 not detected.");

  Serial.println("Checking DS18B20...");

  tempSensor.begin();
  tempSensor.setWaitForConversion(false);

  tempSensorAvailable = tempSensor.getDeviceCount() > 0;

  if (tempSensorAvailable) {
    Serial.println("DS18B20 detected.");
  } else {
    Serial.println("DS18B20 not detected. Check DATA wiring and pull-up resistor.");
  }

  lastTemperatureCycleMs = millis() - 2000;

  Serial.println("Connecting to laptop Wi-Fi...");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_NAME, WIFI_PASS);

  lastWiFiTryMs = millis();

  Serial.println("Wearable setup completed.");
}

void loop() {
  updateDemoBPM();
  readPulseSignal();

  readTemperature();
  checkFall();

  connectWiFi();

  if (millis() - lastReportMs >= REPORT_INTERVAL_MS) {
    lastReportMs = millis();

    printReport();
    sendReport();
  }

  delay(5);
}
