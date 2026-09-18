#pragma once

// Change these values for the actual robot wiring and mechanical limits.
#define SERVO_PIN 18
#define SERVO_MIN_ANGLE 0
#define SERVO_MAX_ANGLE 180
#define SERVO_CENTER_ANGLE 90
#define SERIAL_BAUD 115200

// Typical MG995 pulse range. Narrow it if the mechanism binds near its limits.
#define SERVO_MIN_PULSE_US 500
#define SERVO_MAX_PULSE_US 2500
