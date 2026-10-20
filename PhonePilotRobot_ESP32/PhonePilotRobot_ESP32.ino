#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <cJSON.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>
#include <freertos/queue.h>
#include "PhonePilotProtocol.h"
#include "board_config.h"
#include "DriveControl.h"

// This sketch deliberately targets the original ESP32/Bluedroid BLE stack.
// Other stacks must not silently turn the protected GATT write into an open write.
#ifndef CONFIG_BLUEDROID_ENABLED
#error PhonePilot v2 requires an ESP32 board with Bluedroid and MITM GATT permissions.
#endif

namespace {
constexpr char kServiceUuid[] = "6c40cb73-3396-4ec6-bcc0-cb2db37ace10";
constexpr char kCommandUuid[] = "6c40cb73-3396-4ec6-bcc0-cb2db37ace11";
constexpr char kStatusUuid[] = "6c40cb73-3396-4ec6-bcc0-cb2db37ace12";
constexpr uint32_t kSettingsMagic = 0x32505046;
constexpr uint32_t kPairingWindowMs = 60000;
constexpr uint32_t kBootHoldMs = 3000;
constexpr uint16_t kCommandPort = 3333;
constexpr uint16_t kDiscoveryPort = 3334;

struct Profile {
  char ssid[33];
  char password[64];
};
struct Settings {
  uint32_t magic;
  uint32_t generation;
  char mode[8];
  Profile shared;
  Profile hotspot;
  uint32_t checksum;
};
struct BleCommand {
  char json[PhonePilotProtocol::kMaxJson + 1];
  uint16_t length;
};

Servo panServo;
bool servoReady = false;
int currentAngle = SERVO_CENTER_ANGLE;
PhonePilotDrive::Control driveControl;
PhonePilotDrive::Direction driveOutput = PhonePilotDrive::Direction::Stop;
String serialLine;
String tcpLine;
Preferences preferences;
Settings saved = {};
uint8_t savedBank = 0;
bool hasStoredMode = false;
Settings candidate = {};
bool transactionActive = false;
String mode = "USB";
String phase = "waiting_ble";
String errorCode;
String requestId;
String wifiSsid;
String wifiPassword;
bool wifiConnecting = false;
bool wifiReported = false;
volatile bool gotIpAfterAttempt = false;
bool restoreAfterFailure = false;
uint32_t wifiAttemptAt = 0;
uint32_t lastWifiAttemptAt = 0;
uint32_t bootAt = 0;
bool startupApplied = false;
bool setupApActive = false;
String setupSsid;
String setupPassword;
WiFiServer commandServer(kCommandPort);
WiFiClient commandClient;
WiFiUDP discoveryUdp;
bool wifiServicesStarted = false;
uint32_t lastDiscoveryAt = 0;

BLEServer* bleServer = nullptr;
BLECharacteristic* statusCharacteristic = nullptr;
QueueHandle_t bleCommands = nullptr;
PhonePilotProtocol::FrameDecoder bleDecoder;
volatile bool bleConnected = false;
volatile bool bleAuthorized = false;
bool peerWasBonded = false;
bool pairingAllowedAtConnect = false;
uint8_t priorBondCount = 0;
uint8_t priorBondAddresses[16][6] = {};
uint16_t bleConnectionId = 0;
uint16_t bleMtu = 23;
uint32_t blePin = 0;
uint32_t bleConnectedAt = 0;
uint32_t pairingUntil = 0;
uint32_t bootPressedAt = 0;
bool bootHoldHandled = false;
bool statusDirty = true;
String deviceId;
PhonePilotProtocol::AckCache recentAcks;

uint32_t checksum(const Settings& value) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
  uint32_t result = 2166136261UL;
  for (size_t i = 0; i < offsetof(Settings, checksum); ++i) result = (result ^ bytes[i]) * 16777619UL;
  return result;
}

bool validSettings(const Settings& value) {
  if (value.magic != kSettingsMagic || value.checksum != checksum(value)) return false;
  if (memchr(value.mode, 0, sizeof(value.mode)) == nullptr ||
      !PhonePilotProtocol::validMode(value.mode)) return false;
  const Profile* profiles[] = {&value.shared, &value.hotspot};
  for (const Profile* p : profiles) {
    if (memchr(p->ssid, 0, sizeof(p->ssid)) == nullptr ||
        memchr(p->password, 0, sizeof(p->password)) == nullptr) return false;
    if (p->ssid[0] && !PhonePilotProtocol::validCredentials(p->ssid, p->password)) return false;
  }
  return true;
}

bool persistSettings(Settings next) {
  next.magic = kSettingsMagic;
  next.generation = saved.generation + 1;
  next.checksum = checksum(next);
  if (!preferences.begin("phonepilot", false)) return false;
  const uint8_t bank = savedBank ^ 1;
  const char* key = bank ? "cfg1" : "cfg0";
  const bool written = preferences.putBytes(key, &next, sizeof(next)) == sizeof(next);
  Settings check = {};
  const bool verified = written && preferences.getBytes(key, &check, sizeof(check)) == sizeof(check) &&
                        validSettings(check) && check.generation == next.generation;
  preferences.end();
  if (verified) {
    saved = next;
    savedBank = bank;
    hasStoredMode = true;
  }
  return verified;
}

void loadSettings() {
  Settings a = {}, b = {};
  if (!preferences.begin("phonepilot", true)) return;
  const bool validA = preferences.getBytes("cfg0", &a, sizeof(a)) == sizeof(a) && validSettings(a);
  const bool validB = preferences.getBytes("cfg1", &b, sizeof(b)) == sizeof(b) && validSettings(b);
  if (validA || validB) {
    savedBank = validB && (!validA || static_cast<int32_t>(b.generation - a.generation) > 0) ? 1 : 0;
    saved = savedBank ? b : a;
    hasStoredMode = true;
    preferences.end();
    return;
  }
  // v0.4 had one mode/SSID/password tuple. Keep the old keys until a full v2
  // record has been committed; a failed migration never destroys credentials.
  const bool hasLegacyMode = preferences.isKey("mode");
  String oldMode = preferences.getString("mode", "USB");
  String oldSsid = preferences.getString("ssid", "");
  String oldPassword = preferences.getString("password", "");
  preferences.end();
  if (!hasLegacyMode) {
    strcpy(saved.mode, "USB");
    return;
  }
  Settings migrated = {};
  strcpy(migrated.mode, "USB");
  if (PhonePilotProtocol::validMode(oldMode.c_str()) &&
      (oldMode == "USB" || PhonePilotProtocol::validCredentials(oldSsid.c_str(), oldPassword.c_str()))) {
    strlcpy(migrated.mode, oldMode.c_str(), sizeof(migrated.mode));
    if (oldMode == "SHARED" || oldMode == "HOTSPOT") {
      Profile& profile = oldMode == "SHARED" ? migrated.shared : migrated.hotspot;
      strlcpy(profile.ssid, oldSsid.c_str(), sizeof(profile.ssid));
      strlcpy(profile.password, oldPassword.c_str(), sizeof(profile.password));
    }
  }
  if (!persistSettings(migrated)) Serial.println("ERR NVS_MIGRATION");
}

const Profile* profileFor(const Settings& settings, const String& requestedMode) {
  if (requestedMode == "SHARED") return &settings.shared;
  if (requestedMode == "HOTSPOT") return &settings.hotspot;
  return nullptr;
}

bool hasProfile(const Settings& settings, const String& requestedMode) {
  const Profile* profile = profileFor(settings, requestedMode);
  return profile && PhonePilotProtocol::validCredentials(profile->ssid, profile->password);
}

uint32_t pairingPin() {
  if (!preferences.begin("phonepilot", false)) return 0;
  uint32_t pin = preferences.getUInt("ble_pin", 0);
  if (pin < 100000 || pin > 999999) {
    pin = 100000 + esp_random() % 900000;
    if (preferences.putUInt("ble_pin", pin) != sizeof(pin)) pin = 0;
  }
  preferences.end();
  return pin;
}

bool isBonded(const uint8_t* address) {
  const int count = esp_ble_get_bond_device_num();
  if (count <= 0) return false;
  esp_ble_bond_dev_t* bonds = new esp_ble_bond_dev_t[count];
  int found = count;
  const bool loaded = esp_ble_get_bond_device_list(&found, bonds) == ESP_OK;
  bool match = false;
  for (int i = 0; loaded && i < found; ++i) {
    if (memcmp(bonds[i].bd_addr, address, 6) == 0) match = true;
  }
  delete[] bonds;
  return match;
}

void captureBonds() {
  priorBondCount = 0;
  int count = min(esp_ble_get_bond_device_num(), 16);
  if (count <= 0) return;
  esp_ble_bond_dev_t bonds[16] = {};
  if (esp_ble_get_bond_device_list(&count, bonds) != ESP_OK) return;
  priorBondCount = count;
  for (int i = 0; i < count; ++i) memcpy(priorBondAddresses[i], bonds[i].bd_addr, 6);
}

bool wasBondedBefore(const uint8_t* address) {
  for (int i = 0; i < priorBondCount; ++i)
    if (memcmp(priorBondAddresses[i], address, 6) == 0) return true;
  return false;
}

bool removeUnexpectedBonds() {
  int count = min(esp_ble_get_bond_device_num(), 16);
  if (count <= 0) return false;
  esp_ble_bond_dev_t bonds[16] = {};
  if (esp_ble_get_bond_device_list(&count, bonds) != ESP_OK) return true;
  bool unexpected = false;
  for (int i = 0; i < count; ++i) {
    if (!wasBondedBefore(bonds[i].bd_addr)) {
      unexpected = true;
      if (!pairingAllowedAtConnect) esp_ble_remove_bond_device(bonds[i].bd_addr);
    }
  }
  return unexpected;
}

void startPairingWindow() {
  if (!bleServer || blePin < 100000) return;
  pairingUntil = millis() + kPairingWindowMs;
  Serial.printf("BLE PAIRING PIN %06lu (60 seconds)\n", static_cast<unsigned long>(blePin));
}

void sendResponse(const String& line) {
  Serial.println(line);
  if (commandClient && commandClient.connected()) commandClient.println(line);
}

const char* driveDirectionName(PhonePilotDrive::Direction direction) {
  if (direction == PhonePilotDrive::Direction::Forward) return "forward";
  if (direction == PhonePilotDrive::Direction::Backward) return "backward";
  return "stop";
}

void printDriveStatus() {
  sendResponse("DRIVE_STATE ready=1 direction=" + String(driveDirectionName(driveOutput)));
}

void writeMotorPins(PhonePilotDrive::Direction direction) {
  // Disable both bridges before changing direction inputs.
  digitalWrite(MOTOR_ENA_PIN, LOW);
  digitalWrite(MOTOR_ENB_PIN, LOW);
  const bool moving = direction != PhonePilotDrive::Direction::Stop;
  const bool forward = direction == PhonePilotDrive::Direction::Forward;
  digitalWrite(MOTOR_IN1_PIN, moving && (forward != MOTOR_A_INVERT) ? HIGH : LOW);
  digitalWrite(MOTOR_IN2_PIN, moving && (forward == MOTOR_A_INVERT) ? HIGH : LOW);
  digitalWrite(MOTOR_IN3_PIN, moving && (forward != MOTOR_B_INVERT) ? HIGH : LOW);
  digitalWrite(MOTOR_IN4_PIN, moving && (forward == MOTOR_B_INVERT) ? HIGH : LOW);
  if (moving) {
    digitalWrite(MOTOR_ENA_PIN, HIGH);
    digitalWrite(MOTOR_ENB_PIN, HIGH);
  }
}

void updateDrive() {
  const auto next = driveControl.update(millis());
  if (next != driveOutput) {
    driveOutput = next;
    writeMotorPins(next);
    printDriveStatus();
  }
}

void stopDrive() {
  driveControl.command(PhonePilotDrive::Direction::Stop, millis());
  updateDrive();
}

void printServoStatus() {
  sendResponse("STATUS ready=" + String(servoReady ? 1 : 0) + " angle=" + String(currentAngle) +
               " min=" + String(SERVO_MIN_ANGLE) + " max=" + String(SERVO_MAX_ANGLE));
  printDriveStatus();
}

void printWifiStatus() {
  const String state = mode == "USB" || wifiSsid.isEmpty() ? "not_configured" :
                       WiFi.status() == WL_CONNECTED ? "connected" :
                       wifiConnecting ? "connecting" : "disconnected";
  sendResponse("WIFI STATUS state=" + state + " ip=" +
               (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : ""));
}

void stopWifiServices() {
  stopDrive();
  if (!wifiServicesStarted) return;
  if (commandClient) commandClient.stop();
  commandServer.stop();
  discoveryUdp.stop();
  wifiServicesStarted = false;
  tcpLine = "";
}

void startWifiServices() {
  if (wifiServicesStarted) return;
  commandServer.begin(kCommandPort);
  commandServer.setNoDelay(true);
  discoveryUdp.begin(kDiscoveryPort);
  wifiServicesStarted = true;
  lastDiscoveryAt = 0;
}

void startSetupAp() {
  if (setupApActive) return;
  if (!preferences.begin("phonepilot", true)) return;
  setupPassword = preferences.getString("setup_pass", "");
  preferences.end();
  if (setupPassword.length() < 8) {
    Serial.println("ERR SETUP_PASSWORD_REQUIRED");
    return;
  }
  WiFi.mode(WIFI_AP_STA);
  setupSsid = "PhonePilot-Setup-" + deviceId.substring(6);
  setupApActive = WiFi.softAP(setupSsid.c_str(), setupPassword.c_str());
  if (setupApActive) {
    startWifiServices();
    Serial.println("SETUP SSID " + setupSsid);
    Serial.println("SETUP IP " + WiFi.softAPIP().toString());
  }
}

void configureActiveWifi(const Settings& settings, const String& selectedMode) {
  const Profile* profile = profileFor(settings, selectedMode);
  wifiSsid = profile ? String(profile->ssid) : "";
  wifiPassword = profile ? String(profile->password) : "";
}

void applyMode(const Settings& settings, const String& selectedMode) {
  // Keep a legacy setup-AP socket alive long enough to receive the final
  // result after STA joins the target network.
  bool keepSetupClient = false;
  if (setupApActive && commandClient && commandClient.connected()) {
    const IPAddress remote = commandClient.remoteIP();
    const IPAddress ap = WiFi.softAPIP();
    keepSetupClient = remote[0] == ap[0] && remote[1] == ap[1] && remote[2] == ap[2];
  }
  if (!keepSetupClient) stopWifiServices();
  WiFi.disconnect(false);
  gotIpAfterAttempt = false;
  wifiConnecting = false;
  wifiReported = false;
  mode = selectedMode;
  configureActiveWifi(settings, selectedMode);
  statusDirty = true;
  Serial.println("MODE " + mode);
  if (mode == "USB") {
    phase = "ready";
    if (!setupApActive) WiFi.mode(WIFI_OFF);
    statusDirty = true;
    return;
  }
  phase = "connecting_wifi";
  WiFi.mode(setupApActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  wifiConnecting = true;
  wifiAttemptAt = lastWifiAttemptAt = millis();
  statusDirty = true;
  Serial.println("WIFI CONNECTING mode=" + mode + " ssid=" + wifiSsid);
}

void applySavedMode() {
  startupApplied = true;
  if (!hasStoredMode) {
    mode = "USB";
    phase = "waiting_ble";
    statusDirty = true;
    return;
  }
  const String selected = saved.mode;
  if (selected != "USB" && !hasProfile(saved, selected)) {
    mode = "USB";
    phase = "waiting_ble";
    startSetupAp();
    statusDirty = true;
    return;
  }
  applyMode(saved, selected);
}

void finishWifiFailure() {
  wifiConnecting = false;
  WiFi.disconnect(false);
  if (transactionActive) {
    transactionActive = false;
    Serial.println("WIFI ROLLBACK mode=" + String(saved.mode));
    applyMode(saved, saved.mode);
    restoreAfterFailure = mode != "USB";
    errorCode = "CONNECT_TIMEOUT";
    phase = "error";
  } else {
    errorCode = "CONNECT_TIMEOUT";
    phase = "error";
  }
  if (setupApActive) startWifiServices();
  statusDirty = true;
  sendResponse("ERR WIFI_TIMEOUT");
}

void updateWifi() {
  if (mode == "USB") return;
  const auto decision = PhonePilotProtocol::wifiDecision(
      gotIpAfterAttempt && WiFi.status() == WL_CONNECTED && WiFi.SSID() == wifiSsid,
      wifiConnecting, millis() - wifiAttemptAt, millis() - lastWifiAttemptAt);
  if (decision == PhonePilotProtocol::WifiDecision::Connected) {
    wifiConnecting = false;
    if (transactionActive) {
      if (persistSettings(candidate)) {
        transactionActive = false;
        Serial.println("WIFI CONFIG COMMITTED mode=" + mode);
        sendResponse("OK CONFIG_SAVED mode=" + mode);
      } else {
        transactionActive = false;
        applyMode(saved, saved.mode);
        restoreAfterFailure = mode != "USB";
        errorCode = "INVALID_CONFIG";
        phase = "error";
        statusDirty = true;
        sendResponse("ERR WIFI_SAVE_FAILED");
        return;
      }
    }
    startWifiServices();
    if (!wifiReported) {
      wifiReported = true;
      Serial.println("WIFI CONNECTED mode=" + mode + " ip=" + WiFi.localIP().toString());
      if (!restoreAfterFailure) {
        phase = "ready";
        errorCode = "";
      }
      restoreAfterFailure = false;
      statusDirty = true;
    }
    if (setupApActive) {
      if (commandClient && commandClient.connected()) delay(50);
      WiFi.softAPdisconnect(true);
      setupApActive = false;
    }
    if (!lastDiscoveryAt || millis() - lastDiscoveryAt >= 1500) {
      lastDiscoveryAt = millis();
      IPAddress broadcast = WiFi.localIP();
      const IPAddress mask = WiFi.subnetMask();
      for (int i = 0; i < 4; ++i) broadcast[i] |= static_cast<uint8_t>(~mask[i]);
      discoveryUdp.beginPacket(broadcast, kDiscoveryPort);
      discoveryUdp.printf("PHONEPILOT_ESP32 port=%u", kCommandPort);
      discoveryUdp.endPacket();
    }
    return;
  }
  if (wifiReported) {
    wifiReported = false;
    phase = "error";
    errorCode = "CONNECT_TIMEOUT";
    statusDirty = true;
  }
  if (decision == PhonePilotProtocol::WifiDecision::Timeout) {
    finishWifiFailure();
  } else if (decision == PhonePilotProtocol::WifiDecision::Retry && !transactionActive) {
    applyMode(saved, saved.mode);
  }
}

void startTransaction(const String& selectedMode, const String& ssid, const String& password) {
  candidate = saved;
  strlcpy(candidate.mode, selectedMode.c_str(), sizeof(candidate.mode));
  if (selectedMode != "USB" && !ssid.isEmpty()) {
    Profile& profile = selectedMode == "SHARED" ? candidate.shared : candidate.hotspot;
    strlcpy(profile.ssid, ssid.c_str(), sizeof(profile.ssid));
    strlcpy(profile.password, password.c_str(), sizeof(profile.password));
  }
  errorCode = "";
  restoreAfterFailure = false;
  startupApplied = true;
  if (selectedMode == "USB") {
    if (persistSettings(candidate)) applyMode(saved, "USB");
    else {
      errorCode = "INVALID_CONFIG";
      phase = "error";
      statusDirty = true;
    }
    return;
  }
  transactionActive = true;
  applyMode(candidate, selectedMode);
}

String wifiState() {
  if (mode == "USB" || wifiSsid.isEmpty()) return "not_configured";
  if (WiFi.status() == WL_CONNECTED) return "connected";
  return wifiConnecting ? "connecting" : "disconnected";
}

String jsonString(cJSON* root) {
  char* printed = cJSON_PrintUnformatted(root);
  String result = printed ? String(printed) : "";
  cJSON_free(printed);
  return result;
}

String statusJson() {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "v", 1);
  cJSON_AddStringToObject(root, "type", "status");
  cJSON_AddStringToObject(root, "deviceId", deviceId.c_str());
  cJSON_AddStringToObject(root, "mode", mode.c_str());
  cJSON_AddStringToObject(root, "phase", phase.c_str());
  cJSON_AddStringToObject(root, "wifiState", wifiState().c_str());
  if (mode == "USB" || wifiSsid.isEmpty()) cJSON_AddNullToObject(root, "ssid");
  else cJSON_AddStringToObject(root, "ssid", wifiSsid.c_str());
  if (WiFi.status() == WL_CONNECTED) cJSON_AddStringToObject(root, "ip", WiFi.localIP().toString().c_str());
  else cJSON_AddNullToObject(root, "ip");
  if (requestId.isEmpty()) cJSON_AddNullToObject(root, "requestId");
  else cJSON_AddStringToObject(root, "requestId", requestId.c_str());
  if (errorCode.isEmpty()) cJSON_AddNullToObject(root, "errorCode");
  else cJSON_AddStringToObject(root, "errorCode", errorCode.c_str());
  const String result = jsonString(root);
  cJSON_Delete(root);
  return result;
}

void setReadableStatus() {
  if (!statusCharacteristic) return;
  const String json = statusJson();
  if (json.length() > PhonePilotProtocol::kMaxJson) {
    Serial.println("ERR BLE_STATUS_TOO_LARGE");
    return;
  }
  uint8_t frame[PhonePilotProtocol::kMaxJson + 2];
  frame[0] = json.length() & 0xff;
  frame[1] = json.length() >> 8;
  memcpy(frame + 2, json.c_str(), json.length());
  statusCharacteristic->setValue(frame, json.length() + 2);
}

void notifyJson(const String& json) {
  if (!statusCharacteristic || !bleConnected || !bleAuthorized ||
      json.length() > PhonePilotProtocol::kMaxJson) return;
  uint8_t frame[PhonePilotProtocol::kMaxJson + 2];
  frame[0] = json.length() & 0xff;
  frame[1] = json.length() >> 8;
  memcpy(frame + 2, json.c_str(), json.length());
  const size_t chunk = bleMtu > 3 ? bleMtu - 3 : 20;
  for (size_t offset = 0; offset < json.length() + 2 && bleConnected; offset += chunk) {
    const size_t count = min(chunk, json.length() + 2 - offset);
    statusCharacteristic->setValue(frame + offset, count);
    statusCharacteristic->notify();
    delay(12);
  }
  setReadableStatus();
}

void publishStatus() {
  statusDirty = false;
  setReadableStatus();
  notifyJson(statusJson());
  Serial.println("BLE STATUS mode=" + mode + " phase=" + phase + " wifi=" + wifiState() +
                 (errorCode.isEmpty() ? "" : " error=" + errorCode));
}

void sendAck(const String& id, const char* result, const char* code = nullptr) {
  recentAcks.remember(id.c_str(), result, code);
  cJSON* root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "v", 1);
  cJSON_AddStringToObject(root, "type", "ack");
  cJSON_AddStringToObject(root, "requestId", id.c_str());
  cJSON_AddStringToObject(root, "result", result);
  if (code) cJSON_AddStringToObject(root, "code", code);
  else cJSON_AddNullToObject(root, "code");
  notifyJson(jsonString(root));
  cJSON_Delete(root);
}

const char* requiredString(cJSON* root, const char* name) {
  cJSON* field = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsString(field) ? field->valuestring : nullptr;
}

void handleBleCommand(const char* json, size_t length) {
  if (memchr(json, 0, length)) return;
  cJSON* root = cJSON_ParseWithLengthOpts(json, length + 1, nullptr, true);
  if (!root) return;
  const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "v");
  const char* id = requiredString(root, "requestId");
  if (!PhonePilotProtocol::validUuid(id)) { cJSON_Delete(root); return; }
  const String currentId = id;
  if (!cJSON_IsNumber(version) || version->valuedouble != 1) {
    sendAck(currentId, "error", "UNSUPPORTED_VERSION");
    cJSON_Delete(root);
    return;
  }
  PhonePilotProtocol::AckRecord previous;
  if (recentAcks.find(currentId.c_str(), previous)) {
    sendAck(currentId, previous.result, previous.code[0] ? previous.code : nullptr);
    statusDirty = true;
    cJSON_Delete(root);
    return;
  }
  const char* type = requiredString(root, "type");
  const char* selected = requiredString(root, "mode");
  if (!type || !PhonePilotProtocol::validMode(selected) ||
      (strcmp(type, "select_mode") && strcmp(type, "set_config"))) {
    sendAck(currentId, "error", "INVALID_CONFIG");
    cJSON_Delete(root);
    return;
  }
  if (transactionActive) {
    sendAck(currentId, "error", "INVALID_CONFIG");
    cJSON_Delete(root);
    return;
  }
  String ssid;
  String password;
  if (!strcmp(type, "select_mode")) {
    if (cJSON_GetObjectItemCaseSensitive(root, "ssid") ||
        cJSON_GetObjectItemCaseSensitive(root, "password")) {
      sendAck(currentId, "error", "INVALID_CONFIG");
      cJSON_Delete(root);
      return;
    }
    if (strcmp(selected, "USB") && !hasProfile(saved, selected)) {
      sendAck(currentId, "error", "NEED_CREDENTIALS");
      cJSON_Delete(root);
      return;
    }
  } else if (!strcmp(selected, "USB")) {
    if (cJSON_GetObjectItemCaseSensitive(root, "ssid") ||
        cJSON_GetObjectItemCaseSensitive(root, "password")) {
      sendAck(currentId, "error", "INVALID_CONFIG");
      cJSON_Delete(root);
      return;
    }
  } else {
    const char* s = requiredString(root, "ssid");
    const char* p = requiredString(root, "password");
    if (!PhonePilotProtocol::validCredentials(s, p)) {
      sendAck(currentId, "error", "INVALID_CONFIG");
      cJSON_Delete(root);
      return;
    }
    ssid = s;
    password = p;
  }
  requestId = currentId;
  sendAck(currentId, "accepted");
  if (mode == selected && !strcmp(type, "select_mode") && phase == "ready") {
    statusDirty = true;
  } else {
    startTransaction(selected, ssid, password);
  }
  cJSON_Delete(root);
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server, esp_ble_gatts_cb_param_t* param) override {
    if (bleConnected) {
      server->disconnect(param->connect.conn_id);
      return;
    }
    bleConnected = true;
    bleAuthorized = false;
    bleConnectionId = param->connect.conn_id;
    bleConnectedAt = millis();
    bleMtu = 23;
    bleDecoder.reset();
    captureBonds();
    peerWasBonded = wasBondedBefore(param->connect.remote_bda);
    pairingAllowedAtConnect = static_cast<int32_t>(pairingUntil - millis()) > 0;
    Serial.println("BLE CONNECTED");
    if (!peerWasBonded && !priorBondCount && !pairingAllowedAtConnect)
      server->disconnect(bleConnectionId);
  }
  void onDisconnect(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
    if (param->disconnect.conn_id != bleConnectionId) return;
    bleConnected = false;
    bleAuthorized = false;
    bleDecoder.reset();
    if (bleCommands) xQueueReset(bleCommands);
    statusDirty = true;
    BLEDevice::startAdvertising();
    Serial.println("BLE DISCONNECTED");
  }
  void onMtuChanged(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
    bleMtu = param->mtu.mtu;
  }
};

class SecurityCallbacks : public BLESecurityCallbacks {
  bool onSecurityRequest() override {
    return peerWasBonded || priorBondCount || pairingAllowedAtConnect;
  }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
    const uint8_t required = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    const bool secure = result.success && (result.auth_mode & required) == required;
    const bool newBond = removeUnexpectedBonds();
    const bool permitted = pairingAllowedAtConnect ||
                           (priorBondCount && !newBond);
    const bool bonded = isBonded(result.bd_addr) || (pairingAllowedAtConnect && newBond) ||
                        (priorBondCount && !newBond);
    bleAuthorized = secure && permitted && bonded;
    if (!bleAuthorized) {
      if (bleServer && bleConnected) bleServer->disconnect(bleConnectionId);
      Serial.println("BLE AUTH REJECTED");
    } else {
      statusDirty = true;
      Serial.println("BLE BONDED AUTHENTICATED");
    }
  }
};

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic, esp_ble_gatts_cb_param_t* param) override {
    if (!bleAuthorized || !bleConnected || !bleCommands ||
        param->write.conn_id != bleConnectionId) return;
    const String bytes = characteristic->getValue();
    for (size_t i = 0; i < bytes.length(); ++i) {
      const auto result = bleDecoder.push(static_cast<uint8_t>(bytes[i]), millis());
      if (result == PhonePilotProtocol::FrameResult::Complete) {
        BleCommand command = {};
        memcpy(command.json, bleDecoder.json(), bleDecoder.jsonLength());
        command.length = bleDecoder.jsonLength();
        if (xQueueSend(bleCommands, &command, 0) != pdTRUE) Serial.println("ERR BLE_QUEUE_FULL");
      } else if (result == PhonePilotProtocol::FrameResult::Invalid) {
        Serial.println("ERR BLE_FRAME_LENGTH");
      }
    }
  }
};

void startBle() {
  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  char id[13];
  snprintf(id, sizeof(id), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  deviceId = id;
  const String name = "PhonePilot-ESP32-" + deviceId.substring(6);
  bleCommands = xQueueCreate(4, sizeof(BleCommand));
  if (!bleCommands || !BLEDevice::init(name)) {
    Serial.println("ERR BLE_INIT");
    return;
  }
  const uint32_t pin = pairingPin();
  if (pin < 100000 || pin > 999999) {
    Serial.println("ERR BLE_PIN_NVS");
    return;
  }
  blePin = pin;
  BLEDevice::setMTU(247);
  BLESecurity* security = new BLESecurity();
  security->setPassKey(true, pin);
  security->setCapability(ESP_IO_CAP_OUT);
  security->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  security->setKeySize(16);
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
  BLEService* service = bleServer->createService(kServiceUuid);
  BLECharacteristic* command = service->createCharacteristic(kCommandUuid, BLECharacteristic::PROPERTY_WRITE);
  command->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
  command->setCallbacks(new CommandCallbacks());
  statusCharacteristic = service->createCharacteristic(
      kStatusUuid, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  statusCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
  BLE2902* cccd = new BLE2902();
  cccd->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);
  statusCharacteristic->addDescriptor(cccd);
  setReadableStatus();
  service->start();
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setScanResponse(true);
  if (esp_ble_get_bond_device_num() == 0) startPairingWindow();
  BLEDevice::startAdvertising();
  Serial.println("BLE ADVERTISING " + name + " id=" + deviceId);
}

bool decodeBase64(const String& encoded, String& decoded) {
  unsigned char bytes[128];
  size_t length = 0;
  if (mbedtls_base64_decode(bytes, sizeof(bytes) - 1, &length,
                            reinterpret_cast<const unsigned char*>(encoded.c_str()),
                            encoded.length()) != 0) return false;
  bytes[length] = 0;
  decoded = reinterpret_cast<const char*>(bytes);
  return decoded.length() == length;
}

void moveServo(int target) {
  if (!servoReady) { sendResponse("ERR SERVO_NOT_READY"); return; }
  currentAngle = constrain(target, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);
  panServo.write(currentAngle);
  sendResponse("OK ANGLE " + String(currentAngle));
}

bool parseAngle(const String& text, int& angle) {
  if (text.isEmpty()) return false;
  char* end = nullptr;
  const long value = strtol(text.c_str(), &end, 10);
  if (*end || end == text.c_str()) return false;
  angle = static_cast<int>(constrain(value, -10000L, 10000L));
  return true;
}

void handleLegacyConfig(const String& line, bool fromNetwork) {
  String upper = line;
  upper.toUpperCase();
  if (fromNetwork) {
    const IPAddress remote = commandClient.remoteIP();
    const IPAddress ap = WiFi.softAPIP();
    if (!setupApActive || remote[0] != ap[0] || remote[1] != ap[1] || remote[2] != ap[2]) {
      sendResponse("ERR CONFIG_SETUP_ONLY");
      return;
    }
  }
  if (transactionActive) { sendResponse("ERR CONFIG_BUSY"); return; }
  String selected;
  String args;
  if (upper == "CONFIG USB") selected = "USB";
  else if (upper.startsWith("CONFIG SHARED ")) { selected = "SHARED"; args = line.substring(14); }
  else if (upper.startsWith("CONFIG HOTSPOT ")) { selected = "HOTSPOT"; args = line.substring(15); }
  else if (upper.startsWith("WIFI_CONFIG ")) { selected = "HOTSPOT"; args = line.substring(12); }
  else { sendResponse("ERR WIFI_INVALID_CONFIG"); return; }
  String ssid, password;
  if (selected != "USB") {
    const int split = args.indexOf(' ');
    if (split <= 0 || !decodeBase64(args.substring(0, split), ssid) ||
        !decodeBase64(args.substring(split + 1), password) ||
        !PhonePilotProtocol::validCredentials(ssid.c_str(), password.c_str())) {
      sendResponse("ERR WIFI_INVALID_CONFIG");
      return;
    }
  }
  if (selected != "USB") sendResponse("OK CONFIG_ACCEPTED mode=" + selected);
  startTransaction(selected, ssid, password);
  if (selected == "USB") sendResponse(errorCode.isEmpty() ? "OK CONFIG_SAVED mode=USB" : "ERR WIFI_SAVE_FAILED");
}

void handleCommand(String line, bool fromNetwork = false) {
  line.trim();
  if (line.isEmpty()) return;
  String upper = line;
  upper.toUpperCase();
  if (upper == "PING") sendResponse("PONG");
  else if (upper == "DRIVE STOP") { stopDrive(); printDriveStatus(); }
  else if (upper == "DRIVE FORWARD" || upper == "DRIVE BACKWARD") {
    driveControl.command(upper == "DRIVE FORWARD" ? PhonePilotDrive::Direction::Forward :
                         PhonePilotDrive::Direction::Backward, millis());
    updateDrive();
  }
  else if (upper == "STATUS") printServoStatus();
  else if (upper == "WIFI_STATUS") printWifiStatus();
  else if (upper == "MODE_STATUS") sendResponse("MODE " + mode);
  else if (upper == "SETUP_AP" && !fromNetwork) {
    startSetupAp();
    sendResponse(setupApActive ? "OK SETUP_AP" : "ERR SETUP_AP_UNAVAILABLE");
  }
  else if (upper.startsWith("SETUP_PASSWORD ") && !fromNetwork) {
    const String chosen = line.substring(15);
    if (chosen.length() < 8 || chosen.length() > 63 || !preferences.begin("phonepilot", false)) {
      sendResponse("ERR SETUP_PASSWORD_INVALID");
      return;
    }
    const bool stored = preferences.putString("setup_pass", chosen) == chosen.length();
    preferences.end();
    if (stored && setupApActive) {
      WiFi.softAPdisconnect(true);
      setupApActive = false;
    }
    if (stored) startSetupAp();
    sendResponse(stored ? "OK SETUP_PASSWORD_SAVED" : "ERR SETUP_PASSWORD_SAVE_FAILED");
  }
  else if (upper.startsWith("CONFIG ") || upper.startsWith("WIFI_CONFIG ")) handleLegacyConfig(line, fromNetwork);
  else if (upper == "CENTER") moveServo(SERVO_CENTER_ANGLE);
  else {
    int angle = 0;
    if (upper.startsWith("S") && parseAngle(upper.substring(1), angle)) moveServo(angle);
    else if (upper.startsWith("ANGLE ") && parseAngle(upper.substring(6), angle)) moveServo(angle);
    else sendResponse("ERR UNKNOWN_COMMAND");
  }
}

void readSerial() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') { handleCommand(serialLine); serialLine = ""; }
    else if (serialLine.length() < 255) serialLine += c;
  }
}

void readTcp() {
  if (!wifiServicesStarted) return;
  if (!commandClient || !commandClient.connected()) {
    if (commandClient) stopDrive();
    if (commandClient) commandClient.stop();
    WiFiClient incoming = commandServer.available();
    if (incoming) {
      commandClient = incoming;
      commandClient.setNoDelay(true);
      tcpLine = "";
      sendResponse("PHONEPILOT_ESP32_READY v0.4-arduino");
      printServoStatus();
      printWifiStatus();
    }
  }
  while (commandClient && commandClient.connected() && commandClient.available()) {
    const char c = static_cast<char>(commandClient.read());
    if (c == '\r') continue;
    if (c == '\n') { handleCommand(tcpLine, true); tcpLine = ""; }
    else if (tcpLine.length() < 255) tcpLine += c;
  }
}

void updateBootButton() {
  if (digitalRead(0) == LOW) {
    if (!bootPressedAt) bootPressedAt = millis();
    if (!bootHoldHandled && millis() - bootPressedAt >= kBootHoldMs) {
      bootHoldHandled = true;
      startPairingWindow();
      startSetupAp();
    }
  } else {
    bootPressedAt = 0;
    bootHoldHandled = false;
  }
}
}  // namespace

void setup() {
  bootAt = millis();
  Serial.begin(SERIAL_BAUD);
  const int motorPins[] = {MOTOR_ENA_PIN, MOTOR_ENB_PIN, MOTOR_IN1_PIN, MOTOR_IN2_PIN, MOTOR_IN3_PIN, MOTOR_IN4_PIN};
  for (int pin : motorPins) { digitalWrite(pin, LOW); pinMode(pin, OUTPUT); }
  writeMotorPins(PhonePilotDrive::Direction::Stop);
  pinMode(0, INPUT_PULLUP);
  panServo.setPeriodHertz(50);
  panServo.attach(SERVO_PIN, SERVO_MIN_PULSE_US, SERVO_MAX_PULSE_US);
  servoReady = panServo.attached();
  if (servoReady) panServo.write(currentAngle);
  Serial.println("PHONEPILOT_ESP32_READY v2-arduino");
  printServoStatus();
  loadSettings();
  WiFi.onEvent([](WiFiEvent_t event) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) gotIpAfterAttempt = true;
  });
  startBle();
  if (!hasProfile(saved, saved.mode) && strcmp(saved.mode, "USB")) startSetupAp();
}

void loop() {
  updateDrive();
  readSerial();
  updateBootButton();
  if (!startupApplied && PhonePilotProtocol::startupFallbackDue(
          millis() - bootAt, bleConnected && bleAuthorized, millis() - bleConnectedAt))
    applySavedMode();
  BleCommand command;
  while (bleCommands && xQueueReceive(bleCommands, &command, 0) == pdTRUE)
    if (bleConnected && bleAuthorized) handleBleCommand(command.json, command.length);
  updateWifi();
  readTcp();
  if (statusDirty) publishStatus();
  delay(2);
}
