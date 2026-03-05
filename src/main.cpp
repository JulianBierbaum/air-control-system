#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Stepper.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// --- PIN-DEFINITIONEN ---
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

// --- EINSTELLUNGEN ---
const char* ssid = "HTL-Weiz";
const char* password = "HTL-Weiz";

// Open-Meteo API für Weiz
const char* weatherApiUrl = "http://api.open-meteo.com/v1/forecast?latitude=47.2185&longitude=15.6214&current=temperature_2m,relative_humidity_2m";

const int STEPS_PER_REV = 2048; 
Stepper myStepper(STEPS_PER_REV, IN1, IN3, IN2, IN4);

DHT dht(DHTPIN, DHTTYPE);
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
WebServer server(80);

// --- GLOBALE VARIABLEN ---
float inTemp = 0.0, inHum = 0.0;
float outTemp = 0.0, outHum = 0.0;
int gasValue = 0;
bool windowOpen = false;
bool manualOverride = false;

// 1 = Grün, 2 = Gelb (ab 24°C), 3 = Rot (ab 28°C), 4 = Gas (Rot blinkend + Piepen)
int airQualityStatus = 1; 

unsigned long lastApiCall = 0;
unsigned long lastSensorRead = 0;
unsigned long lastLedUpdate = 0;

// Fading Variablen für die LED
float currentR = 0, currentG = 255, currentB = 0;
int targetR = 0, targetG = 255, targetB = 0;

// --- HILFSFUNKTIONEN ---

float calculateAbsoluteHumidity(float temp, float hum) {
  float eSaturation = 6.112 * exp((17.67 * temp) / (temp + 243.5));
  return (eSaturation * hum * 2.1674) / (273.15 + temp);
}

// Setzt nur die Zielwerte, das Fading passiert im Loop
void setTargetRGB(int r, int g, int b) {
  targetR = r;
  targetG = g;
  targetB = b;
}

void moveWindow(bool openWindow) {
  if (windowOpen == openWindow) return; 
  
  if (openWindow) {
    myStepper.step(STEPS_PER_REV / 2); 
  } else {
    myStepper.step(-STEPS_PER_REV / 2); 
  }
  
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
  
  windowOpen = openWindow;
}

// --- WEBSERVER ROUTINES ---
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta http-equiv='refresh' content='10'>";
  html += "<style>body{font-family: Arial; text-align: center; background:#222; color:#fff;}";
  html += ".card{background:#333; padding:20px; border-radius:10px; display:inline-block; margin:10px; vertical-align: top; text-align: left;}";
  html += ".ampel{font-size: 50px; text-align: center;} h1{text-align: center;}</style></head><body>";
  html += "<h1>Air-Control-System Weiz</h1>";
  
  html += "<div class='card'><h2>Innenraum</h2>";
  html += "Temp: " + String((int)inTemp) + " &deg;C<br>Feuchte: " + String((int)inHum) + " %</div>";
  
  html += "<div class='card'><h2>Außen (Wetter API)</h2>";
  html += "Temp: " + String((int)outTemp) + " &deg;C<br>Feuchte: " + String((int)outHum) + " %</div>";
  
  html += "<div class='card' style='text-align: center;'><h2>Status</h2>";
  
  // Ampel-Logik mit bedingtem Bild
  if(airQualityStatus == 1) {
    html += "<div class='ampel'>🟢 Gute Luft</div>";
  } else if(airQualityStatus == 2) {
    html += "<div class='ampel'>🟡 Mäßig</div>";
  } else if(airQualityStatus == 3) {
    html += "<div class='ampel'>🔴 Zu Heiß</div>";
  } else {
    html += "<div class='ampel'>🚨 Gas Alarm!</div>";
    // NEU: Das GIF wird genau hier eingebaut (mittig, abgerundete Ecken)
    html += "<br><img src='https://media.tenor.com/7p-Jnh69pqsAAAAe/alarm-german.png' style='width:100%; max-width:250px; border-radius:10px; margin-top:15px;' alt='ALARM'>";
  }
  
  // Etwas Abstand nach oben für das Fenster-Icon
  html += "<div style='margin-top: 15px;'>";
  if(windowOpen) html += "<div class='ampel'>🪟 Fenster ist OFFEN</div>";
  else html += "<div class='ampel'>🚪 Fenster ist ZU</div>";
  html += "</div>";
  
  if(manualOverride) html += "<p><em>Manueller Modus aktiv</em></p>";
  html += "</div>";

  html += "<div class='card'><h2>Legende & Infos</h2>";
  html += "<p>🟢 <b>Grün:</b> Alles im Lot (Temp < 24&deg;C).</p>";
  html += "<p>🟡 <b>Gelb:</b> Es wird warm (Temp &ge; 24&deg;C).</p>";
  html += "<p>🔴 <b>Rot:</b> Zu heiß (Temp &ge; 28&deg;C).</p>";
  html += "<p>🚨 <b>Blinkend Rot + Ton:</b> Gas/Rauch erkannt!</p>";
  html += "<hr>";
  html += "<p><b>MQ-2 Sensorwert:</b> " + String(gasValue) + " / 4095</p>";
  html += "</div>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void fetchWeatherData() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(weatherApiUrl);
    
    // Wichtiger Headereintrag für API-Calls
    http.addHeader("Authorization", "Bearer DEIN_API_TOKEN_PLATZHALTER"); 
    
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
  
  // Buzzer initialisieren und stumm schalten
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); 
  
  dht.begin();
  myStepper.setSpeed(15); // Motor etwas schneller gemacht
  
  // --- DER TRICK GEGEN DEN DISPLAY-SCHNEE ---
  tft.initR(INITR_BLACKTAB); 
  tft.fillScreen(ST77XX_BLACK); 
  tft.initR(INITR_144GREENTAB); 
  tft.setRotation(3); 
  // ------------------------------------------
  
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  
  WiFi.begin(ssid, password);
  Serial.print("Verbinde mit WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi verbunden!");
  Serial.print("IP-Adresse: ");
  Serial.println(WiFi.localIP()); 
  
  server.on("/", handleRoot);
  server.begin();
  
  fetchWeatherData(); 
}

void loop() {
  server.handleClient();
  unsigned long currentMillis = millis();
  
  // --- 1. LED & BUZZER ANIMATION (Fading & Blinken) ---
  if (currentMillis - lastLedUpdate >= 10) {
    lastLedUpdate = currentMillis;

    if (airQualityStatus == 4) { // Stufe 4 = GAS ALARM
      // Alarm-Modus: Rotes Blinken und Piepen (500ms an, 500ms aus)
      if ((currentMillis / 500) % 2 == 0) {
        analogWrite(LED_R, 255); analogWrite(LED_G, 0); analogWrite(LED_B, 0);
        digitalWrite(BUZZER_PIN, HIGH); // Buzzer AN
      } else {
        analogWrite(LED_R, 0); analogWrite(LED_G, 0); analogWrite(LED_B, 0);
        digitalWrite(BUZZER_PIN, LOW); // Buzzer AUS
      }
      currentR = 255; currentG = 0; currentB = 0;
    } else {
      digitalWrite(BUZZER_PIN, LOW); // Sicherstellen, dass der Buzzer still ist
      
      // Fading-Modus für weiche Übergänge bei normalen Farben
      if (currentR < targetR) currentR += 2; else if (currentR > targetR) currentR -= 2;
      if (currentG < targetG) currentG += 2; else if (currentG > targetG) currentG -= 2;
      if (currentB < targetB) currentB += 2; else if (currentB > targetB) currentB -= 2;

      // Toleranz abfangen
      if (abs(currentR - targetR) < 2) currentR = targetR;
      if (abs(currentG - targetG) < 2) currentG = targetG;
      if (abs(currentB - targetB) < 2) currentB = targetB;

      analogWrite(LED_R, (int)currentR);
      analogWrite(LED_G, (int)currentG);
      analogWrite(LED_B, (int)currentB);
    }
  }

  // --- 2. SENSOREN AUSLESEN ---
  if (currentMillis - lastSensorRead >= 2000) {
    lastSensorRead = currentMillis;
    inTemp = dht.readTemperature();
    inHum = dht.readHumidity();
    gasValue = analogRead(MQ2_PIN);
    
    // STATUS LOGIK
    if (gasValue > 3000) { 
      airQualityStatus = 4; // Gas (Blinken wird oben gesteuert)
    } else if (inTemp >= 28.0) {
      airQualityStatus = 3; // Durchgehend Rot
      setTargetRGB(255, 0, 0); 
    } else if (inTemp >= 24.0 || inHum > 60.0) {
      airQualityStatus = 2; // Gelb
      setTargetRGB(255, 255, 0); 
    } else {
      airQualityStatus = 1; // Grün
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
    
    if(airQualityStatus == 1) tft.println("GUT");
    else if(airQualityStatus == 2) tft.println("WARM / OKAY");
    else if(airQualityStatus == 3) tft.println("ZU HEISS");
    else tft.println("GAS ALARM");
    
    tft.setTextColor(ST77XX_WHITE);
  }

  // --- 3. WETTER API ---
  if (currentMillis - lastApiCall >= 900000) {
    lastApiCall = currentMillis;
    fetchWeatherData();
  }

  // --- 4. JOYSTICK STEUERUNG ---
  if (digitalRead(JOYSTICK_BTN) == LOW) {
    manualOverride = !manualOverride; 
    delay(300); // Entprellen
  }

  if (manualOverride) {
    int yVal = analogRead(JOYSTICK_Y);
    if (yVal < 1000 && !windowOpen) {
      moveWindow(true);
    } else if (yVal > 3000 && windowOpen) {
      moveWindow(false);
    }
  } else {
    // --- 5. SCHLAUE LÜFTUNGSLOGIK ---
    if (gasValue > 3000) {
      moveWindow(true); // Notfall: Immer auf!
    } else {
      float inAH = calculateAbsoluteHumidity(inTemp, inHum);
      float outAH = calculateAbsoluteHumidity(outTemp, outHum);
      
      bool needToOpen = windowOpen; 
      
      if (inHum > 60.0 && outAH < inAH) {
        needToOpen = true; // Entfeuchten
      } else if (inTemp >= 24.0 && outTemp < inTemp && outTemp > 10.0) {
        needToOpen = true; // Kühlen 
      }
      
      if (outAH >= inAH && outTemp >= inTemp) {
        needToOpen = false; // Draußen schwül und heiß -> zu bleiben!
      } else if (inHum <= 55.0 && inTemp < 24.0) {
        needToOpen = false; // Alles im Wohlfühlbereich -> zu machen!
      }
      
      moveWindow(needToOpen);
    }
  }
}