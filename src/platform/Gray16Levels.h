#pragma once

// Glyph coverage -> panel level for the Picco's 16-level path
// (GfxRenderer::setGray16Levels). Coverage c/15 stands for that share of ink
// over paper, a mix linear in reflectance; each value gets the driver level
// nearest to that mix in lightness (L*), using the levels' measured L*.

#include <cmath>
#include <cstdint>

namespace gray16 {

// L* of the driver's default levels (0 = white .. 15 = darkest), from photos
// of the reader drawing them on a clean page (picco-gray-cal design-turn.json).
constexpr float LEVEL_LSTAR[16] = {99.9f, 90.3f, 82.4f, 80.1f, 75.3f, 69.7f, 64.6f, 62.1f,
                                   55.5f, 52.9f, 44.6f, 42.0f, 41.6f, 36.9f, 30.1f, 26.5f};

// Levels whose drive path starts with the deep black (Ssd2677Gray16Luts.h
// default paths: levels 5, 9, 13, 15). The 16-level text base draws these
// black, so the page reads before the gray passes finish it.
constexpr uint16_t DEEP_LEVELS = (1u << 5) | (1u << 9) | (1u << 13) | (1u << 15);
// The lightest deep level (deep black, then the largest pull-back): the
// driver pulls it back inside the page turn (HalDisplay::beginGray16Text's
// `soft` plane), so it is never black for long.
constexpr uint8_t SOFT_DEEP_LEVEL = 5;
// Frames of that in-turn pull-back. Straight after the deep black the pull
// is steep: 3 frames left the level at L* ~49, 6 at ~84 (two photos of
// CMD:TEXT16GRID, 2026-11-18); 5 lands it near 72.
constexpr uint8_t TEXT_SOFT_PULL = 5;

// L* of each level as the 16-level text turn draws it (cross-dissolve from a
// text page, then the passes): the same two photos, scaled to one another and
// averaged; level 5 estimated for TEXT_SOFT_PULL. 6-10 L* darker than on a
// clean page (LEVEL_LSTAR): page history.
constexpr float TEXT_LSTAR[16] = {99.1f, 82.5f, 75.9f, 74.8f, 68.5f, 72.0f, 61.6f, 59.8f,
                                  52.5f, 50.7f, 45.5f, 43.3f, 42.0f, 37.3f, 35.6f, 29.7f};

inline float lstarToY(float L) {
  const float f = (L + 16.0f) / 116.0f;
  return f > 6.0f / 29.0f ? f * f * f : 3.0f * (6.0f / 29.0f) * (6.0f / 29.0f) * (f - 4.0f / 29.0f);
}

inline float yToLstar(float Y) {
  const float f = Y > 0.008856f ? cbrtf(Y) : Y / (3.0f * (6.0f / 29.0f) * (6.0f / 29.0f)) + 4.0f / 29.0f;
  return 116.0f * f - 16.0f;
}

// out[c] = level for 4-bit coverage c (0 = no ink .. 15 = full ink).
inline void coverageLevels(uint8_t out[16], const float lstar[16] = LEVEL_LSTAR) {
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
// the ink/paper mix, among the deep-path levels from TEXT_BASE_COVERAGE up and
// the others below it, so the base (the deep levels, black) reads on its own.
inline void textCoverageLevels(uint8_t out[16], const float lstar[16] = LEVEL_LSTAR) {
  const float yWhite = lstarToY(lstar[0]);
  const float yInk = lstarToY(lstar[15]);
  for (int c = 0; c < 16; ++c) {
    const float target = yToLstar(yWhite - (yWhite - yInk) * (c / 15.0f));
    const bool deep = c >= TEXT_BASE_COVERAGE;
    int best = -1;
    for (int l = 0; l < 16; ++l) {
      if (((DEEP_LEVELS >> l) & 1) != deep) continue;
      if (best < 0 || std::fabs(lstar[l] - target) < std::fabs(lstar[best] - target)) best = l;
    }
    out[c] = static_cast<uint8_t>(best);
  }
}

// The same for 8-bit coverage (TrueType glyphs kept at 8 bpp): out[c] for
// c = 0..255, the base split at the same coverage (TEXT_BASE_COVERAGE / 15).
inline void textCoverageLevels8(uint8_t out[256], const float lstar[16] = LEVEL_LSTAR) {
  const float yWhite = lstarToY(lstar[0]);
  const float yInk = lstarToY(lstar[15]);
  for (int c = 0; c < 256; ++c) {
    const float target = yToLstar(yWhite - (yWhite - yInk) * (c / 255.0f));
    const bool deep = c * 15 >= TEXT_BASE_COVERAGE * 255;
    int best = -1;
    for (int l = 0; l < 16; ++l) {
      if (((DEEP_LEVELS >> l) & 1) != deep) continue;
      if (best < 0 || std::fabs(lstar[l] - target) < std::fabs(lstar[best] - target)) best = l;
    }
    out[c] = static_cast<uint8_t>(best);
  }
}

}  // namespace gray16
