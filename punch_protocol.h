#pragma once
#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace PunchProtocol {
inline bool validEventType(int type) { return type == 0 || type == 1; }

inline bool formatUid(const uint8_t* bytes, size_t count, char* out, size_t capacity) {
  if (!bytes || !out || (count != 4 && count != 7 && count != 10) || capacity < count * 2 + 1) return false;
  for (size_t i = 0; i < count; ++i) snprintf(out + i * 2, capacity - i * 2, "%02X", bytes[i]);
  return true;
}

inline bool formatUidDecimal(const uint8_t* bytes, size_t count, bool reverse, char* out, size_t capacity) {
  if (!bytes || !out || (count != 4 && count != 7 && count != 10)) return false;
  // A ten-byte UID can exceed uint64_t, so convert base 256 to base 10
  // without truncating any card bits. digits[] is stored least-first.
  char digits[25] = {'0'};
  size_t length = 1;
  for (size_t i = 0; i < count; ++i) {
    unsigned carry = bytes[reverse ? count - 1 - i : i];
    for (size_t j = 0; j < length; ++j) {
      unsigned value = unsigned(digits[j] - '0') * 256 + carry;
      digits[j] = char('0' + value % 10);
      carry = value / 10;
    }
    while (carry) {
      if (length == sizeof(digits)) return false;
      digits[length++] = char('0' + carry % 10);
      carry /= 10;
    }
  }
  if (capacity <= length) return false;
  for (size_t i = 0; i < length; ++i) out[i] = digits[length - 1 - i];
  out[length] = '\0';
  return true;
}

inline bool formatTimestamp(time_t seconds, long microseconds, char* out, size_t capacity) {
  if (seconds < 1700000000 || microseconds < 0 || microseconds >= 1000000 || capacity < 25) return false;
  struct tm utc;
  if (!gmtime_r(&seconds, &utc) || utc.tm_year > 8099) return false;
  char date[20];
  if (strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc) != 19) return false;
  return snprintf(out, capacity, "%s.%03ldZ", date, microseconds / 1000) == 24;
}

inline bool validTimestamp(const char* ts) {
  if (!ts || strlen(ts) != 24) return false;
  for (int i = 0; i < 24; ++i) {
    char separator = i == 4 || i == 7 ? '-' : i == 10 ? 'T' : i == 13 || i == 16 ? ':' : i == 19 ? '.' : i == 23 ? 'Z' : 0;
    if (separator ? ts[i] != separator : ts[i] < '0' || ts[i] > '9') return false;
  }
  return true;
}

inline void setPayload(JsonDocument& d, const char* ref, const char* device, int type, const char* timestamp) {
  d.clear();
  d["cardRefCode"] = ref;
  d["deviceCode"] = device;
  d["eventType"] = type;
  d["eventTimestamp"] = timestamp;
}

inline bool validPayload(const JsonDocument& d) {
  if (!d.is<JsonObjectConst>() || d.size() != 4 || !d["cardRefCode"].is<const char*>() ||
      !d["deviceCode"].is<const char*>() || !d["eventType"].is<int>() ||
      !d["eventTimestamp"].is<const char*>()) return false;
  const char* ref = d["cardRefCode"].as<const char*>();
  const char* device = d["deviceCode"].as<const char*>();
  return strlen(ref) > 0 && strlen(ref) <= 25 && strlen(device) > 0 && strlen(device) <= 32 &&
         validEventType(d["eventType"].as<int>()) && validTimestamp(d["eventTimestamp"].as<const char*>());
}

// The endpoint must acknowledge the entire array. Errors and redirects retain it.
inline bool acceptedStatus(int status) { return status == 200; }
}
