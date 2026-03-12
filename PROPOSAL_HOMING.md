# Homing Logic Implementation (B18 Light Barrier)

This document contains the code snippets and integration steps for adding absolute positioning to the window flap using a B18 light barrier and a "thorn" trigger.

## 1. Hardware Setup
- **Sensor:** B18 Light Barrier (Slot Sensor)
- **Pin:** GPIO 21 (Recommended)
- **Signal:** 
    - `HIGH`: Beam Clear (Window is open/moving)
    - `LOW`: Beam Broken (Window is at "Home" / Closed position)

## 2. Global Definitions
Add these to the top of `src/main.cpp`:

```cpp
#define LIGHT_BARRIER_PIN 21
#define HOMING_TIMEOUT_STEPS 3000 // Slightly more than one full revolution
```

## 3. The Homing Function
Add this function before `setup()`:

```cpp
void homeWindow() {
  Serial.println("[HOMING] Starting sequence...");
  
  int stepsTaken = 0;
  bool sensorHit = false;

  // 1. Move slowly backwards towards the "Closed" position
  // Adjust 'LOW'/'HIGH' based on whether your sensor is Normally Open or Closed
  while (digitalRead(LIGHT_BARRIER_PIN) == HIGH && stepsTaken < HOMING_TIMEOUT_STEPS) {
    myStepper.step(-1); 
    stepsTaken++;
    delay(5); // Slow speed for precision
    
    if (stepsTaken % 100 == 0) Serial.print(".");
  }

  // 2. Check if we actually hit the sensor or just timed out
  if (digitalRead(LIGHT_BARRIER_PIN) == LOW) {
    Serial.println("\n[HOMING] Success: Sensor triggered.");
    sensorHit = true;
  } else {
    Serial.println("\n[HOMING] ERROR: Timeout reached without triggering sensor!");
  }

  // 3. Finalize State
  windowOpen = false;
  
  // Power down motor coils to prevent heat
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
  
  Serial.println("[HOMING] Sequence complete.");
}
```

## 4. Refactored moveWindow()
Replace your existing `moveWindow()` with this version that respects the sensor:

```cpp
void moveWindow(bool openWindow) {
  if (windowOpen == openWindow) return; 
  
  if (openWindow) {
    Serial.println("Opening window...");
    myStepper.step(STEPS_PER_REV / 4); 
  } else {
    Serial.println("Closing window (monitoring sensor)...");
    // Move backwards but stop immediately if sensor is hit
    for (int i = 0; i < (STEPS_PER_REV / 4); i++) {
      if (digitalRead(LIGHT_BARRIER_PIN) == LOW) {
        Serial.println("Closed position reached via sensor.");
        break; 
      }
      myStepper.step(-1);
    }
  }
  
  // Always power down coils
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
  
  windowOpen = openWindow;
}
```

## 5. Integration in setup()
Add these lines inside your `void setup()`:

```cpp
void setup() {
  // ... existing pinMode calls ...
  pinMode(LIGHT_BARRIER_PIN, INPUT_PULLUP); 

  // ... after motor/stepper initialization ...
  homeWindow(); // Establish 'Zero' position on boot
}
```
