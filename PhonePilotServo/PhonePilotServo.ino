#include <Arduino.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <mbedtls/base64.h>
#include "board_config.h"

namespace {
Servo panServo;
String inputLine;
int currentAngle = SERVO_CENTER_ANGLE;
bool servoReady = false;
Preferences preferences;
String wifiSsid;
String wifiPassword;
bool wifiConnecting = false;
bool wifiConnectedReported = false;
unsigned long wifiAttemptStartedAt = 0;
unsigned long lastWifiAttemptAt = 0;
WiFiServer commandServer(3333);
WiFiClient commandClient;
WiFiUDP discoveryUdp;
String wifiInputLine;
bool wifiServicesStarted = false;
unsigned long lastDiscoveryAt = 0;

constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
constexpr unsigned long DISCOVERY_INTERVAL_MS = 1500;
constexpr uint16_t COMMAND_PORT = 3333;
constexpr uint16_t DISCOVERY_PORT = 3334;

void sendResponse(const String& line) {
  Serial.println(line);
  if (commandClient && commandClient.connected()) {
    commandClient.println(line);
  }
}

int clampAngle(int value) {
  return constrain(value, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);
}

void printStatus() {
  sendResponse(
      "STATUS ready=" + String(servoReady ? 1 : 0) +
      " angle=" + String(currentAngle) +
      " min=" + String(SERVO_MIN_ANGLE) +
      " max=" + String(SERVO_MAX_ANGLE));
}

bool parseInteger(const String& text, int& value) {
  if (text.length() == 0) return false;
  char* end = nullptr;
  const long parsed = strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0') return false;
  value = static_cast<int>(parsed);
  return true;
}

bool decodeBase64(const String& encoded, String& decoded) {
  unsigned char output[128];
  size_t outputLength = 0;
  const int result = mbedtls_base64_decode(
      output,
      sizeof(output) - 1,
      &outputLength,
      reinterpret_cast<const unsigned char*>(encoded.c_str()),
      encoded.length());
  if (result != 0 || outputLength >= sizeof(output)) return false;
  output[outputLength] = '\0';
  decoded = String(reinterpret_cast<char*>(output));
  return true;
}

bool saveWifiCredentials(const String& ssid, const String& password) {
  if (!preferences.begin("phonepilot", false)) return false;
  const size_t ssidWritten = preferences.putString("ssid", ssid);
  const size_t passwordWritten = preferences.putString("password", password);
  preferences.end();
  return ssidWritten > 0 && passwordWritten > 0;
}

void loadWifiCredentials() {
  if (!preferences.begin("phonepilot", true)) return;
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  preferences.end();
}

void startWifiConnection() {
  if (wifiSsid.length() == 0 || wifiConnecting || WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  wifiConnecting = true;
  wifiConnectedReported = false;
  wifiAttemptStartedAt = millis();
  lastWifiAttemptAt = wifiAttemptStartedAt;
  sendResponse("WIFI CONNECTING");
}

void printWifiStatus() {
  if (WiFi.status() == WL_CONNECTED) {
    sendResponse("WIFI STATUS state=connected ip=" + WiFi.localIP().toString());
  } else if (wifiConnecting) {
    sendResponse("WIFI STATUS state=connecting ip=");
  } else if (wifiSsid.length() > 0) {
    sendResponse("WIFI STATUS state=disconnected ip=");
  } else {
    sendResponse("WIFI STATUS state=not_configured ip=");
  }
}

void startWifiServices() {
  if (wifiServicesStarted) return;
  commandServer.begin(COMMAND_PORT);
  commandServer.setNoDelay(true);
  discoveryUdp.begin(DISCOVERY_PORT);
  wifiServicesStarted = true;
  lastDiscoveryAt = 0;
}

void stopWifiServices() {
  if (!wifiServicesStarted) return;
  if (commandClient) commandClient.stop();
  commandServer.stop();
  discoveryUdp.stop();
  wifiInputLine = "";
  wifiServicesStarted = false;
}

IPAddress wifiBroadcastAddress() {
  const IPAddress local = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  IPAddress broadcast;
  for (int i = 0; i < 4; ++i) {
    broadcast[i] = local[i] | static_cast<uint8_t>(~mask[i]);
  }
  return broadcast;
}

void broadcastDiscovery() {
  if (!wifiServicesStarted || WiFi.status() != WL_CONNECTED) return;
  const unsigned long now = millis();
  if (lastDiscoveryAt != 0 && now - lastDiscoveryAt < DISCOVERY_INTERVAL_MS) return;
  lastDiscoveryAt = now;
  discoveryUdp.beginPacket(wifiBroadcastAddress(), DISCOVERY_PORT);
  discoveryUdp.printf("PHONEPILOT_ESP32 port=%u", COMMAND_PORT);
  discoveryUdp.endPacket();
}

void updateWifiConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnecting = false;
    startWifiServices();
    if (!wifiConnectedReported) {
      wifiConnectedReported = true;
      sendResponse("WIFI CONNECTED ip=" + WiFi.localIP().toString());
    }
    broadcastDiscovery();
    return;
  }

  stopWifiServices();
  wifiConnectedReported = false;
  const unsigned long now = millis();
  if (wifiConnecting && now - wifiAttemptStartedAt >= WIFI_CONNECT_TIMEOUT_MS) {
    wifiConnecting = false;
    WiFi.disconnect();
    sendResponse("ERR WIFI_TIMEOUT");
    return;
  }
  if (!wifiConnecting && wifiSsid.length() > 0 && now - lastWifiAttemptAt >= WIFI_RETRY_INTERVAL_MS) {
    startWifiConnection();
  }
}

void moveServo(int target) {
  if (!servoReady) {
    sendResponse("ERR SERVO_NOT_READY");
    return;
  }

  currentAngle = clampAngle(target);
  panServo.write(currentAngle);
  sendResponse("OK ANGLE " + String(currentAngle));
}

void handleCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  String upper = line;
  upper.toUpperCase();

  if (upper == "PING") {
    sendResponse("PONG");
    return;
  }
  if (upper == "STATUS") {
    printStatus();
    return;
  }
  if (upper == "WIFI_STATUS") {
    printWifiStatus();
    return;
  }
  if (upper.startsWith("WIFI_CONFIG ")) {
    const String arguments = line.substring(12);
    const int separator = arguments.indexOf(' ');
    if (separator <= 0 || separator >= static_cast<int>(arguments.length()) - 1) {
      sendResponse("ERR WIFI_INVALID_CONFIG");
      return;
    }
    String ssid;
    String password;
    if (!decodeBase64(arguments.substring(0, separator), ssid) ||
        !decodeBase64(arguments.substring(separator + 1), password) ||
        ssid.length() == 0 || ssid.length() > 32 || password.length() < 8 || password.length() > 63) {
      sendResponse("ERR WIFI_INVALID_CONFIG");
      return;
    }
    if (!saveWifiCredentials(ssid, password)) {
      sendResponse("ERR WIFI_SAVE_FAILED");
      return;
    }
    wifiSsid = ssid;
    wifiPassword = password;
    stopWifiServices();
    WiFi.disconnect(true);
    wifiConnecting = false;
    lastWifiAttemptAt = millis() - WIFI_RETRY_INTERVAL_MS;
    sendResponse("OK WIFI_SAVED");
    startWifiConnection();
    return;
  }
  if (upper == "CENTER") {
    moveServo(SERVO_CENTER_ANGLE);
    return;
  }

  // Canonical command used by the Android USB bridge: S95
  if (upper.startsWith("S") && upper.length() > 1) {
    int value = 0;
    if (parseInteger(upper.substring(1), value)) {
      moveServo(value);
      return;
    }
  }

  // Human-readable command for Serial Monitor: ANGLE 95
  if (upper.startsWith("ANGLE ")) {
    int value = 0;
    if (parseInteger(upper.substring(6), value)) {
      moveServo(value);
      return;
    }
  }

  sendResponse("ERR UNKNOWN_COMMAND " + line);
}

void readSerialCommands() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      handleCommand(inputLine);
      inputLine = "";
      continue;
    }
    if (inputLine.length() < 255) inputLine += c;
  }
}

void readWifiCommands() {
  if (!wifiServicesStarted) return;

  if (!commandClient || !commandClient.connected()) {
    if (commandClient) commandClient.stop();
    WiFiClient incoming = commandServer.available();
    if (incoming) {
      commandClient = incoming;
      commandClient.setNoDelay(true);
      wifiInputLine = "";
      sendResponse("PHONEPILOT_ESP32_READY v0.3-arduino");
      printStatus();
      printWifiStatus();
    }
  }

  while (commandClient && commandClient.connected() && commandClient.available() > 0) {
    const char c = static_cast<char>(commandClient.read());
    if (c == '\r') continue;
    if (c == '\n') {
      handleCommand(wifiInputLine);
      wifiInputLine = "";
      continue;
    }
    if (wifiInputLine.length() < 255) wifiInputLine += c;
  }
}
}  // namespace

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(250);

  panServo.setPeriodHertz(50);
  panServo.attach(SERVO_PIN, SERVO_MIN_PULSE_US, SERVO_MAX_PULSE_US);
  // PWM channel 0 is valid, so check attached() instead of treating attach()==0 as failure.
  servoReady = panServo.attached();
  if (servoReady) {
    panServo.write(currentAngle);
    delay(300);
  }

  Serial.println("PHONEPILOT_ESP32_READY v0.3-arduino");
  printStatus();
  loadWifiCredentials();
  if (wifiSsid.length() > 0) startWifiConnection();
}

void loop() {
  readSerialCommands();
  updateWifiConnection();
  readWifiCommands();
  delay(2);
}
