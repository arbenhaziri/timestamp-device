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

// IMA ADPCM decoder state for one phrase (see voice_samples.h).
const int16_t STEPS[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
  50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
  253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
  1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
  3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
  11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
  32767};
const int8_t INDEX_ADJUST[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

struct Adpcm {
  int32_t predictor = 0;
  int index = 0;
  int16_t next(uint8_t code) {
    int32_t step = STEPS[index];
    int32_t diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    predictor += (code & 8) ? -diff : diff;
    if (predictor > 32767) predictor = 32767;
    if (predictor < -32768) predictor = -32768;
    index += INDEX_ADJUST[code & 7];
    if (index < 0) index = 0;
    if (index > 88) index = 88;
    return static_cast<int16_t>(predictor);
  }
};

bool play(const uint8_t *data, uint32_t length) {
  if (!ready) return false;
  Adpcm decoder;
  for (uint32_t pos = 0; pos < length; pos += BLOCK) {
    size_t count = length - pos;
    if (count > BLOCK) count = BLOCK;
    for (size_t i = 0; i < count; ++i) {
      uint32_t n = pos + i;
      uint8_t code = (n & 1) ? data[n >> 1] >> 4 : data[n >> 1] & 15;
      int16_t sample = static_cast<int16_t>(decoder.next(code) * level);
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
  return play(access_granted, access_granted_samples);
}

bool playDenied() {
  return play(access_denied, access_denied_samples);
}

void end() {
  output.end();
  ready = false;
}
}
