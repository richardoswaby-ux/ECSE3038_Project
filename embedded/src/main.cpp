#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "env.h"

// ==========================================
// 1. Hardware Pin Definitions
// ==========================================
#define ONE_WIRE_BUS 4  // DS18B20 Data Pin (Requires 4.7kΩ pull-up to 3.3V)
#define PIR_PIN 15      // HC-SR501 Output Pin (Powered by 5V/VIN, Output is 3.3V)
#define LIGHT_PIN 22    // Gate of IRLZ44N MOSFET for 12V Light
#define FAN_PIN 23      // Gate of IRLZ44N MOSFET for 12V Fan

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// Non-blocking timer variables
unsigned long previousMillis = 0;
const long interval = 5000; // Poll the server every 5 seconds

// ==========================================
// 2. Setup & Boot Sequence
// ==========================================
void setup() {
    Serial.begin(115200);
    
    // Configure Actuators
    pinMode(LIGHT_PIN, OUTPUT);
    pinMode(FAN_PIN, OUTPUT);
    digitalWrite(LIGHT_PIN, LOW);
    digitalWrite(FAN_PIN, LOW);
    
    // Configure Sensors
    pinMode(PIR_PIN, INPUT);
    sensors.begin();

    // Standard Wi-Fi Boilerplate
    Serial.print("Connecting to Wi-Fi");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
    } else {
        Serial.println("\nWi-Fi timeout. Booting in offline mode.");
    }
}

// ==========================================
// 3. Execution Loop
// ==========================================
void loop() {
    unsigned long currentMillis = millis();

    // Enterprise Wi-Fi Reconnection Guard
    if (WiFi.status() != WL_CONNECTED) {
        if (currentMillis - previousMillis >= interval) {
            previousMillis = currentMillis;
            Serial.println("Network dropped. Attempting to reconnect...");
            WiFi.disconnect();
            WiFi.reconnect();
        }
        return; // Safely abort HTTP requests until reconnected
    }

    // Non-blocking execution gate
    if (currentMillis - previousMillis >= interval) {
        previousMillis = currentMillis;

        // --- A. DATA ACQUISITION ---
        sensors.requestTemperatures();
        float currentTemp = sensors.getTempCByIndex(0);
        bool presenceDetected = digitalRead(PIR_PIN);
        
        Serial.printf("Temp: %.2f C | Presence: %s\n", currentTemp, presenceDetected ? "Yes" : "No");

        // --- B. UPLINK (POST /data) ---
        HTTPClient http;
        http.begin(String(SERVER_BASE_URL) + "/data");
        http.addHeader("Content-Type", "application/json");

        JsonDocument postDoc;
        postDoc["temperature"] = currentTemp;
        postDoc["presence"] = presenceDetected;
        postDoc.shrinkToFit();

        String postPayload;
        serializeJson(postDoc, postPayload);
        
        int postCode = http.POST(postPayload);
        if (postCode == 201) {
            Serial.println("Telemetry successfully ingested by cloud.");
        } else {
            // Translated diagnostic error output
            Serial.printf("Uplink Error: %d - %s\n", postCode, http.errorToString(postCode).c_str());
        }
        http.end(); // CRITICAL: Free the TCP Socket

        // --- C. DOWNLINK (GET /state) ---
        http.begin(String(SERVER_BASE_URL) + "/state");
        int getCode = http.GET();
        
        if (getCode == 200) {
            String getPayload = http.getString();
            JsonDocument getDoc;
            DeserializationError error = deserializeJson(getDoc, getPayload);
            
            if (!error) {
                // Parse server commands
                bool fanCommand = getDoc["fan"];
                bool lightCommand = getDoc["light"];
                
                // Blind Actuation
                digitalWrite(FAN_PIN, fanCommand ? HIGH : LOW);
                digitalWrite(LIGHT_PIN, lightCommand ? HIGH : LOW);
                
                Serial.printf("Actuator State -> Fan: %s | Light: %s\n", fanCommand ? "ON" : "OFF", lightCommand ? "ON" : "OFF");
            } else {
                Serial.printf("Deserialization Error: %s\n", error.c_str());
            }
        } else {
            // Translated diagnostic error output
            Serial.printf("Downlink Error: %d - %s\n", getCode, http.errorToString(getCode).c_str());
        }
        http.end(); // CRITICAL: Free the TCP Socket
        Serial.println("-----------------------------------");
    }
}