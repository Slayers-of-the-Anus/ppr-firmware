#include "../PhonePilotRobot_ESP32/DriveControl.h"
#include <cassert>
#include <stdint.h>
using namespace PhonePilotDrive;
int main() {
  Control c;
  assert(c.update(0) == Direction::Stop);
  c.command(Direction::Forward, 10);
  assert(c.update(10) == Direction::Forward);
  c.command(Direction::Forward, 600);
  assert(c.update(1299) == Direction::Forward);
  assert(c.update(1300) == Direction::Stop);
  c.command(Direction::Forward, 1400);
  c.command(Direction::Backward, 1450);
  assert(c.update(1450) == Direction::Stop);
  assert(c.update(1549) == Direction::Stop);
  assert(c.update(1550) == Direction::Backward);
  c.command(Direction::Stop, 1600);
  assert(c.update(1600) == Direction::Stop);
  assert(c.update(3000) == Direction::Stop);
  // STOP during a reversal cancels the pending movement.
  c.command(Direction::Forward, 3100);
  c.command(Direction::Backward, 3150);
  c.command(Direction::Stop, 3160);
  assert(c.update(3300) == Direction::Stop);
  c.command(Direction::Forward, 3400);
  c.command(Direction::Stop, 3410);
  c.command(Direction::Backward, 3420);
  assert(c.update(3509) == Direction::Stop);
  assert(c.update(3510) == Direction::Backward);
  c.command(Direction::Stop, 3600);
  // Millis rollover must not disable the watchdog.
  c.command(Direction::Forward, UINT32_MAX - 100);
  assert(c.update(598) == Direction::Forward);
  assert(c.update(599) == Direction::Stop);
}
