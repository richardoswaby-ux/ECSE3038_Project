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
#define ONE_WIRE_BUS 4 // DS18B20 Data Pin 
#define PIR_PIN 15     // HC-SR501 Output Pin
#define LIGHT_PIN 22   // Gate of IRLZ44N MOSFET for 12V Light
#define FAN_PIN 23     // Gate of IRLZ44N MOSFET for 12V Fan

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// Non-blocking timer variables
unsigned long previousMillis = 0;
const long interval = 5000; // Poll the server every 5 seconds

// State tracking and Endpoints
bool motionLatched = false;
String dataEndpoint;
String stateEndpoint;

// ==========================================
// 2. Setup & Boot Sequence
// ==========================================
void setup() {
    Serial.begin(115200);

    // Initialize endpoints once to prevent heap fragmentation
    String base = String(SERVER_BASE_URL);
    if (base.endsWith("/")) {
        base.remove(base.length() - 1); // Sanitize trailing slash if accidentally added in env.h
    }
    dataEndpoint = base + "/data";
    stateEndpoint = base + "/state";

    // Configure Actuators
    pinMode(LIGHT_PIN, OUTPUT);
    pinMode(FAN_PIN, OUTPUT);
    digitalWrite(LIGHT_PIN, LOW); // Fail-safe default OFF
    digitalWrite(FAN_PIN, LOW);   

    // Configure Sensors
    pinMode(PIR_PIN, INPUT);
    sensors.begin();
    
    // Prevent DS18B20 from blocking the CPU for 750ms during readings
    sensors.setWaitForConversion(false);
    sensors.requestTemperatures(); // Kick off the very first reading

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
    // --- 1. CONTINUOUS FAST-POLLING (PIR Latch) ---
    if (digitalRead(PIR_PIN) == HIGH) {
        motionLatched = true;
    }

    unsigned long currentMillis = millis();

    // Wi-Fi Reconnection Guard
    if (WiFi.status() != WL_CONNECTED) {
        if (currentMillis - previousMillis >= interval) {
            previousMillis = currentMillis;
            Serial.println("Network dropped. Attempting to reconnect...");
            WiFi.disconnect();
            WiFi.reconnect();
        }
        return; // Skip HTTP requests this cycle, but fast-polling continues
    }

    // --- 2. TIMER GATE ---
    if (currentMillis - previousMillis >= interval) {
        previousMillis = currentMillis;

        // --- 3. DATA ACQUISITION ---
        float currentTemp = sensors.getTempCByIndex(0);
        sensors.requestTemperatures(); // Request next reading non-blockingly

        bool presenceToSend = motionLatched;
        motionLatched = false;

        Serial.printf("Temp: %.2f C | Presence: %s\n", currentTemp, presenceToSend ? "Yes" : "No");

        //Declared at the root of the Timer Gate so they remain in scope for both Uplink and Downlink!
        WiFiClientSecure secureClient;
        secureClient.setInsecure();
        HTTPClient http;

        // --- 4. UPLINK (POST /data) ---
        if (currentTemp == DEVICE_DISCONNECTED_C) {
            Serial.println("Hardware Warning: DS18B20 disconnected. Skipping telemetry upload.");
        } else {
            http.begin(secureClient, dataEndpoint);
            http.addHeader("Content-Type", "application/json");

            JsonDocument postDoc;
            postDoc["temperature"] = currentTemp;
            postDoc["presence"] = presenceToSend;

            String postPayload;
            serializeJson(postDoc, postPayload);

            int postCode = http.POST(postPayload);

            if (postCode == 201 || postCode == 200) {
                String dummy = http.getString(); // Flush TCP receive buffer
                Serial.println("Telemetry successfully ingested by cloud.");
            } else if (postCode > 0) {
                Serial.printf("Uplink HTTP Error: %d\n", postCode);
            } else {
                Serial.printf("Uplink Network Error: %s\n", http.errorToString(postCode).c_str());
            }
            http.end(); // Free the TCP Socket
        }

        // --- 5. DOWNLINK (GET /state) ---
        http.begin(secureClient, stateEndpoint);
        int getCode = http.GET();

        if (getCode == 200) {
            String payload = http.getString();
            JsonDocument doc; 
            DeserializationError error = deserializeJson(doc, payload);

if (!error) {
    // RIGOROUS DATA CONTRACT VALIDATION: 
    // Explicitly check that the keys exist AND are booleans
    if (doc.containsKey("fan") && doc["fan"].is<bool>() && 
        doc.containsKey("light") && doc["light"].is<bool>()) {
        
        bool fanState = doc["fan"].as<bool>();
        bool lightState = doc["light"].as<bool>();
        
        // Actuate physical MOSFETs
        digitalWrite(FAN_PIN, fanState ? HIGH : LOW);
        digitalWrite(LIGHT_PIN, lightState ? HIGH : LOW);
        
        Serial.printf("State Synced -> Fan: %s | Light: %s\n", fanState ? "ON" : "OFF", lightState ? "ON" : "OFF");
    } else {
        // Trap the error if the server sends a valid JSON with the wrong schema
        Serial.println("Downlink Error: Data contract violation. Missing or invalid keys in payload.");
    }
} else { 
    Serial.printf("Downlink Error: JSON Parsing failed - %s\n", error.c_str()); 
}
        } else if (getCode > 0) {
            Serial.printf("Downlink HTTP Error: %d\n", getCode);
        } else {
            Serial.printf("Downlink Network Error: %s\n", http.errorToString(getCode).c_str());
        }
        http.end(); // Free the TCP Socket
        
        Serial.println("-----------------------------------");

    } // END TIMER GATE
} // END LOOP

