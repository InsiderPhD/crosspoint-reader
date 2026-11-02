#pragma once

#include <cstdint>

class GfxRenderer;

// The Reading Stats "Rhythm" tab (ported from YACP's Reading Rhythm page, on
// top of our own ReadingStatsStore day buckets): a 13-week dot calendar, the
// same 13 weeks as weekly reading-time bars, and reading days per month over
// the last 12 months. Stateless and allocation-free; the tab owns the scroll.
namespace ReadingRhythmView {

// Least height the three stacked cards need. Fixed, so the tab can size its
// scroll range before drawing.
int contentHeight();

// Draws the cards with their top-left at (x, y). Any availableHeight beyond
// contentHeight() makes the two bar charts taller. referenceDayOrdinal is the
// last day shown (must be non-zero); markToday rings it as today.
void draw(GfxRenderer& renderer, int x, int y, int width, int availableHeight, uint32_t referenceDayOrdinal,
          bool markToday);

}  // namespace ReadingRhythmView
