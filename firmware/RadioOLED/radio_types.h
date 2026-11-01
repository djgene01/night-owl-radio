// Types used in function signatures in RadioOLED.ino. They live here because
// the Arduino builder puts its generated prototypes above anything declared in
// the .ino itself.
#pragma once
#include <Arduino.h>

// one biquad section, normalised so a0 == 1
struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
struct BiquadState { float z1 = 0, z2 = 0; };

// what the display task copies out of the shared state each frame
struct UiSnapshot {
  String station, artist, song, status;
  uint32_t titleSeq;
};
