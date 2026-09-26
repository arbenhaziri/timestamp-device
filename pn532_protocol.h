#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Pn532Protocol {
constexpr uint8_t kAddress = 0x24;
constexpr uint8_t kAck[6] = {0, 0, 0xff, 0, 0xff, 0};

inline size_t commandFrame(const uint8_t* command, size_t length, uint8_t* out, size_t capacity) {
  if (!command || !out || !length || length > 32 || capacity < length + 8) return 0;
  out[0] = 0; out[1] = 0; out[2] = 0xff;
  out[3] = uint8_t(length + 1); out[4] = uint8_t(-(length + 1)); out[5] = 0xd4;
  uint8_t sum = 0xd4;
  for (size_t i = 0; i < length; ++i) { out[6 + i] = command[i]; sum += command[i]; }
  out[6 + length] = uint8_t(-sum);
  out[7 + length] = 0;
  return length + 8;
}

inline bool isAck(const uint8_t* wire, size_t length) {
  return wire && length == 7 && (wire[0] & 1) && memcmp(wire + 1, kAck, sizeof(kAck)) == 0;
}

inline int responsePayload(const uint8_t* wire, size_t size, uint8_t command,
                           uint8_t* payload, size_t capacity) {
  if (!wire || size < 10 || !(wire[0] & 1) || wire[1] || wire[2] || wire[3] != 0xff) return -1;
  size_t length = wire[4];
  if (length < 2 || uint8_t(length + wire[5]) != 0 || length + 8 > size ||
      length - 2 > capacity || wire[6] != 0xd5 || wire[7] != uint8_t(command + 1) ||
      wire[length + 7] != 0) return -1;
  uint8_t sum = 0;
  for (size_t i = 6; i <= length + 6; ++i) sum += wire[i];
  if (sum) return -1;
  if (length > 2) memcpy(payload, wire + 8, length - 2);
  return int(length - 2);
}

// Returns 1 for a card, 0 for no card, -1 for a malformed/unsupported reply.
inline int cardUid(const uint8_t* payload, size_t size, uint8_t uid[10], uint8_t& uidLength) {
  uidLength = 0;
  if (!payload || !size) return -1;
  if (payload[0] == 0 && size == 1) return 0;
  if (size < 6 || payload[0] != 1 || payload[1] != 1) return -1;
  uint8_t length = payload[5];
  if ((length != 4 && length != 7 && length != 10) || size < size_t(6 + length)) return -1;
  memcpy(uid, payload + 6, length);
  uidLength = length;
  return 1;
}
}
