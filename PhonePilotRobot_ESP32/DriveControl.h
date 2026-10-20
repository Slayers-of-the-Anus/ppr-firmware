#pragma once
#include <stdint.h>

namespace PhonePilotDrive {
enum class Direction { Stop, Forward, Backward };
constexpr uint32_t kWatchdogMs = 700;
constexpr uint32_t kReversePauseMs = 100;

// Pure state machine: no motor is enabled during a reversal or after lease expiry.
class Control {
 public:
  void command(Direction next, uint32_t now) {
    lastCommandAt_ = now;
    if (next == Direction::Stop) {
      if (output_ != Direction::Stop) stoppedAt_ = now;
      target_ = output_ = Direction::Stop;
      reversing_ = false;
      return;
    }
    if ((output_ != Direction::Stop && output_ != next) ||
        (output_ == Direction::Stop && lastMoving_ != Direction::Stop && lastMoving_ != next &&
         static_cast<uint32_t>(now - stoppedAt_) < kReversePauseMs)) {
      if (output_ != Direction::Stop) stoppedAt_ = now;
      output_ = Direction::Stop;
      reversing_ = true;
    }
    target_ = next;
    if (!reversing_) output_ = lastMoving_ = next;
  }
  Direction update(uint32_t now) {
    if (target_ != Direction::Stop && static_cast<uint32_t>(now - lastCommandAt_) >= kWatchdogMs) {
      command(Direction::Stop, now);
    } else if (reversing_ && static_cast<uint32_t>(now - stoppedAt_) >= kReversePauseMs) {
      reversing_ = false;
      output_ = lastMoving_ = target_;
    }
    return output_;
  }
 private:
  Direction target_ = Direction::Stop;
  Direction output_ = Direction::Stop;
  Direction lastMoving_ = Direction::Stop;
  uint32_t lastCommandAt_ = 0;
  uint32_t stoppedAt_ = 0;
  bool reversing_ = false;
};
}  // namespace PhonePilotDrive
