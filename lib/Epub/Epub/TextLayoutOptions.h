#pragma once

#include <cstdint>

// User text-layout controls that reach the EPUB layout engine as one value, so adding a
// control does not grow every Section / parser signature again. Every field changes where
// words land, so all of them are written into the section cache header and compared on load
// (Section.cpp): a changed value rebuilds the section instead of drawing a stale layout.
//
// The defaults are "no change": a default-constructed value lays text out exactly as the
// engine did before these controls existed.
struct TextLayoutOptions {
  // Extra letter spacing in half-pixel steps, negative tightens. See GfxRenderer::drawText.
  int8_t characterSpacing = 0;
  // Width of the gap between words, as a percentage of the font's own space advance.
  uint8_t wordSpacingPercent = 100;
  // Reserved in the cache header for the paragraph-indent control; always 0 for now.
  uint8_t paragraphIndent = 0;

  // An inter-word space advance scaled by wordSpacingPercent. Layout-time only (once per
  // word gap, never per glyph or per pixel), and the divisor is a constant.
  [[nodiscard]] constexpr int scaleSpace(const int advance) const {
    return wordSpacingPercent == 100 ? advance : (advance * wordSpacingPercent + 50) / 100;
  }
};
