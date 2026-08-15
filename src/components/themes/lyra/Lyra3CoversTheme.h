

#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

namespace Lyra3CoversMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.homeCoverTileHeight = 320;
  v.homeRecentBooksCount = 3;
  return v;
}();
}  // namespace Lyra3CoversMetrics

class Lyra3CoversTheme : public LyraTheme {
 public:
  // Must be overridden here (LyraTheme's returns LyraMetrics): this theme draws
  // three cover tiles and a taller strip, and hitTestRecentBookCover reads
  // homeRecentBooksCount/homeCoverTileHeight from here. Without it every tap in
  // the strip resolved to a single column, i.e. always the first book.
  // LyraLibraryTheme inherits this — it shares these metrics by design.
  const ThemeMetrics& themeMetrics() const override { return Lyra3CoversMetrics::values; }

  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
};
