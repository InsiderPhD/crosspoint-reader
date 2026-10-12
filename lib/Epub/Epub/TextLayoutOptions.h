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
  // First-line paragraph indent. PARAGRAPH_INDENT_BOOK keeps the engine's own rules (the
  // book's CSS text-indent, else an em-space, both dropped while extra paragraph spacing is
  // on). Any other value is the user's choice and replaces a missing or positive CSS indent
  // whatever the paragraph-spacing setting: PARAGRAPH_INDENT_NONE removes it, larger values
  // indent by (value - 1) word-space advances. See ParsedText::resolveFirstLineIndent.
  static constexpr uint8_t PARAGRAPH_INDENT_BOOK = 0;
  static constexpr uint8_t PARAGRAPH_INDENT_NONE = 1;
  static constexpr uint8_t PARAGRAPH_INDENT_COUNT = 6;  // book, none, then 1..4 spaces
  uint8_t paragraphIndent = PARAGRAPH_INDENT_BOOK;

  [[nodiscard]] constexpr bool overridesParagraphIndent() const { return paragraphIndent != PARAGRAPH_INDENT_BOOK; }
  // Width of the user's indent in word-space advances; 0 for "book" and "none".
  [[nodiscard]] constexpr int paragraphIndentSpaces() const {
    return paragraphIndent > PARAGRAPH_INDENT_NONE ? paragraphIndent - PARAGRAPH_INDENT_NONE : 0;
  }

  // An inter-word space advance scaled by wordSpacingPercent. Layout-time only (once per
  // word gap, never per glyph or per pixel), and the divisor is a constant.
  [[nodiscard]] constexpr int scaleSpace(const int advance) const {
    return wordSpacingPercent == 100 ? advance : (advance * wordSpacingPercent + 50) / 100;
  }
};
