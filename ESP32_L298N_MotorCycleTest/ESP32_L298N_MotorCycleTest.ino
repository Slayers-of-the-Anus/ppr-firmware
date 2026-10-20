#include <Arduino.h>

// Standalone motor test for the original ESP32 + L298N.
// Remove ENA/ENB jumpers before connecting ESP32 GPIOs.
// Motor A: OUT1/OUT2. Optional motor B: OUT3/OUT4.
// Share GND between ESP32, L298N and the motor power supply.
constexpr int IN1_PIN = 25;
constexpr int IN2_PIN = 26;
constexpr int IN3_PIN = 27;
constexpr int IN4_PIN = 33;
constexpr int ENA_PIN = 32;
constexpr int ENB_PIN = 23;

// Change the corresponding flag if a motor turns in the wrong direction.
constexpr bool MOTOR_A_INVERT = false;
constexpr bool MOTOR_B_INVERT = false;
constexpr unsigned long STARTUP_DELAY_MS = 5000;
constexpr unsigned long RUN_TIME_MS = 3000;
constexpr unsigned long STOP_TIME_MS = 2000;

enum class Direction { Stop, Forward, Backward };
void setDirection(Direction direction);

void setDirection(Direction direction) {
  // Keep both bridges disabled while changing their direction inputs.
  digitalWrite(ENA_PIN, LOW);
  digitalWrite(ENB_PIN, LOW);

  const bool moving = direction != Direction::Stop;
  const bool forward = direction == Direction::Forward;
  digitalWrite(IN1_PIN, moving && (forward != MOTOR_A_INVERT) ? HIGH : LOW);
  digitalWrite(IN2_PIN, moving && (forward == MOTOR_A_INVERT) ? HIGH : LOW);
  digitalWrite(IN3_PIN, moving && (forward != MOTOR_B_INVERT) ? HIGH : LOW);
  digitalWrite(IN4_PIN, moving && (forward == MOTOR_B_INVERT) ? HIGH : LOW);

  if (moving) {
    digitalWrite(ENA_PIN, HIGH);
    digitalWrite(ENB_PIN, HIGH);
  }
}

void setup() {
  // Disable the motors before initializing the other outputs.
  const int pins[] = {ENA_PIN, ENB_PIN, IN1_PIN, IN2_PIN, IN3_PIN, IN4_PIN};
  for (int pin : pins) {
    digitalWrite(pin, LOW);
    pinMode(pin, OUTPUT);
  }
  setDirection(Direction::Stop);

  Serial.begin(115200);
  Serial.println("L298N MOTOR TEST READY");
  Serial.println("Automatic full-output test starts in 5 seconds.");
  delay(STARTUP_DELAY_MS);
}

void loop() {
  Serial.println("FORWARD (3 seconds)");
  setDirection(Direction::Forward);
  delay(RUN_TIME_MS);

  setDirection(Direction::Stop);
  Serial.println("STOP (2 seconds)");
  delay(STOP_TIME_MS);

  Serial.println("BACKWARD (3 seconds)");
  setDirection(Direction::Backward);
  delay(RUN_TIME_MS);

  setDirection(Direction::Stop);
  Serial.println("STOP (2 seconds)");
  delay(STOP_TIME_MS);
}
