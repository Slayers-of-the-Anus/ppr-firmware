#pragma once

// Change these values for the actual robot wiring and mechanical limits.
#define SERVO_PIN 18
#define SERVO_MIN_ANGLE 0
#define SERVO_MAX_ANGLE 180
#define SERVO_CENTER_ANGLE 90
#define SERIAL_BAUD 115200

// L298N: remove ENA/ENB jumpers before connecting these GPIOs.
// Both bridges move together. Swap a motor's wires or invert its channel if needed.
#define MOTOR_IN1_PIN 25
#define MOTOR_IN2_PIN 26
#define MOTOR_IN3_PIN 27
#define MOTOR_IN4_PIN 33
#define MOTOR_ENA_PIN 32
#define MOTOR_ENB_PIN 23
#define MOTOR_A_INVERT false
#define MOTOR_B_INVERT false

// Typical MG995 pulse range. Narrow it if the mechanism binds near its limits.
#define SERVO_MIN_PULSE_US 500
#define SERVO_MAX_PULSE_US 2500
