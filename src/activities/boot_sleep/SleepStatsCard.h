#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;

// Renders the sleep-screen reading-stat card: the single rounded box of
// user-selected reading stats shown near the bottom of the sleep screen.
// Extracted from SleepActivity so the settings preview (SleepStatsSettingsActivity)
// draws a byte-identical card without duplicating the layout/formatting logic.
namespace SleepStatsCard {

inline constexpr size_t STAT_LINE_LEN = 64;
inline constexpr uint8_t MAX_STAT_LINES = 4;

// One rendered stat line: either a text string, or (when weekDots is set) a row
// of weekCount weekday cells for the weekly-streak stat (Mon..Sun of the current
// week). weekMask bit d (0 = Monday) is set when that weekday met the daily goal;
// each cell is labelled with its weekday letter, which the fill hides when met.
struct StatLine {
  char text[STAT_LINE_LEN];
  bool weekDots;
  uint8_t weekMask;
  uint8_t weekCount;
};

// Formats the user-selected sleep-screen stats (slots 1-3) into `lines`,
// skipping any slot set to SLEEP_STAT_NONE (and the "if found" slot when no
// contact is set). Most stats are one line; the "if found" stat emits two (a
// label line and the contact on its own line). Returns the number of lines
// filled (0..MAX_STAT_LINES).
//
// When previewMode is true, slots that would normally be skipped because their
// data is unavailable (no book open, clock not set, no contact entered) are
// filled with representative sample values instead, so the settings preview
// always demonstrates every enabled slot rather than showing nothing.
uint8_t buildLines(StatLine lines[MAX_STAT_LINES], bool previewMode = false);

// Draws the selected stats in a single bordered box (white fill + black rounded
// border + black centred lines; a caller's invertScreen() flips it for a dark
// screen). No-op when no stats are selected. Does not trigger a display refresh.
//
// anchorBottomY < 0 anchors the box's bottom edge to the lower ~1/8 margin of the
// screen (the sleep-screen default). Pass an explicit y to place the box's bottom
// edge elsewhere (the settings preview positions it above the button hints).
//
// clearRegionOnly is the greyscale-cover compositing path: the box is painted into
// the BW base image once, then each greyscale plane pass clears the box rectangle
// so displayGrayBuffer() leaves that region as the BW box while the rest of the
// cover renders in greyscale (single refresh, greyscale preserved).
//
// previewMode is forwarded to buildLines (see above).
void draw(GfxRenderer& renderer, bool clearRegionOnly = false, int anchorBottomY = -1, bool previewMode = false);

}  // namespace SleepStatsCard
