#include <Arduino.h>
#include <esp_arduino_version.h>
#include <esp32-hal-ledc.h>

#if ESP_ARDUINO_VERSION_MAJOR < 3
#error This sketch requires Arduino ESP32 core 3.x (pin-based LEDC API).
#endif

// GPIO19 -> controller signal input. Connect controller GND to ESP32 GND.
// Output logic level is 3.3 V. This pin does not power the motor.
constexpr uint8_t PWM_PIN = 19;
constexpr uint32_t PWM_FREQUENCY_HZ = 100;  // Set to 200 for the other measured frequency.
constexpr uint8_t PWM_RESOLUTION_BITS = 14;

// HIGH pulse widths measured by the owner, in microseconds.
constexpr uint16_t NEUTRAL_US = 1400;
constexpr uint16_t FORWARD_US = 1418;
constexpr uint16_t BACKWARD_US = 1374;
constexpr unsigned long STARTUP_NEUTRAL_MS = 5000;
constexpr unsigned long RUN_TIME_MS = 3000;
constexpr unsigned long NEUTRAL_TIME_MS = 2000;

static_assert(PWM_FREQUENCY_HZ == 100 || PWM_FREQUENCY_HZ == 200,
              "Use one of the controller's measured frequencies: 100 or 200 Hz.");
static_assert(FORWARD_US < 1000000UL / PWM_FREQUENCY_HZ,
              "The HIGH pulse must fit within the PWM period.");

bool pwmReady = false;

bool setPulse(uint16_t pulseUs) {
  // Duty is pulseWidth / period. Changing frequency must preserve pulse width.
  constexpr uint32_t periodTicks = 1UL << PWM_RESOLUTION_BITS;
  const uint32_t duty = static_cast<uint32_t>(
      (static_cast<uint64_t>(pulseUs) * PWM_FREQUENCY_HZ * periodTicks + 500000ULL) /
      1000000ULL);
  if (!ledcWrite(PWM_PIN, duty)) {
    // Attempt neutral before stopping the automatic cycle.
    const uint32_t neutralDuty = static_cast<uint32_t>(
        (static_cast<uint64_t>(NEUTRAL_US) * PWM_FREQUENCY_HZ * periodTicks + 500000ULL) /
        1000000ULL);
    ledcWrite(PWM_PIN, neutralDuty);
    pwmReady = false;
    Serial.println("ERROR: PWM write failed; automatic cycle stopped");
    return false;
  }
  Serial.printf("GPIO%u: HIGH=%u us, frequency=%lu Hz, duty=%lu/%lu\n",
                static_cast<unsigned>(PWM_PIN), static_cast<unsigned>(pulseUs),
                static_cast<unsigned long>(PWM_FREQUENCY_HZ),
                static_cast<unsigned long>(duty), static_cast<unsigned long>(periodTicks));
  return true;
}

void setup() {
  // Keep the existing L298N bridges disabled if still connected to the robot.
  digitalWrite(32, LOW);
  pinMode(32, OUTPUT);
  digitalWrite(23, LOW);
  pinMode(23, OUTPUT);
  digitalWrite(PWM_PIN, LOW);
  pinMode(PWM_PIN, OUTPUT);

  Serial.begin(115200);
  // On the original ESP32, APB clock + 14 bits supports both 100 and 200 Hz
  // with sub-microsecond pulse-width resolution.
  if (!ledcSetClockSource(LEDC_USE_APB_CLK) ||
      !ledcAttach(PWM_PIN, PWM_FREQUENCY_HZ, PWM_RESOLUTION_BITS)) {
    Serial.println("ERROR: could not initialize hardware PWM");
    return;
  }
  pwmReady = true;
  Serial.println("MOTOR PULSE TEST: initial neutral for 5 seconds");
  if (!setPulse(NEUTRAL_US)) return;
  delay(STARTUP_NEUTRAL_MS);
}

void loop() {
  if (!pwmReady) {
    delay(1000);
    return;
  }

  Serial.println("FORWARD for 3 seconds");
  if (!setPulse(FORWARD_US)) return;
  delay(RUN_TIME_MS);

  Serial.println("NEUTRAL for 2 seconds");
  if (!setPulse(NEUTRAL_US)) return;
  delay(NEUTRAL_TIME_MS);

  Serial.println("BACKWARD for 3 seconds");
  if (!setPulse(BACKWARD_US)) return;
  delay(RUN_TIME_MS);

  Serial.println("NEUTRAL for 2 seconds");
  if (!setPulse(NEUTRAL_US)) return;
  delay(NEUTRAL_TIME_MS);
}
