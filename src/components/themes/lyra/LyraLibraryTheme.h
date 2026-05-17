#pragma once

#include "components/themes/lyra/Lyra3CoversTheme.h"

class GfxRenderer;

// Lyra Library is Lyra 3 Covers with the third tile permanently dedicated to
// a "Library" entry point. The first two tiles host the most recent books;
// the third tile shows a library icon + label and launches LibraryActivity
// when activated. Layout metrics are identical to Lyra3Covers (reuses its
// homeCoverHeight, tile width, etc.) so the existing thumb_226.bmp cache is
// shared between the two themes.
class LyraLibraryTheme : public Lyra3CoversTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;

  int getLibrarySlotIndex() const override { return LIBRARY_SLOT; }

  static constexpr int LIBRARY_SLOT = 2;        // Third tile (0-indexed).
  static constexpr int RECENT_SLOT_COUNT = 2;   // Slots 0..1 host actual books.
};
