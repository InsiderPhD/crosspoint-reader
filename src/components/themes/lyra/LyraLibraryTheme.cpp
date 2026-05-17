#include "LyraLibraryTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cstdint>
#include <string>
#include <vector>

#include "BookFusionBookIdStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "components/icons/library.h"
#include "fontIds.h"

// Tile-internal constants. Match Lyra3CoversTheme's namespace anon constants
// (kept in this file rather than a shared header to avoid leaking implementation
// details across themes).
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
// LibraryIcon in components/icons/library.h is 32x32 — drawing at any other
// size scales the 1-bit bitmap and produces a "broken"-looking result.
constexpr int LIBRARY_ICON_SIZE = 32;
constexpr int LIBRARY_LABEL_GAP = 12;  // vertical pixels between icon and label
}  // namespace

void LyraLibraryTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = (rect.width - 2 * Lyra3CoversMetrics::values.contentSidePadding) / 3;
  const int tileY = rect.y;
  const int coverHeight = Lyra3CoversMetrics::values.homeCoverHeight;

  // First pass: bitmap content for all 3 tiles. This pass writes into the
  // framebuffer and gets captured by storeCoverBuffer() at the end, so that
  // subsequent renders (selector moves) don't re-read covers from SD.
  if (!coverRendered) {
    // Slots 0 and 1: real book covers (if available).
    for (int i = 0; i < RECENT_SLOT_COUNT; i++) {
      const int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;
      const int coverX = tileX + hPaddingInSelection;
      const int coverY = tileY + hPaddingInSelection;
      const int coverDrawW = tileWidth - 2 * hPaddingInSelection;

      bool drewCover = false;
      if (i < static_cast<int>(recentBooks.size())) {
        const std::string& coverPath = recentBooks[i].coverBmpPath;
        if (!coverPath.empty()) {
          const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, coverHeight);
          HalFile file;
          if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              const float bmpW = static_cast<float>(bitmap.getWidth());
              const float bmpH = static_cast<float>(bitmap.getHeight());
              const float bmpRatio = bmpW / bmpH;
              const float tileRatio = static_cast<float>(coverDrawW) / static_cast<float>(coverHeight);
              const float cropX = 1.0f - (tileRatio / bmpRatio);
              renderer.drawBitmap(bitmap, coverX, coverY, coverDrawW, coverHeight, cropX);
              drewCover = true;
            }
            file.close();
          }
        }
      }

      renderer.drawRect(coverX, coverY, coverDrawW, coverHeight, true);
      if (!drewCover) {
        // Empty-slot placeholder: same treatment Lyra3CoversTheme uses when no
        // cover image exists. A solid mid-block + the generic cover icon at
        // top-left. Keeps the tile visually filled.
        renderer.fillRect(coverX, coverY + coverHeight / 3, coverDrawW, 2 * coverHeight / 3, true);
        renderer.drawIcon(CoverIcon, coverX + 24, coverY + 24, 32, 32);
      }
    }

    // Slot 2: library button. Drawn as a stacked icon + label *inside* the
    // cover frame to read as a button. The frame's border remains so the
    // visual rhythm with the adjacent book tiles is preserved.
    {
      const int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * LIBRARY_SLOT;
      const int coverX = tileX + hPaddingInSelection;
      const int coverY = tileY + hPaddingInSelection;
      const int coverDrawW = tileWidth - 2 * hPaddingInSelection;

      renderer.drawRect(coverX, coverY, coverDrawW, coverHeight, true);

      // Measure the label up-front so we can stack icon + label as one unit
      // and center them vertically inside the cover frame.
      const char* label = tr(STR_VIEW_ALL_COVERS);
      const int labelLineH = renderer.getLineHeight(SMALL_FONT_ID);
      const int labelW = renderer.getTextWidth(SMALL_FONT_ID, label);
      const int stackH = LIBRARY_ICON_SIZE + LIBRARY_LABEL_GAP + labelLineH;
      const int stackTop = coverY + (coverHeight - stackH) / 2;

      const int iconX = coverX + (coverDrawW - LIBRARY_ICON_SIZE) / 2;
      renderer.drawIcon(LibraryIcon, iconX, stackTop, LIBRARY_ICON_SIZE, LIBRARY_ICON_SIZE);

      const int labelX = coverX + (coverDrawW - labelW) / 2;
      const int labelY = stackTop + LIBRARY_ICON_SIZE + LIBRARY_LABEL_GAP;
      renderer.drawText(SMALL_FONT_ID, labelX, labelY, label, true);
    }

    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  // Second pass: text + selection box for all 3 tiles. Runs every frame —
  // it's cheap because no SD reads, just text rasterization.
  for (int i = 0; i < 3; i++) {
    const int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;
    const int maxLineWidth = tileWidth - 2 * hPaddingInSelection;
    const bool tileSelected = (selectorIndex == i);

    // Compose the text block for this tile.
    std::vector<std::string> titleLines;
    bool hasProgress = false;
    int progressPercent = 0;

    if (i == LIBRARY_SLOT) {
      titleLines = renderer.wrappedText(SMALL_FONT_ID, tr(STR_VIEW_ALL_COVERS), maxLineWidth, 2);
    } else if (i < static_cast<int>(recentBooks.size())) {
      const bool isBookFusionBook = BookFusionBookIdStore::loadBookId(recentBooks[i].path.c_str()) != 0;
      const std::string displayTitle =
          isBookFusionBook ? std::string("& ") + recentBooks[i].title : recentBooks[i].title;
      titleLines = renderer.wrappedText(SMALL_FONT_ID, displayTitle.c_str(), maxLineWidth, 3);
      hasProgress = recentBooks[i].progressPercent >= 0;
      progressPercent = recentBooks[i].progressPercent;
    } else {
      titleLines = renderer.wrappedText(SMALL_FONT_ID, tr(STR_NO_OPEN_BOOK), maxLineWidth, 2);
    }

    const int titleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int dynamicBlockHeight = (static_cast<int>(titleLines.size()) + (hasProgress ? 1 : 0)) * titleLineHeight;
    const int dynamicTitleBoxHeight = dynamicBlockHeight + hPaddingInSelection + 5;

    if (tileSelected) {
      // Selection-box geometry matches Lyra3CoversTheme so the visual rhythm
      // across themes is identical.
      renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                               Color::LightGray);
      renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection, coverHeight, Color::LightGray);
      renderer.fillRectDither(tileX + tileWidth - hPaddingInSelection, tileY + hPaddingInSelection, hPaddingInSelection,
                              coverHeight, Color::LightGray);
      renderer.fillRoundedRect(tileX, tileY + coverHeight + hPaddingInSelection, tileWidth, dynamicTitleBoxHeight,
                               cornerRadius, false, false, true, true, Color::LightGray);
    }

    int currentY = tileY + coverHeight + hPaddingInSelection + 5;
    for (const auto& line : titleLines) {
      renderer.drawText(SMALL_FONT_ID, tileX + hPaddingInSelection, currentY, line.c_str(), true);
      currentY += titleLineHeight;
    }
    if (hasProgress) {
      char progressBuf[8];
      snprintf(progressBuf, sizeof(progressBuf), "%d%%", progressPercent);
      renderer.drawText(SMALL_FONT_ID, tileX + hPaddingInSelection, currentY, progressBuf, true);
    }
  }
}
