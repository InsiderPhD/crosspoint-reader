#pragma once

#include "components/themes/lyra/LyraCarouselTheme.h"

class GfxRenderer;

// Licorice theme metrics (zero runtime cost).
namespace LicoriceMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraCarouselMetrics::values;
  // Classic/Dashboard/Bookshelf's thumbnail height, so no new size has to be
  // primed (see UITheme::getCoverThumbHeights). The bar's small cover is shrunk
  // from it through CoverCache and then drawn 1:1.
  v.homeCoverHeight = 400;
  // No snapshot is taken (drawRecentBookCover never calls storeCoverBuffer):
  // the only artwork is the bar's small cached 1-bit cover, cheaper to re-read
  // each frame than to hold the strip in RAM. The height is only the rect
  // HomeActivity passes to the hit test, which this theme ignores.
  v.homeCoverTileHeight = 380;
  v.homeRecentBooksCount = 1;
  v.homeLiveClock = true;
  v.homeHideButtonHints = true;  // the layout runs to the bottom of the screen
  return v;
}();

#if FREEINK_CAP_TOUCH
// Gesture mode (Full Touch off) reclaims the action-bar strip; see BaseMetrics.
constexpr ThemeMetrics noActionBarValues = [] {
  ThemeMetrics v = values;
  v.buttonHintsHeight = 0;
  return v;
}();
#endif
}  // namespace LicoriceMetrics

// A desk-calendar home, top to bottom:
//
//   [        big clock + date        ]
//   [ This month ]  [     Today      ]
//   [ 2x2 folder ]  [ a clipping     ]
//   [   the book you are reading     ]
//
//  - This month (a square per day, read days filled) IS the Statistics button.
//  - Today is the day's reading against the Daily Goal as a ring, + streak.
//  - The folder holds the other home menu items as square tiles, iOS style.
//  - The clipping is a random one from /My Clippings.txt, re-picked on every
//    visit to Home; it is read straight from the file, one entry, so no
//    clippings store is loaded.
//  - The book bar at the bottom is the one book place (selector index 0).
//
// Notes:
//  - The clock digits are drawn as round-pen strokes (no font in the firmware
//    is anywhere near clock size, and a scaled glyph would be blocky).
//  - Inherits LyraCarouselTheme for its icon set only; the strip, the menu,
//    both hit tests and navigation are this theme's.
//  - Landscape puts the clock and the first card row in a left column and the
//    rest on the right; Home normally stays in portrait.
class LicoriceTheme : public LyraCarouselTheme {
 public:
  const ThemeMetrics& themeMetrics() const override;

  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;

  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;

  // Both hit-test the geometry the last draw recorded.
  int hitTestRecentBookCover(const Rect& rect, int slotCount, int lx, int ly) const override;
  int hitTestButtonMenu(const Rect& rect, int buttonCount, int lx, int ly) const override;

  // Moves around the drawn layout (month card, folder grid, book bar) instead
  // of the linear menu order, which jumps across the screen here.
  int homeNavigate(int selectorIndex, int coverCount, int totalCount, NavDirection dir) const override;

  void onHomeEnter() const override { quoteStale = true; }

  static constexpr int kFolderSlots = 4;
  static constexpr size_t kQuoteMax = 320;
  static constexpr size_t kQuoteSourceMax = 96;

 private:
  // Recorded by drawRecentBookCover.
  mutable Rect bookBar;
  mutable Rect statsCard;
  mutable Rect folderCard;
  // Recorded by drawButtonMenu: Stats' menu index (shown as the month card)
  // and the menu index in each folder tile, row by row; -1 = empty.
  mutable int statsMenuIndex = -1;
  mutable int folderMenuIndex[kFolderSlots] = {-1, -1, -1, -1};
  mutable Rect folderTiles[kFolderSlots];
  // The clipping on show, kept for the visit so redraws do not reshuffle it.
  // Fixed buffers in the theme object (~420 B, allocated once with the theme)
  // rather than strings re-allocated on every pick.
  mutable bool quoteStale = true;
  mutable char quote[kQuoteMax] = {};
  mutable char quoteSource[kQuoteSourceMax] = {};
};
