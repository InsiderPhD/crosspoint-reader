#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

// Bookshelf theme metrics (zero runtime cost).
//
// The strip runs from the header down towards the button grid, and
// drawRecentBookCover lays itself out inside that span against the grid's top.
namespace BookshelfMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  // Reuses Classic/Dashboard's thumbnail height so this theme never asks for a
  // size UITheme::getCoverThumbHeights does not prime (a BookFusion cover at an
  // unprimed height stays blank forever). The hero cover is the only thing
  // drawn near this size; the shelf covers are resampled from the same file.
  v.homeCoverHeight = 400;
  // Snapshot strip. Tall enough to hold the shelf even in X4 Pro gesture mode,
  // where the grid (and so the shelf above it) sits 44 px lower.
  v.homeCoverTileHeight = 600;
  v.homeRecentBooksCount = 6;  // books on the shelf: at most 5 face-out, the rest spine-out
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
}  // namespace BookshelfMetrics

// Home screen modelled on KOReader's bookshelf.koplugin and Apple Books: a
// "Continue Reading" card at the top previews the focused book (large cover,
// title, author, progress), and the recent books stand on a drawn shelf below
// it -- some face-out, some spine-out. Moving the selector along the shelf
// changes what the card previews; the card itself opens the book it shows.
//
// The home menu is split in two:
//  - Menu item 0 (Library) is drawn at the end of the shelf as a small stack
//    of books lying flat, so the selector steps off the last book onto it.
//  - The remaining items are a grid of large buttons: 2x2 in portrait, 4x1 in
//    landscape (two rows would squeeze out the card there).
// This leans on HomeActivity listing Library first; the label still comes from
// the menu callback, so only the position is assumed.
//
// Rendering split, to keep the per-selection cost to one cover read:
//  - The shelf (books + plank) is static, so it is drawn once and captured by
//    the home screen's cover snapshot.
//  - The hero card, the Library stack and the grid are drawn after the
//    snapshot is taken, so each selection change repaints only those.
class BookshelfTheme : public LyraTheme {
 public:
  const ThemeMetrics& themeMetrics() const override;

  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;

  // Draws the Library stack and the button grid on a frame whose shelf was
  // just laid out; any other caller gets LyraTheme's plain list.
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;

  // Shelf books resolve to their own index; the hero card resolves to the book
  // it is previewing, so tapping it after selecting a shelf book opens that
  // book (TouchListNav's second-tap-activates rule). Geometry is recorded at
  // draw time because the hit tests are handed no renderer.
  int hitTestRecentBookCover(const Rect& rect, int slotCount, int lx, int ly) const override;
  int hitTestButtonMenu(const Rect& rect, int buttonCount, int lx, int ly) const override;

  // Up/Down move between the shelf and the grid rows, landing on whatever sits
  // nearest horizontally; Left/Right keep the linear step (along the shelf,
  // then through the grid in reading order).
  int homeNavigate(int selectorIndex, int coverCount, int totalCount, NavDirection dir) const override;
};
