#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Stepper.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#define DHTPIN 4
#define DHTTYPE DHT11
#define MQ2_PIN 34
#define JOYSTICK_X 35
#define JOYSTICK_Y 32
#define JOYSTICK_BTN 33

// RGB Pins
#define LED_R 27 
#define LED_G 26
#define LED_B 25

// Buzzer Pin
#define BUZZER_PIN 22 

// Display Pins (SPI)
#define TFT_CS    5
#define TFT_RST   17
#define TFT_DC    16

// Stepper Pins 
#define IN1 13
#define IN2 12
#define IN3 14
#define IN4 15

const char* ssid = "HTL-Weiz";
const char* password = "HTL-Weiz";

// Open-Meteo API for Weiz, Austria
const char* weatherApiUrl = "http://api.open-meteo.com/v1/forecast?latitude=47.2185&longitude=15.6214&current=temperature_2m,relative_humidity_2m";

const int STEPS_PER_REV = 2048; 
Stepper myStepper(STEPS_PER_REV, IN1, IN3, IN2, IN4);

DHT dht(DHTPIN, DHTTYPE);
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
WebServer server(80);

float inTemp = 0.0, inHum = 0.0;
float outTemp = 0.0, outHum = 0.0;
int gasValue = 0;
bool windowOpen = false;
bool targetWindowOpen = false; // Target state for non-blocking movement
bool manualOverride = false;

// Non-blocking motor variables
int currentMotorStep = 0;
int targetMotorStep = 0;
unsigned long lastStepTime = 0;
const int STEP_DELAY = 2; // ms between steps

// Hysteresis: Minimum time (2 minutes) to stay in one position
unsigned long lastWindowStateChange = 0;
const unsigned long MIN_WINDOW_HOLD_TIME = 120000; 

// 1 = Green, 2 = Yellow (from 24°C), 3 = Red (from 28°C), 4 = Gas (Blinking Red + Beeping)
int airQualityStatus = 1; 

unsigned long lastApiCall = 0;
unsigned long lastSensorRead = 0;
unsigned long lastLedUpdate = 0;

float currentR = 0, currentG = 255, currentB = 0;
int targetR = 0, targetG = 255, targetB = 0;


float calculateAbsoluteHumidity(float temp, float hum) {
  // Magnus formula
  float saturation = 6.112 * exp((17.67 * temp) / (temp + 243.5));
  return (saturation * hum * 2.1674) / (273.15 + temp);
}

void setTargetRGB(int r, int g, int b) {
  targetR = r;
  targetG = g;
  targetB = b;
}

// Sets the target state of the window (Open or Closed)
void setWindowTarget(bool open, bool ignoreTimer = false) {
  if (targetWindowOpen == open) return; // Already moving to this target
  
  // Only change if minimum hold time is over OR it's an emergency/manual override
  if (ignoreTimer || (millis() - lastWindowStateChange >= MIN_WINDOW_HOLD_TIME)) {
    targetWindowOpen = open;
    targetMotorStep = open ? (STEPS_PER_REV / 4) : 0;
  }
}

// Called in loop, performs 1 step per call if needed
void updateMotor() {
  if (currentMotorStep == targetMotorStep) return; 

  if (millis() - lastStepTime >= STEP_DELAY) {
    lastStepTime = millis();
    
    if (currentMotorStep < targetMotorStep) {
      myStepper.step(1);
      currentMotorStep++;
    } else {
      myStepper.step(-1);
      currentMotorStep--;
    }

    // Target reached
    if (currentMotorStep == targetMotorStep) {
      windowOpen = targetWindowOpen;
      lastWindowStateChange = millis(); // Reset hysteresis timer
      
      // Coils off (Power Save)
      digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
      digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
      Serial.println(windowOpen ? "Window OPEN" : "Window CLOSED");
    }
  }
}

// --- WEBSERVER ROUTINES ---
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta http-equiv='refresh' content='10'>";
  html += "<style>body{font-family: Arial; text-align: center; background:#222; color:#fff;}";
  html += ".card{background:#333; padding:20px; border-radius:10px; display:inline-block; margin:10px; vertical-align: top; text-align: left;}";
  html += ".ampel{font-size: 50px; text-align: center;} h1{text-align: center;}</style></head><body>";
  html += "<h1>Air-Control-System Weiz</h1>";
  
  html += "<div class='card'><h2>Indoor</h2>";
  html += "Temp: " + String((int)inTemp) + " &deg;C<br>Humidity: " + String((int)inHum) + " %</div>";
  
  html += "<div class='card'><h2>Outdoor (Weather API)</h2>";
  html += "Temp: " + String((int)outTemp) + " &deg;C<br>Humidity: " + String((int)outHum) + " %</div>";
  
  html += "<div class='card' style='text-align: center;'><h2>Status</h2>";
  
  // Status Logic with conditional image
  if(airQualityStatus == 1) {
    html += "<div class='ampel'>🟢 Good Air</div>";
  } else if(airQualityStatus == 2) {
    html += "<div class='ampel'>🟡 Moderate</div>";
  } else if(airQualityStatus == 3) {
    html += "<div class='ampel'>🔴 Too Hot</div>";
  } else {
    html += "<div class='ampel'>🚨 Gas Alarm!</div>";
    // GIF for alarm (centered, rounded corners)
    html += "<br><img src='https://media.tenor.com/7p-Jnh69pqsAAAAe/alarm-german.png' style='width:100%; max-width:250px; border-radius:10px; margin-top:15px;' alt='ALARM'>";
  }
  
  // Window status icon
  html += "<div style='margin-top: 15px;'>";
  if(windowOpen) html += "<div class='ampel'>🪟 Window is OPEN</div>";
  else html += "<div class='ampel'>🚪 Window is CLOSED</div>";
  html += "</div>";
  
  if(manualOverride) html += "<p><em>Manual Mode Active</em></p>";
  html += "</div>";

  html += "<div class='card'><h2>Legend & Info</h2>";
  html += "<p>🟢 <b>Green:</b> Everything fine (Temp < 24&deg;C).</p>";
  html += "<p>🟡 <b>Yellow:</b> Getting warm (Temp &ge; 24&deg;C).</p>";
  html += "<p>🔴 <b>Red:</b> Too hot (Temp &ge; 28&deg;C).</p>";
  html += "<p>🚨 <b>Blinking Red + Sound:</b> Gas/Smoke detected!</p>";
  html += "<hr>";
  html += "<p><b>MQ-2 Sensor Value:</b> " + String(gasValue) + " / 4095</p>";
  html += "</div>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void fetchWeatherData() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(weatherApiUrl);
    
    // Important header entry for API calls
    http.addHeader("Authorization", "Bearer YOUR_API_TOKEN_PLACEHOLDER"); 
    
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      JsonDocument doc;
      deserializeJson(doc, payload);
      outTemp = doc["current"]["temperature_2m"];
      outHum = doc["current"]["relative_humidity_2m"];
    }
    http.end();
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(MQ2_PIN, INPUT);
  pinMode(JOYSTICK_BTN, INPUT_PULLUP);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  
  // Initialize buzzer and mute it
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); 
  
  dht.begin();
  myStepper.setSpeed(15); // Increased motor speed
  
  // --- FIX FOR DISPLAY STATIC ---
  tft.initR(INITR_BLACKTAB); 
  tft.fillScreen(ST77XX_BLACK); 
  tft.initR(INITR_144GREENTAB); 
  tft.setRotation(3); 
  // ------------------------------------------
  
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP()); 
  
  server.on("/", handleRoot);
  server.begin();
  
  fetchWeatherData(); 
}

void loop() {
  server.handleClient();
  updateMotor(); // Motor moves in background (non-blocking)
  unsigned long currentMillis = millis();
  
  // --- 1. LED & BUZZER ANIMATION (Fading & Blinking) ---
  if (currentMillis - lastLedUpdate >= 10) {
    lastLedUpdate = currentMillis;

    if (airQualityStatus == 4) { // Level 4 = GAS ALARM
      // Alarm Mode: Red blinking and beeping (500ms on, 500ms off)
      if ((currentMillis / 500) % 2 == 0) {
        analogWrite(LED_R, 255); analogWrite(LED_G, 0); analogWrite(LED_B, 0);
        digitalWrite(BUZZER_PIN, HIGH); // Buzzer ON
      } else {
        analogWrite(LED_R, 0); analogWrite(LED_G, 0); analogWrite(LED_B, 0);
        digitalWrite(BUZZER_PIN, LOW); // Buzzer OFF
      }
      currentR = 255; currentG = 0; currentB = 0;
    } else {
      digitalWrite(BUZZER_PIN, LOW); // Ensure buzzer is silent
      
      // Fading mode for smooth transitions with normal colors
      if (currentR < targetR) currentR += 2; else if (currentR > targetR) currentR -= 2;
      if (currentG < targetG) currentG += 2; else if (currentG > targetG) currentG -= 2;
      if (currentB < targetB) currentB += 2; else if (currentB > targetB) currentB -= 2;

      // Handle tolerance
      if (abs(currentR - targetR) < 2) currentR = targetR;
      if (abs(currentG - targetG) < 2) currentG = targetG;
      if (abs(currentB - targetB) < 2) currentB = targetB;

      analogWrite(LED_R, (int)currentR);
      analogWrite(LED_G, (int)currentG);
      analogWrite(LED_B, (int)currentB);
    }
  }

  // --- 2. READING SENSORS ---
  if (currentMillis - lastSensorRead >= 2000) {
    lastSensorRead = currentMillis;
    inTemp = dht.readTemperature();
    inHum = dht.readHumidity();
    gasValue = analogRead(MQ2_PIN);
    
    // STATUS LOGIC
    if (gasValue > 3000) { 
      airQualityStatus = 4; // Gas (blinking handled above)
    } else if (inTemp >= 28.0) {
      airQualityStatus = 3; // Solid Red
      setTargetRGB(255, 0, 0); 
    } else if (inTemp >= 24.0 || inHum > 60.0) {
      airQualityStatus = 2; // Yellow
      setTargetRGB(255, 255, 0); 
    } else {
      airQualityStatus = 1; // Green
      setTargetRGB(0, 255, 0);
    }
    
    // Display Update
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(0, 10);
    tft.print("IN Temp: "); tft.print((int)inTemp); tft.println(" C");
    tft.print("IN Hum:  "); tft.print((int)inHum); tft.println(" %");
    tft.println();
    tft.print("OUT Temp:"); tft.print((int)outTemp); tft.println(" C");
    tft.print("OUT Hum: "); tft.print((int)outHum); tft.println(" %");
    tft.println();
    
    tft.print("Status: ");
    if(airQualityStatus == 1) tft.setTextColor(ST77XX_GREEN);
    else if(airQualityStatus == 2) tft.setTextColor(ST77XX_YELLOW);
    else tft.setTextColor(ST77XX_RED);
    
    if(airQualityStatus == 1) tft.println("GOOD");
    else if(airQualityStatus == 2) tft.println("WARM / OKAY");
    else if(airQualityStatus == 3) tft.println("TOO HOT");
    else tft.println("GAS ALARM");
    
    tft.setTextColor(ST77XX_WHITE);
  }

  // --- 3. WEATHER API ---
  if (currentMillis - lastApiCall >= 900000) {
    lastApiCall = currentMillis;
    fetchWeatherData();
  }

  // --- 4. JOYSTICK CONTROL ---
  if (digitalRead(JOYSTICK_BTN) == LOW) {
    manualOverride = !manualOverride; 
    delay(300); // Debounce
  }

  if (manualOverride) {
    int yVal = analogRead(JOYSTICK_Y);
    if (yVal < 1000) {
      setWindowTarget(true, true); // Manual: Ignore timer!
    } else if (yVal > 3000) {
      setWindowTarget(false, true); // Manual: Ignore timer!
    }
  } else {
    // --- 5. SMART VENTILATION LOGIC ---
    if (gasValue > 3000) {
      setWindowTarget(true, true); // Gas Emergency: Ignore timer!
    } else {
      float inAH = calculateAbsoluteHumidity(inTemp, inHum);
      float outAH = calculateAbsoluteHumidity(outTemp, outHum);
      
      bool needToOpen = targetWindowOpen; // Start from current target
      
      if (inHum > 60.0 && outAH < inAH) {
        needToOpen = true; // Dehumidify
      } else if (inTemp >= 24.0 && outTemp < inTemp && outTemp > 10.0) {
        needToOpen = true; // Cool 
      }
      
      if (outAH >= inAH && outTemp >= inTemp) {
        needToOpen = false; // Outside humid and hot -> stay closed!
      } else if (inHum <= 55.0 && inTemp < 24.0) {
        needToOpen = false; // Everything in comfort zone -> close window!
      }
      
      setWindowTarget(needToOpen); // Normal mode: Respect timer
    }
  }
}
