#pragma once

// Glyph coverage -> panel level for the Picco's 16-level path
// (GfxRenderer::setGray16Levels). Coverage c/15 stands for that share of ink
// over paper, a mix linear in reflectance; each value gets the driver level
// nearest to that mix in lightness (L*), using the levels' measured L*.

#include <cmath>
#include <cstdint>

namespace gray16 {

// The levels' tones and which of them start with the deep black come from the
// display driver at runtime (HalDisplay::gray16LevelLstar, gray16DeepLevels,
// gray16TextSoftLevel): they follow the paths in force, which have changed
// (jpirnay's re-spread paths replaced picco-gray-cal's design set, whose levels
// 8-15 sat within 7 L* of black). Only the soft pull-back's frame count stays here.

// Frames of the in-turn pull-back for paths that have a soft level (deep black,
// then the largest pull-back). Straight after the deep black the pull is steep:
// 3 frames left the design paths' level at L* ~49, 6 at ~84 (CMD:TEXT16GRID,
// 2026-11-18); 5 lands it near 72.
constexpr uint8_t TEXT_SOFT_PULL = 5;

inline float lstarToY(float L) {
  const float f = (L + 16.0f) / 116.0f;
  return f > 6.0f / 29.0f ? f * f * f : 3.0f * (6.0f / 29.0f) * (6.0f / 29.0f) * (f - 4.0f / 29.0f);
}

inline float yToLstar(float Y) {
  const float f = Y > 0.008856f ? cbrtf(Y) : Y / (3.0f * (6.0f / 29.0f) * (6.0f / 29.0f)) + 4.0f / 29.0f;
  return 116.0f * f - 16.0f;
}

// out[c] = level for 4-bit coverage c (0 = no ink .. 15 = full ink).
inline void coverageLevels(uint8_t out[16], const float lstar[16]) {
  const float yWhite = lstarToY(lstar[0]);
  const float yInk = lstarToY(lstar[15]);
  for (int c = 0; c < 16; ++c) {
    const float target = yToLstar(yWhite - (yWhite - yInk) * (c / 15.0f));
    uint8_t best = 0;
    for (uint8_t l = 1; l < 16; ++l)
      if (std::fabs(lstar[l] - target) < std::fabs(lstar[best] - target)) best = l;
    out[c] = best;
  }
}

// Coverage at or above this starts black in the 16-level text base, as on a
// B/W page.
constexpr int TEXT_BASE_COVERAGE = 8;

// out[c] = level for 4-bit coverage c in 16-level text: the nearest in L* to
// the ink/paper mix, among the deep-path levels (deepLevels bits) from
// TEXT_BASE_COVERAGE up and the others below it, so the base (the deep levels,
// black) reads on its own.
inline void textCoverageLevels(uint8_t out[16], const float lstar[16], const uint16_t deepLevels) {
  const float yWhite = lstarToY(lstar[0]);
  const float yInk = lstarToY(lstar[15]);
  for (int c = 0; c < 16; ++c) {
    const float target = yToLstar(yWhite - (yWhite - yInk) * (c / 15.0f));
    const bool deep = c >= TEXT_BASE_COVERAGE;
    int best = -1;
    for (int l = 0; l < 16; ++l) {
      if (((deepLevels >> l) & 1) != deep) continue;
      if (best < 0 || std::fabs(lstar[l] - target) < std::fabs(lstar[best] - target)) best = l;
    }
    out[c] = static_cast<uint8_t>(best);
  }
}

// The same for 8-bit coverage (TrueType glyphs kept at 8 bpp): out[c] for
// c = 0..255, the base split at the same coverage (TEXT_BASE_COVERAGE / 15).
inline void textCoverageLevels8(uint8_t out[256], const float lstar[16], const uint16_t deepLevels) {
  const float yWhite = lstarToY(lstar[0]);
  const float yInk = lstarToY(lstar[15]);
  for (int c = 0; c < 256; ++c) {
    const float target = yToLstar(yWhite - (yWhite - yInk) * (c / 255.0f));
    const bool deep = c * 15 >= TEXT_BASE_COVERAGE * 255;
    int best = -1;
    for (int l = 0; l < 16; ++l) {
      if (((deepLevels >> l) & 1) != deep) continue;
      if (best < 0 || std::fabs(lstar[l] - target) < std::fabs(lstar[best] - target)) best = l;
    }
    out[c] = static_cast<uint8_t>(best);
  }
}

}  // namespace gray16
