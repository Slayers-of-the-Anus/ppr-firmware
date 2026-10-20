#include <Arduino.h>
#include <ESP32Servo.h>

// Standalone servo test: signal on GPIO18, external servo supply, common GND.
constexpr int SERVO_PIN = 18;
constexpr int MIN_ANGLE = 60;
constexpr int MAX_ANGLE = 120;
constexpr int CENTER_ANGLE = 90;
constexpr int MIN_PULSE_US = 500;
constexpr int MAX_PULSE_US = 2500;
constexpr unsigned long STEP_DELAY_MS = 20;
constexpr unsigned long ENDPOINT_PAUSE_MS = 1000;
constexpr unsigned long STARTUP_PAUSE_MS = 3000;

static_assert(MIN_ANGLE >= 0 && MAX_ANGLE <= 180 && MIN_ANGLE < MAX_ANGLE,
              "Servo angle range must be within 0..180 degrees.");
static_assert(CENTER_ANGLE >= MIN_ANGLE && CENTER_ANGLE <= MAX_ANGLE,
              "Center angle must be within the selected range.");

Servo testServo;
int currentAngle = CENTER_ANGLE;

void sweepTo(int target) {
  Serial.printf("SERVO %d -> %d degrees\n", currentAngle, target);
  const int step = target > currentAngle ? 1 : -1;
  while (currentAngle != target) {
    currentAngle += step;
    testServo.write(currentAngle);
    delay(STEP_DELAY_MS);
  }
  Serial.printf("PAUSE at %d degrees\n", currentAngle);
  delay(ENDPOINT_PAUSE_MS);
}

void setup() {
  // Disable the L298N channels if the rest of the robot is still connected.
  // These are the ENA/ENB pins used by the main PhonePilot sketch.
  digitalWrite(32, LOW);
  pinMode(32, OUTPUT);
  digitalWrite(23, LOW);
  pinMode(23, OUTPUT);

  Serial.begin(115200);
  testServo.setPeriodHertz(50);
  testServo.attach(SERVO_PIN, MIN_PULSE_US, MAX_PULSE_US);
  if (!testServo.attached()) {
    Serial.println("ERROR: servo could not attach to GPIO18");
    return;
  }

  testServo.write(CENTER_ANGLE);
  Serial.println("SERVO CYCLE TEST READY: GPIO18");
  Serial.printf("Center: %d degrees; cycling starts in 3 seconds.\n", CENTER_ANGLE);
  delay(STARTUP_PAUSE_MS);
}

void loop() {
  if (!testServo.attached()) {
    delay(1000);
    return;
  }
  sweepTo(MIN_ANGLE);
  sweepTo(MAX_ANGLE);
}
