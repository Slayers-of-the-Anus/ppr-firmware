#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace PhonePilotProtocol {
constexpr size_t kMaxJson = 512;
constexpr uint32_t kFrameTimeoutMs = 3000;

enum class FrameResult { More, Complete, Invalid };
enum class WifiDecision { Wait, Connected, Timeout, Retry };

struct AckRecord {
  char id[37] = {};
  char result[9] = {};
  char code[24] = {};
};

class AckCache {
 public:
  bool find(const char* id, AckRecord& out) const {
    for (const AckRecord& record : records_) {
      if (record.id[0] && !strcmp(record.id, id)) {
        out = record;
        return true;
      }
    }
    return false;
  }
  void remember(const char* id, const char* result, const char* code) {
    AckRecord& record = records_[next_];
    copy(record.id, id);
    copy(record.result, result);
    copy(record.code, code ? code : "");
    next_ = (next_ + 1) % 8;
  }

 private:
  template <size_t N>
  static void copy(char (&destination)[N], const char* source) {
    const size_t length = strlen(source) < N - 1 ? strlen(source) : N - 1;
    memcpy(destination, source, length);
    destination[length] = 0;
  }
  AckRecord records_[8] = {};
  size_t next_ = 0;
};

inline bool startupFallbackDue(uint32_t bootElapsed, bool phoneConnected, uint32_t connectedElapsed) {
  return bootElapsed >= 5000 && (!phoneConnected || connectedElapsed >= 15000);
}

inline WifiDecision wifiDecision(bool confirmedIpOnSelectedSsid, bool connecting,
                                uint32_t attemptElapsed, uint32_t retryElapsed) {
  if (confirmedIpOnSelectedSsid) return WifiDecision::Connected;
  if (connecting && attemptElapsed >= 15000) return WifiDecision::Timeout;
  if (!connecting && retryElapsed >= 10000) return WifiDecision::Retry;
  return WifiDecision::Wait;
}

class FrameDecoder {
 public:
  FrameResult push(uint8_t byte, uint32_t now) {
    if (used_ && static_cast<uint32_t>(now - lastByteAt_) > kFrameTimeoutMs) reset();
    lastByteAt_ = now;
    if (used_ < 2) {
      header_[used_++] = byte;
      if (used_ == 2) {
        expected_ = static_cast<uint16_t>(header_[0] | (header_[1] << 8));
        if (!expected_ || expected_ > kMaxJson) {
          reset();
          return FrameResult::Invalid;
        }
      }
      return FrameResult::More;
    }
    json_[used_++ - 2] = static_cast<char>(byte);
    if (used_ == expected_ + 2) {
      json_[expected_] = '\0';
      used_ = 0;
      return FrameResult::Complete;
    }
    return FrameResult::More;
  }

  const char* json() const { return json_; }
  size_t jsonLength() const { return expected_; }
  void reset() { used_ = 0; expected_ = 0; }

 private:
  uint8_t header_[2] = {};
  char json_[kMaxJson + 1] = {};
  uint16_t expected_ = 0;
  uint16_t used_ = 0;
  uint32_t lastByteAt_ = 0;
};

inline bool validMode(const char* mode) {
  return mode && (!strcmp(mode, "USB") || !strcmp(mode, "SHARED") || !strcmp(mode, "HOTSPOT"));
}

inline bool validUuid(const char* value) {
  if (!value || strlen(value) != 36) return false;
  for (size_t i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
    } else if (!((value[i] >= '0' && value[i] <= '9') ||
                 (value[i] >= 'a' && value[i] <= 'f') ||
                 (value[i] >= 'A' && value[i] <= 'F'))) return false;
  }
  return true;
}

inline bool validCredentials(const char* ssid, const char* password) {
  if (!ssid || !password) return false;
  const size_t ssidLen = strlen(ssid);
  const size_t passLen = strlen(password);
  if (ssidLen < 1 || ssidLen > 32 || passLen < 8 || passLen > 63) return false;
  for (size_t i = 0; i < ssidLen; ++i) {
    const unsigned char c = static_cast<unsigned char>(ssid[i]);
    if (c < 0x20 || c == 0x7f) return false;
    if (c < 0x80) continue;
    size_t continuation = (c >= 0xc2 && c <= 0xdf) ? 1 :
                          (c >= 0xe0 && c <= 0xef) ? 2 :
                          (c >= 0xf0 && c <= 0xf4) ? 3 : 0;
    if (!continuation || i + continuation >= ssidLen) return false;
    const unsigned char firstNext = static_cast<unsigned char>(ssid[i + 1]);
    if ((c == 0xe0 && firstNext < 0xa0) || (c == 0xed && firstNext > 0x9f) ||
        (c == 0xf0 && firstNext < 0x90) || (c == 0xf4 && firstNext > 0x8f)) return false;
    while (continuation--) {
      if ((static_cast<unsigned char>(ssid[++i]) & 0xc0) != 0x80) return false;
    }
  }
  return true;
}
}  // namespace PhonePilotProtocol
