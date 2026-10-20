#include "../PhonePilotRobot_ESP32/PhonePilotProtocol.h"
#include <cassert>
#include <cstring>
#include <string>

using namespace PhonePilotProtocol;

int main() {
  FrameDecoder decoder;
  const std::string json = R"({"v":1,"type":"select_mode","requestId":"12345678-1234-1234-1234-123456789abc","mode":"USB"})";
  assert(decoder.push(json.size() & 0xff, 0) == FrameResult::More);
  assert(decoder.push(json.size() >> 8, 1) == FrameResult::More);
  for (size_t i = 0; i < json.size(); ++i) {
    const auto expected = i + 1 == json.size() ? FrameResult::Complete : FrameResult::More;
    assert(decoder.push(static_cast<uint8_t>(json[i]), i + 2) == expected);
  }
  assert(decoder.jsonLength() == json.size());
  assert(std::strcmp(decoder.json(), json.c_str()) == 0);

  // Multiple frames can share a byte stream; a stalled partial frame expires.
  assert(decoder.push(2, 500) == FrameResult::More);
  assert(decoder.push(0, 501) == FrameResult::More);
  assert(decoder.push('x', 502) == FrameResult::More);
  assert(decoder.push(1, 5000) == FrameResult::More);
  assert(decoder.push(0, 5001) == FrameResult::More);
  assert(decoder.push('y', 5002) == FrameResult::Complete);
  assert(std::strcmp(decoder.json(), "y") == 0);

  assert(decoder.push(1, 6000) == FrameResult::More);
  assert(decoder.push(2, 6001) == FrameResult::Invalid);  // 513 > 512
  assert(decoder.push(0, 6002) == FrameResult::More);
  assert(decoder.push(0, 6003) == FrameResult::Invalid);

  assert(validMode("USB") && validMode("SHARED") && validMode("HOTSPOT"));
  assert(!validMode("hotspot") && !validMode("WIFI"));
  assert(validUuid("12345678-1234-1234-1234-123456789abc"));
  assert(!validUuid("1234") && !validUuid("12345678-1234-1234-1234-123456789abz"));
  assert(validCredentials("Workshop WiFi", "password123"));
  assert(validCredentials("\xD0\xA2\xD0\xB5\xD1\x81\xD1\x82", "password123"));
  assert(!validCredentials("", "password123"));
  assert(!validCredentials("ssid", "short"));
  assert(!validCredentials("ssid", "1234567890123456789012345678901234567890123456789012345678901234"));
  assert(!validCredentials("bad\x01ssid", "password123"));
  assert(!validCredentials("\xC3\x28", "password123"));
  assert(!validCredentials("\xE0\x80\x80", "password123"));
  assert(!validCredentials("\xED\xA0\x80", "password123"));
  assert(!validCredentials(std::string(33, 'a').c_str(), "password123"));

  // Boot fallback is postponed only for a phone that connected within 5 seconds.
  assert(!startupFallbackDue(4999, false, 0));
  assert(startupFallbackDue(5000, false, 0));
  assert(!startupFallbackDue(5000, true, 14999));
  assert(startupFallbackDue(5000, true, 15000));

  // Both startup restore and a new profile use the same Wi-Fi outcome gate.
  assert(wifiDecision(false, true, 14999, 14999) == WifiDecision::Wait);
  assert(wifiDecision(false, true, 15000, 15000) == WifiDecision::Timeout);
  assert(wifiDecision(true, true, 100, 100) == WifiDecision::Connected);
  assert(wifiDecision(false, false, 0, 9999) == WifiDecision::Wait);
  assert(wifiDecision(false, false, 0, 10000) == WifiDecision::Retry);

  AckCache acks;
  AckRecord cached;
  assert(!acks.find("12345678-1234-1234-1234-123456789abc", cached));
  acks.remember("12345678-1234-1234-1234-123456789abc", "accepted", nullptr);
  assert(acks.find("12345678-1234-1234-1234-123456789abc", cached));
  assert(std::strcmp(cached.result, "accepted") == 0 && cached.code[0] == 0);
  acks.remember("22345678-1234-1234-1234-123456789abc", "error", "NEED_CREDENTIALS");
  assert(acks.find("22345678-1234-1234-1234-123456789abc", cached));
  assert(std::strcmp(cached.code, "NEED_CREDENTIALS") == 0);
}
