#include <Arduino.h>

#ifndef ESP8266
#error Select an ESP8266 board in Arduino IDE for this sketch.
#endif

#include <ESP8266WiFi.h>

// GPIO5 -> controller signal input. On NodeMCU/Wemos D1 mini this is D1.
// The output is 3.3 V. Connect controller GND to ESP8266 GND.
constexpr uint8_t PWM_PIN = 5;
constexpr uint32_t PWM_FREQUENCY_HZ = 100;  // Change to 200 if needed.
constexpr uint32_t PWM_PERIOD_US = 1000000UL / PWM_FREQUENCY_HZ;

constexpr uint16_t NEUTRAL_US = 1400;
constexpr uint16_t FORWARD_US = 1418;
constexpr uint16_t BACKWARD_US = 1374;
constexpr unsigned long STARTUP_NEUTRAL_MS = 5000;
constexpr unsigned long RUN_TIME_MS = 3000;
constexpr unsigned long NEUTRAL_TIME_MS = 2000;

static_assert(PWM_FREQUENCY_HZ == 100 || PWM_FREQUENCY_HZ == 200,
              "Use one of the measured frequencies: 100 or 200 Hz.");
static_assert(NEUTRAL_US > 0 && NEUTRAL_US < PWM_PERIOD_US &&
              FORWARD_US > 0 && FORWARD_US < PWM_PERIOD_US &&
              BACKWARD_US > 0 && BACKWARD_US < PWM_PERIOD_US,
              "Every HIGH pulse must fit within the PWM period.");

void setPulse(uint16_t pulseUs) {
  // Range equals the period in microseconds, so the duty value is the
  // requested HIGH width in microseconds at both supported frequencies.
  analogWrite(PWM_PIN, pulseUs);
  Serial.printf("GPIO%u (D1): HIGH=%u us, frequency=%lu Hz\n",
                static_cast<unsigned>(PWM_PIN), static_cast<unsigned>(pulseUs),
                static_cast<unsigned long>(PWM_FREQUENCY_HZ));
}

void setup() {
  digitalWrite(PWM_PIN, LOW);
  pinMode(PWM_PIN, OUTPUT);
  Serial.begin(115200);

  // Reduce radio activity during this standalone pulse-width test.
  // Do not overwrite any Wi-Fi settings stored on the board.
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);

  // ESP8266 Arduino core generates PWM in software using timer interrupts.
  // Set the range explicitly because defaults differ between core versions.
  analogWriteRange(PWM_PERIOD_US);
  analogWriteFreq(PWM_FREQUENCY_HZ);

  Serial.println("ESP8266 MOTOR PULSE TEST: initial neutral for 5 seconds");
  setPulse(NEUTRAL_US);
  delay(STARTUP_NEUTRAL_MS);
}

void loop() {
  Serial.println("FORWARD for 3 seconds");
  setPulse(FORWARD_US);
  delay(RUN_TIME_MS);

  Serial.println("NEUTRAL for 2 seconds");
  setPulse(NEUTRAL_US);
  delay(NEUTRAL_TIME_MS);

  Serial.println("BACKWARD for 3 seconds");
  setPulse(BACKWARD_US);
  delay(RUN_TIME_MS);

  Serial.println("NEUTRAL for 2 seconds");
  setPulse(NEUTRAL_US);
  delay(NEUTRAL_TIME_MS);
}
