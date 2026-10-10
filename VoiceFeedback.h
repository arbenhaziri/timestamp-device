#pragma once
#include <stdint.h>

// One caller/task at a time. Playback is synchronous (about 1.1 seconds).
namespace VoiceFeedback {
// GPIO numbers, not board labels. begin() initializes I2S once.
bool begin(int8_t bclk = 4, int8_t ws = 5, int8_t data = 6, float volume = 0.5f);
bool setVolume(float volume); // Valid range 0..1; amplitude, not watts.
bool playGranted();
bool playDenied();
void end();
}
