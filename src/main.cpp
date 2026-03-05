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
#define MQ2_PIN 34       // Umbenannt auf MQ2 (war MQ5)
#define JOYSTICK_X 35
#define JOYSTICK_Y 32
#define JOYSTICK_BTN 33

// RGB Pins (Rot und Blau getauscht)
#define LED_R 27 
#define LED_G 26
#define LED_B 25

// Display Pins (SPI)
#define TFT_CS    5
#define TFT_RST   17
#define TFT_DC    16

// Stepper Pins
#define IN1 13
#define IN2 12
#define IN3 14
#define IN4 27

// --- EINSTELLUNGEN ---
const char* ssid = "HTL-Weiz";
const char* password = "HTL-Weiz";

// Open-Meteo API für Weiz (47.2185, 15.6214)
const char* weatherApiUrl = "http://api.open-meteo.com/v1/forecast?latitude=47.2185&longitude=15.6214&current=temperature_2m,relative_humidity_2m";

const int STEPS_PER_REV = 2048; // Für 28BYJ-48
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
int airQualityStatus = 1; // 1 = Grün, 2 = Gelb, 3 = Rot
unsigned long lastApiCall = 0;
unsigned long lastSensorRead = 0;

// --- HILFSFUNKTIONEN ---

// Berechnung der absoluten Luftfeuchtigkeit
float calculateAbsoluteHumidity(float temp, float hum) {
  float eSaturation = 6.112 * exp((17.67 * temp) / (temp + 243.5));
  return (eSaturation * hum * 2.1674) / (273.15 + temp);
}

void setRGB(int r, int g, int b) {
  analogWrite(LED_R, r);
  analogWrite(LED_G, g);
  analogWrite(LED_B, b);
}

void moveWindow(bool openWindow) {
  if (windowOpen == openWindow) return; // Nichts tun, wenn Zustand bereits erreicht
  
  if (openWindow) {
    myStepper.step(STEPS_PER_REV / 2); // Halbe Umdrehung auf
  } else {
    myStepper.step(-STEPS_PER_REV / 2); // Halbe Umdrehung zu
  }
  
  // Motor stromlos schalten, um Hitze zu vermeiden
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
  html += "Temp: " + String(inTemp) + " &deg;C<br>Feuchte: " + String(inHum) + " %</div>";
  
  html += "<div class='card'><h2>Außen (Wetter API)</h2>";
  html += "Temp: " + String(outTemp) + " &deg;C<br>Feuchte: " + String(outHum) + " %</div>";
  
  html += "<div class='card' style='text-align: center;'><h2>Status</h2>";
  if(airQualityStatus == 1) html += "<div class='ampel'>🟢 Gute Luft</div>";
  else if(airQualityStatus == 2) html += "<div class='ampel'>🟡 Mäßig</div>";
  else html += "<div class='ampel'>🔴 Kritisch / Gas!</div>";
  
  if(windowOpen) html += "<div class='ampel'>🪟 Fenster ist OFFEN</div>";
  else html += "<div class='ampel'>🚪 Fenster ist ZU</div>";
  
  if(manualOverride) html += "<p><em>Manueller Modus aktiv</em></p>";
  html += "</div>";

  // NEU: Legende und Sensorwert für die Kalibrierung
  html += "<div class='card'><h2>Legende & Infos</h2>";
  html += "<p>🟢 <b>Grün:</b> Alles im Lot (Temp < 25&deg;C, Feuchte < 60%).</p>";
  html += "<p>🟡 <b>Gelb:</b> Zu warm oder feucht (Lüften erforderlich).</p>";
  html += "<p>🔴 <b>Rot:</b> Gas/Rauch erkannt! Fenster öffnet sich.</p>";
  html += "<hr>";
  html += "<p><b>MQ-2 Sensorwert:</b> " + String(gasValue) + " / 4095<br>";
  html += "<small>(Nutze diesen Wert, um das Rädchen am <br>Sensor an die frische Luft anzupassen)</small></p>";
  html += "</div>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void fetchWeatherData() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(weatherApiUrl);
    http.addHeader("Authorization", "Bearer DEIN_API_TOKEN_PLATZHALTER"); // Token bleibt erhalten
    
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
  
  // Pins initialisieren
  pinMode(MQ2_PIN, INPUT);
  pinMode(JOYSTICK_BTN, INPUT_PULLUP);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  
  dht.begin();
  myStepper.setSpeed(10); // 10 RPM
  
  // Display initialisieren
  tft.initR(INITR_144GREENTAB); 
  tft.setRotation(3); // NEU: Dreht das Bild (Werte 0 bis 3 ausprobieren!)
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  
  // WLAN verbinden
  WiFi.begin(ssid, password);
  Serial.print("Verbinde mit WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  // IP-Adresse im Seriellen Monitor ausgeben
  Serial.println("");
  Serial.println("WiFi verbunden!");
  Serial.print("IP-Adresse: ");
  Serial.println(WiFi.localIP()); 
  
  server.on("/", handleRoot);
  server.begin();
  
  fetchWeatherData(); // Initiale Wetterdaten laden
}

void loop() {
  server.handleClient();
  unsigned long currentMillis = millis();
  
  // 1. Sensoren auslesen (alle 2 Sekunden)
  if (currentMillis - lastSensorRead >= 2000) {
    lastSensorRead = currentMillis;
    inTemp = dht.readTemperature();
    inHum = dht.readHumidity();
    gasValue = analogRead(MQ2_PIN);
    
    // Status bewerten (Ampel)
    // SCHWELLENWERT ERHÖHT AUF 3000
    if (gasValue > 3000) { 
      airQualityStatus = 3; // Rot
      setRGB(255, 0, 0);
    } else if (inHum > 60.0 || inTemp > 25.0) {
      airQualityStatus = 2; // Gelb
      setRGB(255, 255, 0);
    } else {
      airQualityStatus = 1; // Grün
      setRGB(0, 255, 0);
    }
    
    // Display Update
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(0, 10);
    tft.print("IN Temp: "); tft.print(inTemp); tft.println(" C");
    tft.print("IN Hum:  "); tft.print(inHum); tft.println(" %");
    tft.println();
    tft.print("OUT Temp:"); tft.print(outTemp); tft.println(" C");
    tft.print("OUT Hum: "); tft.print(outHum); tft.println(" %");
    tft.println();
    tft.print("Status: ");
    if(airQualityStatus == 1) tft.setTextColor(ST77XX_GREEN);
    else if(airQualityStatus == 2) tft.setTextColor(ST77XX_YELLOW);
    else tft.setTextColor(ST77XX_RED);
    tft.println(airQualityStatus == 1 ? "GUT" : (airQualityStatus == 2 ? "OKAY" : "KRITISCH"));
    tft.setTextColor(ST77XX_WHITE);
  }

  // 2. Wetter API abfragen (alle 15 Minuten)
  if (currentMillis - lastApiCall >= 900000) {
    lastApiCall = currentMillis;
    fetchWeatherData();
  }

  // 3. Joystick Manuelle Steuerung
  if (digitalRead(JOYSTICK_BTN) == LOW) {
    manualOverride = !manualOverride; // Modus umschalten
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
    // 4. Automatische Logik (Algorithmus)
    if (gasValue > 3000) {
      // Notfallmodus: Immer öffnen
      moveWindow(true);
    } else {
      float inAH = calculateAbsoluteHumidity(inTemp, inHum);
      float outAH = calculateAbsoluteHumidity(outTemp, outHum);
      
      // Lüften, wenn drinnen zu feucht (>60%) UND draußen absolut trockener
      if (inHum > 60.0 && outAH < inAH) {
        moveWindow(true);
      } 
      // Schließen, wenn Feuchtigkeit in Ordnung oder draußen feuchter
      else if (inHum <= 55.0 || outAH > inAH) {
        moveWindow(false);
      }
    }
  }
}