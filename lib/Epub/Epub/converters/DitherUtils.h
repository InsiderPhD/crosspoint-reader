#pragma once

#include <stdint.h>

// 4x4 Bayer matrix for ordered dithering
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Apply Bayer dithering and quantize to 4 levels (0-3)
// Stateless - works correctly with any pixel processing order
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) {
  int bayer = bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 5;  // Scale to +/-40 (half of quantization step 85)

  int adjusted = gray + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  if (adjusted < 64) return 0;
  if (adjusted < 128) return 1;
  if (adjusted < 192) return 2;
  return 3;
}

// Bayer-dithered 4-bit ink level (0 white .. 15 black) for the 16-level planes'
// .px4 cache: floor(ink * 15 / 255 + (t + 0.5) / 16) with t the 0..15 matrix
// entry, in 1/4080ths so it stays integer. A flat tone lands on its level; a
// tone between two levels mixes them in the Bayer pattern.
inline uint8_t applyBayerDither16Level(uint8_t gray, int x, int y) {
  const int ink = 255 - gray;
  const int t = bayer4x4[y & 3][x & 3];
  const int level = (ink * 240 + (2 * t + 1) * 128) / 4080;
  return level > 15 ? 15 : static_cast<uint8_t>(level);
}

// A decoded pixel's cache value: the 2-bit tone (0 black .. 3 white) of the
// .pxc, or with levels4 the 4-bit ink level of the .px4.
inline uint8_t quantizeImagePixel(uint8_t gray, int x, int y, bool useDithering, bool levels4) {
  if (levels4) {
    return useDithering ? applyBayerDither16Level(gray, x, y) : static_cast<uint8_t>(((255 - gray) * 15 + 127) / 255);
  }
  if (useDithering) return applyBayerDither4Level(gray, x, y);
  const uint8_t tone = gray / 85;
  return tone > 3 ? 3 : tone;
}
