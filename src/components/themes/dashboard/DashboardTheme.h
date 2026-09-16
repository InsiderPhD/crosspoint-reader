#pragma once

#include "components/themes/lyra/LyraCarouselTheme.h"

class GfxRenderer;

// Dashboard theme metrics (zero runtime cost).
//
// The strip runs from the header to just above the icon menu: the dashboard
// owns the whole page, with the cover, the stats column, the title block and
// the footer badges all laid out inside it.
namespace DashboardMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraCarouselMetrics::values;
  // Deliberately the same thumbnail cache key Classic already uses, so the
  // dashboard never asks for a size no other theme primes. A cover source that
  // can't be re-read later (the BookFusion API) only writes the heights in
  // UITheme::getCoverThumbHeights, and a height missing from that list leaves
  // those books blank forever.
  v.homeCoverHeight = 400;
  v.homeCoverTileHeight = 600;  // reserved strip; drawRecentBookCover sizes itself within it
  v.homeRecentBooksCount = 1;   // one book, with its stats
  return v;
}();

#if FREEINK_DEVICE_X4PRO
// Gesture mode (Full Touch off) reclaims the action-bar strip; see BaseMetrics.
constexpr ThemeMetrics noActionBarValues = [] {
  ThemeMetrics v = values;
  v.buttonHintsHeight = 0;
  return v;
}();
#endif

// Largest cover box, at the 2:3 the stats column is sized against. The drawn
// box shrinks from here when the screen is short (landscape, X4 Pro gesture
// mode); it never grows, so homeCoverHeight above stays the binding thumbnail
// size.
constexpr int kMaxCoverHeight = 400;
constexpr int kMaxCoverWidth = (kMaxCoverHeight * 2) / 3;
}  // namespace DashboardMetrics

// Stats-forward home screen ported from CrossInk's Dashboard theme: the current
// book's cover on the left, a column of per-book reading stats down the right,
// title and chapter underneath, and a streak / reader-type footer.
//
// Deviations from the CrossInk theme, all deliberate:
//  - The stats come from this fork's ReadingStatsStore rather than CrossInk's
//    per-book BookReadingStats / GlobalReadingStats binary files. The theme
//    reads the store itself, so no other theme's drawRecentBookCover signature
//    had to change.
//  - Pages/min comes from SETTINGS.readingSpeedSecondsPerPage (the reader's
//    lifetime EMA), because this fork does not count per-book page turns.
//  - The date rows key off a valid wall clock, not off an RTC being present:
//    the X4 has no RTC but does get the date from NTP (see TimeUtils).
//  - Inherits LyraCarouselTheme only for its bottom icon menu, which is what
//    frees the page height this layout needs. None of the carousel's cover
//    geometry survives -- drawRecentBookCover and its hit test are replaced.
//  - No sleep screen variant; this fork's SleepActivity owns that surface.
class DashboardTheme : public LyraCarouselTheme {
 public:
  const ThemeMetrics& themeMetrics() const override;

  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;

  // One cover, and it is the only thing in the strip that opens a book, so the
  // default whole-strip-is-slot-0 rule applies. The carousel's override
  // hit-tests geometry this theme never draws, so it has to be undone.
  int hitTestRecentBookCover(const Rect& rect, int slotCount, int lx, int ly) const override;
};
