#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// SFM-V1.7: an eight-byte reply, with XOR over bytes 1..5.
// IDs range from 1 to 10000; capacity (100 templates) is a different limit.
namespace FingerprintProtocol {
inline bool validFrame(const uint8_t* b) {
  uint8_t sum = 0;
  for (size_t i = 1; i <= 5; ++i) sum ^= b[i];
  return b[0] == 0xf5 && b[7] == 0xf5 && b[5] == 0 && b[6] == sum;
}
inline bool validId(uint16_t id) { return id > 0 && id <= 10000; }
inline uint16_t id(const uint8_t* b) { return (uint16_t(b[2]) << 8) | b[3]; }
inline uint16_t matchedId(const uint8_t* b) {
  // A match returns a user role (1..3), not ACK_SUCCESS (0).
  return validFrame(b) && b[1] == 0x0c && b[4] >= 1 && b[4] <= 3 && validId(id(b)) ? id(b) : 0;
}
inline uint16_t enrolledId(const uint8_t* b) {
  return validFrame(b) && b[1] == 0x03 && b[4] == 0 && validId(id(b)) ? id(b) : 0;
}

// Only one command may be in flight. No blocking reads: the web page and
// queue uploader stay responsive while the module waits for a finger.
template<class Port> class Transport {
 public:
  explicit Transport(Port& port) : port_(port) {}
  bool busy() const { return busy_; }
  uint8_t command() const { return command_; }
  bool start(uint32_t now, uint8_t command, uint8_t p1 = 0, uint8_t p2 = 0,
             uint8_t p3 = 0, uint32_t timeout = 1000) {
    if (busy_) return false;
    for (int i = 0; i < 256 && port_.available(); ++i) port_.read();
    uint8_t b[8] = {0xf5, command, p1, p2, p3, 0, uint8_t(command ^ p1 ^ p2 ^ p3), 0xf5};
    if (port_.write(b, sizeof(b)) != sizeof(b)) return false;
    command_ = command; since_ = now; timeout_ = timeout; used_ = 0; busy_ = true;
    return true;
  }
  // 0: pending, 1: validated reply, -1: timeout. Output is zero on failure.
  int poll(uint32_t now, uint8_t (&out)[8]) {
    memset(out, 0, sizeof(out));
    if (!busy_) return 0;
    for (int i = 0; i < 256 && port_.available(); ++i) {
      uint8_t b = port_.read();
      if (!used_ && b != 0xf5) continue;
      frame_[used_++] = b;
      if (used_ != sizeof(frame_)) continue;
      bool expected = frame_[1] == command_ ||
        (command_ >= 1 && command_ <= 2 && frame_[1] == 3) ||
        (command_ == 0xfe && frame_[4] == 0x18);
      if (validFrame(frame_) && expected) {
        memcpy(out, frame_, sizeof(out)); busy_ = false; used_ = 0; return 1;
      }
      memmove(frame_, frame_ + 1, sizeof(frame_) - 1); --used_;
    }
    if (uint32_t(now - since_) >= timeout_) { busy_ = false; used_ = 0; return -1; }
    return 0;
  }
  void abandon() { busy_ = false; used_ = 0; }
 private:
  Port& port_;
  bool busy_ = false;
  uint8_t command_ = 0, used_ = 0, frame_[8] = {};
  uint32_t since_ = 0, timeout_ = 0;
};
}
