#include "VoiceFeedback.h"
#include <Arduino.h>
#include <ESP_I2S.h>
#include <math.h>
#include "voice_samples.h"

namespace {
constexpr uint32_t RATE = 16000;
constexpr size_t BLOCK = 256;
I2SClass output;
int16_t frames[BLOCK * 2];
float level = 0.5f;
bool ready = false;

bool writeFrames(size_t count) {
  const uint8_t *data = reinterpret_cast<const uint8_t *>(frames);
  size_t remaining = count * 2 * sizeof(int16_t);
  while (remaining) {
    size_t written = output.write(data, remaining);
    if (!written || written > remaining) return false;
    data += written;
    remaining -= written;
  }
  return true;
}

bool writeSilence(uint32_t milliseconds) {
  memset(frames, 0, sizeof(frames));
  uint32_t remaining = RATE * milliseconds / 1000;
  while (remaining) {
    size_t count = remaining < BLOCK ? remaining : BLOCK;
    if (!writeFrames(count)) return false;
    remaining -= count;
  }
  return true;
}

bool play(const int16_t *samples, size_t length) {
  if (!ready) return false;
  for (size_t pos = 0; pos < length; pos += BLOCK) {
    size_t count = length - pos;
    if (count > BLOCK) count = BLOCK;
    for (size_t i = 0; i < count; ++i) {
      int16_t sample = static_cast<int16_t>(samples[pos + i] * level);
      frames[2 * i] = frames[2 * i + 1] = sample;
    }
    if (!writeFrames(count)) return false;
  }
  // More than the 90 ms DMA queue in tested Arduino-ESP32 3.3.6:
  // speech is consumed before this function returns; queued data is silence.
  return writeSilence(120);
}
}

namespace VoiceFeedback {
bool setVolume(float volume) {
  if (!isfinite(volume) || volume < 0.0f || volume > 1.0f) return false;
  level = volume;
  return true;
}

bool begin(int8_t bclk, int8_t ws, int8_t data, float volume) {
  if (!setVolume(volume)) return false;
  if (ready) return true; // To change pins, call end() first.
  output.setPins(bclk, ws, data);
  if (!output.begin(I2S_MODE_STD, RATE,
                    I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    output.end();
    return false;
  }
  ready = true;
  if (!writeSilence(120)) {
    end();
    return false;
  }
  return true;
}

bool playGranted() {
  return play(access_granted, sizeof(access_granted) / sizeof(access_granted[0]));
}

bool playDenied() {
  return play(access_denied, sizeof(access_denied) / sizeof(access_denied[0]));
}

void end() {
  output.end();
  ready = false;
}
}
