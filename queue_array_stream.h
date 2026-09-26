#pragma once
#include <cstddef>
#include <cstdint>

// Source must provide read(uint8_t*, size_t) and peek(). The snapshot is a
// validated JSONL file ending in '\n'; later appends are never read.
template <typename Source>
class QueueArrayEncoder {
 public:
  QueueArrayEncoder(Source& source, size_t snapshotBytes)
    : source(source), totalBytes(snapshotBytes ? snapshotBytes + 1 : 2) {}

  int available() const { return failed ? -1 : int(totalBytes - sentBytes); }
  bool complete() const { return !failed && sentBytes == totalBytes; }

  int peek() {
    if (failed || sentBytes == totalBytes) return -1;
    if (sentBytes == 0) return '[';
    if (sentBytes == totalBytes - 1) return ']';
    int b = source.peek();
    if (b < 0) failed = true;
    return b == '\n' ? ',' : b;
  }

  size_t readBytes(char* buffer, size_t length) {
    size_t written = 0;
    while (!failed && written < length && sentBytes < totalBytes) {
      if (sentBytes == 0 || sentBytes == totalBytes - 1) {
        buffer[written++] = sentBytes == 0 ? '[' : ']';
        ++sentBytes;
        continue;
      }
      size_t count = length - written;
      size_t remaining = totalBytes - 1 - sentBytes;
      if (count > remaining) count = remaining;
      size_t got = source.read(reinterpret_cast<uint8_t*>(buffer + written), count);
      if (got != count) { failed = true; break; }
      for (size_t i = 0; i < got; ++i) if (buffer[written + i] == '\n') buffer[written + i] = ',';
      written += got;
      sentBytes += got;
    }
    return written;
  }

 private:
  Source& source;
  const size_t totalBytes;
  size_t sentBytes = 0;
  bool failed = false;
};
