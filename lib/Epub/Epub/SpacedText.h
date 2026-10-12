#pragma once

#include <EpdFontFamily.h>

#include "TextLayoutOptions.h"

class GfxRenderer;

// Plain single-style text laid out with the user's Word Spacing and Character Spacing, for
// the two readers of body-sized text that do not go through ParsedText: the on-page footnote
// area and the TXT reader. Both draw a whole line with one drawText() call, which can carry
// letter spacing but not a scaled word gap, so a non-neutral setting needs the line walked
// run by run.
//
// Gaps follow ParsedText's gapBetween(): every ' ' advances by the font's space advance
// (kerned against its neighbours) scaled by wordSpacingPercent, and each run of non-space
// text is measured and drawn with the letter spacing. advance() and draw() share one walker,
// so a line is drawn exactly as wide as it was measured.
//
// Callers keep their own drawText / width call while TextLayoutOptions::spacingIsNeutral():
// that path stays byte-for-byte what it was before these controls existed. No heap: runs are
// copied through a small stack buffer (a run longer than the buffer is cut at a codepoint
// boundary, which costs one kerning pair at the cut, identically in both functions).
namespace SpacedText {

int advance(const GfxRenderer& renderer, int fontId, const char* text, const TextLayoutOptions& layout,
            EpdFontFamily::Style style = EpdFontFamily::REGULAR);

void draw(const GfxRenderer& renderer, int fontId, int x, int y, const char* text, const TextLayoutOptions& layout,
          bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR);

}  // namespace SpacedText
