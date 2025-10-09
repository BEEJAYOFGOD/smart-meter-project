#include <WiFi.h>
#include <HTTPClient.h>
#include <PZEM004Tv30.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

// WiFi credentials
#define WIFI_SSID "TECNO SPARK 7P"
#define WIFI_PASSWORD "omo123456789"

// Firebase Database URL
const char* baseURL = "https://smart-meter-942ce-default-rtdb.firebaseio.com/smartMeter.json";
const char* settingsURL = "https://smart-meter-942ce-default-rtdb.firebaseio.com/smartMeter/settings.json";
const char* currentDayURL = "https://smart-meter-942ce-default-rtdb.firebaseio.com/smartMeter/currentDay.json";

// Pin definitions
#define PZEM_RX_PIN 16
#define PZEM_TX_PIN 17
#define RELAY_PIN 15
#define BUZZER_PIN 5

// NTP Server for time sync
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;      // WAT (West Africa Time) = UTC+1
const int daylightOffset_sec = 0;

// Hardware objects
PZEM004Tv30 pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN);
Preferences prefs;

// Timing variables
unsigned long prevSend = 0;
unsigned long prevReceive = 0;
unsigned long lastEnergyCalc = 0;
const unsigned long SEND_INTERVAL = 60000;      // Send to Firebase every 60 seconds
const unsigned long RECEIVE_INTERVAL = 2000;    // Check commands every 2 seconds
const unsigned long ENERGY_CALC_INTERVAL = 500; // Calculate energy every 0.5 seconds

// Energy tracking variables
float dailyConsumption = 0.0;        // kWh consumed today
float dailyLimit = 10.5;             // kWh daily limit (default)
float hourlyData[24] = {0};          // Track hourly consumption
int currentHour = -1;
String currentDate = "";
String relayCommand = "auto";        // "auto", "on", "off"
bool relayState = false;             // Current relay state
bool loadStatus = false;             // Load status for Firebase
String controlMode = "auto";         // "auto" or "manual"

// For energy calculation
float lastPower = 0.0;
bool monitoringActive = false;
int consecutiveErrors = 0;
const int MAX_ERRORS = 5;

void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN);
  
  // Initialize pins
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);  // Start with relay OFF (active-LOW)
  digitalWrite(BUZZER_PIN, LOW);  // Buzzer off
  
  Serial.println("\n==================================");
  Serial.println("ESP32 PZEM Smart Meter with Firebase");
  Serial.println("==================================\n");
  
  // Connect to WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  
  // Sync time with NTP
  syncTime();
  
  // Load saved data from local storage
  loadDayDataLocal();
  
  // Get current date
  currentDate = getCurrentDate();
  currentHour = getCurrentHourOnly();
  
  Serial.print("Current Date: ");
  Serial.println(currentDate);
  Serial.print("Loaded Consumption: ");
  Serial.print(dailyConsumption, 3);
  Serial.println(" kWh\n");
  
  // Test PZEM connection
  if (!testPZEMConnection()) {
    Serial.println("ERROR: Cannot communicate with PZEM sensor!");
    Serial.println("Check wiring and restart.");
    while(1) { delay(1000); }
  }
  
  Serial.println("PZEM sensor connected successfully!\n");
  
  // Fetch settings from Firebase
  fetchSettingsFromFirebase();
  
  delay(1000);
  
  // Start monitoring
  turnRelayOn();
}

void loop() {
  unsigned long currentMillis = millis();
  String timestamp = getTimestamp();
  
  // Check if date has changed (new day)
  String newDate = getCurrentDate();
  int newHour = getCurrentHourOnly();
  
  if (newDate != currentDate) {
    // Archive yesterday's data to Firebase history
    archiveDayToHistory();
    
    // Reset for new day
    currentDate = newDate;
    dailyConsumption = 0.0;
    memset(hourlyData, 0, sizeof(hourlyData));
    saveDayDataLocal();
    Serial.println("\n*** NEW DAY STARTED ***\n");
  }
  
  // Track hourly consumption
  if (newHour != currentHour) {
    currentHour = newHour;
  }
  
  // ====== CALCULATE ENERGY (every 0.5 seconds) ======
  if (currentMillis - lastEnergyCalc >= ENERGY_CALC_INTERVAL) {
    lastEnergyCalc = currentMillis;
    
    if (relayState && monitoringActive) {
      if (calculateEnergy()) {
        consecutiveErrors = 0;
        
        // Check daily limit in auto mode
        if (controlMode == "auto") {
          checkDailyLimit();
        }
      } else {
        consecutiveErrors++;
        if (consecutiveErrors >= MAX_ERRORS) {
          Serial.println("\n*** TOO MANY READ ERRORS - SAFETY SHUTOFF ***");
          turnRelayOff();
        }
      }
    }
  }
  
  // ====== SEND DATA TO FIREBASE (every 60 seconds) ======
  if (currentMillis - prevSend >= SEND_INTERVAL) {
    prevSend = currentMillis;
    
    if (WiFi.status() == WL_CONNECTED) {
      updateCurrentDayFirebase();
      saveDayDataLocal(); // Also save locally
    } else {
      Serial.println("WiFi disconnected - data saved locally");
      saveDayDataLocal();
    }
  }
  
  // ====== RECEIVE COMMANDS FROM FIREBASE (every 2 seconds) ======
  if (currentMillis - prevReceive >= RECEIVE_INTERVAL) {
    prevReceive = currentMillis;
    
    if (WiFi.status() == WL_CONNECTED) {
      receiveCommandsFromFirebase();
    }
  }
}

// ====== TIME FUNCTIONS ======
void syncTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.print("Syncing time");
  for (int i = 0; i < 10; i++) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      Serial.println(" Done!");
      return;
    }
    delay(1000);
    Serial.print(".");
  }
  Serial.println(" Failed! Using default time.");
}

String getTimestamp() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buffer[25];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buffer);
  }
  return "2025-10-02 12:00:00"; // Fallback
}

String getCurrentDate() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buffer[15];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
    return String(buffer);
  }
  return "2025-10-02"; // Fallback
}

String getCurrentYearMonth() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buffer[10];
    strftime(buffer, sizeof(buffer), "%Y-%m", &timeinfo);
    return String(buffer);
  }
  return "2025-10"; // Fallback
}

int getCurrentHourOnly() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    return timeinfo.tm_hour;
  }
  return 0;
}

// ====== PZEM FUNCTIONS ======
bool testPZEMConnection() {
  for (int i = 0; i < 3; i++) {
    float testVoltage = pzem.voltage();
    if (!isnan(testVoltage) && testVoltage > 0) {
      return true;
    }
    delay(500);
  }
  return false;
}

bool calculateEnergy() {
  float voltage = pzem.voltage();
  float current = pzem.current();
  float power = pzem.power();
  
  // Validate readings
  if (isnan(voltage) || isnan(power)) {
    Serial.println("Error: Invalid sensor readings!");
    return false;
  }
  
  if (voltage < 0 || voltage > 300 || power < 0 || power > 5000) {
    Serial.println("Error: Reading out of range!");
    return false;
  }
  
  // Calculate energy consumed in this interval
  // Energy (kWh) = Power (W) × Time (h) / 1000
  float timeInHours = ENERGY_CALC_INTERVAL / 3600000.0;
  float energyIncrement = (power * timeInHours) / 1000.0; // Convert to kWh
  
  // Add to daily consumption
  dailyConsumption += energyIncrement;
  
  // Add to hourly data
  if (currentHour >= 0 && currentHour < 24) {
    hourlyData[currentHour] += energyIncrement;
  }
  
  // Display readings
  Serial.println("----------------------------------");
  Serial.print("Voltage: ");
  Serial.print(voltage, 2);
  Serial.println(" V");
  Serial.print("Current: ");
  Serial.print(current, 3);
  Serial.println(" A");
  Serial.print("Power: ");
  Serial.print(power, 2);
  Serial.println(" W");
  Serial.print("Daily Consumption: ");
  Serial.print(dailyConsumption, 4);
  Serial.print(" kWh / ");
  Serial.print(dailyLimit, 1);
  Serial.println(" kWh");
  Serial.print("Percentage: ");
  Serial.print((dailyConsumption / dailyLimit) * 100.0, 1);
  Serial.println("%");
  Serial.println("----------------------------------\n");
  
  return true;
}

// ====== RELAY CONTROL ======
void turnRelayOn() {
  digitalWrite(RELAY_PIN, LOW);  // LOW = ON for active-LOW relay
  relayState = true;
  loadStatus = true;
  monitoringActive = true;
  consecutiveErrors = 0;
  
  Serial.println("\n>>> RELAY TURNED ON <<<");
  Serial.println("Monitoring ACTIVE\n");
}

void turnRelayOff() {
  digitalWrite(RELAY_PIN, HIGH);  // HIGH = OFF for active-LOW relay
  relayState = false;
  loadStatus = false;
  monitoringActive = false;
  
  Serial.println("\n>>> RELAY TURNED OFF <<<");
  Serial.print("Final Daily Consumption: ");
  Serial.print(dailyConsumption, 3);
  Serial.println(" kWh\n");
}

void checkDailyLimit() {
  if (dailyConsumption >= dailyLimit) {
    Serial.println("\n*** DAILY LIMIT REACHED! ***");
    Serial.print("Consumed: ");
    Serial.print(dailyConsumption, 3);
    Serial.println(" kWh");
    Serial.println("Turning relay OFF...\n");
    
    soundBuzzerAlert();
    turnRelayOff();
    
    // Update Firebase immediately
    updateCurrentDayFirebase();
  }
}

void soundBuzzerAlert() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(200);
    digitalWrite(BUZZER_PIN, LOW);
    delay(200);
  }
}

// ====== LOCAL STORAGE (Preferences) ======
void saveDayDataLocal() {
  prefs.begin("smartmeter", false);
  prefs.putString("date", currentDate);
  prefs.putFloat("consumption", dailyConsumption);
  prefs.end();
  Serial.println("Data saved locally");
}

void loadDayDataLocal() {
  prefs.begin("smartmeter", true);
  String savedDate = prefs.getString("date", "");
  float savedConsumption = prefs.getFloat("consumption", 0.0);
  prefs.end();
  
  String today = getCurrentDate();
  if (savedDate == today) {
    dailyConsumption = savedConsumption;
    Serial.println("Restored today's data from local storage");
  } else {
    dailyConsumption = 0.0;
    Serial.println("Starting fresh for new day");
  }
}

// ====== FIREBASE FUNCTIONS ======
void fetchSettingsFromFirebase() {
  HTTPClient http;
  http.begin(settingsURL);
  int httpResponseCode = http.GET();
  
  if (httpResponseCode == HTTP_CODE_OK) {
    String response = http.getString();
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, response);
    
    if (!error) {
      if (doc.containsKey("dailyLimit")) {
        dailyLimit = doc["dailyLimit"];
        Serial.print("Daily Limit fetched: ");
        Serial.print(dailyLimit, 1);
        Serial.println(" kWh");
      }
      if (doc.containsKey("relayCommand")) {
        relayCommand = doc["relayCommand"].as<String>();
        Serial.print("Relay Command: ");
        Serial.println(relayCommand);
      }
    }
  }
  http.end();
}

void updateCurrentDayFirebase() {
  Serial.println("Updating Firebase...");
  
  // Prepare currentDay data
  String payload = "{";
  payload += "\"date\": \"" + currentDate + "\",";
  payload += "\"consumption\": " + String(dailyConsumption, 3) + ",";
  payload += "\"loadStatus\": \"" + String(loadStatus ? "on" : "off") + "\",";
  payload += "\"lastUpdated\": " + String(millis()) + ",";
  payload += "\"controlMode\": \"" + controlMode + "\"";
  payload += "}";
  
  HTTPClient http;
  http.begin(currentDayURL);
  http.addHeader("Content-Type", "application/json");
  int httpResponseCode = http.PATCH(payload);
  
  if (httpResponseCode > 0) {
    Serial.print("Firebase updated: ");
    Serial.println(httpResponseCode);
  } else {
    Serial.print("Firebase update failed: ");
    Serial.println(httpResponseCode);
  }
  http.end();
}

void receiveCommandsFromFirebase() {
  HTTPClient http;
  http.begin(settingsURL);
  int httpResponseCode = http.GET();
  
  if (httpResponseCode == HTTP_CODE_OK) {
    String response = http.getString();
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, response);
    
    if (!error) {
      // Check for relay command changes
      if (doc.containsKey("relayCommand")) {
        String newCommand = doc["relayCommand"].as<String>();
        
        if (newCommand != relayCommand) {
          relayCommand = newCommand;
          Serial.print("New relay command received: ");
          Serial.println(relayCommand);
          
          if (relayCommand == "off") {
            controlMode = "manual";
            turnRelayOff();
          } else if (relayCommand == "on") {
            controlMode = "manual";
            turnRelayOn();
          } else if (relayCommand == "auto") {
            controlMode = "auto";
            Serial.println("Switched to AUTO mode");
            // Check limit immediately
            if (dailyConsumption < dailyLimit) {
              turnRelayOn();
            } else {
              turnRelayOff();
            }
          }
          
          // Update Firebase with new control mode
          updateCurrentDayFirebase();
        }
      }
      
      // Check for daily limit changes
      if (doc.containsKey("dailyLimit")) {
        float newLimit = doc["dailyLimit"];
        if (newLimit != dailyLimit) {
          dailyLimit = newLimit;
          Serial.print("Daily limit updated to: ");
          Serial.print(dailyLimit, 1);
          Serial.println(" kWh");
          
          // If in auto mode, recheck the limit
          if (controlMode == "auto") {
            if (dailyConsumption >= dailyLimit && relayState) {
              Serial.println("New limit exceeded - turning OFF");
              turnRelayOff();
              updateCurrentDayFirebase();
            } else if (dailyConsumption < dailyLimit && !relayState) {
              Serial.println("Below new limit - turning ON");
              turnRelayOn();
              updateCurrentDayFirebase();
            }
          }
        }
      }
    }
  }
  http.end();
}

void archiveDayToHistory() {
  Serial.println("Archiving day to Firebase history...");
  
  String yearMonth = getCurrentYearMonth();
  String historyURL = "https://smart-meter-942ce-default-rtdb.firebaseio.com/smartMeter/history/" + 
                      yearMonth + "/" + currentDate + ".json";
  
  // Prepare hourly data
  String hourlyDataJson = "{";
  for (int i = 0; i < 24; i++) {
    if (i > 0) hourlyDataJson += ",";
    hourlyDataJson += "\"" + String(i) + "\": " + String(hourlyData[i], 4);
  }
  hourlyDataJson += "}";
  
  // Prepare history payload
  String payload = "{";
  payload += "\"totalConsumption\": " + String(dailyConsumption, 3) + ",";
  if (!loadStatus) {
    payload += "\"loadCutoffTime\": " + String(millis()) + ",";
  }
  payload += "\"hourlyData\": " + hourlyDataJson;
  payload += "}";
  
  HTTPClient http;
  http.begin(historyURL);
  http.addHeader("Content-Type", "application/json");
  int httpResponseCode = http.PUT(payload);
  
  if (httpResponseCode > 0) {
    Serial.println("History archived successfully");
  } else {
    Serial.println("History archive failed");
  }
  http.end();
}